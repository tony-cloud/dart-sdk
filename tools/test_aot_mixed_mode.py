#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""End-to-end tests of mixed-mode AOT patch execution (runtime/vm/mixed_mode.h).

A mixed-mode runtime (dartaotruntime built with `dart_mixed_mode = true`)
runs a patch snapshot whose text is mapped without execute permission:
code linked to the release runs from the release (a signed Mach-O dylib
here), everything else runs in the ARM64 simulator.

Part A, identity linking (always): compiles runtime/tests/mixed_mode/
mixed_template.dart as release (K=3), patch (K=5) and an unrelated release
(K=4). Every edit is a same-size immediate change, so release and patch share
one layout and the runtime may link by identity. Checks:
  * the program's functional checks (calls in both directions, virtual and
    closure calls into simulated code, exceptions across mixed frames, GC
    with simulated frames, async/await), with identity linking and with a
    vmcode v1 container synthesized from the runtime's own link records;
  * a stress run that simulates all Dart code (only stubs native);
  * non-PRODUCT runtimes: the CPU profiler (--profiler --profile_period=100)
    with and without stress (stress is repeated: the sampler interrupts at
    arbitrary points), and a profile of the simulated benchmarks that must
    attribute samples to the simulated hot loop (DART_MIXED_PROFILE_DUMP);
  * fail-closed cases, each of which must reject the patch and run the
    release unpatched.

Part B, the patch linker (--linker PATH/ios_linker.dart): builds pure-Dart
release/patch pairs with gen_snapshot link layout support and the linker
(link info, pass 1, pool hints, constrained final build, link, check). The
release is a stripped, signed Mach-O dylib, the linker's --base input, as an
App.framework is on iOS. Runs the vmcode with the mixed runtime and requires
the vmcode's release hash to match the dylib text, the output to equal a
stock build of the patch, and the runtime's linked/unlinked entry counts to
equal the linker's report.
Pairs: the template (K=3 -> K=5) and a pure-Dart port of the linker's
80/320-caller fixture (docs/ios-linker/COMPILER_LINKER.md section 9).

Usage:
  tools/test_aot_mixed_mode.py --release-dir xcodebuild/ProductARM64 \\
      --mixed-dir xcodebuild/ProductMixedARM64 \\
      [--linker .../tools/ios_linker/bin/ios_linker.dart --dart DART] \\
      [--bench [--runs 3] [-n 20000000]]
