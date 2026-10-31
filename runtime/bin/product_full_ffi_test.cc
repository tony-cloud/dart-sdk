// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Positive acceptance uses the real encrypted full installer. The separate
// --reject mode tests generated SBR mutations directly at the public loader;
// it establishes no authentication claim. Neither mode changes loader flags.
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "bin/dartutils.h"
#include "include/dart_api.h"
#include "include/dart_engine.h"
#include "include/dart_native_api.h"
#include "vm/dart_api_impl.h"
#include "vm/dart_api_state.h"
#include "vm/ffi/interpreter_callback.h"
#include "vm/ffi_callback_metadata.h"
#include "vm/handle_visitor.h"
#include "vm/heap/safepoint.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/symbols.h"
#include "vm/thread.h"

#if !defined(PRODUCT) || !defined(DART_PRECOMPILED_RUNTIME) || \
    !defined(TARGET_ARCH_ARM64) || !defined(HOST_ARCH_ARM64)
#error "Requires the native ARM64 PRODUCT precompiled runtime."
#endif

namespace {
constexpr const char* kPlugin = "package:full_ffi_plugin/plugin.dart";
constexpr const char* kRemoved = "package:full_ffi_plugin/removed.dart";
constexpr const char* kAdded = "package:full_ffi_plugin/added.dart";
void Expect(bool ok, const char* message) {
  if (!ok) { std::cerr << message << std::endl; std::exit(1); }
}
void Check(Dart_Handle h) {
  Expect(!Dart_IsError(h), Dart_IsError(h) ? Dart_GetError(h) : "");
}
void Denied(Dart_Handle h, const char* operation) {
  Expect(Dart_IsError(h), operation);
}
Dart_Handle Text(const char* s) { return Dart_NewStringFromCString(s); }
Dart_Handle Lookup(const char* uri) {
  auto h = Dart_LookupLibrary(Text(uri)); Check(h); return h;
}
Dart_Handle Invoke(Dart_Handle lib, const char* name, intptr_t n = 0,
                   Dart_Handle* args = nullptr) {
  auto result = Dart_Invoke(lib, Text(name), n, args); Check(result); return result;
}
int64_t Number(Dart_Handle h) {
  int64_t value = 0; Check(Dart_IntegerToInt64(h, &value)); return value;
}
std::vector<uint8_t> Read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  Expect(input.good(), "cannot read full FFI fixture");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}
void* fixture;
const char* asset_path;
intptr_t resolver_calls, missing_calls, asset_opens, asset_symbols, asset_errors;
intptr_t host_callback_calls;
void* Required(const char* name) {
  void* p = dlsym(fixture, name); Expect(p != nullptr, name); return p;
}
void* FfiResolver(const char* name, uintptr_t argc) {
  ++host_callback_calls;
  if (std::strcmp(name, "FullResolverValue") == 0) {
    Expect(argc == 1, "wrong scalar resolver arity"); ++resolver_calls;
    return Required("FullResolverImplementation");
  }
  if (std::strcmp(name, "FullAddedValue") == 0) {
    Expect(argc == 1, "wrong added binding arity"); ++resolver_calls;
    return Required("FullAddedImplementation");
  }
  if (std::strcmp(name, "FullResolverMissing") == 0) ++missing_calls;
  return nullptr;  // Null must report an error, including process-name collision.
}
Dart_NativeFunction NativeResolver(Dart_Handle, int, bool*) {
  Expect(false, "ordinary ExternalCall reached native resolver"); return nullptr;
}
const uint8_t* NativeSymbol(Dart_NativeFunction) {
  Expect(false, "ordinary native symbol resolver executed"); return nullptr;
}
void* OpenAsset(const char* path, char** error) {
  ++host_callback_calls;
  if (std::strcmp(path, asset_path) != 0) {
    *error = strdup("outside released native fixture map"); return nullptr;
  }
  ++asset_opens;
  void* h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (!h) *error = strdup(dlerror());
  return h;
}
void* AssetSymbol(void* library, const char* name, char** error) {
  ++host_callback_calls;
  if (std::strcmp(name, "FullAssetValue") == 0) ++asset_symbols;
  if (std::strcmp(name, "FullMissingAssetSymbol") == 0) ++asset_errors;
  dlerror(); void* p = dlsym(library, name);
  if (const char* message = dlerror()) *error = strdup(message);
  return p;
}
void CloseAsset(void* library, char** error) {
  ++host_callback_calls;
  if (dlclose(library)) *error = strdup(dlerror());
}
void InspectResolvers(Dart_Handle lib, bool registered) {
  Dart_NativeEntryResolver native = nullptr;
  Dart_NativeEntrySymbol symbol = nullptr;
  Check(Dart_GetNativeResolver(lib, &native));
  Check(Dart_GetNativeSymbol(lib, &symbol));
  Expect(native == (registered ? NativeResolver : nullptr) &&
             symbol == (registered ? NativeSymbol : nullptr),
         "native resolver ownership mismatch");
  const char* uri = nullptr; Check(Dart_StringToCString(Dart_LibraryUrl(lib), &uri));
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t);
  StackZone zone(t);
  const auto& library = Library::Handle(t->zone(), Library::LookupLibrary(
      t, String::Handle(t->zone(), String::New(uri))));
  Expect(library.ffi_native_resolver() == (registered ? FfiResolver : nullptr),
         "FFI resolver ownership mismatch");
}
void Register(Dart_Handle lib) {
  Check(Dart_SetNativeResolver(lib, NativeResolver, NativeSymbol));
  Check(Dart_SetFfiNativeResolver(lib, FfiResolver));
  InspectResolvers(lib, true);
}
void RejectRegistration(Dart_Handle lib) {
  Denied(Dart_SetNativeResolver(lib, NativeResolver, NativeSymbol),
         "retired library accepted native registration");
  Denied(Dart_SetFfiNativeResolver(lib, FfiResolver),
         "retired library accepted FFI registration");
}
void InspectCache(const char* name, bool resolved) {
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone z(t);
  const auto& lib = Library::Handle(t->zone(), Library::LookupLibrary(
      t, String::Handle(t->zone(), String::New(kPlugin))));
  const auto& f = Function::Handle(t->zone(), lib.LookupFunctionAllowPrivate(
      String::Handle(t->zone(), Symbols::New(t, name))));
  Expect(!f.IsNull() && f.HasBytecode() && f.IsInterpreted(),
         "full FFI target is not interpreted bytecode");
  auto& owner = Function::Handle(t->zone(), f.ptr());
  auto& b = Bytecode::Handle(t->zone(), owner.GetBytecode());
  auto& pool = ObjectPool::Handle(t->zone(), b.object_pool());
  auto count_slots = [&]() {
    intptr_t slots = 0;
    for (intptr_t i = 0; i < pool.Length(); ++i)
      if (pool.TypeAt(i) == ObjectPool::EntryType::kNativeFunction) ++slots;
    return slots;
  };
  if (count_slots() == 0) {
    // Compound @Native lowering emits a Dart wrapper whose DirectCall pool
    // entry names the actual paired-flag FFI declaration. Inspect that cache,
    // not the cacheless public wrapper; require a unique real FFI owner.
    auto& target = Function::Handle(t->zone());
    auto& entry = Object::Handle(t->zone());
    for (intptr_t i = 0; i < pool.Length(); ++i) {
      if (pool.TypeAt(i) != ObjectPool::EntryType::kTaggedObject) continue;
      entry = pool.ObjectAt(i);
      if (!entry.IsFunction() || !Function::Cast(entry).is_ffi_native()) continue;
      Expect(target.IsNull() || target.ptr() == entry.ptr(),
             "compound wrapper has multiple FFI cache owners");
      target ^= entry.ptr();
    }
    if (!target.IsNull()) {
      const auto& cls = Class::Handle(t->zone(), target.Owner());
      Expect(cls.library() == lib.ptr() && lib.is_shorebird_full_replacement() &&
                 target.HasBytecode() && target.IsInterpreted(),
             "lowered FFI cache owner is not in the active interpreted domain");
      owner = target.ptr(); b = owner.GetBytecode(); pool = b.object_pool();
    }
  }
  intptr_t slots = 0, nonzero = 0;
  for (intptr_t i = 0; i < pool.Length(); ++i) {
    if (pool.TypeAt(i) != ObjectPool::EntryType::kNativeFunction) continue;
    ++slots; if (pool.RawValueAt(i) != 0) ++nonzero;
  }
  std::cout << "FULL_FFI_CACHE:" << name << ":owner="
            << String::Handle(t->zone(), owner.name()).ToCString()
            << ":expected=" << (resolved ? "resolved" : "zero")
            << ":slots=" << slots << ":nonzero=" << nonzero << std::endl;
  Expect(slots > 0 && (resolved ? nonzero > 0 : nonzero == 0),
         "fresh native caches must start zero and populate only on execution");
}
void InspectFullDomain() {
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone z(t);
  for (const char* uri : {kPlugin}) {
    const auto& lib = Library::Handle(t->zone(), Library::LookupLibrary(
        t, String::Handle(t->zone(), String::New(uri))));
    Expect(!lib.IsNull() && lib.is_shorebird_full_replacement(),
           "plugin was excluded from full mutable domain");
  }
  Expect(Library::Handle(t->zone(), t->isolate_group()->object_store()->root_library())
             .is_shorebird_full_replacement(),
         "native/root snapshot must be a real full-profile baseline");
  Expect(Library::LookupLibrary(t, Symbols::vm_ffi_native_assets()) == Library::null(),
         "native-assets carrier entered active domain");
}
bool wrong_key;
intptr_t key_requests;
bool Key(const char* id, uint8_t* bytes, intptr_t capacity, intptr_t* length) {
  ++key_requests;
  if (!id || std::strcmp(id, "encrypted-aot-host-key") || capacity < 32) return false;
  std::memset(bytes, wrong_key ? 0x43 : 0x42, 32); *length = 32; return true;
}
struct Payload {
  uint8_t* bytes = nullptr; intptr_t length = 0;
  ~Payload() { Dart_FreeAotPatchPayload(bytes); }
};
void RejectEnvelope(const std::string& path,
                    const Dart_AotPatchInstallOptions& options,
                    const char* message) {
  const auto envelope = Read(path); Payload payload;
  const auto h = Dart_InstallAotPatch(envelope.data(), envelope.size(), &options,
                                    &payload.bytes, &payload.length);
  Denied(h, "invalid encrypted envelope accepted");
  Expect(payload.bytes == nullptr && payload.length == 0 &&
             std::strstr(Dart_GetError(h), message) != nullptr,
         Dart_GetError(h));
}
void MakeRunnable(Dart_Isolate isolate) {
  Dart_ExitScope(); Dart_ExitIsolate();
  char* error = Dart_IsolateMakeRunnable(isolate);
  Expect(error == nullptr, error ? error : "");
  Dart_EnterIsolate(isolate); Dart_EnterScope();
}
struct CallbackCounts {
  intptr_t callbacks, roots, code;
  bool keepalive, ports;
  bool operator==(const CallbackCounts& other) const {
    return callbacks == other.callbacks && roots == other.roots &&
           code == other.code && keepalive == other.keepalive &&
           ports == other.ports;
  }
};
intptr_t LivePersistentHandles(dart::ApiState* state) {
  class Visitor : public dart::HandleVisitor {
   public:
    explicit Visitor(const dart::PersistentHandles& handles) : handles_(handles) {}
    void VisitHandle(dart::uword address) override {
      if (!handles_.IsFreeHandle(
              reinterpret_cast<Dart_PersistentHandle>(address))) ++count;
    }
    intptr_t count = 0;
   private:
    const dart::PersistentHandles& handles_;
  };
  intptr_t live = 0;
  state->RunWithLockedPersistentHandles([&](dart::PersistentHandles& handles) {
    Visitor visitor(handles); handles.Visit(&visitor); live = visitor.count;
  });
  return live;
}
CallbackCounts CountCallbacks() {
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone zone(t);
  intptr_t callbacks = 0;
  for (auto* entry = t->isolate()->ffi_callback_list_head(); entry;
       entry = entry->list_next()) ++callbacks;
  const auto& code = GrowableObjectArray::Handle(
      t->zone(), t->isolate_group()->object_store()->ffi_callback_code());
  return {callbacks, LivePersistentHandles(t->isolate_group()->api_state()),
          code.IsNull() ? 0 : code.Length(),
          t->isolate()->HasOpenNativeCallables(), t->isolate()->HasLivePorts()};
}
void ExpectCallbackCounts(const CallbackCounts& expected, const char* message) {
  const auto actual = CountCallbacks();
  if (!(actual == expected)) {
    auto print = [](const char* label, const CallbackCounts& value) {
      std::cerr << label << ": callbacks=" << value.callbacks
                << " roots=" << value.roots << " code=" << value.code
                << " keepalive=" << value.keepalive << " ports=" << value.ports
                << std::endl;
    };
    print("expected", expected); print("actual", actual);
  }
  Expect(actual == expected, message);
}
void ExpectDeadCallbacks(const std::vector<uintptr_t>& pointers) {
  auto* t = dart::Thread::Current();
  dart::TransitionNativeToVM transition(t); dart::StackZone zone(t);
  for (auto pointer : pointers) {
    Expect(!dart::FfiCallbackMetadata::Instance(pointer)
                ->LookupMetadataForTrampolineUnlocked(pointer).IsLive(),
           "closed full-domain callback retained live metadata");
  }
}
void RunCallbacks(Dart_Isolate isolate) {
  const auto initial = CountCallbacks();
  Dart_Handle path = Text(asset_path);
  const auto static_pointer = static_cast<uintptr_t>(
      Number(Invoke(Lookup(kPlugin), "prepareStaticCallback", 1, &path)));
  const auto static_counts = CountCallbacks();
  Expect(static_counts.callbacks == initial.callbacks + 1 &&
             static_counts.roots == initial.roots + 1 &&
             static_counts.code == initial.code &&
             static_counts.keepalive == initial.keepalive &&
             static_counts.ports == initial.ports,
         "noncloseable Pointer.fromFunction must own one root/list entry only");
  const intptr_t count = 2 * dart::FfiCallbackMetadata::NumCallbackTrampolinesPerPage() + 17;
  Dart_Handle args[] = {Text(asset_path), Dart_NewInteger(count)};
  auto values = Invoke(Lookup(kPlugin), "prepareCallbacks", 2, args);
  intptr_t length = 0; Check(Dart_ListLength(values, &length));
  Expect(length == count + 7, "callback pool did not cross signed alias pages");
  std::vector<uintptr_t> pointers;
  std::set<uintptr_t> alias_pages;
  for (intptr_t i = 0; i < length; ++i) {
    auto value = Dart_ListGetAt(values, i); Check(value);
    pointers.push_back(static_cast<uintptr_t>(Number(value)));
    alias_pages.insert(pointers.back() & dart::FfiCallbackMetadata::kPageMask);
    {
      auto* thread = dart::Thread::Current();
      dart::TransitionNativeToVM transition(thread);
      dart::StackZone zone(thread);
      const auto metadata = dart::FfiCallbackMetadata::Instance(pointers.back())
          ->LookupMetadataForTrampolineUnlocked(pointers.back());
      Expect(metadata.IsLive() && dart::IsInterpreterFfiCallbackEntry(
                 metadata.target_entry_point()),
             "full callback did not use released static interpreter entry");
    }
  }
  Expect(pointers[0] == static_pointer, "static callback identity changed");
  Expect(alias_pages.size() > 1, "callback pool did not occupy multiple signed alias pages");
  const auto live_counts = CountCallbacks();
  Expect(live_counts.callbacks > static_counts.callbacks &&
             live_counts.roots > static_counts.roots &&
             live_counts.code == static_counts.code &&
             live_counts.keepalive && live_counts.ports,
         "full callback registration did not retain descriptors/list/ports/keepalive");
  const auto self_pointer = static_cast<uintptr_t>(
      Number(Invoke(Lookup(kPlugin), "runSelfClose")));
  ExpectDeadCallbacks({self_pointer});
  ExpectCallbackCounts(live_counts,
      "self-close leaked full-domain callback roots/list/keepalive/ports");
  auto threads = reinterpret_cast<int64_t (*)(void*, void*)>(Required("FullThreads"));
  Dart_ExitScope(); Dart_ExitIsolate();
  Expect(threads(reinterpret_cast<void*>(pointers[5]),
                 reinterpret_cast<void*>(pointers[4])) == 105,
         "concurrent foreign group callbacks returned wrong values");
  Dart_EnterIsolate(isolate); Dart_EnterScope();
  for (intptr_t i = 0; i < 32; ++i) {
    if (Number(Invoke(Lookup(kPlugin), "listenerDone")) == 1) break;
    Check(Dart_HandleMessage());
  }
  Expect(Number(Invoke(Lookup(kPlugin), "listenerDone")) == 1,
         "queued scalar/compound listener delivery incomplete");
  Invoke(Lookup(kPlugin), "finishCallbacks");
  Expect(Number(Invoke(Lookup(kPlugin), "callbackOwnerCount")) == 0,
         "closed callback wrappers remain in full-domain owner fields/lists");
  // The native workers have joined above and queued payloads were consumed.
  // Closed addresses are inspected, never invoked. The noncloseable static
  // pointer at index zero remains valid until isolate shutdown.
  ExpectDeadCallbacks(std::vector<uintptr_t>(pointers.begin() + 1, pointers.end()));
  ExpectCallbackCounts(static_counts,
      "closing full callbacks leaked descriptor roots/list/code/keepalive/ports");
  {
    auto* t = dart::Thread::Current();
    dart::TransitionNativeToVM transition(t); dart::StackZone zone(t);
    Expect(dart::FfiCallbackMetadata::Instance(static_pointer)
               ->LookupMetadataForTrampolineUnlocked(static_pointer).IsLive(),
           "noncloseable static callback was incorrectly retired by cleanup");
  }
  std::cout << "FULL_FFI_CLEANUP:dead-metadata-roots-list-ports-keepalive-static-survivor" << std::endl;
  std::cout << "FULL_FFI_CALLBACKS:local-static-capture-aggregate-listener-group-gc-close-pages" << std::endl;
}

