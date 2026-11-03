#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Closed-world devirtualization and type checks in structural AOT snapshots.

The structural profile compiles app classes as open (patches can add
subclasses). Calls and type checks are optimized over the *base* class
hierarchy (class ids below ObjectStore::shorebird_base_num_cids) instead, which
is sound because patches cannot change the supertypes of existing classes and
every class a patch adds gets a larger class id. The one remaining hole, a patch
adding an overriding member to an existing class, is handled by a per-class
override guard (IsolateGroup::shorebird_overridden_cids) tested at every
devirtualized site.

This test compiles a base program whose AOT code contains devirtualized calls
and range based type checks, installs a structural patch and checks that

 1. a patch-added subclass overriding a devirtualized method is dispatched to;
 2. a patch-added class that implements a base class or interface passes AOT
    `is`/`as` checks (also nested in generic type arguments);
 3. a patched body of a devirtualized target runs interpreted;
 4. patch-added overrides (method, getter, operator, field getter, added
    field) on existing classes are accepted and dispatched to at devirtualized
    sites, including inlined field getters;
 5. the same override added to a superclass with existing base subclasses, and
    that only the expected classes lose their devirtualized calls (the runtime
    trace lists the flagged classes: unrelated classes keep the fast path).

Expected values are computed by running the patched source on the host JIT VM.
Requires the same artifacts as test_aot_structural_patch.py. Use --product for
the PRODUCT artifacts.
"""

import argparse
import contextlib
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


# Identical in BASE and UPDATED: the AOT call sites under test.
COMMON = r"""
abstract class Shape {
  final int w;
  Shape(this.w);
  int area();
  int get sides => 3;
  int scale(int k) => w * k;
  int operator +(Shape o) => w + o.w;
  int get tag => w + 1000;
}
class Sq extends Shape {
  Sq(int w) : super(w);
  int area() => w * w;
}
class Special extends Sq {
  Special(int w) : super(w);
}
class Circle extends Shape {
  Circle(int w) : super(w);
  int area() => w * 3;
  int get sides => 0;
}
class Plain extends Shape {
  Plain(int w) : super(w);
  int area() => w;
}
class Box<T> {
  final T value;
  Box(this.value);
}
abstract class Tagged {
  int tagId();
}
abstract class Base0 {
  int f() => 1;
  int get g => 10;
}
class A extends Base0 {}
class B extends A {}
class D extends A {
  int f() => 3;
}
class E extends Base0 {}
class F extends B {}

int callArea(Shape s) => s.area();
int callSides(Shape s) => s.sides;
int callScale(Shape s, int k) => s.scale(k);
int callPlus(Shape a, Shape b) => a + b;
int callW(Shape s) => s.w;
int callTag(Shape s) => s.tag;
int callF(Base0 x) => x.f();
int callG(Base0 x) => x.g;
bool isShape(Object o) => o is Shape;
bool isSq(Object o) => o is Sq;
bool isTagged(Object o) => o is Tagged;
bool isBoxOfShape(Object o) => o is Box<Shape>;
Shape asShape(Object o) => o as Shape;
Tagged asTagged(Object o) => o as Tagged;
int takesShapes(List<Shape> l) => l.length;
int takesBox(Box<Shape> b) => b.value.area();
"""

BASE = COMMON + r"""
int newSubclass() => 1;
int newImplementors() => 1;
int patchedBody() => 1;
int addedOverrides() => 1;
int addedFieldOverrides() => 1;
int superclassOverride() => 1;
int unrelatedClasses() => 1;
"""

UPDATED = r"""
abstract class Shape {
  final int w;
  Shape(this.w);
  int area();
  int get sides => 3;
  int scale(int k) => w * k;
  int operator +(Shape o) => w + o.w;
  int get tag => w + 1000;
}
class Sq extends Shape {
  Sq(int w) : super(w);
  int area() => w * w;
  // Added overrides of inherited members.
  int get sides => 4;
  int scale(int k) => w * k + 1;
  int operator +(Shape o) => 99;
}
class Special extends Sq {
  Special(int w) : super(w);
}
class Circle extends Shape {
  Circle(int w) : super(w);
  int area() => w * 4;  // patched body of an existing devirtualized target
  int get sides => 0;
  // An added field overrides the inherited getter.
  final int tag = 55;
}
class Plain extends Shape {
  Plain(int w) : super(w);
  int area() => w;
  // A getter overrides the inherited field getter (inlined at call sites).
  int get w => 777;
}
class Box<T> {
  final T value;
  Box(this.value);
}
abstract class Tagged {
  int tagId();
}
abstract class Base0 {
  int f() => 1;
  int get g => 10;
}
class A extends Base0 {
  // Added to a superclass with base subclasses B, F (D declares its own).
  int f() => 2;
}
class B extends A {}
class D extends A {
  int f() => 3;
}
class E extends Base0 {}
class F extends B {}

