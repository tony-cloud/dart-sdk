#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Guarded inlining of patchable functions in structural AOT snapshots.

`gen_snapshot --shorebird-structural-patching` inlines small patchable
functions behind a per-function guard: the inlined copy runs only while the
function has no bytecode, otherwise the original call is made (through
`Function::entry_point`, i.e. into the interpreter once patched). See
"Inlining patchable functions behind a per-function guard" in
docs/shorebird-structural-patching.md.

This test compiles a base program whose callers inline small callees
(static function, getter in a loop, setter, operator, constructor, instance
method, polymorphic targets, optional positional and named parameters,
transitive chains), checks with `--trace_shorebird_patchable_inlining` that
gen_snapshot really inlined every callee under test, and then checks that

 1. the unpatched snapshot computes the base program's results;
 2. after a structural patch that changes only the callees (callers are
    identical in base and update, so they stay AOT code), every former inline
    site observes the new semantics, including a callee inlined into an
    inlined callee (transitive case: changing the innermost or the middle
    function) and changed default values of omitted optional arguments;
 3. a stack trace through a patched inlined callee has the frames of the
    non-inlined call (the fallback call is attributed to the call site).

Expected values come from running the base and the updated source on the host
JIT VM. Requires the same artifacts as test_aot_structural_patch.py. Use
--product for the PRODUCT artifacts.
"""

import argparse
import contextlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# Callees. CALLEES_BASE and CALLEES_UPDATED differ only in function bodies
# (including default values), never in signatures or fields.
CALLEES_BASE = r"""
int twice(int x) => x * 2;
class Chip {
  int get weight => 3;
}
class Box {
  int v = 0;
  set value(int x) {
    v = x + 1;
  }
}
class Vec {
  final int x;
  Vec(this.x);
  Vec operator +(Vec o) => Vec(x + o.x);
}
class Pt {
  final int x;
  final int y;
  Pt(int a, int b) : x = a, y = b;
}
int leafC(int x) => x + 1;
int midB(int x) => leafC(x);
int topA(int x) => midB(x) + 3;
int leafC2(int x) => x + 1;
int midB2(int x) => leafC2(x);
int topA2(int x) => midB2(x) + 3;
int scale(int a, {int by = 2}) => a * by;
int pos(int a, [int b = 1]) => a + b;
abstract class Shape {
  int sides();
}
class Tri extends Shape {
  int sides() => 3;
}
class Sq extends Shape {
  int sides() => 4;
}
class Point {
  final int x;
  final int y;
  Point(this.x, this.y);
  int dot(Point o) => x * o.x + y * o.y;
}
int thrower(int x) => x;
int unchanged(int x) => x + 7;
"""

CALLEES_UPDATED = r"""
int twice(int x) => x * 3;
class Chip {
  int get weight => 4;
}
class Box {
  int v = 0;
  set value(int x) {
    v = x + 2;
  }
}
class Vec {
  final int x;
  Vec(this.x);
  Vec operator +(Vec o) => Vec(x + o.x + 100);
}
class Pt {
  final int x;
  final int y;
  Pt(int a, int b) : x = a + 10, y = b + 20;
}
int leafC(int x) => x + 100;
int midB(int x) => leafC(x);
int topA(int x) => midB(x) + 3;
int leafC2(int x) => x + 1;
int midB2(int x) => leafC2(x) * 5;
int topA2(int x) => midB2(x) + 3;
int scale(int a, {int by = 5}) => a * by;
int pos(int a, [int b = 10]) => a + b;
abstract class Shape {
  int sides();
}
class Tri extends Shape {
  int sides() => 30;
}
class Sq extends Shape {
  int sides() => 4;
}
class Point {
  final int x;
  final int y;
  Point(this.x, this.y);
  int dot(Point o) => x * o.x + y * o.y + 1000;
}
int thrower(int x) => throw StateError('patched');
int unchanged(int x) => x + 7;
"""

# Callers: identical in base and update (so they stay AOT code).
CALLERS = r"""
int callTwice(int x) => twice(x) + 1;
int chipLoop(Chip c, int n) {
  var acc = 0;
  for (var i = 0; i < n; i++) {
    acc += c.weight;
  }
  return acc;
}
int setBox(Box b, int x) {
  b.value = x;
  return b.v;
}
int addVec(Vec a, Vec b) {
  var s = 0;
  for (var i = 0; i < 2; i++) {
    s += (a + b).x;
  }
  return s;
}
int makePt(int a, int b) {
  final p = Pt(a, b);
  return p.x * 100 + p.y;
}
// In loops: outside loops the AOT inliner only inlines leaf callees.
int callTop(int x) {
  var s = 0;
  for (var i = 0; i < 2; i++) {
    s = s * 1000 + topA(x + i);
  }
  return s;
}
int callTop2(int x) {
  var s = 0;
  for (var i = 0; i < 2; i++) {
    s = s * 1000 + topA2(x + i);
  }
  return s;
}
int callScale(int a) => scale(a) * 1000 + scale(a, by: 4);
int callPos(int a) => pos(a) * 1000 + pos(a, 2);
// Devirtualized over the base classes Tri and Sq: a polymorphic call whose
// two targets are inlined, each behind its own guard.
int sidesOf(Shape x) => x.sides();
int sumSides(List<Shape> l) {
  var s = 0;
  for (var i = 0; i < l.length; i++) {
    s = s * 100 + sidesOf(l[i]);
  }
  return s;
}
int callDot(Point p) => p.dot(p);
int callThrower(int x) {
  try {
    return thrower(x);
  } catch (e, st) {
    // Frames of `thrower` in the stack trace: one, as for a non-inlined call.
    return 1000 +
        st.toString().split('\n').where((l) => l.contains('thrower')).length;
  }
}
int callUnchanged(int x) => unchanged(x);
"""

PROBES = {
    'Twice': 'callTwice(5)',
    'GetterLoop': 'chipLoop(Chip(), 1000)',
    'Setter': 'setBox(Box(), 10)',
    'Operator': 'addVec(Vec(1), Vec(2))',
    'Constructor': 'makePt(3, 4)',
    'TransitiveLeaf': 'callTop(5)',
    'TransitiveMiddle': 'callTop2(5)',
    'NamedDefault': 'callScale(3)',
    'PositionalDefault': 'callPos(3)',
    'Polymorphic': 'sumSides(<Shape>[Tri(), Sq(), Tri()])',
    'Method': 'callDot(Point(2, 3))',
    'StackTrace': 'callThrower(9)',
    'Unchanged': 'callUnchanged(1)',
}

DRIVER = '\n'.join(
    ["import 'body.dart';"] +
    [f"@pragma('vm:entry-point') int probe{name}() => {expr};"
     for name, expr in PROBES.items()] + ['void main() {}'])

RUNNER = '\n'.join(
    ["import 'body.dart';", 'void main() {'] +
    [f"  print('probe{name} ${{{expr}}}');" for name, expr in PROBES.items()] +
    ['}'])

# Callees (as printed by --trace_shorebird_patchable_inlining) that must be
# inlined somewhere in the base snapshot.
EXPECTED_INLINED = [
    'twice', 'Chip.weight', 'Box.value=', 'Vec.+', 'new Pt', 'leafC', 'midB',
    'topA', 'leafC2', 'midB2', 'scale', 'pos', 'Tri.sides', 'Sq.sides',
    'Point.dot', 'thrower', 'unchanged',
]
# (a, b): some compiled function inlined both `a` and `b` (for the chains:
# `b` inlined into the inlined `a`; for Tri/Sq: both polymorphic targets).
EXPECTED_TRANSITIVE = [('midB', 'leafC'), ('midB2', 'leafC2'),
                       ('Tri.sides', 'Sq.sides')]

HARNESS = r"""
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "include/dart_api.h"
#include "include/dart_engine.h"

