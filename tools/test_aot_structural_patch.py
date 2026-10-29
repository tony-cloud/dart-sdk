#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Host acceptance for opted-in, boot-only structural interpreter updates.

Requires a matching SDK built with dart_enable_shorebird_interpreter=true and
dart_dynamic_modules=false, including dart, gen_snapshot and
dart_engine_aot_shared. Tests native AOT callers of added declarations and fields,
GC, message serialization, explicit baseline/boot guards, and failed-group
teardown followed by a clean base in the same VM process. No JIT code is used.
"""

import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import tempfile


HOST = r'''
abstract class HostValue { int read(); }
int dispatchHost(HostValue value) => value.read();
bool checkHostType(Object value) => value is HostValue;
HostValue castHostType(Object value) => value as HostValue;
int dispatchHostList(List<HostValue> values) => values.single.read();
'''

BASE = r'''
import 'dart:isolate';
import 'host.dart';

@pragma('vm:entry-point')
class Existing {
  @pragma('vm:entry-point')
  int old = 10;
  Existing();
}

Object? held;
int messageResult = -1;
int anchor() => 7;
int entry() => 1;
int existingValue(Existing value) => value.old;
int newInstanceFields() => 1;
int genericValue() => 1;
int dispatchValue() => 1;
int tearOffValue() => 1;
int cacheValue() => 1;
int readCachedValue() => 1;
void startMessage() {}
int readMessage() => messageResult;
void clearHeld() { held = null; }
'''

UPDATED = r'''
import 'dart:isolate';
import 'host.dart';
import 'helper.dart';

@pragma('vm:entry-point')
class Existing {
  @pragma('vm:entry-point')
  int old = 10;
  int added = 42;
  final int fixed = 44;
  late int lazy = 43;
  Object? nullable;
  static int total = 45;
  Existing();
}

class Added<T> implements HostValue {
  final T value;
  int count;
  Added(this.value, this.count);
  int read() => count;
}

Object? held;
int messageResult = -1;
int anchor() => 7;
int entry() => even(4) ? importedHelper(anchor()) : 0;
int existingValue(Existing value) {
  if (value.added != 42 || value.fixed != 44 || value.lazy != 43 ||
      value.nullable != null || Existing.total != 45) return -1;
  value.added = 50;
  value.lazy = 51;
  value.nullable = 'present';
  return value.old + value.added + value.lazy + value.fixed;
}
int newInstanceFields() {
  final value = Existing();
  return existingValue(value);
}
int genericValue() {
  final value = Added<String>('added', 23);
  final list = <Added<String>>[value];
  return value is Added<String> && value is! Added<int> &&
      list.single.value == 'added' ? list.single.count : -1;
}
int dispatchValue() {
  final value = Added<int>(3, 29);
  return checkHostType(value) && castHostType(value).read() == 29
      ? dispatchHost(value) + dispatchHostList(<HostValue>[value]) : -1;
}
int tearOffValue() {
  final value = Added<int>(3, 31);
  final method = value.read;
  final helper = importedHelper;
  return method() + helper(0);
}
int cacheValue() {
  held = <Object>[Added<int>(5, 37), Existing()];
  existingValue((held as List<Object>)[1] as Existing);
  return 1;
}
int readCachedValue() {
  final values = held as List<Object>;
  final added = values[0] as Added<int>;
  final existing = values[1] as Existing;
  return added.value + added.read() + existing.old + existing.added +
      existing.lazy + existing.fixed;
}
void startMessage() {
  final added = Added<String>('message', 41);
  final existing = Existing();
  existingValue(existing);
  final port = RawReceivePort();
  port.handler = (Object? message) {
    final values = message as List<Object>;
    final copiedAdded = values[0] as Added<String>;
    final copiedExisting = values[1] as Existing;
    messageResult = copiedAdded.value == 'message' &&
        copiedExisting.nullable == 'present' ? copiedAdded.read() +
        copiedExisting.old + copiedExisting.added + copiedExisting.lazy +
        copiedExisting.fixed : -1;
    port.close();
  };
  port.sendPort.send(<Object>[added, existing]);
}
int readMessage() => messageResult;
void clearHeld() { held = null; }
bool even(int value) => value == 0 || odd(value - 1);
bool odd(int value) => value != 0 && even(value - 1);
'''

DRIVER = r'''
import 'body.dart';
@pragma('vm:entry-point') int probe() => entry();
@pragma('vm:entry-point') int probeExisting(Existing value) => existingValue(value);
@pragma('vm:entry-point') int probeDefaults() => newInstanceFields();
@pragma('vm:entry-point') int probeGeneric() => genericValue();
@pragma('vm:entry-point') int probeDispatch() => dispatchValue();
@pragma('vm:entry-point') int probeTearOff() => tearOffValue();
@pragma('vm:entry-point') int probeCache() => cacheValue();
@pragma('vm:entry-point') int probeCached() => readCachedValue();
@pragma('vm:entry-point') void probeMessage() => startMessage();
@pragma('vm:entry-point') int probeMessageResult() => readMessage();
@pragma('vm:entry-point') void releaseHeld() => clearHeld();
void main() {}
'''

HARNESS = r'''
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
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

Dart_Handle Invoke(const char* name, int argc = 0, Dart_Handle* args = nullptr) {
  auto value = Dart_Invoke(Dart_RootLibrary(),
                          Dart_NewStringFromCString(name), argc, args);
  Check(value);
  return value;
}

int64_t Probe(const char* name, int argc = 0, Dart_Handle* args = nullptr) {
  int64_t value = 0;
  Check(Dart_IntegerToInt64(Invoke(name, argc, args), &value));
  return value;
}

void Expect(const char* name, int64_t expected) {
  const auto actual = Probe(name);
  if (actual != expected) {
    std::cerr << name << ": " << actual << " != " << expected << std::endl;
    std::exit(3);
  }
}

void MakeRunnable(Dart_Isolate isolate) {
  Dart_ExitScope();
  DartEngine_ReleaseIsolate();
  char* error = Dart_IsolateMakeRunnable(isolate);
  if (error != nullptr) { std::cerr << error << std::endl; std::exit(1); }
  DartEngine_AcquireIsolate(isolate);
  Dart_EnterScope();
}

void CheckCleanFallback(DartEngine_SnapshotData snapshot) {
  // Tear down the poisoned group first. The engine has no per-isolate removal
  // API, so use the public raw group API for this final fallback group and do
  // not call Engine::Shutdown a second time with its stale isolate inventory.
  const std::string uri(snapshot.script_uri);
  Dart_ExitScope();
  DartEngine_ReleaseIsolate();
  DartEngine_Shutdown();
  char* error = nullptr;
  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  auto fallback = Dart_CreateIsolateGroup(uri.c_str(), "clean-fallback",
      snapshot.snapshot_data, snapshot.snapshot_text, &flags,
      nullptr, nullptr, &error);
  if (error != nullptr || fallback == nullptr) {
    std::cerr << (error == nullptr ? "No fallback isolate" : error) << std::endl;
    std::exit(1);
  }
  Dart_EnterScope();
  Expect("probe", 1);
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  std::cout << "Failed structural group destroyed; fresh base remained clean."
            << std::endl;
}

int main(int argc, char** argv) {
  if (argc != 5 || !Dart_IsPrecompiledRuntime()) return 2;
  const std::string mode(argv[4]);
  char* error = nullptr;
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  auto isolate = DartEngine_CreateIsolate(snapshot, &error);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  DartEngine_AcquireIsolate(isolate);
  Dart_EnterScope();

  Dart_PersistentHandle existing = nullptr;
  std::atomic<bool> collected(false);
  Dart_WeakPersistentHandle weak = nullptr;
  if (mode == "valid") {
    // Allocate a base-sized object without running its constructor/main.
    auto library = Dart_LookupLibrary(Dart_NewStringFromCString(argv[3]));
    Check(library);
    auto type = Dart_GetNonNullableType(
        library, Dart_NewStringFromCString("Existing"), 0, nullptr);
    Check(type);
    auto instance = Dart_Allocate(type);
    Check(instance);
    Check(Dart_SetField(instance, Dart_NewStringFromCString("old"),
                        Dart_NewInteger(10)));
    existing = Dart_NewPersistentHandle(instance);
    weak = Dart_NewWeakPersistentHandle(instance, &collected, 0,
        [](void*, void* peer) {
          static_cast<std::atomic<bool>*>(peer)->store(true);
        });
    if (weak == nullptr) return 4;
  }
  if (mode == "late") MakeRunnable(isolate);
  std::ifstream input(argv[2], std::ios::binary);
  std::vector<uint8_t> patch((std::istreambuf_iterator<char>(input)), {});
  auto result = Dart_ReloadBytecodePatch(patch.data(), patch.size());
  if (mode != "valid") {
    if (!Dart_IsError(result)) {
      std::cerr << "Unsupported structural install accepted: " << mode << std::endl;
      return 5;
    }
    const std::string message(Dart_GetError(result));
    if ((mode == "reject-profile" &&
         message.find("structural AOT baseline") == std::string::npos) ||
        (mode == "late" &&
         message.find("before the isolate is runnable") == std::string::npos)) {
      std::cerr << "Wrong rejection for " << mode << ": " << message << std::endl;
      return 6;
    }
    std::cout << "Expected " << mode << " rejection: " << message << std::endl;
    CheckCleanFallback(snapshot);
    return 0;
  }
  Check(result);
  auto repeated = Dart_ReloadBytecodePatch(patch.data(), patch.size());
  if (!Dart_IsError(repeated)) {
    std::cerr << "Repeated structural installation was accepted" << std::endl;
    return 7;
  }
  MakeRunnable(isolate);
  Expect("probe", 12);
  auto old_instance = Dart_HandleFromPersistent(existing);
  if (Probe("probeExisting", 1, &old_instance) != 155) return 8;
  Expect("probeDefaults", 155);
  Expect("probeGeneric", 23);
  Expect("probeDispatch", 58);
  Expect("probeTearOff", 36);
  Expect("probeCache", 1);
  Dart_NotifyLowMemory();
  Expect("probeCached", 197);
  Invoke("probeMessage");
  Check(Dart_RunLoop());
  Expect("probeMessageResult", 196);
  Invoke("releaseHeld");
  Dart_DeletePersistentHandle(existing);
  // Release local handles too: weak-key extension storage must not retain its
  // receiver after the application no longer references it.
  Dart_ExitScope();
  Dart_EnterScope();
  for (int i = 0; i < 10 && !collected.load(); ++i) Dart_NotifyLowMemory();
  if (!collected.load()) {
    std::cerr << "Added-field storage retained an unreachable receiver" << std::endl;
    return 9;
  }
  Dart_DeleteWeakPersistentHandle(weak);
  Dart_ExitScope();
  DartEngine_ReleaseIsolate();
  DartEngine_Shutdown();
  std::cout << "Structural AOT additions, preexisting fields, GC and messages passed."
            << std::endl;
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--prepare-only', action='store_true',
                        help='Write fixtures/harness only; do not build or run')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if sys.platform not in ('darwin', 'linux'):
        raise SystemExit('This native acceptance harness supports macOS/Linux hosts.')

    def run(*command):
        subprocess.run([str(value) for value in command], cwd=root, check=True)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else
                 tempfile.TemporaryDirectory(prefix='dart-aot-structural-patch-'))
    with workspace as tmp:
        work = Path(tmp)
        source = work / 'body.dart'
        source.write_text(BASE)
        (work / 'host.dart').write_text(HOST)
        driver = work / 'driver.dart'
        driver.write_text(DRIVER)
        (work / 'helper.dart').write_text('int importedHelper(int value) => value + 5;\n')
        harness = work / 'harness.cc'
        harness.write_text(HARNESS)
        if args.prepare_only:
            print(f'Prepared structural fixtures and harness in {work}; nothing executed.')
            return

        dart = build / 'dart'
        platform = build / 'vm_platform.dill'
        if not dart.is_file():
            dart = build / 'dart-sdk/bin/dart'
            platform = build / 'dart-sdk/lib/_internal/vm_platform.dill'
        bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                    f'--platform={platform}', '--shorebird-structural-patching']

        def snapshot(path, output, *, structural):
            kernel = output.with_suffix('.dill')
            profile = ['--shorebird-structural-patching'] if structural else []
            run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot', '--no-tfa',
                *profile, f'--platform={platform}', f'--output={kernel}', path)
            if sys.platform == 'darwin':
                run(build / 'gen_snapshot', '--snapshot-kind=app-aot-macho-dylib',
                    *profile, f'--macho={output}', kernel)
                run('codesign', '--force', '--sign', '-', output)
            else:
                run(build / 'gen_snapshot', '--snapshot-kind=app-aot-elf',
                    *profile, f'--elf={output}', kernel)

        base_snapshot = work / 'base.snapshot'
        plain_snapshot = work / 'body-only.snapshot'
        snapshot(driver, base_snapshot, structural=True)
        snapshot(driver, plain_snapshot, structural=False)
        baseline = work / 'baseline.bytecode'
        run(*bytecode, '-o', baseline, driver)
        source.write_text(UPDATED)
        patch = work / 'structural.bytecode'
        run(*bytecode, f'--patch-base={baseline}', '-o', patch, driver)
        # The new declarations can be registered before a body tries to resolve
        # this missing target. Exercise the poisoned-group teardown path.
        malformed = work / 'late-reference-failure.bytecode'
        content = patch.read_bytes()
        if b'anchor' not in content:
            raise AssertionError('Missing baseline anchor reference in fixture')
        malformed.write_bytes(content.replace(b'anchor', b'absent'))

        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            '-ldart_engine_aot_shared', f'-Wl,-rpath,{build}', '-o', executable)
        run(executable, plain_snapshot, patch, source.as_uri(), 'reject-profile')
        run(executable, base_snapshot, patch, source.as_uri(), 'late')
        run(executable, base_snapshot, malformed, source.as_uri(), 'reject-malformed')
        run(executable, base_snapshot, patch, source.as_uri(), 'valid')

        const_source = work / 'const.dart'
        const_source.write_text('''
@pragma('vm:entry-point')
class Existing { final int old; const Existing(this.old); }
@pragma('vm:entry-point') int probe() => 1;
void main() {}
''')
        const_snapshot = work / 'const.snapshot'
        snapshot(const_source, const_snapshot, structural=True)
        const_baseline = work / 'const-baseline.bytecode'
        run(*bytecode, '-o', const_baseline, const_source)
        const_source.write_text('''
@pragma('vm:entry-point')
class Existing {
  final int old;
  final int added = 42;
  const Existing(this.old);
}
@pragma('vm:entry-point') int probe() => 1;
void main() {}
''')
        const_patch = work / 'const.bytecode'
        run(*bytecode, f'--patch-base={const_baseline}', '-o', const_patch, const_source)
        run(executable, const_snapshot, const_patch, const_source.as_uri(), 'reject-const')


if __name__ == '__main__':
    main()
