#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Host acceptance for complete-domain, boot-only no-JIT replacement.

Runs a native AOT snapshot as an inactive baseline and installs either its bundled
base or an entire new bytecode component before application execution. Requires
matching dart, gen_snapshot, platform, and dart_engine_aot_shared artifacts built
with the Shorebird interpreter and AOT patching enabled. --product exercises the
shipping PRODUCT runtime. This test does not establish Flutter embedder safety.
"""

import argparse
import contextlib
import hashlib
from pathlib import Path
import platform as host_platform
import struct
import subprocess
import sys
import tempfile

BASE = r'''
import 'dart:isolate';
import 'removed.dart';

@pragma('vm:entry-point')
class Existing<T> {
  final T old;
  const Existing(this.old);
  int read() => (old as int) + removed();
}
@pragma('vm:entry-point')
class RemovedClass {}
@pragma('vm:entry-point')
class Mutable {
  @pragma('vm:entry-point')
  Mutable();
  int old = 2;
  int obsolete = 3;
  int removedMethod() => obsolete;
}
class Parent { int read() => 1; }
class Derived extends Parent {}
const staticValue = 3;
int eager = 5;
late int lazy = 7;
int state() => staticValue + eager + lazy;
int constants() => 1;
int layouts() => 1;
int inheritance() => 1;
int callback() => 1;
Object? held;
int cache() { held = Existing<int>(11); return 1; }
int readCache() => (held as Existing<int>).old;
void clearCache() { held = null; }
int messageResult = 0;
void startMessage() {}
int readMessage() => messageResult;
'''

UPDATED = r'''
import 'dart:isolate';
import 'added.dart';

@pragma('vm:entry-point')
class Existing<T> {
  final T old;
  final int added;
  const Existing(this.old, {this.added = 41});
  int read() => (old as int) + added;
}
@pragma('vm:entry-point')
class Mutable {
  @pragma('vm:entry-point')
  Mutable();
  int old = 12;
  final int added = 13;
  late int lazy = 14;
}
class Parent { int read() => 20; }
class OtherParent { int read() => 30; }
mixin Extra { int extra() => 4; }
class Derived extends OtherParent with Extra {
  int read() => super.read() + extra();
}
const staticValue = 30;
int eager = 50;
late int lazy = 70;
int state() => staticValue + eager + lazy;
int constants() {
  const a = Existing<int>(7);
  const equal = Existing<int>(7, added: 41);
  const unequal = Existing<int>(7, added: 42);
  const nested = <Existing<int>>[Existing<int>(7)];
  const nestedEqual = <Existing<int>>[Existing<int>(7, added: 41)];
  const mapping = <String, Existing<int>>{'a': Existing<int>(7)};
  const unicode = '完整更新';
  if (!identical(a, equal) || identical(a, unequal) ||
      !identical(nested, nestedEqual) || !identical(mapping['a'], a) ||
      a is! Existing<int> || a is Existing<String> ||
      unicode.codeUnitAt(0) != 0x5b8c) return -1;
  return a.read() + unequal.read();
}
int layouts() {
  final value = Mutable();
  value.old += 1;
  var removedField = false;
  var removedMethod = false;
  try { (value as dynamic).obsolete; }
  on NoSuchMethodError { removedField = true; }
  try { (value as dynamic).removedMethod(); }
  on NoSuchMethodError { removedMethod = true; }
  if (!removedField || !removedMethod) return -1;
  return value.old + value.added + value.lazy;
}
int inheritance() {
  final value = Derived();
  if (value is Parent || value is! OtherParent || value is! Extra) return -1;
  return value.read();
}
int callback() => <int>[3, 1, 2].map((n) => n + addedValue()).reduce((a, b) => a + b);
Object? held;
int cache() { held = <Object>[const Existing<int>(11), Mutable()]; return 1; }
int readCache() {
  final values = held as List<Object>;
  return (values[0] as Existing<int>).read() + (values[1] as Mutable).added;
}
void clearCache() { held = null; }
int messageResult = 0;
void child(List<Object> message) {
  final reply = message[0] as SendPort;
  final value = message[1] as Existing<int>;
  reply.send(value.read() + state());
}
void startMessage() {
  final port = RawReceivePort();
  port.handler = (Object? message) {
    messageResult = message as int;
    port.close();
  };
  Isolate.spawn(child, <Object>[port.sendPort, const Existing<int>(7)]);
}
int readMessage() => messageResult;
'''

# This consumer remains byte-for-byte unchanged across the replacement. In
# particular, its omitted constructor argument must use the new default, and
# its constant references must resolve to new-domain canonical objects.
DRIVER = r'''
import 'export_only.dart';
@pragma('vm:entry-point') int probe() => const Existing<int>(7).read();
@pragma('vm:entry-point') int probeState() => state();
@pragma('vm:entry-point') int probeConstants() => constants();
@pragma('vm:entry-point') int probeLayouts() => layouts();
@pragma('vm:entry-point') int probeInheritance() => inheritance();
@pragma('vm:entry-point') int probeCallback() => callback();
@pragma('vm:entry-point') int probeCache() => cache();
@pragma('vm:entry-point') int probeCached() => readCache();
@pragma('vm:entry-point') void releaseHeld() => clearCache();
@pragma('vm:entry-point') void probeMessage() => startMessage();
@pragma('vm:entry-point') int probeMessageResult() => readMessage();
@pragma('vm:entry-point', 'call') int callOnly() => 99;
int entrypointRuns = 0;
@pragma('vm:entry-point') int probeEntrypointRuns() => entrypointRuns;
void main() { ++entrypointRuns; }
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

void RunEntrypoint() {
  // Flutter resolves main as a closure after installation, rather than calling
  // it directly with Dart_Invoke. No Dart reference pre-creates this tear-off.
  auto entrypoint = Dart_GetField(Dart_RootLibrary(),
      Dart_NewStringFromCString("main"));
  Check(entrypoint);
  if (!Dart_IsClosure(entrypoint)) std::exit(9);
  Check(Dart_InvokeClosure(entrypoint, 0, nullptr));
  Expect("probeEntrypointRuns", 1);
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


std::vector<uint8_t> Read(const char* path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { std::cerr << "Cannot read fixture" << std::endl; std::exit(1); }
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {});
}

void Denied(Dart_Handle result, const char* operation) {
  if (!Dart_IsError(result)) {
    std::cerr << "Unexpected success: " << operation << std::endl;
    std::exit(4);
  }
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

struct Payload {
  uint8_t* bytes = nullptr;
  intptr_t length = 0;
  ~Payload() { Dart_FreeAotPatchPayload(bytes); }
};

void RejectEnvelope(const std::string& path,
                    const Dart_AotPatchInstallOptions& options,
                    const char* expected) {
  const auto envelope = Read(path.c_str());
  Payload payload;
  const int previous = key_requests;
  auto result = Dart_InstallAotPatch(envelope.data(), envelope.size(), &options,
                                   &payload.bytes, &payload.length);
  Denied(result, "invalid encrypted wrapper");
  if (payload.bytes != nullptr || payload.length != 0 ||
      key_requests != previous + 1 ||
      std::strstr(Dart_GetError(result), expected) == nullptr) {
    std::cerr << "Unexpected envelope rejection: " << Dart_GetError(result)
              << std::endl;
    std::exit(6);
  }
}

void CannotRun(Dart_Isolate isolate) {
  Denied(Dart_Invoke(Dart_RootLibrary(), Dart_NewStringFromCString("probe"),
                     0, nullptr), "inactive/failed application invocation");
  Dart_ExitScope();
  Dart_ExitIsolate();
  char* error = Dart_IsolateMakeRunnable(isolate);
  if (error == nullptr) {
    std::cerr << "Inactive/failed group became runnable" << std::endl;
    std::exit(5);
  }
  std::free(error);
  Dart_EnterIsolate(isolate);
  Dart_EnterScope();
}

void RejectRawLibrary(const std::vector<uint8_t>& bytes) {
  auto data = Dart_NewExternalTypedData(Dart_TypedData_kUint8,
      const_cast<uint8_t*>(bytes.data()), bytes.size());
  Check(data);
  Denied(Dart_LoadLibraryFromBytecode(data), "raw library into full-profile group");
}

int main(int argc, char** argv) {
  // snapshot, payload, base payload, body URI, removed URI, mode
  if ((argc != 7 && argc != 10) || !Dart_IsPrecompiledRuntime()) return 2;
  const std::string mode(argv[6]);
  const bool encrypted = mode == "encrypted";
  if (encrypted && (argc != 10 || !Dart_AotPatchingEnabled())) return 2;
  char* error = nullptr;
  if (!DartEngine_Init(&error)) {
    std::cerr << (error == nullptr ? "Embedder init failed" : error) << std::endl;
    return 1;
  }
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  configured_script_uri = snapshot.script_uri;
  Dart_InitializeParams params = {};
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  params.initialize_isolate = InitializeChild;
  error = Dart_Initialize(&params);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  auto isolate = CreateRoot(snapshot, "replacement-parent");
  const bool plain = mode == "reject-profile";
  if (!plain) CannotRun(isolate);
  if (plain) {
    // Native-only functions still require a retained implicit closure; an
    // invocation-only entry-point pragma must not permit tear-off retrieval.
    Expect("callOnly", 99);
    Denied(Dart_GetField(Dart_RootLibrary(),
        Dart_NewStringFromCString("callOnly")), "native call-only tear-off");
  }
  const std::string base_path(argv[3]);
  const auto raw_path = base_path.substr(0, base_path.find_last_of('/')) +
                        "/plain.bytecode";
  const auto raw_library = Read(raw_path.c_str());
  if (!plain) RejectRawLibrary(raw_library);
  auto old_root = Dart_NewPersistentHandle(Dart_RootLibrary());
  auto body = Dart_LookupLibrary(Dart_NewStringFromCString(argv[4]));
  Check(body);
  auto old_type_value = Dart_GetNonNullableType(body,
      Dart_NewStringFromCString("RemovedClass"), 0, nullptr);
  Check(old_type_value);
  auto old_type = Dart_NewPersistentHandle(old_type_value);
  // Some profiles reject extracting mutable tear-offs even before installation.
  // If obtainable, the cached closure must still be unusable after replacement.
  auto closure = Dart_GetField(Dart_RootLibrary(), Dart_NewStringFromCString("probe"));
  Dart_PersistentHandle old_closure = Dart_IsError(closure) ? nullptr :
      Dart_NewPersistentHandle(closure);
  std::cout << (old_closure == nullptr ? "Preinstall app tear-off access rejected" :
                                      "Captured app tear-off for retired-handle check")
            << std::endl;
  const auto payload = Read(argv[2]);
  if (plain) {
    const uint32_t manifest_size = static_cast<uint32_t>(payload.at(12)) |
        (static_cast<uint32_t>(payload.at(13)) << 8) |
        (static_cast<uint32_t>(payload.at(14)) << 16) |
        (static_cast<uint32_t>(payload.at(15)) << 24);
    if (24 + static_cast<uint64_t>(manifest_size) >= payload.size()) return 8;
    const std::vector<uint8_t> unwrapped(payload.begin() + 24 + manifest_size,
                                         payload.end());
    RejectRawLibrary(unwrapped);
  }
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
    options.base_snapshot_hash = argv[8];
    options.patch_snapshot_hash = argv[9];
    options.target_os = "ios";
    options.target_arch = "arm64";
    options.runtime_mode = "dart-bytecode-interpreter";
    const std::string envelopes(argv[7]);
    wrong_key = true;
    RejectEnvelope(envelopes + "/valid.json", options, "AES-GCM decryption failed");
    wrong_key = false;
    RejectEnvelope(envelopes + "/tampered.json", options, "AES-GCM decryption failed");
    RejectEnvelope(envelopes + "/expired.json", options, "offline license has expired");
    const auto envelope = Read((envelopes + "/valid.json").c_str());
    {
      Payload plaintext;
      const int previous = key_requests;
      Check(Dart_InstallAotPatch(envelope.data(), envelope.size(), &options,
                                &plaintext.bytes, &plaintext.length));
      if (key_requests != previous + 1 ||
          plaintext.length != static_cast<intptr_t>(payload.size()) ||
          std::memcmp(payload.data(), plaintext.bytes, payload.size()) != 0) {
        std::cerr << "Decrypted wrapper differs from compiler output" << std::endl;
        return 7;
      }
      result = Dart_ReloadBytecodePatch(plaintext.bytes, plaintext.length);
      // Free the decrypted wrapper before executing any replacement code.
    }
  } else {
    result = Dart_ReloadBytecodePatch(payload.data(), payload.size());
  }
  if (mode.rfind("reject-", 0) == 0) {
    Denied(result, mode.c_str());
    if (!plain) CannotRun(isolate);
    Dart_DeletePersistentHandle(old_root);
    Dart_DeletePersistentHandle(old_type);
    if (old_closure != nullptr) Dart_DeletePersistentHandle(old_closure);
    if (plain) { ShutdownRoot(); return 0; }
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    isolate = CreateRoot(snapshot, "fresh-base-fallback");
    CannotRun(isolate);
    const auto base = Read(argv[3]);
    Check(Dart_ReloadBytecodePatch(base.data(), base.size()));
    MakeRunnable(isolate);
    RunEntrypoint();
    Expect("probe", 7);
    Expect("probeState", 15);
    ShutdownRoot();
    std::cout << mode << ": rejected; fresh group installed bundled base" << std::endl;
    return 0;
  }
  Check(result);
  RejectRawLibrary(raw_library);
  Denied(Dart_ReloadBytecodePatch(payload.data(), payload.size()), "repeat install");
  Denied(Dart_Invoke(Dart_HandleFromPersistent(old_root),
      Dart_NewStringFromCString("probe"), 0, nullptr), "retired root invocation");
  Denied(Dart_New(Dart_HandleFromPersistent(old_type), Dart_Null(), 0, nullptr),
         "retired type construction");
  Denied(Dart_NewListOfType(Dart_HandleFromPersistent(old_type), 0),
         "retired type in SDK list");
  if (old_closure != nullptr) {
    Denied(Dart_InvokeClosure(Dart_HandleFromPersistent(old_closure), 0, nullptr),
           "retired closure invocation");
    Dart_DeletePersistentHandle(old_closure);
  }
  Dart_DeletePersistentHandle(old_root);
  Dart_DeletePersistentHandle(old_type);
  MakeRunnable(isolate);
  RunEntrypoint();
  Denied(Dart_GetField(Dart_RootLibrary(),
      Dart_NewStringFromCString("callOnly")), "interpreted call-only tear-off");
  if (mode == "base") {
    Expect("probe", 7);
    Expect("probeState", 15);
  } else {
    Expect("probe", 48);
    Expect("probeState", 150);
    Expect("probeConstants", 97);
    Expect("probeLayouts", 40);
    Expect("probeInheritance", 34);
    Expect("probeCallback", 21);
    auto updated_body = Dart_LookupLibrary(Dart_NewStringFromCString(argv[4]));
    Check(updated_body);
    Denied(Dart_GetNonNullableType(updated_body,
        Dart_NewStringFromCString("RemovedClass"), 0, nullptr), "removed class");
    Denied(Dart_LookupLibrary(Dart_NewStringFromCString(argv[5])), "removed library");
    auto mutable_type = Dart_GetNonNullableType(updated_body,
        Dart_NewStringFromCString("Mutable"), 0, nullptr);
    Check(mutable_type);
    auto mutable_value = Dart_New(mutable_type, Dart_Null(), 0, nullptr);
    Check(mutable_value);
    Denied(Dart_GetField(mutable_value, Dart_NewStringFromCString("obsolete")),
           "removed field");
    Denied(Dart_Invoke(mutable_value, Dart_NewStringFromCString("removedMethod"),
                       0, nullptr), "removed method");
    Expect("probeCache", 1);
    Dart_NotifyDestroyed();
    Expect("probeCached", 65);
    Invoke("probeMessage");
    Check(Dart_RunLoop());
    Expect("probeMessageResult", 198);
    Invoke("releaseHeld");
    Dart_NotifyDestroyed();
  }
  // A failed repeated request must leave the already committed domain usable.
  if (encrypted) {
    RejectEnvelope(std::string(argv[7]) + "/tampered.json", options,
                   "AES-GCM decryption failed");
    Dart_SetAotPatchKeyCallback(nullptr);
    std::cout << "AES producer -> full replacement -> free passed" << std::endl;
  }
  Expect("probe", mode == "base" ? 7 : 48);
  ShutdownRoot();
  std::cout << "Full-domain " << mode << " acceptance passed" << std::endl;
  return 0;
}
'''

MALFORMED_PRODUCER = r'''
import 'dart:io';
import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/declarations.dart';
import 'package:vm/bytecode/full_replacement.dart';

void main(List<String> args) {
  final bundle = decodeShorebirdReplacement(File(args[0]).readAsBytesSync());
  final component = Component.read(BufferedReader(LinkReader(), bundle.component));
  // Deliberately invalid instruction in a loaded body, after identity staging.
  component.codes.first.bytecodes[0] = 255;
  final writer = BufferedWriter(component.stringTable, component.objectTable,
      LinkWriter());
  component.write(writer);
  final bytes = writer.getContents();
  final previous = bundle.manifest;
  final manifest = ShorebirdReplacementManifest(
      hostAbi: previous.hostAbi, baselineDigest: previous.baselineDigest,
      currentDigest: replacementDigest(bytes), rootUri: previous.rootUri,
      baselineUris: previous.baselineUris, currentUris: previous.currentUris,
      removedUris: previous.removedUris);
  File(args[1]).writeAsBytesSync(encodeShorebirdReplacement(manifest, bytes));
}
'''


def digest_artifact(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def host_abi(runtime, snapshot_compiler, platform, product):
    # A reproducible, length-prefixed descriptor avoids concatenation ambiguity.
    values = ['shorebird-host-abi-v1', sys.platform, host_platform.machine(),
              'PRODUCT' if product else 'non-PRODUCT', 'full-replacement-v1',
              digest_artifact(runtime), digest_artifact(snapshot_compiler),
              digest_artifact(platform)]
    descriptor = b''.join(struct.pack('<I', len(value.encode('utf-8'))) +
                          value.encode('utf-8') for value in values)
    return hashlib.sha256(descriptor).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--product', action='store_true')
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--envelope-producer', type=Path)
    parser.add_argument('--envelope-packages', type=Path)
    parser.add_argument('--envelope-dart', type=Path)
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
        parser.error('This native acceptance harness supports macOS/Linux hosts.')
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else tempfile.TemporaryDirectory(
                     prefix='dart-aot-full-replacement-'))

    def run(*command):
        subprocess.run([str(item) for item in command], cwd=root, check=True,
                       timeout=300)

    with workspace as directory:
        work = Path(directory)
        source = work / 'body.dart'
        source.write_text(BASE)
        driver = work / 'driver.dart'
        driver.write_text(DRIVER)
        # These have no declarations for PRODUCT dictionary pruning to retain.
        # Their library identities must nevertheless survive in the complete
        # native baseline domain and match the bundled base/update manifests.
        (work / 'export_only.dart').write_text(
            "export 'body.dart';\nexport 'empty.dart';\n")
        (work / 'empty.dart').write_text('library empty;\n')
        removed = work / 'removed.dart'
        removed.write_text('int removed() => 0;\n')
        (work / 'added.dart').write_text('int addedValue() => 5;\n')
        harness = work / 'harness.cc'
        harness.write_text(HARNESS)
        if args.prepare_only:
            print(f'Prepared replacement fixtures in {work}; nothing executed.')
            return
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
        runtime = build / ('lib' + runtime_name +
                           ('.dylib' if sys.platform == 'darwin' else '.so'))
        for artifact in (dart, platform, snapshot_compiler, runtime):
            if not artifact.is_file():
                parser.error(f'Required artifact missing: {artifact}')
        abi = host_abi(runtime, snapshot_compiler, platform, args.product)
        defines = ['-Ddart.vm.product=true'] if args.product else []
        profile = ['--shorebird-full-replacement', f'--shorebird-host-abi={abi}']
        bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                    *defines, f'--platform={platform}', *profile]
        baseline = work / 'base.sbr'
        run(*bytecode, '-o', baseline, driver)

        def snapshot(output, full):
            kernel = output.with_suffix('.dill')
            kernel_profile = [*profile, f'--shorebird-replacement-base={baseline}'] if full else []
            native_profile = ['--shorebird-full-replacement'] if full else []
            run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot', '--no-tfa',
                *defines, *kernel_profile, f'--platform={platform}',
                f'--output={kernel}', driver)
            if sys.platform == 'darwin':
                run(snapshot_compiler, '--snapshot-kind=app-aot-macho-dylib',
                    *native_profile, f'--macho={output}', kernel)
                run('codesign', '--force', '--sign', '-', output)
            else:
                run(snapshot_compiler, '--snapshot-kind=app-aot-elf',
                    *native_profile, f'--elf={output}', kernel)

        base_snapshot = work / 'base.snapshot'
        snapshot(base_snapshot, True)
        plain_snapshot = work / 'plain.snapshot'
        snapshot(plain_snapshot, False)
        plain_bytecode = work / 'plain.bytecode'
        additive_bytecode = work / 'additive.bytecode'
        legacy_compiler = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                           *defines, f'--platform={platform}']
        run(*legacy_compiler, '-o', plain_bytecode, driver)
        run(*legacy_compiler, '--shorebird-structural-patching',
            '-o', additive_bytecode, driver)
        source.write_text(UPDATED)
        removed.unlink()
        patch = work / 'update.sbr'
        run(*bytecode, f'--patch-base={baseline}', '-o', patch, driver)
        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            f'-l{runtime_name}', f'-Wl,-rpath,{build}', '-o', executable)

        def exercise(payload, mode, native=base_snapshot):
            run(executable, native, payload, baseline, source.as_uri(),
                removed.as_uri(), mode)

        exercise(baseline, 'base')
        exercise(patch, 'valid')
        if args.envelope_producer:
            envelopes = work / 'envelopes'
            run(args.envelope_dart.resolve(),
                f'--packages={args.envelope_packages.resolve()}',
                args.envelope_producer.resolve(), baseline, patch, envelopes)
            run(executable, base_snapshot, patch, baseline, source.as_uri(),
                removed.as_uri(), 'encrypted', envelopes,
                digest_artifact(baseline), digest_artifact(patch))
        exercise(patch, 'reject-profile', plain_snapshot)
        exercise(plain_bytecode, 'reject-body-profile')
        exercise(additive_bytecode, 'reject-additive-profile')
        content = patch.read_bytes()
        manifest_length = struct.unpack_from('<I', content, 12)[0]
        component_start = 24 + manifest_length
        for name, offset in [('host', 28), ('baseline', 60), ('digest', 92)]:
            invalid = bytearray(content)
            invalid[offset] ^= 1
            path = work / f'wrong-{name}.sbr'
            path.write_bytes(invalid)
            exercise(path, f'reject-{name}')
        for name, invalid in [
                ('truncated', content[:-1]),
                ('trailing', content + b'\0'),
                ('unwrapped', content[component_start:]),
                ('version', content[:8] + struct.pack('<I', 2) + content[12:])]:
            path = work / f'{name}.sbr'
            path.write_bytes(invalid)
            exercise(path, f'reject-{name}')
        # Match the digest deliberately so rejection cannot stop at the outer
        # checksum. The malformed instruction must poison the staging group.
        malformed_producer = work / 'make_malformed.dart'
        malformed_producer.write_text(MALFORMED_PRODUCER)
        malformed = work / 'malformed-component.sbr'
        run(dart, f'--packages={root / ".dart_tool/package_config.json"}',
            malformed_producer, patch, malformed)
        exercise(malformed, 'reject-malformed-component')
        # Corrupt a used URI's string span while preserving every outer binding.
        # A checksum-only rejection would not exercise the native bounds check.
        component = bytearray(content[component_start:])
        strings = struct.unpack_from('<I', component, 12)[0]
        one_byte, two_byte = struct.unpack_from('<II', component, strings)
        strings_data = strings + 8 + 4 * (one_byte + two_byte)
        previous = 0
        root_string_index = None
        for index in range(one_byte):
            end = struct.unpack_from('<I', component, strings + 8 + index * 4)[0]
            if component[strings_data + previous:strings_data + end] == driver.as_uri().encode():
                root_string_index = index
                break
            previous = end
        if root_string_index is None:
            raise AssertionError('Root URI missing from bytecode string table')
        for name, end in [('oversized-string', 0xffffffff),
                          ('decreasing-string', max(previous - 1, 0))]:
            invalid_component = bytearray(component)
            struct.pack_into('<I', invalid_component,
                             strings + 8 + root_string_index * 4, end)
            invalid = bytearray(content[:component_start]) + invalid_component
            invalid[92:124] = hashlib.sha256(invalid_component).digest()
            path = work / f'{name}.sbr'
            path.write_bytes(invalid)
            exercise(path, f'reject-{name}')
        print('Full-domain replacement host acceptance passed; Flutter/device unproven.')


if __name__ == '__main__':
    main()
