#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Host regression: AOT calls to patchable functions bypass PatchStaticCall.

Build gen_snapshot, dart_engine_aot_shared, and dart with
dart_enable_shorebird_interpreter=true and dart_dynamic_modules=false first.

In a no-TFA (patchable) AOT build, calls to patchable functions load the
target Function from the object pool and call through Function::entry_point,
so a bytecode patch (which swaps the entry point) is observed without the
CallStaticFunction stub or the PatchStaticCall runtime entry. This checks that

  * the optimized code of AOT callers (static calls, constructors, factories,
    constructor and function and method tear-offs, super method calls, super
    constructor calls) never references the CallStaticFunction stub, and does
    reference the target Function objects, and
  * the same callers run the AOT bodies before a patch and the interpreted
    replacement bodies after it.
"""

import argparse
import contextlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile


HARNESS = r'''
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
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
  auto value = Dart_Invoke(Dart_RootLibrary(),
                           Dart_NewStringFromCString(name), 0, nullptr);
  Check(value);
  int64_t result = 0;
  Check(Dart_IntegerToInt64(value, &result));
  return result;
}

// usage: harness <snapshot> <patch> <probe>...
// Prints "<phase> <probe> <value>" before and after loading the patch.
int main(int argc, char** argv) {
  if (argc < 4 || !Dart_IsPrecompiledRuntime()) return 2;
  char* error = nullptr;
  auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error) { std::cerr << error << std::endl; return 1; }
  auto isolate = DartEngine_CreateIsolate(snapshot, &error);
  if (error) { std::cerr << error << std::endl; return 1; }
  DartEngine_AcquireIsolate(isolate);
  Dart_EnterScope();
  for (int i = 3; i < argc; ++i) {
    std::cout << "before " << argv[i] << " " << Probe(argv[i]) << std::endl;
  }
  std::ifstream input(argv[2], std::ios::binary);
  std::vector<uint8_t> patch((std::istreambuf_iterator<char>(input)), {});
  auto result = Dart_ReloadBytecodePatch(patch.data(), patch.size());
  if (Dart_IsError(result)) {
    std::cerr << Dart_GetError(result) << std::endl;
    return 4;
  }
  for (int i = 3; i < argc; ++i) {
    std::cout << "after " << argv[i] << " " << Probe(argv[i]) << std::endl;
  }
  Dart_ExitScope();
  DartEngine_ReleaseIsolate();
  DartEngine_Shutdown();
  return 0;
}
'''

# Patchable declarations; only the numbers differ between base and patch.
def body(value):
    return f'''
int target(int x) => x + {value};

class Box {{
  final int v;
  // Constructor bodies are not patchable (they call the dropped Object
  // constructor), so their values stay fixed: they are called from AOT code
  // through the Function entry point both before and after the patch.
  Box(int x) : v = x + 1;
  Box.named(int x) : v = x * 2 + 1;
  factory Box.make(int x) => Box(x + {value});
}}

class Base {{
  int method(int x) => x + {value};
}}

// These do not mention the value: they stay AOT code and call the patched
// Base.method through a super call, and Box.new through a super constructor
// call.
class Derived extends Base {{
  @override
  int method(int x) => super.method(x) * 2;
}}

class SuperCtor extends Box {{
  SuperCtor(int x) : super(x + 1);
}}
'''


DRIVER = r'''
import 'body.dart';

@pragma('vm:never-inline')
int Function(int) functionTearOff() => target;
@pragma('vm:never-inline')
int Function(int) methodTearOff(Base b) => b.method;
@pragma('vm:never-inline')
Box Function(int) constructorTearOff() => Box.new;

@pragma('vm:entry-point')
int probeStatic() => target(10);
@pragma('vm:entry-point')
int probeCtor() => Box(10).v;
@pragma('vm:entry-point')
int probeNamedCtor() => Box.named(10).v;
@pragma('vm:entry-point')
int probeFactory() => Box.make(10).v;
@pragma('vm:entry-point')
int probeFunctionTearOff() => functionTearOff()(10);
@pragma('vm:entry-point')
int probeMethodTearOff() => methodTearOff(Base())(10);
@pragma('vm:entry-point')
int probeConstructorTearOff() => constructorTearOff()(10).v;
@pragma('vm:entry-point')
int probeSuperMethod() => Derived().method(10);
@pragma('vm:entry-point')
int probeSuperCtor() => SuperCtor(10).v;
void main() { print(probeStatic()); }
'''

# probe -> value as a function of the number baked into body().
EXPECTED = {
    'probeStatic': lambda v: 10 + v,
    'probeCtor': lambda v: 11,
    'probeNamedCtor': lambda v: 21,
    'probeFactory': lambda v: 11 + v,
    'probeFunctionTearOff': lambda v: 10 + v,
    'probeMethodTearOff': lambda v: 10 + v,
    'probeConstructorTearOff': lambda v: 11,
    'probeSuperMethod': lambda v: (10 + v) * 2,
    'probeSuperCtor': lambda v: 12,
}

# Functions that the optimized code of the (unpatched) probes must reference
# directly as Function objects in the object pool.
EXPECTED_POOL_FUNCTIONS = ["'target'", "'Box.'", "'Box.named'", "'Box.make'",
                           "'method'"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()

    def run(*command, **kwargs):
        return subprocess.run([str(x) for x in command], cwd=root, check=True,
                              **kwargs)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else
                 tempfile.TemporaryDirectory(prefix='dart-aot-direct-calls-'))
    with workspace as tmp:
        work = Path(tmp)
        source = work / 'body.dart'
        source.write_text(body(1))
        driver = work / 'driver.dart'
        driver.write_text(DRIVER)
        kernel = work / 'base.dill'
        platform = build / 'vm_platform.dill'
        run(build / 'dart', root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
            '--no-tfa', f'--platform={platform}', f'--output={kernel}', driver)

        snapshot = work / 'base.snapshot'
        disassembly = work / 'base.disasm.txt'
        if sys.platform == 'darwin':
            snapshot_flags = ['--snapshot-kind=app-aot-macho-dylib',
                              f'--macho={snapshot}']
        else:
            snapshot_flags = ['--snapshot-kind=app-aot-elf', f'--elf={snapshot}']
        with open(disassembly, 'w') as out:
            run(build / 'gen_snapshot', '--disassemble',
                '--print-flow-graph-filter=probe', *snapshot_flags, kernel,
                stdout=out, stderr=subprocess.STDOUT)
        if sys.platform == 'darwin':
            run('codesign', '--force', '--sign', '-', snapshot)

        text = disassembly.read_text()
        if 'probeSuperCtor' not in text:
            raise AssertionError('Disassembly of the probes was not produced')
        if 'CallStaticFunction' in text:
            raise AssertionError(
                'Patchable calls still go through the CallStaticFunction stub '
                '(PatchStaticCall runtime entry)')
        for name in EXPECTED_POOL_FUNCTIONS:
            if not re.search(r'Function ' + re.escape(name), text):
                raise AssertionError(
                    f'Optimized probes never load patchable target {name} '
                    'from the object pool')

        # Replacement bytecode: same shapes, different constants.
        platform_flag = f'--platform={platform}'
        bytecode_command = [build / 'dart',
                            root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                            platform_flag]
        baseline = work / 'baseline.bytecode'
        run(*bytecode_command, '-o', baseline, driver)
        source.write_text(body(2))
        patch = work / 'patch.bytecode'
        run(*bytecode_command, f'--patch-base={baseline}', '-o', patch, driver)

        harness = work / 'harness.cc'
        harness.write_text(HARNESS)
        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            '-ldart_engine_aot_shared', f'-Wl,-rpath,{build}', '-o', executable)

        probes = list(EXPECTED)
        result = run(executable, snapshot, patch, *probes,
                     stdout=subprocess.PIPE, text=True)
        seen = {}
        for line in result.stdout.splitlines():
            phase, probe, value = line.split()
            seen[(phase, probe)] = int(value)
        for probe, expected in EXPECTED.items():
            for phase, number in (('before', 1), ('after', 2)):
                actual = seen.get((phase, probe))
                if actual != expected(number):
                    raise AssertionError(
                        f'{probe} {phase} patch: got {actual}, expected '
                        f'{expected(number)}')
        print('AOT callers of patchable static, constructor, tear-off and super '
              'targets ran AOT bodies, then the patched replacements, without '
              'the CallStaticFunction stub.')


if __name__ == '__main__':
    main()