The release dir provides gen_snapshot, dartaotruntime and vm_platform.dill
and must be built from the same sources as the mixed runtime (snapshot
hash). Product mode is detected from its args.gn. macOS arm64 only.
"""

import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import statistics
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEMPLATE = os.path.join(ROOT, 'runtime', 'tests', 'mixed_mode',
                        'mixed_template.dart')
EXPECTED_CHECKS = 11
VMCODE_MAGIC = b'OSBVMC\x00\x01'
VMCODE_ALIGN = 16384

failures = []


def log(msg):
    print(msg, flush=True)


def run(cmd, env=None, check=True):
    full_env = dict(os.environ)
    for key in list(full_env):
        if key.startswith('DART_MIXED_'):
            del full_env[key]
    full_env.update(env or {})
    proc = subprocess.run([str(c) for c in cmd],
                          env=full_env,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          text=True,
                          errors='replace')
    if check and proc.returncode != 0:
        raise RuntimeError('%s failed (%d):\n%s\n%s' %
                           (' '.join(str(c) for c in cmd), proc.returncode,
                            proc.stdout[-2000:], proc.stderr[-3000:]))
    return proc


def record(name, ok, detail=''):
    log('%s %s%s' % ('PASS' if ok else 'FAIL', name, detail))
    if not ok:
        failures.append(name)
    return ok


def is_product(build_dir):
    args = os.path.join(build_dir, 'args.gn')
    if not os.path.exists(args):
        return 'Product' in build_dir
    return re.search(r'^\s*is_product\s*=\s*true', open(args).read(),
                     re.M) is not None


# ------------------------------------------------------------------ ELF/vmcode


def elf_symbol_bytes(path, name):
    """Bytes of ELF symbol `name` (its st_size), as the linker hashes them."""
    data = open(path, 'rb').read()
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
    sections = [
        struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    for sh in sections:
        if sh[1] not in (2, 11):  # SHT_SYMTAB, SHT_DYNSYM
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 24):
            st_name, _, _, st_shndx, st_value, st_size = struct.unpack_from(
                '<IBBHQQ', data, off)
            start = strtab[4] + st_name
            sym = data[start:data.index(b'\0', start)].decode()
            if sym == name:
                sec = sections[st_shndx]
                file_off = st_value - sec[3] + sec[4]
                return data[file_off:file_off + st_size]
    raise RuntimeError('%s: no symbol %s' % (path, name))


def text_image(elf):
    for name in ('_kDartSnapshotText', '_kDartIsolateSnapshotInstructions'):
        try:
            return elf_symbol_bytes(elf, name)
        except RuntimeError:
            pass
    raise RuntimeError('%s: no instructions image symbol' % elf)


# InstructionsSection header words that describe the container (bss_offset,
# instructions_relocated_address, build_id_offset): they differ between the
# ELF the linker reads and the Mach-O dylib that ships, the code does not.
CONTAINER_WORDS = range(0x50, 0x68)


def macho_text(dylib, elf_text):
    """The shipped release text: the dylib's bytes matching the ELF text."""
    syms = run(['nm', dylib]).stdout
    addr = int(re.search(r'^([0-9a-f]+) T _kDartSnapshotText$', syms,
                         re.M).group(1), 16)
    # A dylib's __TEXT segment starts at file offset 0 with vmaddr 0.
    text = open(dylib, 'rb').read()[addr:addr + len(elf_text)]
    diff = [
        i for i in range(len(elf_text))
        if text[i] != elf_text[i] and i not in CONTAINER_WORDS
    ]
    if len(text) != len(elf_text) or diff:
        raise RuntimeError('%s text differs from the ELF at %s' %
                           (dylib, [hex(i) for i in diff[:8]]))
    return text


def encode_vmcode(links, patch_elf_bytes, release_text, patch_text_size,
                  header_patch=None):
    """vmcode v1 (COMPILER_LINKER.md section 6) from (p, r, size, flags)."""
    links = sorted(links)
    table_end = 128 + 16 * len(links)
    elf_offset = (table_end + VMCODE_ALIGN - 1) // VMCODE_ALIGN * VMCODE_ALIGN
    header = bytearray(128)
    header[0:8] = VMCODE_MAGIC
    struct.pack_into('<IIQQIIII', header, 8, 1, elf_offset, elf_offset,
                     len(patch_elf_bytes), len(links), 16, 128, 0)
    header[48:80] = hashlib.sha256(release_text).digest()
    header[80:112] = hashlib.sha256(patch_elf_bytes).digest()
    linked = sum(l[2] for l in links)
    struct.pack_into('<IIII', header, 112, len(release_text), patch_text_size,
                     linked, 0)
    if header_patch:
        header_patch(header)
    out = bytearray(header)
    for l in links:
        out += struct.pack('<IIII', *l)
    out += bytes(elf_offset - len(out))
    out += patch_elf_bytes
    return bytes(out)


def read_records(path):
    data = open(path, 'rb').read()
    return [struct.unpack_from('<IIII', data, i) for i in range(0, len(data), 16)]


# -------------------------------------------------------------- compilation


class Toolchain:

    def __init__(self, release_dir):
        self.release_dir = release_dir
        self.dart = os.path.join(ROOT, 'tools', 'sdks', 'dart-sdk', 'bin',
                                 'dart')
        self.packages = os.path.join(ROOT, '.dart_tool', 'package_config.json')
        self.gen_kernel = os.path.join(ROOT, 'pkg', 'vm', 'bin',
                                       'gen_kernel.dart')
        self.gen_snapshot = os.path.join(release_dir, 'gen_snapshot')
        self.platform = os.path.join(release_dir, 'vm_platform.dill')
        self.runtime = os.path.join(release_dir, 'dartaotruntime')
        self.defines = (['-Ddart.vm.product=true']
                        if is_product(release_dir) else [])

    def kernel(self, source_text, fixture, dill):
        # One source URI for every variant: linker identities include it.
        with open(fixture, 'w') as f:
            f.write(source_text)
        run([
            self.dart, '--packages=' + self.packages, self.gen_kernel, '--aot',
            '--platform=' + self.platform
        ] + self.defines + ['-o', dill, fixture])

    def gen(self, args):
        return run([self.gen_snapshot, '--deterministic'] + args).stdout

    def elf(self, dill, out, extra=()):
        self.gen(['--snapshot_kind=app-aot-elf', '--elf=' + out] +
                 list(extra) + [dill])

    def dylib(self, dill, out, extra=()):
        self.gen(['--snapshot_kind=app-aot-macho-dylib', '--macho=' + out] +
                 list(extra) + [dill])
        if '--strip' in extra:
            # gen_snapshot keeps local function symbols in a Mach-O dylib;
            # an App.framework ships without them (Xcode strips it).
            run(['strip', '-x', out])
        run(['codesign', '-f', '-s', '-', out])


