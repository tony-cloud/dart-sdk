#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""End-to-end test of mixed-mode AOT patch execution (runtime/vm/mixed_mode.h).

Compiles runtime/tests/mixed_mode/mixed_template.dart three times:
  base  (K=3)  the release: Mach-O dylib, loaded natively (signed ad hoc)
  patch (K=5)  the patch:   ELF, loaded with non-executable text
  other (K=4)  an unrelated release, for the mismatched-release case
Every edit is a same-size immediate change, so base and patch share one
layout and the runtime's identity linking (or the link table it dumps) is
valid.

Then runs, with a dartaotruntime built with `dart_mixed_mode = true`:
  * the functional checks of the program (calls in both directions, virtual
    and closure calls into simulated code, exceptions across mixed frames, GC
    with simulated frames, async/await) with identity linking and with a
    link table round-tripped through a file;
  * a stress run simulating all Dart code (only stubs native);
  * fail-closed cases, each of which must reject the patch and run the
    release unpatched: gate exhaustion, truncated table, bad magic, entry
    mismatch, a table that links changed code, entry count mismatch, a
    different release, an unmapped release (with and without a table), and
    unlinked stubs that C++ enters;
  * optionally (--bench) the benchmarks, native release vs mixed patch.

Usage:
  tools/test_aot_mixed_mode.py --release-dir xcodebuild/ReleaseARM64 \\
      --mixed-dir xcodebuild/ReleaseMixedARM64 [--bench] [--runs 3]