// Added classes (class ids above the base hierarchy).
class Tri extends Shape {
  Tri(int w) : super(w);
  int area() => w * 5;
  int get sides => 3 + 100;
  int scale(int k) => 5000 + k;
}
class Tri2 extends Tri {
  Tri2(int w) : super(w);
  int area() => w * 6;
}
class Imp implements Shape {
  int get w => 7;
  int area() => 70;
  int get sides => 71;
  int scale(int k) => 72 + k;
  int operator +(Shape o) => 73;
  int get tag => 74;
}
class Named implements Tagged {
  int tagId() => 88;
}
class MyBox<T> extends Box<T> {
  MyBox(T v) : super(v);
}
class NewBase0 extends Base0 {
  int f() => 9;
}

int callArea(Shape s) => s.area();
int callSides(Shape s) => s.sides;
int callScale(Shape s, int k) => s.scale(k);
int callPlus(Shape a, Shape b) => a + b;
int callW(Shape s) => s.w;
int callTag(Shape s) => s.tag;
int callF(Base0 x) => x.f();
int callG(Base0 x) => x.g;
bool isShape(Object o) => o is Shape;
bool isSq(Object o) => o is Sq;
bool isTagged(Object o) => o is Tagged;
bool isBoxOfShape(Object o) => o is Box<Shape>;
Shape asShape(Object o) => o as Shape;
Tagged asTagged(Object o) => o as Tagged;
int takesShapes(List<Shape> l) => l.length;
int takesBox(Box<Shape> b) => b.value.area();

int boolBits(List<bool> bits) {
  var r = 0;
  for (final b in bits) {
    r = r * 2 + (b ? 1 : 0);
  }
  return r;
}

// 1. A patch-added subclass overrides methods used at devirtualized sites.
int newSubclass() {
  final t = Tri(2);
  final t2 = Tri2(2);
  return callArea(t) * 1000000 + callArea(t2) * 10000 + callSides(t) * 100 +
      callScale(t, 4) % 97 + callArea(Sq(3)) + callArea(Plain(1)) +
      callTag(t) + callW(t2) + callPlus(t, Sq(1));
}

// 2. Added classes implementing a base class/interface and AOT is/as checks.
int newImplementors() {
  final imp = Imp();
  final bits = <bool>[
    isShape(Tri(1)), isShape(imp), isShape(Sq(1)), isShape(Plain(1)),
    isShape(1), isShape('x'), isShape(Named()), isSq(Special(1)), isSq(Tri(1)),
    isSq(imp), isTagged(Named()), isTagged(imp), isBoxOfShape(Box<Tri>(Tri(1))),
    isBoxOfShape(MyBox<Imp>(imp)), isBoxOfShape(Box<Named>(Named())),
    isBoxOfShape(Box<Sq>(Sq(1))),
  ];
  var casts = 0;  // (A failing cast of a Smi after another failing cast of the
  // same call site crashes the slow type test stub: a pre-existing issue.)
  casts += asShape(Tri(1)).w + asShape(imp).w + asShape(Sq(2)).w;
  casts += asTagged(Named()).tagId();
  try {
    asShape(Named());
    casts += 1000;
  } on TypeError {
    casts += 1;
  }
  casts += takesShapes(<Shape>[Tri(1), imp, Sq(1)]) +
      takesBox(Box<Shape>(Tri(3))) + takesBox(MyBox<Shape>(imp));
  return boolBits(bits) * 10000 + casts;
}