# ------------------------------------------------------------ program checks


def stable_output(text):
    """Program output without timing lines."""
    return '\n'.join(l for l in text.splitlines() if not l.startswith('TIME '))


def check_program(name, proc, variant, rejected, extra=None):
    out = proc.stdout
    passes = out.count('PASS ')
    ok = (proc.returncode == 0 and passes == EXPECTED_CHECKS and
          'FAIL ' not in out and ('variant K=%d' % variant) in out)
    was_rejected = 'patch rejected' in proc.stderr
    ok = ok and (was_rejected == rejected)
    if ok and extra is not None:
        ok = extra(proc)
    reason = ''
    m = re.search(r'patch rejected \((.*?)\); running', proc.stderr)
    if m:
        reason = ' [%s]' % m.group(1)
    record(
        name, ok, ': exit=%d checks=%d/%d variant=K=%d rejected=%s%s' %
        (proc.returncode, passes, EXPECTED_CHECKS, variant, was_rejected,
         reason))
    if not ok:
        log(proc.stderr[-2000:])
    return ok


def link_stats(stderr):
    m = re.search(r'linked=(\d+) unlinked=(\d+) linked-bytes=(\d+)/(\d+)',
                  stderr)
    return tuple(int(x) for x in m.groups()) if m else None


def first_stub_offset(aot):
    data = open(aot, 'rb').read()
    shoff, = struct.unpack_from('<Q', data, 0x28)
    shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
    sections = [
        struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
        for i in range(shnum)
    ]
    text = None
    stubs = []
    for sh in sections:
        if sh[1] != 2:
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 24):
            st_name, _, _, _, st_value, _ = struct.unpack_from(
                '<IBBHQQ', data, off)
            start = strtab[4] + st_name
            sym = data[start:data.index(b'\0', start)].decode(errors='replace')
            if sym == '_kDartSnapshotText':
                text = st_value
            elif sym.startswith('stub '):
                stubs.append(st_value)
    if text is None or not stubs:
        return None
    return min(stubs) - text