The release dir provides gen_snapshot, dartaotruntime and vm_platform.dill and
must be built from the same sources as the mixed runtime (snapshot hash).
Product mode is detected from the release dir's args.gn. macOS arm64 only.
"""

import argparse
import collections
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
HEADER = struct.Struct('<IIQQQII')  # MixedLinkTableHeader
ENTRY = struct.Struct('<III')  # MixedLinkTableEntry
UNLINKED = 0xFFFFFFFF

failures = []


def log(msg):
    print(msg, flush=True)


def run(cmd, env=None, check=True):
    full_env = dict(os.environ)
    for key in list(full_env):
        if key.startswith('DART_MIXED_'):
            del full_env[key]
    full_env.update(env or {})
    proc = subprocess.run(cmd,
                          env=full_env,
                          stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          text=True)
    if check and proc.returncode != 0:
        raise RuntimeError('%s failed (%d):\n%s\n%s' %
                           (' '.join(cmd), proc.returncode, proc.stdout,
                            proc.stderr[-3000:]))
    return proc


def is_product(build_dir):
    args = os.path.join(build_dir, 'args.gn')
    if not os.path.exists(args):
        return 'Product' in build_dir
    return re.search(r'^\s*is_product\s*=\s*true', open(args).read(),
                     re.M) is not None


def compile_variants(release_dir, out):
    dart = os.path.join(ROOT, 'tools', 'sdks', 'dart-sdk', 'bin', 'dart')
    packages = os.path.join(ROOT, '.dart_tool', 'package_config.json')
    gen_kernel = os.path.join(ROOT, 'pkg', 'vm', 'bin', 'gen_kernel.dart')
    gen_snapshot = os.path.join(release_dir, 'gen_snapshot')
    platform = os.path.join(release_dir, 'vm_platform.dill')
    defines = ['-Ddart.vm.product=true'] if is_product(release_dir) else []
    template = open(TEMPLATE).read()
    for name, k in (('base', 3), ('patch', 5), ('other', 4)):
        source = os.path.join(out, name + '.dart')
        with open(source, 'w') as f:
            f.write(template.replace('__K__', str(k)))
        dill = os.path.join(out, name + '.dill')
        run([
            dart, '--packages=' + packages, gen_kernel, '--aot',
            '--platform=' + platform
        ] + defines + ['-o', dill, source])
        run([
            gen_snapshot, '--deterministic', '--snapshot_kind=app-aot-elf',
            '--elf=' + os.path.join(out, name + '.aot'), dill
        ])
        dylib = os.path.join(out, name + '.dylib')
        run([
            gen_snapshot, '--deterministic',
            '--snapshot_kind=app-aot-macho-dylib', '--macho=' + dylib, dill
        ])
        run(['codesign', '-f', '-s', '-', dylib])


def check_output(name, proc, variant, rejected, extra=None):
    out = proc.stdout
    passes = out.count('PASS ')
    ok = (proc.returncode == 0 and passes == EXPECTED_CHECKS and
          'FAIL ' not in out and ('variant K=%d' % variant) in out)
    was_rejected = 'patch rejected' in proc.stderr
    ok = ok and (was_rejected == rejected)
    if ok and extra is not None:
        ok = extra(proc)
    reason = ''
    m = re.search(r'patch rejected \(([^)]*)', proc.stderr)
    if m:
        reason = ' [%s]' % m.group(1)
    log('%s %s: exit=%d checks=%d/%d variant=%s rejected=%s%s' %
        ('PASS' if ok else 'FAIL', name, proc.returncode, passes,
         EXPECTED_CHECKS, 'K=%d' % variant, was_rejected, reason))
    if not ok:
        failures.append(name)
        log(proc.stderr[-2000:])
    return ok


def link_stats(stderr):
    m = re.search(r'linked=(\d+) unlinked=(\d+) linked-bytes=(\d+)/(\d+)',
                  stderr)
    return tuple(int(x) for x in m.groups()) if m else None


def first_stub_offset(aot):
    nm = None
    for cand in ('mac-arm64', 'mac-x64', 'linux-x64'):
        path = os.path.join(ROOT, 'buildtools', cand, 'clang', 'bin',
                            'llvm-nm')
        if os.path.exists(path):
            nm = path
            break
    if nm is None:
        return None
    syms = run([nm, '-n', aot]).stdout.splitlines()
    text = stub = None
    for line in syms:
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        if parts[2] == '_kDartSnapshotText':
            text = int(parts[0], 16)
        elif parts[2].startswith('stub ') and stub is None:
            stub = int(parts[0], 16)
    if text is None or stub is None:
        return None
    return stub - text


def write_table(path, header, entries):
    with open(path, 'wb') as f:
        f.write(HEADER.pack(*header))
        for e in entries:
            f.write(ENTRY.pack(*e))


def read_table(path):
    data = open(path, 'rb').read()
    header = list(HEADER.unpack_from(data, 0))
    entries = [
        list(ENTRY.unpack_from(data, HEADER.size + i * ENTRY.size))
        for i in range(header[5])
    ]
    return header, entries


def bench(release_dir, mixed, out, runs, n):
    times = collections.defaultdict(list)
    native = os.path.join(release_dir, 'dartaotruntime')
    for _ in range(runs):
        for mode, cmd, env in (
            ('native', [native, os.path.join(out, 'base.dylib'), str(n)], {}),
            ('mixed', [mixed, os.path.join(out, 'patch.aot'), str(n)], {
                'DART_MIXED_BASE': os.path.join(out, 'base.dylib')
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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--release-dir', required=True)
    parser.add_argument('--mixed-dir', required=True)
    parser.add_argument('--out', help='work dir (default <mixed-dir>/'
                        'mixed_mode_test)')
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

    log('compiling variants into %s (product=%s)' %
        (out, is_product(release_dir)))
    compile_variants(release_dir, out)
    base = os.path.join(out, 'base.dylib')
    other = os.path.join(out, 'other.dylib')
    patch = os.path.join(out, 'patch.aot')
    table = os.path.join(out, 'good.lt')
    small = '1000'

    proc = run([os.path.join(release_dir, 'dartaotruntime'), base, small],
               check=False)
    check_output('release runs natively', proc, 3, False)

    def mixed_run(env, args=(), check=False):
        env = dict(env)
        env.setdefault('DART_MIXED_BASE', base)
        env['DART_MIXED_VERBOSE'] = '1'
        return run([mixed] + list(args) + [patch, small], env, check=check)

    proc = mixed_run({'DART_MIXED_DUMP_LINK_TABLE': table},
                     args=['--verbose_gc'])

    def linked_well(proc):
        stats = link_stats(proc.stderr)
        scavenges = proc.stderr.count('Scavenge')
        ok = (stats is not None and stats[2] * 100 >= 95 * stats[3] and
              stats[1] <= 40 and scavenges > 0 and
              'native->sim 0 ' not in proc.stderr)
        log('  linked=%s unlinked=%s bytes=%s/%s scavenges=%d' %
            (stats + (scavenges,) if stats else ('?',) * 4 + (scavenges,)))
        return ok

    check_output('patch, identity linking', proc, 5, False, linked_well)
    proc = mixed_run({'DART_MIXED_LINK_TABLE': table})
    check_output('patch, link table from file', proc, 5, False,
                 lambda p: 'table=file' in p.stderr)

    offset = first_stub_offset(patch)
    if offset is None:
        log('SKIP stress: llvm-nm or symbols not found')
    else:
        proc = mixed_run({'DART_MIXED_UNLINK_BELOW': hex(offset)},
                         args=['--verbose_gc'])
        check_output(
            'stress, all Dart code simulated', proc, 5, False, lambda p:
            (link_stats(p.stderr) or (0, 0))[1] > 1000)

    # Fail-closed cases: the release (K=3, or K=4 for `other`) must run.
    header, entries = read_table(table)
    linked = [i for i, e in enumerate(entries) if e[2] != UNLINKED]
    unlinked = [i for i, e in enumerate(entries) if e[2] == UNLINKED]

    def variant_table(name, h, es):
        path = os.path.join(out, name + '.lt')
        write_table(path, h, es)
        return path

    raw = open(table, 'rb').read()
    truncated = os.path.join(out, 'truncated.lt')
    open(truncated, 'wb').write(raw[:20])
    bad_magic = os.path.join(out, 'bad_magic.lt')
    open(bad_magic, 'wb').write(b'XXXX' + raw[4:])
    es = [list(e) for e in entries]
    es[linked[len(linked) // 2]][0] += 4
    shifted = variant_table('shifted', header, es)
    es = [list(e) for e in entries]
    es[unlinked[0]][2] = es[unlinked[0]][0]  # Claim changed code is linked.
    links_changed = variant_table('links_changed', header, es)
    es = [list(e) for e in entries] + [list(entries[-1])]
    h = list(header)
    h[5] += 1
    extra_entry = variant_table('extra_entry', h, es)

    cases = [
        ('gate exhaustion', {'DART_MIXED_GATE_LIMIT': '64'}, 3),
        ('truncated link table', {'DART_MIXED_LINK_TABLE': truncated}, 3),
        ('bad link table magic', {'DART_MIXED_LINK_TABLE': bad_magic}, 3),
        ('link table entry mismatch', {'DART_MIXED_LINK_TABLE': shifted}, 3),
        ('link table links changed code', {
            'DART_MIXED_LINK_TABLE': links_changed
        }, 3),
        ('link table entry count mismatch', {
            'DART_MIXED_LINK_TABLE': extra_entry
        }, 3),
        ('different release', {
            'DART_MIXED_LINK_TABLE': table,
            'DART_MIXED_BASE': other
        }, 4),
        ('unmapped release (table)', {
            'DART_MIXED_LINK_TABLE': table,
            'DART_MIXED_TEST_UNMAP_BASE': '1'
        }, 3),
        ('unmapped release (identity)', {
            'DART_MIXED_TEST_UNMAP_BASE': '1'
        }, 3),
        ('stubs entered from C++ unlinked', {'DART_MIXED_UNLINK_ALL': '1'}, 3),
        ('missing link table file', {
            'DART_MIXED_LINK_TABLE': os.path.join(out, 'missing.lt')
        }, 3),
    ]
    for name, env, variant in cases:
        check_output('fail-closed: ' + name, mixed_run(env), variant, True)

    if args.bench:
        bench(release_dir, mixed, out, args.runs, args.n)

    if failures:
        log('FAILED: %d case(s): %s' % (len(failures), ', '.join(failures)))
        return 1
    log('ALL PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