// 3. The patched body of an existing devirtualized target runs interpreted.
int patchedBody() => callArea(Circle(2)) * 100 + callArea(Sq(5)) + callTag(Circle(1));

// 4. Added overrides on existing classes.
int addedOverrides() {
  final sq = Sq(3);
  final special = Special(4);
  final circle = Circle(5);
  return callSides(sq) * 1000000000 + callSides(special) * 100000000 +
      callSides(circle) * 10000000 + callScale(sq, 10) * 100000 +
      callScale(special, 10) * 1000 + callScale(circle, 10) % 997 +
      callPlus(sq, circle) + callPlus(circle, sq) % 31 + callPlus(special, sq) +
      callSides(Tri(1)) + callSides(Plain(1)) * 10;
}

int addedFieldOverrides() {
  final circle = Circle(5);
  final plain = Plain(5);
  final sq = Sq(5);
  return callTag(circle) * 1000000 + callTag(sq) * 10000 + callTag(plain) +
      callW(plain) * 100 + callW(sq) + callW(circle) * 7;
}

// 5. Override added to a superclass that has base subclasses.
int superclassOverride() {
  return callF(A()) * 100000 + callF(B()) * 10000 + callF(F()) * 1000 +
      callF(D()) * 100 + callF(E()) * 10 + callF(NewBase0());
}