def part_a(tc, mixed, out, bench_args):
    log('== Part A: identity linking (%s)' % out)
    fixture = os.path.join(out, 'main.dart')
    template = open(TEMPLATE).read()
    for name, k in (('base', 3), ('patch', 5), ('other', 4)):
        dill = os.path.join(out, name + '.dill')
        tc.kernel(template.replace('__K__', str(k)), fixture, dill)
        tc.elf(dill, os.path.join(out, name + '.aot'))
        tc.dylib(dill, os.path.join(out, name + '.dylib'))
    base = os.path.join(out, 'base.dylib')
    other = os.path.join(out, 'other.dylib')
    patch = os.path.join(out, 'patch.aot')
    records = os.path.join(out, 'records.bin')
    small = '1000'

    check_program('release runs natively', run([tc.runtime, base, small],
                                                check=False), 3, False)

    def mixed_run(env, args=(), target=patch):
        env = dict(env)
        env.setdefault('DART_MIXED_BASE', base)
        env['DART_MIXED_VERBOSE'] = '1'
        return run([mixed] + list(args) + [target, small], env, check=False)

    def linked_well(proc):
        stats = link_stats(proc.stderr)
        scavenges = proc.stderr.count('Scavenge')
        log('  linked=%s unlinked=%s bytes=%s/%s scavenges=%d' %
            (stats + (scavenges,) if stats else ('?',) * 4 + (scavenges,)))
        return (stats is not None and stats[2] * 100 >= 95 * stats[3] and
                stats[1] <= 40 and scavenges > 0 and
                'native->sim 0 ' not in proc.stderr)

    proc = mixed_run({'DART_MIXED_DUMP_LINK_TABLE': records},
                     args=['--verbose_gc'])
    check_program('patch, identity linking', proc, 5, False, linked_well)

    # A vmcode container built from the runtime's own records.
    patch_elf = open(patch, 'rb').read()
    release_text = macho_text(base, text_image(os.path.join(out, 'base.aot')))
    other_text = macho_text(other, text_image(os.path.join(out,
                                                            'other.aot')))
    patch_text_size = len(text_image(patch))
    links = read_records(records)

    def vmcode(name, ls=None, header_patch=None, elf=None, text=None,
               mutate=None):
        data = encode_vmcode(links if ls is None else ls,
                             patch_elf if elf is None else elf,
                             release_text if text is None else text,
                             patch_text_size, header_patch)
        if mutate:
            data = mutate(bytearray(data))
        path = os.path.join(out, name + '.vmcode')
        open(path, 'wb').write(bytes(data))
        return path

    good = vmcode('good')
    check_program('patch, vmcode from runtime records', mixed_run({},
                                                                  target=good),
                  5, False, lambda p: 'table=vmcode' in p.stderr)

    offset = first_stub_offset(patch)
    if offset is None:
        log('SKIP stress: no symbols')
    else:
        proc = mixed_run({'DART_MIXED_UNLINK_BELOW': hex(offset)},
                         args=['--verbose_gc'])
        check_program('stress, all Dart code simulated', proc, 5, False,
                      lambda p: (link_stats(p.stderr) or (0, 0))[1] > 1000)

    # The profiler samples simulated code from the simulated registers
    # (MixedMode::SampleSimulatedState); not available in PRODUCT runtimes.
    profiler = ['--profiler', '--profile_period=100']
    if is_product(tc.release_dir):
        log('SKIP profiler: PRODUCT runtime')
    else:
        check_program('profiler', mixed_run({}, args=profiler), 5, False)
        for i in range(3 if offset is not None else 0):
            proc = mixed_run({'DART_MIXED_UNLINK_BELOW': hex(offset)},
                             args=profiler)
            check_program(
                'profiler, stress (%d/3)' % (i + 1), proc, 5, False,
                lambda p: ((link_stats(p.stderr) or (0, 0))[1] > 1000 and
                           'profiler samples while simulating: walked' in
                           p.stderr))
        # numLoopB (simulated, ~40 ms at n = 4M) must lead the profile.
        proc = run([mixed] + profiler + [patch, '4000000'],
                   dict(DART_MIXED_BASE=base, DART_MIXED_VERBOSE='1',
                        DART_MIXED_PROFILE_DUMP='1'),
                   check=False)
        top = re.findall(r'mixed-profile: exclusive (\d+) inclusive \d+ '
                         r'(\S+) (.*)', proc.stderr)
        ok = (proc.returncode == 0 and bool(top) and
              top[0][1:] == ('Dart', 'numLoopB') and int(top[0][0]) >= 50)
        record('profiler attributes simulated code', ok,
               ': exit=%d top=%s' % (proc.returncode, top[:3]))
        if not ok:
            log(proc.stderr[-2000:])

    # Fail-closed: the release (K=3, or K=4 for `other`) must run instead.
    def flip_elf_byte(d):
        d[-100] ^= 0xFF
        return d

    def pad_byte(d):
        d[128 + 16 * len(links) + 8] = 1
        return d

    unlinked_entry = None  # A patch entry the runtime did not link.
    linked_starts = set(l[0] for l in links)
    proc = mixed_run({})
    for m in re.finditer(r'unlinked entry \d+ offset (0x[0-9a-f]+) size (\d+)',
                         proc.stderr):
        off, size = int(m.group(1), 16), int(m.group(2))
        if off not in linked_starts:
            unlinked_entry = (off, size)
            break
    # Keep the records sorted and disjoint so that only the entry check fails.
    # A record near the middle that is large enough to shrink by 16 bytes.
    mid = next(i for i in range(len(links) // 2, len(links))
               if links[i][2] >= 32)
    p, r, size, flags = links[mid]
    shifted = links[:mid] + [(p + 16, r + 16, size - 16, 0)] + links[mid + 1:]
    resized = links[:mid] + [(p, r, size - 16, 0)] + links[mid + 1:]
    # (name, env, patch, variant that must run, expected rejection reason)
    cases = [
        ('gate exhaustion', {'DART_MIXED_GATE_LIMIT': '64'}, good, 3,
         'more gates'),
        ('truncated vmcode', {}, vmcode('truncated', mutate=lambda d: d[:100]),
         3, 'bad vmcode magic'),
        ('bad vmcode magic', {},
         vmcode('bad_magic',
                header_patch=lambda h: h.__setitem__(slice(6, 8), b'\0\2')),
         3, 'bad vmcode magic'),
        ('bad vmcode version', {},
         vmcode('bad_version',
                header_patch=lambda h: struct.pack_into('<I', h, 8, 2)), 3,
         'unsupported vmcode version'),
        ('malformed vmcode header', {},
         vmcode('bad_header',
                header_patch=lambda h: struct.pack_into('<I', h, 36, 12)), 3,
         'malformed vmcode header'),
        ('nonzero vmcode padding', {}, vmcode('bad_pad', mutate=pad_byte), 3,
         'malformed vmcode padding'),
        ('patch ELF hash mismatch', {}, vmcode('elf_hash',
                                               mutate=flip_elf_byte), 3,
         'ELF hash mismatch'),
        ('different release', {'DART_MIXED_BASE': other}, good, 4,
         'different release'),
        ('release hash mismatch', {}, vmcode('text_hash', text=other_text), 3,
         'different release'),
        ('link record not at an entry', {},
         vmcode('shifted', ls=shifted), 3,
         'does not start a patch code entry'),
        ('link record size mismatch', {},
         vmcode('resized', ls=resized), 3,
         'size differs'),
        ('nonzero link flags', {},
         vmcode('flags', ls=[links[0][:3] + (1,)] + links[1:]), 3,
         'malformed link record'),
        ('overlapping link records', {},
         vmcode('overlap', ls=links + [links[len(links) // 2]]), 3,
         'not sorted or overlap'),
        ('link record outside the release', {},
         vmcode('outside',
                ls=links[:-1] + [links[-1][:1] + (len(release_text),) +
                                 links[-1][2:]]), 3, 'outside'),
        ('unmapped release', {'DART_MIXED_TEST_UNMAP_BASE': '1'}, good, 3,
         'no release image is mapped'),
        ('unmapped release (identity)', {
            'DART_MIXED_TEST_UNMAP_BASE': '1'
        }, patch, 3, 'needs the release image'),
        ('stubs entered from C++ unlinked (no records)', {},
         vmcode('empty', ls=[]), 3, 'stub entered from C++'),
        ('stubs entered from C++ unlinked (identity)', {
            'DART_MIXED_UNLINK_ALL': '1'
        }, patch, 3, 'stub entered from C++'),
    ]
    if unlinked_entry is not None:
        off, size = unlinked_entry
        cases.append(('record links changed code', {},
                      vmcode('links_changed',
                             ls=links + [(off, off, size, 0)]), 3,
                      'does not match the release'))
    else:
        record('fail-closed: record links changed code', False,
               ': no unlinked entry found')
    for name, env, target, variant, reason in cases:
        check_program('fail-closed: ' + name, mixed_run(env, target=target),
                      variant, True, lambda p, r=reason: r in p.stderr)

    if bench_args is not None:
        bench(tc.runtime, mixed, base, patch, *bench_args)


def bench(native, mixed, base, patch, runs, n):
    times = collections.defaultdict(list)
    for _ in range(runs):
        for mode, cmd, env in (('native', [native, base, str(n)], {}),
                               ('mixed', [mixed, patch, str(n)], {
                                   'DART_MIXED_BASE': base
                               })):
            for line in run(cmd, env).stdout.splitlines():
                if line.startswith('TIME '):
                    name, us = line[5:].rsplit(' ', 1)
                    times[(mode, name)].append(int(us[:-2]))
    names = []
    for (_, name) in times:
        if name not in names:
            names.append(name)
    log('benchmark (median of %d, n=%d)' % (runs, n))
    for name in names:
        a = statistics.median(times[('native', name)])
        b = statistics.median(times[('mixed', name)])
        log('  %-36s native %9.1f ms  mixed %9.1f ms  ratio %7.1fx' %
            (name, a / 1000, b / 1000, b / max(a, 1)))


# --------------------------------------------------------- Part B: linker

FIXTURE = '''const label = 'Shorebird baseline';
@pragma('vm:never-inline')
int sharedLeaf(int x) => (x * 3 + 1) & 65535;

class Model {
  final int seed;
  Model(this.seed);
  @pragma('vm:never-inline')
  int evaluate(int x) => sharedLeaf(x + seed);
}

CALLERS

@pragma('vm:never-inline')
int compute(int x) {
  final model = Model(x);
  return SUM;
}

void main(List<String> args) {
  final x = args.isEmpty ? 12345 : int.parse(args[0]);
  final value = compute(x);
  print('$label: $value');
}
'''


def fixture_source(fanout):
    callers = '\n'.join(
        "@pragma('vm:never-inline')\nint caller%d(Model m, int x) => "
        'm.evaluate(x + %d);' % (i, i) for i in range(fanout))
    total = ' + '.join('caller%d(model, x)' % i for i in range(fanout))
    return FIXTURE.replace('CALLERS', callers).replace('SUM', total)


def fixture_cases(fanout):
    b = fixture_source(fanout)
    return [
        ('p0_identical', b),
        ('p1_string', b.replace('Shorebird baseline', 'Shorebird patched!')),
        ('p2_one_caller',
         b.replace('int caller17(Model m, int x) => m.evaluate(x + 17);',
                   'int caller17(Model m, int x) => m.evaluate(x + 117);')),
        ('p3_shared_leaf', b.replace('(x * 3 + 1)', '(x * 7 + 2)')),
        ('p4_field_layout',
         b.replace('final int seed;\n  Model(this.seed);',
                   'final int seed;\n  final int offset;\n'
                   '  Model(this.seed) : offset = seed ~/ 3;').replace(
                       'sharedLeaf(x + seed)', 'sharedLeaf(x + seed + offset)')),
        ('p5_new_function',
         b.replace(
             "const label =",
             "@pragma('vm:never-inline')\nint newFeature(int x) => x ^ 12345;"
             "\n\nconst label =").replace('final value = compute(',
                                          'final value = newFeature(9) + compute(')),
        ('p6_remove_caller', b.replace('caller17(model, x) + ', '')),
    ]


# Release builds ship stripped (the linker locates the text image by its
# symbol and the sidecar's size).
STRIP = ['--strip']


def part_b(tc, mixed, out, linker, dart):
    log('== Part B: patch linker (%s)' % out)
    template = open(TEMPLATE).read()
    pairs = [('template_k3_k5', template.replace('__K__', '3'),
              [('patch_k5', template.replace('__K__', '5'))], '1000')]
    for fanout in (80, 320):
        cases = fixture_cases(fanout)
        pairs.append(('fixture%d' % fanout, cases[0][1], cases, None))

    def tool(args):
        return run([dart, linker] + args)

    for pair_name, release_src, patches, arg in pairs:
        d = os.path.join(out, pair_name)
        os.makedirs(d, exist_ok=True)
        fixture = os.path.join(d, 'main.dart')
        rel_dill = os.path.join(d, 'rel.dill')
        tc.kernel(release_src, fixture, rel_dill)
        app_link = os.path.join(d, 'App.link')
        tc.elf(rel_dill, os.path.join(d, 'App.aot'),
               STRIP + ['--print_link_info_to=' + app_link])
        release = os.path.join(d, 'App.dylib')
        tc.dylib(rel_dill, release, STRIP)
        release_text = macho_text(release, text_image(os.path.join(d,
                                                                   'App.aot')))
        args = [arg] if arg else []
        release_out = stable_output(
            run([tc.runtime, os.path.join(d, 'App.dylib')] + args).stdout)
        for case_name, patch_src in patches:
            c = os.path.join(d, case_name)
            os.makedirs(c, exist_ok=True)
            dill = os.path.join(c, 'patch.dill')
            tc.kernel(patch_src, fixture, dill)
            tc.dylib(dill, os.path.join(c, 'stock.dylib'), STRIP)
            tc.elf(dill, os.path.join(c, 'pass1.aot'), STRIP + [
                '--link_base=' + app_link,
                '--print_link_info_to=' + os.path.join(c, 'pass1.link')
            ])
            tool([
                'pool-hints', '--base-link', app_link, '--patch-link',
                os.path.join(c, 'pass1.link'), '--out',
                os.path.join(c, 'constraints.link')
            ])
            tc.elf(dill, os.path.join(c, 'out.aot'), STRIP + [
                '--link_base=' + os.path.join(c, 'constraints.link'),
                '--print_link_info_to=' + os.path.join(c, 'out.link')
            ])
            report = os.path.join(c, 'report.json')
            vm = os.path.join(c, 'out.vmcode')
            # The release input is the shipped Mach-O, whose text the vmcode
            # hashes (COMPILER_LINKER.md section 6.1).
            tool([
                'link', '--base', release, '--base-link', app_link,
                '--patch-elf',
                os.path.join(c, 'out.aot'), '--patch-link',
                os.path.join(c, 'out.link'), '--out', vm, '--report', report
            ])
            check = tool([
                'check', '--vmcode', vm, '--base', release, '--patch-elf',
                os.path.join(c, 'out.aot')
            ]).stdout.strip()
            header = open(vm, 'rb').read(128)
            hash_ok = (header[48:80] == hashlib.sha256(release_text).digest())
            rep = json.load(open(report))
            stock = stable_output(
                run([tc.runtime, os.path.join(c, 'stock.dylib')] +
                    args).stdout)
            proc = run([mixed, vm] + args, {
                'DART_MIXED_BASE': os.path.join(d, 'App.dylib'),
                'DART_MIXED_VERBOSE': '1'
            },
                       check=False)
            stats = link_stats(proc.stderr)
            ok = (hash_ok and proc.returncode == 0 and stable_output(proc.stdout) == stock and
                  'patch rejected' not in proc.stderr and stats is not None and
                  stats[0] == rep['linked_entries'] and
                  stats[1] == rep['unlinked_entries'])
            record(
                '%s/%s' % (pair_name, case_name), ok,
                ': linker linked=%d unlinked=%d (%.2f%% bytes); runtime %s; '
                'output %s stock%s; release hash %s; check: %s' %
                (rep['linked_entries'], rep['unlinked_entries'],
                 100.0 * rep['linked_code_bytes'] /
                 max(1, rep['linked_code_bytes'] + rep['unlinked_code_bytes']),
                 'linked=%d unlinked=%d' % stats[:2] if stats else '?',
                 '==' if stable_output(proc.stdout) == stock else '!=',
                 ' (release differs)' if stock != release_out else '',
                 'ok' if hash_ok else 'MISMATCH',
                 check[:120]))
            if not ok:
                log(proc.stderr[-1500:])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--release-dir', required=True)
    parser.add_argument('--mixed-dir', required=True)
    parser.add_argument('--out', help='work dir (default <mixed-dir>/'
                        'mixed_mode_test)')
    parser.add_argument('--linker', help='path to ios_linker.dart (part B)')
    parser.add_argument('--dart', help='Dart VM to run the linker with')
    parser.add_argument('--skip-identity', action='store_true')
    parser.add_argument('--bench', action='store_true')
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('-n', type=int, default=20000000)
    args = parser.parse_args()
    release_dir = os.path.abspath(args.release_dir)
    mixed = os.path.join(os.path.abspath(args.mixed_dir), 'dartaotruntime')
    out = os.path.abspath(args.out or
                          os.path.join(args.mixed_dir, 'mixed_mode_test'))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    tc = Toolchain(release_dir)
    log('release dir %s (product=%s), mixed runtime %s' %
        (release_dir, is_product(release_dir), mixed))
    if not args.skip_identity:
        a = os.path.join(out, 'identity')
        os.makedirs(a)
        part_a(tc, mixed, a, (args.runs, args.n) if args.bench else None)
    if args.linker:
        b = os.path.join(out, 'linker')
        os.makedirs(b)
        part_b(tc, mixed, b, os.path.abspath(args.linker), args.dart or
               tc.dart)

    if failures:
        log('FAILED: %d case(s): %s' % (len(failures), ', '.join(failures)))
        return 1
    log('ALL PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