void ExpectDiagnostic(Dart_Handle result, const char* expected) {
  Denied(result, "admission mutation unexpectedly loaded");
  Expect(expected != nullptr && expected[0] != '\0', "missing exact admission diagnostic");
  const char* actual = Dart_GetError(result);
  if (std::strstr(actual, expected) == nullptr) {
    std::cerr << "Expected admission diagnostic: " << expected
              << "\nActual admission diagnostic: " << actual << std::endl;
    Expect(false, "mutation failed at the wrong loader layer");
  }
}
void ExpectNoNativeExecution() {
  Expect(host_callback_calls == 0 && resolver_calls == 0 && missing_calls == 0 &&
             asset_opens == 0 && asset_symbols == 0 && asset_errors == 0 &&
             reinterpret_cast<int64_t (*)()>(Required("FullCalls"))() == 0,
         "admission validation invoked a host resolver, callback or native export");
}
Dart_Isolate CreateRejectionGroup(const DartEngine_SnapshotData& snapshot,
                                  const char* name) {
  resolver_calls = missing_calls = asset_opens = asset_symbols = asset_errors = 0;
  host_callback_calls = 0;
  reinterpret_cast<void (*)()>(Required("FullReset"))();
  Dart_IsolateFlags flags; Dart_IsolateFlagsInitialize(&flags);
  char* error = nullptr;
  auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, name,
      snapshot.snapshot_data, snapshot.snapshot_text, &flags, nullptr, nullptr, &error);
  Expect(error == nullptr && isolate != nullptr, error ? error : "isolate creation failed");
  NativeAssetsApi assets = {}; assets.dlopen_absolute = OpenAsset;
  assets.dlsym = AssetSymbol; assets.dlclose = CloseAsset;
  Dart_InitializeNativeAssetsResolver(&assets);
  Dart_EnterScope(); InspectFullDomain();
  return isolate;
}
void CannotRun(Dart_Isolate isolate) {
  Denied(Dart_Invoke(Dart_RootLibrary(), Text("probe"), 0, nullptr),
         "rejected/inactive native application executed");
  Dart_ExitScope(); Dart_ExitIsolate();
  char* error = Dart_IsolateMakeRunnable(isolate);
  Expect(error != nullptr, "rejected/inactive full group became runnable");
  std::free(error);
  Dart_EnterIsolate(isolate); Dart_EnterScope();
}
struct SavedPublication {
  Dart_PersistentHandle root, plugin, removed, map, libraries;
  SavedPublication() {
    root = Dart_NewPersistentHandle(Dart_RootLibrary());
    plugin = Dart_NewPersistentHandle(Lookup(kPlugin));
    removed = Dart_NewPersistentHandle(Lookup(kRemoved));
    Dart_Handle map_handle, libraries_handle;
    {
      using namespace dart;
      auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone z(t);
      auto* store = t->isolate_group()->object_store();
      map_handle = Api::NewHandle(t, store->native_assets_map());
      libraries_handle = Api::NewHandle(t, store->libraries());
    }
    map = Dart_NewPersistentHandle(map_handle);
    libraries = Dart_NewPersistentHandle(libraries_handle);
  }
  void CheckUnpublished(bool poisoned) const {
    using namespace dart;
    auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone z(t);
    auto* store = t->isolate_group()->object_store();
    auto unwrap = [](Dart_PersistentHandle handle) {
      return Api::UnwrapHandle(Dart_HandleFromPersistent(handle));
    };
    auto lookup = [&](const char* uri) {
      return Library::LookupLibrary(t, String::Handle(t->zone(), String::New(uri)));
    };
    Expect(store->root_library() == unwrap(root) && lookup(kPlugin) == unwrap(plugin) &&
               lookup(kRemoved) == unwrap(removed) && lookup(kAdded) == Library::null() &&
               store->libraries() == unwrap(libraries) &&
               store->native_assets_map() == unwrap(map),
           "failed validation changed active library/root/map publication");
    Expect(store->shorebird_replacement_installation_state() ==
               (poisoned ? Smi::New(1) : Smi::null()),
           "mutation reached the wrong staging/poison boundary");
  }
  ~SavedPublication() {
    for (auto handle : {root, plugin, removed, map, libraries})
      Dart_DeletePersistentHandle(handle);
  }
};
// Deliberately direct SBR loader validation, distinct from the authenticated
// encrypted B/U consumer above. Each process tests one generated mutation.
void RunRejection(const DartEngine_SnapshotData& snapshot,
                  const std::vector<uint8_t>& mutation,
                  const std::vector<uint8_t>& base,
                  const char* diagnostic, bool poisoned, const char* name) {
  auto isolate = CreateRejectionGroup(snapshot, "full-ffi-admission-reject");
  InspectResolvers(Lookup(kPlugin), false); Register(Lookup(kPlugin));
  InspectResolvers(Lookup(kRemoved), false); Register(Lookup(kRemoved));
  CannotRun(isolate);
  {
    SavedPublication publication;
    const auto before = CountCallbacks();
    ExpectDiagnostic(Dart_ReloadBytecodePatch(mutation.data(), mutation.size()), diagnostic);
    publication.CheckUnpublished(poisoned);
    ExpectNoNativeExecution();
    ExpectCallbackCounts(before, "rejected mutation created callback descriptors/roots/ports");
    CannotRun(isolate);
    if (poisoned) {
      RejectRegistration(Dart_HandleFromPersistent(publication.plugin));
      RejectRegistration(Dart_HandleFromPersistent(publication.removed));
      ExpectDiagnostic(Dart_ReloadBytecodePatch(base.data(), base.size()),
                       "Full replacement installation already attempted/late.");
      publication.CheckUnpublished(true);
      ExpectNoNativeExecution();
      ExpectCallbackCounts(before, "poisoned retry changed callback ownership");
    } else {
      InspectResolvers(Dart_HandleFromPersistent(publication.plugin), true);
      InspectResolvers(Dart_HandleFromPersistent(publication.removed), true);
    }
  }
  Dart_ExitScope(); Dart_ShutdownIsolate();

  // Recovery always destroys the failed group, even for preflight failures.
  // Valid B is installed into a genuinely fresh group, not retried in poison.
  isolate = CreateRejectionGroup(snapshot, "full-ffi-admission-recovery");
  InspectResolvers(Lookup(kPlugin), false); Register(Lookup(kPlugin));
  Check(Dart_ReloadBytecodePatch(base.data(), base.size()));
  ExpectNoNativeExecution(); InspectResolvers(Lookup(kPlugin), true);
  Check(dart::bin::DartUtils::SetupCoreLibraries(false, false, false, dart::bin::DartIoSettings{}));
  MakeRunnable(isolate);
  Expect(Number(Invoke(Dart_RootLibrary(), "probe")) == 10 &&
             Number(Invoke(Lookup(kPlugin), "runNonFfi")) == 12,
         "fresh recovery group failed to execute bundled B");
  Expect(Number(Invoke(Lookup(kPlugin), "runResolver")) == 109 && resolver_calls == 1 &&
             Number(Invoke(Lookup(kPlugin), "runAsset")) == 211 &&
             asset_opens == 1 && asset_symbols == 1,
         "fresh recovery group lost released native resolver/assets");
  Dart_ExitScope(); Dart_ShutdownIsolate();
  std::cout << "FULL_FFI_REJECT:" << name << ":"
            << (poisoned ? "poisoned" : "preflight") << ":fresh-B-recovery" << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
  // snapshot, SBR, released dylib, base|update, envelope directory, B hash, payload hash
  const bool reject = argc == 9 && std::strcmp(argv[4], "--reject") == 0;
  if ((!reject && argc != 8) || !Dart_IsPrecompiledRuntime() || !Dart_AotPatchingEnabled()) return 2;
  const bool update = std::strcmp(argv[4], "update") == 0;
  Expect(reject || update || std::strcmp(argv[4], "base") == 0, "unknown mode");
  asset_path = argv[3]; fixture = dlopen(asset_path, RTLD_NOW | RTLD_GLOBAL);
  Expect(fixture != nullptr, "cannot open released native fixture");
  char* error = nullptr;
  Expect(DartEngine_Init(&error), error ? error : "engine init failed");
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  Expect(error == nullptr, error ? error : "");
  Dart_InitializeParams init = {}; init.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  error = Dart_Initialize(&init); Expect(error == nullptr, error ? error : "");
  const auto sbr = Read(argv[2]);
  if (reject) {
    Expect(std::strcmp(argv[7], "poisoned") == 0 || std::strcmp(argv[7], "preflight") == 0,
           "unknown expected staging boundary");
    RunRejection(snapshot, sbr, Read(argv[5]), argv[6],
                 std::strcmp(argv[7], "poisoned") == 0, argv[8]);
    DartEngine_Shutdown(); error = Dart_Cleanup();
    Expect(error == nullptr, error ? error : ""); dlclose(fixture); return 0;
  }
  Dart_SetAotPatchKeyCallback(Key);
  Dart_AotPatchInstallOptions options = {};
  options.app_id = "encrypted-aot-host-app"; options.app_build_id = "1.0.0+1";
  options.base_flavor_id = "free"; options.base_license_type = "free";
  options.flavor_id = "pro"; options.license_type = "pro";
  options.sdk_hash = "encrypted-aot-host-sdk";
  options.base_snapshot_hash = argv[6]; options.patch_snapshot_hash = argv[7];
  options.target_os = "ios"; options.target_arch = "arm64";
  options.runtime_mode = "dart-bytecode-interpreter";
  const std::string envelopes(argv[5]);
  for (intptr_t group = 0; group < 2; ++group) {
    resolver_calls = missing_calls = asset_opens = asset_symbols = asset_errors = 0;
    host_callback_calls = 0;
    reinterpret_cast<void (*)()>(Required("FullReset"))();
    Dart_IsolateFlags flags; Dart_IsolateFlagsInitialize(&flags);
    auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, "full-ffi",
        snapshot.snapshot_data, snapshot.snapshot_text, &flags, nullptr, nullptr, &error);
    Expect(error == nullptr && isolate != nullptr, error ? error : "isolate creation failed");
    NativeAssetsApi assets = {}; assets.dlopen_absolute = OpenAsset;
    assets.dlsym = AssetSymbol; assets.dlclose = CloseAsset;
    Dart_InitializeNativeAssetsResolver(&assets);
    Dart_EnterScope();
    InspectFullDomain();
    auto old_root = Dart_NewPersistentHandle(Dart_RootLibrary());
    auto old_plugin = Dart_NewPersistentHandle(Lookup(kPlugin));
    auto old_removed = Dart_NewPersistentHandle(Lookup(kRemoved));
    auto old_type_value = Dart_GetNonNullableType(Lookup(kPlugin), Text("Plugin"), 0, nullptr);
    Check(old_type_value); auto old_type = Dart_NewPersistentHandle(old_type_value);
    auto removed_type = Dart_GetNonNullableType(Lookup(kRemoved), Text("RemovedPluginClass"), 0, nullptr);
    Check(removed_type); auto old_removed_type = Dart_NewPersistentHandle(removed_type);
    InspectResolvers(Lookup(kPlugin), false); InspectResolvers(Lookup(kRemoved), false);
    Register(Lookup(kPlugin)); Register(Lookup(kRemoved));
    Denied(Dart_Invoke(Dart_RootLibrary(), Text("probe"), 0, nullptr), "inactive N executed");
    auto tearoff = Dart_GetField(Dart_RootLibrary(), Text("probe"));
    auto old_function = Dart_IsError(tearoff) ? nullptr : Dart_NewPersistentHandle(tearoff);
    // Root the released map across allocations/install rather than holding a raw GC pointer.
    Dart_Handle map_handle;
    {
      using namespace dart;
      auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone z(t);
      const auto& map = Array::Handle(t->zone(), t->isolate_group()->object_store()->native_assets_map());
      ::Expect(!map.IsNull(), "released map missing");
      map_handle = Api::NewHandle(t, map.ptr());
    }
    auto map_root = Dart_NewPersistentHandle(map_handle);
    wrong_key = true;
    RejectEnvelope(envelopes + "/valid.json", options, "AES-GCM decryption failed");
    wrong_key = false;
    RejectEnvelope(envelopes + "/tampered.json", options, "AES-GCM decryption failed");
    RejectEnvelope(envelopes + "/expired.json", options, "offline license has expired");
    Expect(resolver_calls == 0 && asset_opens == 0 && missing_calls == 0 &&
               reinterpret_cast<int64_t (*)()>(Required("FullCalls"))() == 0,
           "envelope validation executed native application code");
    const auto envelope = Read(envelopes + "/valid.json");
    {
      Payload payload;
      Check(Dart_InstallAotPatch(envelope.data(), envelope.size(), &options,
                                &payload.bytes, &payload.length));
      Expect(payload.length == static_cast<intptr_t>(sbr.size()) &&
                 std::memcmp(payload.bytes, sbr.data(), sbr.size()) == 0,
             "decrypted component differs from signed producer output");
      Check(Dart_ReloadBytecodePatch(payload.bytes, payload.length));
    }
    Expect(resolver_calls == 0 && asset_opens == 0,
           "full validation resolved a native function");
    auto plugin = Lookup(kPlugin);
    InspectFullDomain();
    Expect(!Dart_IdentityEquals(plugin, Dart_HandleFromPersistent(old_plugin)) &&
               !Dart_IdentityEquals(Dart_RootLibrary(), Dart_HandleFromPersistent(old_root)),
           "full publication reused library identity");
    auto current_type = Dart_GetNonNullableType(plugin, Text("Plugin"), 0, nullptr); Check(current_type);
    Expect(!Dart_IdentityEquals(current_type, Dart_HandleFromPersistent(old_type)), "plugin class identity reused");
    InspectResolvers(plugin, true);
    if (update) {
      Denied(Dart_LookupLibrary(Text(kRemoved)), "removed plugin library survived");
      auto added = Lookup(kAdded); InspectResolvers(added, false); Register(added);
    } else {
      auto removed = Lookup(kRemoved);
      Expect(!Dart_IdentityEquals(removed, Dart_HandleFromPersistent(old_removed)), "retained removed URI reused identity");
      auto type = Dart_GetNonNullableType(removed, Text("RemovedPluginClass"), 0, nullptr); Check(type);
      Expect(!Dart_IdentityEquals(type, Dart_HandleFromPersistent(old_removed_type)), "retained class identity reused");
      InspectResolvers(removed, true); Denied(Dart_LookupLibrary(Text(kAdded)), "update-only library in B");
    }
    {
      using namespace dart;
      auto* t = Thread::Current(); TransitionNativeToVM transition(t); StackZone z(t);
      const auto& map = Array::Handle(t->zone(), t->isolate_group()->object_store()->native_assets_map());
      ::Expect(Api::UnwrapHandle(Dart_HandleFromPersistent(map_root)) == map.ptr(), "released map identity changed");
      ::Expect(Library::LookupLibrary(t, Symbols::vm_ffi_native_assets()) == Library::null(), "native-assets carrier published");
    }
    RejectRegistration(Dart_HandleFromPersistent(old_plugin));
    Denied(Dart_Invoke(Dart_HandleFromPersistent(old_plugin), Text("runResolver"), 0, nullptr), "retired library ran");
    Denied(Dart_New(Dart_HandleFromPersistent(old_type), Dart_Null(), 0, nullptr), "retired class constructed");
    if (old_function) {
      Denied(Dart_InvokeClosure(Dart_HandleFromPersistent(old_function), 0, nullptr), "retired closure ran");
      Dart_DeletePersistentHandle(old_function);
    }
    for (auto h : {old_root, old_plugin, old_removed, old_type, old_removed_type, map_root}) Dart_DeletePersistentHandle(h);
    for (const char* name : {"resolvedValue", "runAddressOf", "changeRecord", "assetValue", "processValue"}) InspectCache(name, false);
    Check(dart::bin::DartUtils::SetupCoreLibraries(false, false, false, dart::bin::DartIoSettings{}));
    MakeRunnable(isolate); plugin = Lookup(kPlugin);
    Expect(Number(Invoke(Dart_RootLibrary(), "entrypointRuns")) == 0, "main ran before publication checks");
    Expect(Number(Invoke(Dart_RootLibrary(), "probe")) == (update ? 48 : 10), "unchanged consumer used old default");
    Expect(Number(Invoke(plugin, "runNonFfi")) == 12, "ordinary Dart/const/layout regression");
    for (int i = 0; i < 2; ++i) Expect(Number(Invoke(plugin, "runResolver")) == 109, "resolver result");
    Expect(resolver_calls == 1, "direct resolver cache not reused"); InspectCache("resolvedValue", true);
    for (int i = 0; i < 2; ++i) Expect(Number(Invoke(plugin, "runAddressOf")) == 110, "addressOf result");
    Expect(resolver_calls == 2, "addressOf independent cache not reused"); InspectCache("runAddressOf", true);
    for (int i = 0; i < 2; ++i) Expect(Number(Invoke(plugin, "runAsset")) == 211, "asset result");
    Expect(asset_opens == 1 && asset_symbols == 1, "native assets route/cache mismatch");
    InspectCache("assetValue", true);
    for (int i = 0; i < 2; ++i) Expect(Number(Invoke(plugin, "runProcess")) == 312, "process fallback result");
    InspectCache("processValue", true);
    auto native_count = reinterpret_cast<int64_t (*)()>(Required("FullCalls"));
    const auto before = native_count();
    Expect(Number(Invoke(plugin, "runErrors")) == 4 && missing_calls == 2 &&
               asset_errors == 2 && asset_opens == 3 && native_count() == before,
           "error resolution cached, fell through, or executed native code");
    Dart_Handle argument = Text(asset_path);
    Expect(Number(Invoke(plugin, "runCalls", 1, &argument)) == (update ? 544 : 8), "native call/new layout result");
    InspectCache("changeRecord", true);
    if (update) {
      Expect(resolver_calls == 3, "added URI did not use postinstall resolver");
      RunCallbacks(isolate);
    }
    std::cout << "FULL_FFI_PASS:" << argv[4] << ":fresh-group-" << group << std::endl;
    Dart_ExitScope(); Dart_ShutdownIsolate();
  }
  DartEngine_Shutdown(); error = Dart_Cleanup(); Expect(error == nullptr, error ? error : "");
  dlclose(fixture); return 0;
}