int unrelatedClasses() {
  // Classes whose devirtualized calls must be untouched by any of the above.
  return callG(A()) + callG(B()) + callG(D()) + callG(E()) + callG(NewBase0()) +
      callArea(Sq(2)) + callSides(Circle(1)) * 1000;
}
"""

DRIVER = r"""
import 'body.dart';
@pragma('vm:entry-point') int probeNewSubclass() => newSubclass();
@pragma('vm:entry-point') int probeNewImplementors() => newImplementors();
@pragma('vm:entry-point') int probePatchedBody() => patchedBody();
@pragma('vm:entry-point') int probeAddedOverrides() => addedOverrides();
@pragma('vm:entry-point') int probeAddedFieldOverrides() => addedFieldOverrides();
@pragma('vm:entry-point') int probeSuperclassOverride() => superclassOverride();
@pragma('vm:entry-point') int probeUnrelatedClasses() => unrelatedClasses();
void main() {}
"""

PROBES = ('NewSubclass', 'NewImplementors', 'PatchedBody', 'AddedOverrides',
          'AddedFieldOverrides', 'SuperclassOverride', 'UnrelatedClasses')

RUNNER = '\n'.join(
    ["import 'body.dart';", 'void main() {'] +
    [f"  print('probe{name} ${{{name[0].lower() + name[1:]}()}}');"
     for name in PROBES] + ['}'])

# Classes (by user visible name) that must lose their devirtualized calls, per
# added member, and the ones that must keep them.
EXPECTED_FLAGGED = {
    ('Sq', 'get:sides'): ['Sq', 'Special'],
    ('Sq', 'scale'): ['Sq', 'Special'],
    ('Sq', '+'): ['Sq', 'Special'],
    ('Circle', 'get:tag'): ['Circle'],
    ('Plain', 'get:w'): ['Plain'],
    ('A', 'f'): ['A', 'B', 'F'],
}
NEVER_FLAGGED = {'Shape', 'Base0', 'D', 'E', 'Tri', 'Tri2', 'Imp', 'Box',
                 'Tagged', 'NewBase0', 'Named', 'MyBox'}

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
  auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, "closed-world",
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
                 tempfile.TemporaryDirectory(prefix='dart-aot-closed-world-'))
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

        # Expected values: the same patch source on the host JIT VM.
        body.write_text(UPDATED)
        expected = run(dart, *defines, work / 'runner.dart', capture=True).stdout
        probes = work / 'probes.txt'
        probes.write_text(expected)
        names = [line.split()[0] for line in expected.splitlines()]
        assert names == ['probe' + name for name in PROBES], names
        print(expected, end='', flush=True)

        # Base snapshot: structural profile, trivial probe bodies, AOT call sites.
        body.write_text(BASE)
        base_kernel = work / 'base.dill'
        base_snapshot = work / 'base.snapshot'
        run(dart, packages, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
            '--no-tfa', *defines, '--shorebird-structural-patching',
            f'--platform={platform}', f'--output={base_kernel}',
            work / 'driver.dart')
        if sys.platform == 'darwin':
            run(snapshot_compiler, '--snapshot-kind=app-aot-macho-dylib',
                '--shorebird-structural-patching', f'--macho={base_snapshot}',
                base_kernel)
            run('codesign', '--force', '--sign', '-', base_snapshot)
        else:
            run(snapshot_compiler, '--snapshot-kind=app-aot-elf',
                '--shorebird-structural-patching', f'--elf={base_snapshot}',
                base_kernel)
        baseline = work / 'baseline.bytecode'
        run(*bytecode, '-o', baseline, work / 'driver.dart')
        body.write_text(UPDATED)
        patch = work / 'closed-world.bytecode'
        run(*bytecode, f'--patch-base={baseline}', '-o', patch,
            work / 'driver.dart')

        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            f'-l{runtime_name}', f'-Wl,-rpath,{build}', '-o', executable)
        # The unpatched base must not satisfy the probes (it returns constants).
        base_run = run(executable, base_snapshot, 'none', probes, check=False,
                       capture=True)
        if base_run.returncode == 0:
            raise AssertionError('Unpatched base unexpectedly passed the probes')
        # The VM flags are fixed by the embedding API; the environment enables
        # the trace of the classes whose devirtualized calls are disabled.
        trace_env = dict(os.environ,
                         DART_TRACE_SHOREBIRD_STRUCTURAL_PATCHING='1')
        result = run(executable, base_snapshot, patch, probes, capture=True,
                     env=trace_env)
        print(result.stdout, end='', flush=True)

        # Only the classes inheriting an added override lose their fast path.
        flagged = {}
        for line in (result.stdout + result.stderr).splitlines():
            match = re.match(
                r'Shorebird structural patch: (\S+)\.(\S+) overrides an '
                r'inherited member; devirtualized calls disabled for (\d+) '
                r'class\(es\):(.*)$', line)
            if match:
                flagged[(match.group(1), match.group(2))] = match.group(4).split()
        for key, classes in EXPECTED_FLAGGED.items():
            if sorted(flagged.get(key, [])) != sorted(classes):
                raise AssertionError(
                    f'{key}: flagged {flagged.get(key)}, expected {classes}; '
                    f'all: {flagged}')
        unexpected = set(flagged) - set(EXPECTED_FLAGGED)
        if unexpected:
            raise AssertionError(f'Unexpected overrides recorded: {unexpected}')
        every = {name for classes in flagged.values() for name in classes}
        if every & NEVER_FLAGGED:
            raise AssertionError(
                f'Classes that must keep their fast path were flagged: '
                f'{every & NEVER_FLAGGED}')
        print('Override guard flagged exactly: '
              f'{sorted(every)}', flush=True)
        print('Closed-world structural probes passed.', flush=True)


if __name__ == '__main__':
    main()
