#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Failing `as` casts of a Smi after a cast that a patch-added class passed.

A structural patch can add a class that implements an interface of the base
snapshot. An AOT `x as I` check does not know that class: its class id is not
in the (base world) range check and its type testing stub falls through to the
slow type test, which caches the *positive* answer in the call site's
SubtypeTestCache. A later cast of a Smi to the same type is not screened out
of the slow path (a type that is not a supertype of int cannot be decided for
Smis at the call site when the type has no optimized stub), so the Smi reaches
the Subtype{1,2}TestCache stubs, which only handle heap objects. This used to
crash with a SIGSEGV loading the class id of the Smi.

The test installs a patch that adds `Imp implements Shape`, casts an Imp to
Shape (creating and populating the cache), then casts Smis, Strings and
non-Shape objects to Shape and checks that each failing cast throws TypeError.
The same crash also reproduces in stock AOT (no Shorebird flags), so the test
also compiles and runs a plain program with and without TFA.
Expected values are computed by running the patched source on the host JIT VM.
Requires the same artifacts as test_aot_structural_patch.py. Use --product for
the PRODUCT artifacts.
"""

import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import tempfile


# Identical in BASE and UPDATED: the AOT call sites under test.
COMMON = r"""
abstract class Shape {
  int area();
}
class Sq implements Shape {
  final int s;
  Sq(this.s);
  int area() => s * s;
}
class Tri implements Shape {
  final int s;
  Tri(this.s);
  int area() => s * s ~/ 2;
}
class Named {
  int id() => 7;
}
Shape asShape(Object o) => o as Shape;
bool isShape(Object o) => o is Shape;
"""

BASE = COMMON + r"""
int castProbe() => 1;
int isProbe() => 1;
"""

UPDATED = COMMON + r"""
class Imp implements Shape {
  int area() => 11;
}

int failing(Object o) {
  try {
    asShape(o);
    return 1000;
  } on TypeError {
    return 1;
  }
}

int castProbe() {
  var r = 0;
  // A class added by the patch passes the cast: the slow type test caches the
  // positive answer at the (AOT) call site.
  r += asShape(Imp()).area();
  r += asShape(Sq(3)).area() + asShape(Tri(4)).area();
  // Then everything that is not a Shape fails, including Smis, which must not
  // reach the class-id based subtype test cache lookup.
  r += failing(Named()) * 100;
  r += failing(3) * 10000;
  r += failing('x') * 1000000;
  r += failing(1.5) * 100000000;
  r += failing(Named()) * 3 + failing(0x7fffffff);
  return r;
}

int isProbe() {
  final bits = <bool>[
    isShape(Imp()), isShape(Sq(1)), isShape(Named()), isShape(3),
    isShape('x'), isShape(Imp()), isShape(4),
  ];
  var r = 0;
  for (final b in bits) {
    r = r * 2 + (b ? 1 : 0);
  }
  return r;
}
"""

DRIVER = r"""
import 'body.dart';
@pragma('vm:entry-point') int probeCast() => castProbe();
@pragma('vm:entry-point') int probeIs() => isProbe();
void main() {}
"""

PROBES = ('Cast', 'Is')

# The same crash without any Shorebird feature (an upstream Dart bug): a
# fully instantiated generic type whose type testing stub cannot use class id
# ranges (function type argument) takes the slow type test for a positive
# answer, which creates the call site's SubtypeTestCache; a later Smi that is
# not a subtype then reaches the cache lookup stub.
PLAIN = r"""
class Box<T> {}
typedef F = int Function(int);
@pragma('vm:never-inline')
Object make() => Box<F>();
@pragma('vm:never-inline')
Box<F> asBox(Object o) => o as Box<F>;
void tryCast(Object o) {
  try {
    asBox(o);
    print('ok');
  } on TypeError {
    print('TypeError');
  }
}
void main() {
  tryCast(make());
  tryCast(Box<int>());
  tryCast(3);
  tryCast('x');
  tryCast(make());
  print('done');
}
"""
PLAIN_EXPECTED = 'ok\nTypeError\nTypeError\nTypeError\nok\ndone\n'

RUNNER = '\n'.join(
    ["import 'body.dart';", 'void main() {'] +
    [f"  print('probe{name} ${{{name.lower()}Probe()}}');"
     for name in PROBES] + ['}'])

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
  auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, "smi-cast",
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
                 tempfile.TemporaryDirectory(prefix='dart-aot-smi-cast-'))
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
        patch = work / 'smi-cast.bytecode'
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
        result = run(executable, base_snapshot, patch, probes, capture=True)
        print(result.stdout, end='', flush=True)

        print('Smi cast after a patch-added subtype passed.', flush=True)

        # The same crash with no Shorebird feature: stock AOT, with and
        # without TFA.
        aotruntime = build / ('dartaotruntime_product' if args.product
                              else 'dartaotruntime')
        if not aotruntime.is_file():
            parser.error(f'Required artifact is missing: {aotruntime}')
        plain = work / 'plain.dart'
        plain.write_text(PLAIN)
        for name, tfa in (('tfa', []), ('no-tfa', ['--no-tfa'])):
            kernel = work / f'plain-{name}.dill'
            run(dart, packages, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                *tfa, *defines, f'--platform={platform}', f'--output={kernel}',
                plain)
            if sys.platform == 'darwin':
                snapshot = work / f'plain-{name}.dylib'
                run(snapshot_compiler, '--snapshot-kind=app-aot-macho-dylib',
                    f'--macho={snapshot}', kernel)
                run('codesign', '--force', '--sign', '-', snapshot)
            else:
                snapshot = work / f'plain-{name}.elf'
                run(snapshot_compiler, '--snapshot-kind=app-aot-elf',
                    f'--elf={snapshot}', kernel)
            plain_run = run(aotruntime, snapshot, check=False, capture=True)
            if plain_run.returncode != 0 or plain_run.stdout != PLAIN_EXPECTED:
                raise AssertionError(
                    f'Plain AOT ({name}) Smi cast: exit {plain_run.returncode}, '
                    f'output {plain_run.stdout!r}\n{plain_run.stderr[:2000]}')
            print(f'Plain AOT ({name}) Smi cast passed.', flush=True)


if __name__ == '__main__':
    main()
