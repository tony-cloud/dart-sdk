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
import hashlib
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
  late int? nullableLate = null;
  late int uninitialized;
  late final int finalLate;
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
      value.nullable != null || value.nullableLate != null ||
      Existing.total != 45) return -1;
  var rejected = false;
  try { value.uninitialized; } on Object catch (error) {
    rejected = error.toString().contains('LateInitializationError');
  }
  if (!rejected) return -2;
  value.uninitialized = 46;
  value.finalLate = 47;
  rejected = false;
  try { value.finalLate = 48; } on Object catch (error) {
    rejected = error.toString().contains('LateInitializationError');
  }
  if (!rejected || value.uninitialized != 46 || value.finalLate != 47) return -3;
  value.added = 50;
  value.lazy = 51;
  // Sidecar -> receiver cycle must not keep an otherwise unreachable key alive.
  value.nullable = value;
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
bool graphHasBackReferences(List<Object> graph) {
  if (graph.length != 4 || graph[0] is! Existing ||
      graph[1] is! Added<Object>) return false;
  final existing = graph[0] as Existing;
  final added = graph[1] as Added<Object>;
  return identical(graph[0], graph[3]) && identical(graph, graph[2]) &&
      identical(existing.nullable, graph) && identical(added.value, graph);
}
void messageChild(List<Object> message) {
  final reply = message[0] as SendPort;
  final graph = message[1] as List<Object>;
  if (!graphHasBackReferences(graph)) throw StateError('Child graph lost identity');
  final existing = graph[0] as Existing;
  final added = graph[1] as Added<Object>;
  if (existing.old != 10 || existing.added != 50 || existing.lazy != 51 ||
      existing.fixed != 44 || existing.nullableLate != null ||
      existing.uninitialized != 46 || existing.finalLate != 47 || added.count != 41) {
    throw StateError('Child graph lost structural field values');
  }
  existing.added = 70;
  existing.lazy = 71;
  existing.uninitialized = 72;
  added.count = 61;
  reply.send(<Object>[graph, Isolate.current.debugName ?? '']);
}
void startMessage() {
  final graph = <Object>[];
  final existing = Existing();
  existingValue(existing);
  final added = Added<Object>(graph, 41);
  graph.addAll(<Object>[existing, added, graph, existing]);
  existing.nullable = graph;
  final port = RawReceivePort();
  port.handler = (Object? message) {
    port.close();
    if (message is! List || message.length != 2 ||
        message[0] is! List<Object> || message[1] != 'structural-copy-worker') {
      throw StateError('Child isolate failed: $message');
    }
    final copiedGraph = message[0] as List<Object>;
    if (!graphHasBackReferences(copiedGraph) || identical(graph, copiedGraph) ||
        identical(existing, copiedGraph[0]) || existing.added != 50 ||
        existing.lazy != 51 || added.count != 41) {
      throw StateError('Cross-isolate graph copy changed sender or identity');
    }
    final copiedExisting = copiedGraph[0] as Existing;
    final copiedAdded = copiedGraph[1] as Added<Object>;
    if (copiedExisting.nullableLate != null || copiedExisting.uninitialized != 72 ||
        copiedExisting.finalLate != 47) {
      throw StateError('Cross-isolate late field state changed');
    }
    messageResult = copiedAdded.count + copiedExisting.old + copiedExisting.added +
        copiedExisting.lazy + copiedExisting.fixed;
  };
  Isolate.spawn<List<Object>>(messageChild, <Object>[port.sendPort, graph],
      debugName: 'structural-copy-worker', onExit: port.sendPort,
      onError: port.sendPort).then<void>((_) {}, onError: (Object error) {
        port.close();
        throw error;
      });
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


CONST_MALFORMED_PRODUCER = r'''
import 'dart:io';
import 'dart:typed_data';
import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/declarations.dart';
import 'package:dart2bytecode/patch.dart';

Uint8List clearClassCapabilities(List<int> bytes) {
  final component = Component.read(BufferedReader(LinkReader(), bytes));
  final replacements = {
    for (final cls in component.classes)
      cls: ClassDeclaration(
        cls.name,
        cls.flags & ~(ClassDeclaration.hasConstConstructorFlag |
            ClassDeclaration.isDeeplyImmutableFlag),
        cls.script, cls.position, cls.endPosition, cls.typeParameters,
        cls.numTypeArguments, cls.superType, cls.interfaces, cls.members,
        cls.annotations,
      ),
  };
  for (var i = 0; i < component.classes.length; ++i) {
    component.classes[i] = replacements[component.classes[i]]!;
  }
  for (final library in component.libraries) {
    for (var i = 0; i < library.classes.length; ++i) {
      library.classes[i] = replacements[library.classes[i]]!;
    }
  }
  final writer = BufferedWriter(component.stringTable, component.objectTable,
      LinkWriter());
  component.write(writer);
  return writer.getContents();
}

void main(List<String> args) {
  // Explicit malformed-data fixture only. The production compiler rejects this
  // change; the actual native baseline retains its const class capability.
  final baseline = clearClassCapabilities(File(args[0]).readAsBytesSync());
  final updated = clearClassCapabilities(File(args[1]).readAsBytesSync());
  File(args[2]).writeAsBytesSync(createBytecodePatch(baseline, updated,
      shorebirdStructuralPatching: true));
}
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
  Dart_ExitIsolate();
  char* error = Dart_IsolateMakeRunnable(isolate);
  if (error != nullptr) { std::cerr << error << std::endl; std::exit(1); }
  Dart_EnterIsolate(isolate);
  Dart_EnterScope();
}

const char* configured_script_uri = nullptr;

Dart_Handle SetupCoreHooks() {
  auto isolate = Dart_LookupLibrary(Dart_NewStringFromCString("dart:isolate"));
  if (Dart_IsError(isolate)) return isolate;
  auto async = Dart_LookupLibrary(Dart_NewStringFromCString("dart:async"));
  if (Dart_IsError(async)) return async;
  auto closure = Dart_Invoke(isolate,
      Dart_NewStringFromCString("_getIsolateScheduleImmediateClosure"), 0, nullptr);
  if (Dart_IsError(closure)) return closure;
  auto result = Dart_Invoke(async,
      Dart_NewStringFromCString("_setScheduleImmediateClosure"), 1, &closure);
  if (Dart_IsError(result)) return result;
  result = Dart_Invoke(isolate, Dart_NewStringFromCString("_setupHooks"), 0, nullptr);
  if (Dart_IsError(result)) return result;
  // Isolate.spawn requires a script URI hook. Its same-group spawn path does
  // not read that file; this is the normal retained SDK platform setter.
  auto io = Dart_LookupLibrary(Dart_NewStringFromCString("dart:io"));
  if (Dart_IsError(io)) return io;
  auto platform = Dart_GetNonNullableType(io,
      Dart_NewStringFromCString("_Platform"), 0, nullptr);
  if (Dart_IsError(platform)) return platform;
  return Dart_SetField(platform, Dart_NewStringFromCString("_nativeScript"),
                       Dart_NewStringFromCString(configured_script_uri));
}

bool InitializeChild(void** child_data, char** error) {
  *child_data = nullptr;
  Dart_EnterScope();
  auto result = SetupCoreHooks();
  if (Dart_IsError(result)) {
    *error = ::strdup(Dart_GetError(result));
    Dart_ExitScope();
    return false;
  }
  Dart_ExitScope();
  return true;
}

Dart_Isolate CreateRoot(DartEngine_SnapshotData snapshot, const char* name) {
  char* error = nullptr;
  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, name,
      snapshot.snapshot_data, snapshot.snapshot_text, &flags,
      nullptr, nullptr, &error);
  if (error != nullptr || isolate == nullptr) {
    std::cerr << (error == nullptr ? "No root isolate" : error) << std::endl;
    std::exit(1);
  }
  Dart_EnterScope();
  Check(SetupCoreHooks());
  return isolate;
}

void ShutdownRoot() {
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  // Only the snapshot reader/partial embedder initializer was used from the
  // engine; roots and spawned children use the public embedding API directly.
  DartEngine_Shutdown();
  char* error = Dart_Cleanup();
  if (error != nullptr) { std::cerr << error << std::endl; std::exit(1); }
}

void CheckCleanFallback(DartEngine_SnapshotData snapshot) {
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  CreateRoot(snapshot, "clean-fallback");
  Expect("probe", 1);
  ShutdownRoot();
  std::cout << "Failed structural group destroyed; fresh base remained clean."
            << std::endl;
}


bool wrong_key = false;
int key_requests = 0;

bool ResolveKey(const char* id, uint8_t* buffer, intptr_t capacity,
                intptr_t* length) {
  ++key_requests;
  if (id == nullptr || std::strcmp(id, "encrypted-aot-host-key") != 0 ||
      capacity < 32) return false;
  // Public TEST-ONLY key shared with the real Dart fixture producer.
  std::memset(buffer, wrong_key ? 0x43 : 0x42, 32);
  *length = 32;
  return true;
}

std::vector<uint8_t> Read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { std::cerr << "Cannot read " << path << std::endl; std::exit(1); }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {});
}

struct Payload {
  uint8_t* bytes = nullptr;
  intptr_t length = 0;
  ~Payload() { Dart_FreeAotPatchPayload(bytes); }
};

void RejectEncrypted(const std::string& path,
                     const Dart_AotPatchInstallOptions& options,
                     const char* expected_error) {
  const auto envelope = Read(path);
  Payload payload;
  const int previous = key_requests;
  auto result = Dart_InstallAotPatch(envelope.data(), envelope.size(), &options,
                                   &payload.bytes, &payload.length);
  if (!Dart_IsError(result) || payload.bytes != nullptr || payload.length != 0 ||
      key_requests != previous + 1 ||
      std::strstr(Dart_GetError(result), expected_error) == nullptr) {
    std::cerr << "Unexpected encrypted rejection for " << path << std::endl;
    if (Dart_IsError(result)) std::cerr << Dart_GetError(result) << std::endl;
    std::exit(10);
  }
  std::cout << "Expected encrypted rejection: " << expected_error << std::endl;
}

int main(int argc, char** argv) {
  if ((argc != 5 && argc != 8) || !Dart_IsPrecompiledRuntime()) return 2;
  const std::string mode(argv[4]);
  const bool encrypted = mode == "encrypted";
  const bool valid = mode == "valid" || encrypted;
  if (encrypted && (argc != 8 || !Dart_AotPatchingEnabled())) return 2;
  char* error = nullptr;
  if (!DartEngine_Init(&error)) {
    std::cerr << (error == nullptr ? "Embedder initialization failed" : error)
              << std::endl;
    return 1;
  }
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  configured_script_uri = snapshot.script_uri;
  Dart_InitializeParams params;
  std::memset(&params, 0, sizeof(params));
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  params.initialize_isolate = InitializeChild;
  error = Dart_Initialize(&params);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  auto isolate = CreateRoot(snapshot, "structural-parent");

  Dart_PersistentHandle existing = nullptr;
  std::atomic<bool> collected(false);
  Dart_WeakPersistentHandle weak = nullptr;
  if (valid) {
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
  const auto patch = Read(argv[2]);
  Dart_AotPatchInstallOptions options = {};
  Dart_Handle result = Dart_Null();
  if (encrypted) {
    Dart_SetAotPatchKeyCallback(ResolveKey);
    options.app_id = "encrypted-aot-host-app";
    options.app_build_id = "1.0.0+1";
    options.base_flavor_id = "free";
    options.base_license_type = "free";
    options.flavor_id = "pro";
    options.license_type = "pro";
    options.sdk_hash = "encrypted-aot-host-sdk";
    options.base_snapshot_hash = argv[6];
    options.patch_snapshot_hash = argv[7];
    options.target_os = "ios";
    options.target_arch = "arm64";
    options.runtime_mode = "dart-bytecode-interpreter";
    const std::string envelopes(argv[5]);
    // Authentication failures do not enter the structural loader or consume
    // the single boot installation attempt. Do not run app code before install.
    wrong_key = true;
    RejectEncrypted(envelopes + "/valid.json", options, "AES-GCM decryption failed");
    wrong_key = false;
    RejectEncrypted(envelopes + "/tampered.json", options, "AES-GCM decryption failed");
    RejectEncrypted(envelopes + "/expired.json", options, "offline license has expired");
    const auto envelope = Read(envelopes + "/valid.json");
    {
      Payload payload;
      const int previous = key_requests;
      Check(Dart_InstallAotPatch(envelope.data(), envelope.size(), &options,
                                &payload.bytes, &payload.length));
      if (key_requests != previous + 1 ||
          payload.length != static_cast<intptr_t>(patch.size()) ||
          std::memcmp(patch.data(), payload.bytes, patch.size()) != 0) {
        std::cerr << "Decrypted structural delta differs from compiler output" << std::endl;
        return 11;
      }
      result = Dart_ReloadBytecodePatch(payload.bytes, payload.length);
      // Free decrypted input before executing any new function or class.
    }
  } else {
    result = Dart_ReloadBytecodePatch(patch.data(), patch.size());
  }
  if (!valid) {
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
    if (mode == "reject-malformed") {
      auto denied = Dart_Invoke(Dart_RootLibrary(),
          Dart_NewStringFromCString("probe"), 0, nullptr);
      const char* poison_error = "A failed structural patch requires a fresh isolate group.";
      if (!Dart_IsError(denied) ||
          std::strstr(Dart_GetError(denied), poison_error) == nullptr) {
        std::cerr << "Poisoned group allowed application invocation" << std::endl;
        return 12;
      }
      Dart_ExitScope();
      Dart_ExitIsolate();
      char* runnable_error = Dart_IsolateMakeRunnable(isolate);
      const bool rejected = runnable_error != nullptr &&
          std::strstr(runnable_error, poison_error) != nullptr;
      std::free(runnable_error);
      Dart_EnterIsolate(isolate);
      Dart_EnterScope();
      if (!rejected) {
        std::cerr << "Poisoned group became runnable" << std::endl;
        return 13;
      }
      std::cout << "Poisoned structural group blocked invocation and runnable transition."
                << std::endl;
    }
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
  Dart_NotifyDestroyed();
  Expect("probeCached", 197);
  Invoke("probeMessage");
  Check(Dart_RunLoop());
  Expect("probeMessageResult", 256);
  if (encrypted) {
    RejectEncrypted(std::string(argv[5]) + "/tampered.json", options,
                    "AES-GCM decryption failed");
    Expect("probe", 12);
    Expect("probeDispatch", 58);
    Dart_SetAotPatchKeyCallback(nullptr);
    std::cout << "Real Dart AES producer -> install -> structural reload -> free passed."
              << std::endl;
  }
  Invoke("releaseHeld");
  Dart_DeletePersistentHandle(existing);
  // Release local handles too: weak-key extension storage must not retain its
  // receiver after the application no longer references it.
  Dart_ExitScope();
  Dart_EnterScope();
  for (int i = 0; i < 10 && !collected.load(); ++i) Dart_NotifyDestroyed();
  if (!collected.load()) {
    std::cerr << "Added-field storage retained an unreachable receiver" << std::endl;
    return 9;
  }
  Dart_DeleteWeakPersistentHandle(weak);
  ShutdownRoot();
  std::cout << "Structural AOT additions, preexisting fields, GC and cross-isolate copies passed."
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
    parser.add_argument('--envelope-producer', type=Path,
                        help='Optional real Dart envelope fixture producer')
    parser.add_argument('--envelope-packages', type=Path,
                        help='Package configuration for the envelope producer')
    parser.add_argument('--envelope-dart', type=Path,
                        help='Dart SDK executable for the envelope producer')
    args = parser.parse_args()
    producer_options = (args.envelope_producer, args.envelope_packages,
                        args.envelope_dart)
    if any(producer_options) and not all(producer_options):
        parser.error('Envelope producer, packages, and Dart must be provided together')
    for path in producer_options:
        if path is not None and not path.resolve().is_file():
            parser.error(f'Envelope producer artifact is missing: {path}')
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if sys.platform not in ('darwin', 'linux'):
        raise SystemExit('This native acceptance harness supports macOS/Linux hosts.')

    def run(*command):
        subprocess.run([str(value) for value in command], cwd=root, check=True,
                       timeout=300)

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
        if args.envelope_producer:
            envelopes = work / 'envelopes'
            run(args.envelope_dart.resolve(),
                f'--packages={args.envelope_packages.resolve()}',
                args.envelope_producer.resolve(), baseline, patch, envelopes)
            baseline_hash = hashlib.sha256(baseline.read_bytes()).hexdigest()
            patch_hash = hashlib.sha256(patch.read_bytes()).hexdigest()
            run(executable, base_snapshot, patch, source.as_uri(), 'encrypted',
                envelopes, baseline_hash, patch_hash)

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
        const_updated = work / 'const-updated.bytecode'
        run(*bytecode, '-o', const_updated, const_source)
        malformed_producer = work / 'make_malformed_const.dart'
        malformed_producer.write_text(CONST_MALFORMED_PRODUCER)
        const_patch = work / 'const.bytecode'
        run(dart, f'--packages={root / ".dart_tool/package_config.json"}',
            malformed_producer, const_baseline, const_updated, const_patch)
        run(executable, const_snapshot, const_patch, const_source.as_uri(), 'reject-const')


if __name__ == '__main__':
    main()