void Check(Dart_Handle value) {
  if (Dart_IsError(value)) {
    std::cerr << Dart_GetError(value) << std::endl;
    std::exit(1);
  }
}

int64_t Probe(const char* name) {
  auto value = Dart_Invoke(Dart_RootLibrary(), Dart_NewStringFromCString(name),
                           0, nullptr);
  Check(value);
  int64_t result = 0;
  Check(Dart_IntegerToInt64(value, &result));
  return result;
}

bool InitializeChild(void** child_data, char** error) {
  *child_data = nullptr;
  return true;
}

int main(int argc, char** argv) {
  // harness <snapshot> <patch-or-"none"> <probes.txt>: lines "name expected".
  if (argc != 4 || !Dart_IsPrecompiledRuntime()) return 2;
  char* error = nullptr;
  if (!DartEngine_Init(&error)) {
    std::cerr << (error == nullptr ? "Embedder initialization failed" : error)
              << std::endl;
    return 1;
  }
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  Dart_InitializeParams params;
  std::memset(&params, 0, sizeof(params));
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  params.initialize_isolate = InitializeChild;
  error = Dart_Initialize(&params);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, "inlining",
      snapshot.snapshot_data, snapshot.snapshot_text, &flags, nullptr, nullptr,
      &error);
  if (error != nullptr || isolate == nullptr) {
    std::cerr << (error == nullptr ? "No root isolate" : error) << std::endl;
    return 1;
  }
  Dart_EnterScope();
  if (std::strcmp(argv[2], "none") != 0) {
    std::ifstream input(argv[2], std::ios::binary);
    std::vector<uint8_t> patch((std::istreambuf_iterator<char>(input)), {});
    Check(Dart_ReloadBytecodePatch(patch.data(), patch.size()));
  }
  Dart_ExitScope();
  Dart_ExitIsolate();
  error = Dart_IsolateMakeRunnable(isolate);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  Dart_EnterIsolate(isolate);
  Dart_EnterScope();
  std::ifstream probes(argv[3]);
  std::string name;
  long long expected;
  int failures = 0;
  while (probes >> name >> expected) {
    const auto actual = Probe(name.c_str());
    std::cout << name << ": " << actual
              << (actual == expected ? " == " : " != ") << expected << std::endl;
    if (actual != expected) ++failures;
  }
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  DartEngine_Shutdown();
  Dart_Cleanup();
  return failures == 0 ? 0 : 3;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--product', action='store_true',
                        help='Use matching PRODUCT compiler, platform, and shared runtime')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if sys.platform not in ('darwin', 'linux'):
        raise SystemExit('This native acceptance harness supports macOS/Linux hosts.')

    def run(*command, check=True, capture=False, env=None):
        return subprocess.run([str(value) for value in command], cwd=root,
                              check=check, timeout=600, env=env,
                              capture_output=capture, text=True)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else
                 tempfile.TemporaryDirectory(prefix='dart-aot-inlining-'))
    with workspace as tmp:
        work = Path(tmp)
        dart = build / 'dart'
        platform = build / ('vm_platform_product.dill' if args.product
                            else 'vm_platform.dill')
        if not dart.is_file():
            dart = build / 'dart-sdk/bin/dart'
            if not args.product:
                platform = build / 'dart-sdk/lib/_internal/vm_platform.dill'
        snapshot_compiler = build / ('gen_snapshot_product' if args.product
                                      else 'gen_snapshot')
        runtime_name = 'dart_engine_aot_shared' + ('_product' if args.product else '')
        runtime_path = build / ('lib' + runtime_name +
                                ('.dylib' if sys.platform == 'darwin' else '.so'))
        for artifact in (dart, platform, snapshot_compiler, runtime_path):
            if not artifact.is_file():
                parser.error(f'Required artifact is missing: {artifact}')
        defines = ['-Ddart.vm.product=true'] if args.product else []
        print(f'Runtime mode: {"PRODUCT" if args.product else "non-PRODUCT"}',
              flush=True)
        bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                    *defines, f'--platform={platform}',
                    '--shorebird-structural-patching']
        packages = f'--packages={root / ".dart_tool/package_config.json"}'

        body = work / 'body.dart'
        (work / 'driver.dart').write_text(DRIVER)
        (work / 'runner.dart').write_text(RUNNER)
        harness = work / 'harness.cc'
        harness.write_text(HARNESS)

        def expected_for(callees, name):
            body.write_text(callees + CALLERS)
            out = run(dart, *defines, work / 'runner.dart', capture=True).stdout
            names = [line.split()[0] for line in out.splitlines()]
            assert names == ['probe' + probe for probe in PROBES], names
            path = work / name
            path.write_text(out)
            return path, out

        base_probes, base_expected = expected_for(CALLEES_BASE,
                                                  'base_probes.txt')
        patch_probes, patch_expected = expected_for(CALLEES_UPDATED,
                                                    'patch_probes.txt')
        print('Base:\n' + base_expected + 'Patched:\n' + patch_expected,
              end='', flush=True)
        changed = {line for line in patch_expected.splitlines()} - {
            line for line in base_expected.splitlines()}
        assert len(changed) == len(PROBES) - 1, (
            'every probe but Unchanged must change', changed)

        # Base snapshot: structural profile, with the inlining trace.
        body.write_text(CALLEES_BASE + CALLERS)
        base_kernel = work / 'base.dill'
        base_snapshot = work / 'base.snapshot'
        run(dart, packages, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
            '--no-tfa', *defines, '--shorebird-structural-patching',
            f'--platform={platform}', f'--output={base_kernel}',
            work / 'driver.dart')
        if sys.platform == 'darwin':
            output = ['--snapshot-kind=app-aot-macho-dylib',
                      f'--macho={base_snapshot}']
        else:
            output = ['--snapshot-kind=app-aot-elf', f'--elf={base_snapshot}']
        compiled = run(snapshot_compiler, '--shorebird-structural-patching',
                       '--trace-shorebird-patchable-inlining', *output,
                       base_kernel, capture=True)
        trace = compiled.stdout + compiled.stderr
        (work / 'inlining_trace.txt').write_text(trace)
        if sys.platform == 'darwin':
            run('codesign', '--force', '--sign', '-', base_snapshot)

        # Which callees were inlined into which compiled functions.
        inlined_into = {}
        for line in trace.splitlines():
            match = re.match(r'Shorebird guarded inlining: (.+) into (.+) '
                             r'\(size \d+\)$', line)
            if match:
                inlined_into.setdefault(match.group(1), set()).add(
                    match.group(2))
        missing = [name for name in EXPECTED_INLINED if name not in inlined_into]
        if missing:
            raise AssertionError(
                f'Callees not inlined in the base snapshot: {missing}; '
                f'inlined: {sorted(inlined_into)}')
        for outer, inner in EXPECTED_TRANSITIVE:
            if not inlined_into[outer] & inlined_into[inner]:
                raise AssertionError(
                    f'No compiled function inlined both {outer} and {inner}: '
                    f'{inlined_into[outer]} / {inlined_into[inner]}')
        print('Guarded inlinings: ' + ', '.join(
            f'{name} -> {sorted(inlined_into[name])}'
            for name in EXPECTED_INLINED), flush=True)

        baseline = work / 'baseline.bytecode'
        run(*bytecode, '-o', baseline, work / 'driver.dart')
        body.write_text(CALLEES_UPDATED + CALLERS)
        patch = work / 'inlining.bytecode'
        run(*bytecode, f'--patch-base={baseline}', '-o', patch,
            work / 'driver.dart')

        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            f'-l{runtime_name}', f'-Wl,-rpath,{build}', '-o', executable)
        # 1. The unpatched snapshot computes the base results.
        result = run(executable, base_snapshot, 'none', base_probes,
                     capture=True, check=False)
        print(result.stdout, end='', flush=True)
        if result.returncode != 0:
            raise AssertionError('Unpatched base results differ from the JIT: '
                                 f'{result.returncode} {result.stderr}')
        # 2./3. Patched callees are observed at every former inline site.
        result = run(executable, base_snapshot, patch, patch_probes,
                     capture=True, check=False)
        print(result.stdout, end='', flush=True)
        if result.returncode != 0:
            raise AssertionError('Patched results differ from the JIT: '
                                 f'{result.returncode} {result.stderr}')
        print('Guarded inlining structural probes passed.', flush=True)


if __name__ == '__main__':
    main()
