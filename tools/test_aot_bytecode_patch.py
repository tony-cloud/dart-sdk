#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Host AOT/interpreter regression: actual dispatch and failed-patch isolation.

Build gen_snapshot, dart_engine_aot_shared, and dart with
dart_enable_shorebird_interpreter=true and dart_dynamic_modules=false first.
This exercises a native AOT caller of an ordinary, unannotated Dart function;
the caller's library is deliberately absent from each bytecode patch.
"""

import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import tempfile


HARNESS = r'''
#include <cstdlib>
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

int64_t Probe(const char* name = "probe") {
  auto value = Dart_Invoke(Dart_RootLibrary(),
                          Dart_NewStringFromCString(name), 0, nullptr);
  Check(value);
  int64_t result = 0;
  Check(Dart_IntegerToInt64(value, &result));
  return result;
}

int main(int argc, char** argv) {
  if (argc != 7 || !Dart_IsPrecompiledRuntime()) return 2;
  char* error = nullptr;
  auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error) { std::cerr << error << std::endl; return 1; }
  auto isolate = DartEngine_CreateIsolate(snapshot, &error);
  if (error) { std::cerr << error << std::endl; return 1; }
  DartEngine_AcquireIsolate(isolate);
  Dart_EnterScope();
  if (Probe() != 1) return 3;
  for (int i = 2; i < argc; ++i) {
    std::cout << "Loading " << argv[i] << std::endl;
    std::ifstream input(argv[i], std::ios::binary);
    std::vector<uint8_t> patch((std::istreambuf_iterator<char>(input)), {});
    auto result = Dart_ReloadBytecodePatch(patch.data(), patch.size());
    std::cout << "Checking reload result" << std::endl;
    const bool expected_success = i == 3;
    if (Dart_IsError(result) == expected_success) {
      std::cerr << "Unexpected reload result: "
                << (Dart_IsError(result) ? Dart_GetError(result) : "success")
                << std::endl;
      return 4;
    }
    if (Dart_IsError(result)) std::cout << Dart_GetError(result) << std::endl;
    const auto actual = Probe();
    std::cout << "AOT probe result: " << actual << std::endl;
    if (actual != (i < 3 ? 1 : 2)) return 5;
    const auto expected = i < 3 ? 1 : 2;
    if (Probe("probeArg") != 10 + expected ||
        Probe("probeClosure") != 20 + expected ||
        Probe("probeMethod") != 30 + expected ||
        Probe("probeNamed") != 13 + expected ||
        Probe("probeDynamic") != 40 + expected ||
        Probe("probeObject") != expected ||
        Probe("probeTearOff") != 50 + expected ||
        Probe("probeMethodTearOff") != 60 + expected ||
        Probe("probeLicense") != expected - 1) return 6;
  }
  Dart_ExitScope();
  DartEngine_ReleaseIsolate();
  DartEngine_Shutdown();
  std::cout << "AOT caller observed ordinary function replacement; rejected "
               "patches preserved prior bodies." << std::endl;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()

    def run(*command):
        subprocess.run([str(x) for x in command], cwd=root, check=True)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else
                 tempfile.TemporaryDirectory(prefix='dart-aot-bytecode-patch-'))
    with workspace as tmp:
        work = Path(tmp)
        source = work / 'body.dart'
        def body(value):
            return (f'int value() => {value};\n'
                    f'int withArg(int x) => x + {value};\n'
                    f'int withClosure(int x) => (() => x + {value})();\n'
                    f'int withNamed({{required int x, int y = 3}}) => x + y + {value};\n'
                    f'class Counter {{ int method(int x) => x + {value}; '
                    f"String toString() => '{value}'; }}\n"
                    f'class OtherCounter extends Counter {{ int method(int x) => x + {value}; }}\n'
                    'enum License { free, pro }\n'
                    "License currentLicenseType() => const bool.fromEnvironment('paid') "
                    '? License.pro : License.free;\n'
                    'int unusedUnchanged() => -42;\n')
        source.write_text(body(1))
        driver = work / 'driver.dart'
        driver.write_text("import 'body.dart';\n"
                          "@pragma('vm:entry-point')\n"
                          "int probe() => value();\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeArg() => withArg(10);\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeClosure() => withClosure(20);\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeMethod() => Counter().method(30);\n"
                          "final Counter counter = DateTime.now().millisecondsSinceEpoch.isEven "
                          "? Counter() : OtherCounter();\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeDynamic() => counter.method(40);\n"
                          "String callObject(Object value) => value.toString();\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeObject() => int.parse(callObject(counter));\n"
                          "final tearOff = withArg;\n"
                          "final methodTearOff = counter.method;\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeTearOff() => tearOff(50);\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeMethodTearOff() => methodTearOff(60);\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeNamed() => withNamed(x: 10);\n"
                          "@pragma('vm:entry-point')\n"
                          "int probeLicense() => currentLicenseType().index;\n"
                          "void main() { print(probe()); }\n")
        kernel = work / 'base.dill'
        platform = build / 'vm_platform.dill'
        run(build / 'dart', root / 'pkg/vm/bin/gen_kernel.dart', '--aot', '--no-tfa',
            f'--platform={platform}', f'--output={kernel}', driver)
        snapshot = work / 'base.snapshot'
        if sys.platform == 'darwin':
            run(build / 'gen_snapshot', '--snapshot-kind=app-aot-macho-dylib',
                f'--macho={snapshot}', kernel)
            run('codesign', '--force', '--sign', '-', snapshot)
        else:
            run(build / 'gen_snapshot', '--snapshot-kind=app-aot-elf',
                f'--elf={snapshot}', kernel)

        baseline = work / 'baseline.bytecode'
        bytecode_command = [
            build / 'dart', root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
            f'--platform={platform}',
        ]
        run(*bytecode_command, '-o', baseline, driver)

        # Source-position-only changes must not become a patch containing every
        # declaration (including functions absent from the native snapshot).
        source.write_text('\n\n' + body(1))
        unchanged = work / 'unchanged.bytecode'
        result = subprocess.run(
            [str(x) for x in [*bytecode_command, f'--patch-base={baseline}',
                             '-o', unchanged, driver]], cwd=root)
        if result.returncode == 0 or unchanged.exists():
            raise AssertionError('Source-position-only delta was not rejected')

        patches = []
        for name, code in [
                ('new-function', body(99) + 'int added() => 7;\n'),
                ('valid', body(2)),
                ('signature', body(99).replace('value()',
                                              'value([int x = 0])'))]:
            source.write_text(code)
            patch = work / f'{name}.bytecode'
            if name == 'valid':
                # A full app compile includes an unchanged unused declaration
                # absent from the AOT snapshot. Delta emission must omit it.
                run(*bytecode_command, '--define=paid=true',
                    f'--patch-base={baseline}', '-o', patch,
                    driver)
            else:
                run(*bytecode_command, '-o', patch, source)
                rejected = work / f'{name}.rejected.bytecode'
                result = subprocess.run(
                    [str(x) for x in [*bytecode_command,
                     f'--patch-base={baseline}', '-o', rejected, driver]],
                    cwd=root)
                if result.returncode == 0 or rejected.exists():
                    raise AssertionError('Structural delta was not rejected')
            patches.append(patch)

        valid = patches[1].read_bytes()
        truncated = work / 'truncated.bytecode'
        truncated.write_bytes(valid[:16])
        version = work / 'unsupported-version.bytecode'
        version.write_bytes(valid[:4] + bytes([255] * 4) + valid[8:])
        patches.extend([truncated, version])

        harness = work / 'harness.cc'
        harness.write_text(HARNESS)
        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            '-ldart_engine_aot_shared', f'-Wl,-rpath,{build}', '-o', executable)
        run(executable, snapshot, *patches)


if __name__ == '__main__':
    main()
