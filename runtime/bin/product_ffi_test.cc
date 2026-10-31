// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Test-only internal embedder linking the real PRODUCT precompiled runtime.
// Loads ordinary bytecode into a new library in a non-full baseline; it does
// not bypass the structural/full replacement admission checks.
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <cstdio>
#endif

#include "bin/dartutils.h"
#include "include/dart_api.h"
#include "include/dart_engine.h"
#include "include/dart_native_api.h"
#include "lib/ffi_dynamic_library.h"
#include "vm/ffi/native_assets.h"
#include "vm/heap/safepoint.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/shorebird_replacement.h"
#include "vm/symbols.h"
#include "vm/thread.h"

#if !defined(PRODUCT) || !defined(DART_PRECOMPILED_RUNTIME) ||                 \
    !defined(TARGET_ARCH_ARM64) || !defined(HOST_ARCH_ARM64)
#error "This test requires the native ARM64 PRODUCT precompiled runtime."
#endif

namespace {
void Expect(bool value, const char* message) {
  if (!value) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}
void Check(Dart_Handle value) {
  Expect(!Dart_IsError(value), Dart_IsError(value) ? Dart_GetError(value) : "");
}

// Observe the existing frozen SDK surface without initializing/replacing its
// lazy FFI bundle or creating synthetic functions/flags in the test.
void InspectBaseline() {
  using namespace dart;
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  auto* zone = thread->zone();
  auto* store = thread->isolate_group()->object_store();
  const auto& root = Library::Handle(zone, store->root_library());
  Expect(
      root.is_shorebird_patchable() && !root.is_shorebird_full_replacement(),
      "test baseline must admit ordinary bytecode, without full replacement");
  const auto& ffi = Library::Handle(zone, Library::FfiLibrary());
  Expect(!ffi.IsNull(), "baseline has no frozen dart:ffi library");
  bool valid = true;
  auto require = [&](bool present, const char* name) {
    if (!present) {
      std::cerr << "Missing baseline FFI prerequisite: " << name << std::endl;
      valid = false;
    }
  };
  auto& cls = Class::Handle(zone);
  for (const char* name : {"Native", "_FfiCall", "_Compound", "Array", "Struct",
                           "Union", "VarArgs", "IntPtr"}) {
    cls = ffi.LookupClassAllowPrivate(
        String::Handle(zone, Symbols::New(thread, name)));
    require(!cls.IsNull(), name);
  }
  cls = ffi.LookupClassAllowPrivate(
      String::Handle(zone, Symbols::New(thread, "_Compound")));
  if (!cls.IsNull()) {
    for (const auto* name : {"_offsetInBytes", "_typedDataBase"}) {
      require(!Field::Handle(zone, cls.LookupFieldAllowPrivate(String::Handle(
                                       zone, Symbols::New(thread, name))))
                   .IsNull(),
              name);
    }
  }
  cls = ffi.LookupClass(Symbols::FfiNative());
  if (!cls.IsNull()) {
    for (const auto* name : {"symbol", "assetId", "isLeaf"}) {
      require(!Field::Handle(zone, cls.LookupFieldAllowPrivate(String::Handle(
                                       zone, Symbols::New(thread, name))))
                   .IsNull(),
              name);
    }
    const auto& resolver =
        Function::Handle(zone, cls.LookupStaticFunctionAllowPrivate(
                                   Symbols::_ffi_resolver_function()));
    require(!resolver.IsNull(), "Native._ffi_resolver_function");
    if (!resolver.IsNull()) {
      require(resolver.HasCode(), "Native._ffi_resolver_function.HasCode");
    }
  }
  for (bool native : {false, true}) {
    cls = native ? store->native_finalizer_class() : store->finalizer_class();
    require(!cls.IsNull(),
            native ? "native_finalizer_class" : "finalizer_class");
    if (cls.IsNull()) continue;
    const auto& function = Function::Handle(
        zone, cls.LookupFunctionAllowPrivate(
                  native ? Symbols::_handleNativeFinalizerMessage()
                         : Symbols::_handleFinalizerMessage()));
    require(!function.IsNull(), native ? "_handleNativeFinalizerMessage"
                                       : "_handleFinalizerMessage");
    if (!function.IsNull())
      require(function.HasCode(), "lazy finalizer function.HasCode");
  }
  Expect(valid, "frozen baseline FFI prerequisite inspection failed");
  std::cout << "No-app-FFI baseline SDK dictionaries/resolver code passed"
            << std::endl;
}

void InspectLoaded(const char* uri) {
  using namespace dart;
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  auto* zone = thread->zone();
  const auto& library = Library::Handle(
      zone,
      Library::LookupLibrary(thread, String::Handle(zone, String::New(uri))));
  Expect(!library.IsNull(), "ordinary bytecode library missing");
  const auto& function = Function::Handle(
      zone, library.LookupFunctionAllowPrivate(Symbols::main()));
  Expect(
      !function.IsNull() && function.HasBytecode() && function.IsInterpreted(),
      "loaded entrypoint must execute real bytecode through the interpreter");
}

struct ExecutableRegion {
  uintptr_t start;
  uintptr_t end;
};
std::vector<ExecutableRegion> ExecutableMappings() {
  std::vector<ExecutableRegion> regions;
#if defined(__APPLE__)
  mach_vm_address_t address = 0;
  natural_t depth = 0;
  while (true) {
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    const auto result = mach_vm_region_recurse(
        mach_task_self(), &address, &size, &depth,
        reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
    if (result == KERN_INVALID_ADDRESS) break;
    Expect(result == KERN_SUCCESS && size > 0,
           "cannot inspect executable mappings");
    if (info.is_submap) {
      ++depth;
      continue;
    }
    if ((info.protection & VM_PROT_EXECUTE) != 0) {
      Expect((info.protection & VM_PROT_WRITE) == 0,
             "writable executable mapping observed");
      regions.push_back({static_cast<uintptr_t>(address),
                         static_cast<uintptr_t>(address + size)});
    }
    address += size;
  }
#else
  auto* file = std::fopen("/proc/self/maps", "r");
  Expect(file != nullptr, "cannot inspect /proc/self/maps");
  char line[4096];
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    unsigned long start, end;
    char permissions[5];
    if (std::sscanf(line, "%lx-%lx %4s", &start, &end, permissions) != 3)
      continue;
    if (permissions[2] == 'x') {
      Expect(permissions[1] != 'w', "writable executable mapping observed");
      regions.push_back({start, end});
    }
  }
  std::fclose(file);
#endif
  return regions;
}
void CompareExecutableMappings(const std::vector<ExecutableRegion>& before) {
  const auto after = ExecutableMappings();
  for (const auto& region : after) {
    uintptr_t cursor = region.start;
    for (const auto& old : before) {
      if (old.start <= cursor && cursor < old.end) cursor = old.end;
      if (cursor >= region.end) break;
    }
    Expect(cursor >= region.end,
           "new executable mapping observed during bytecode FFI calls");
  }
  std::cout << "No added executable or writable-executable mappings observed "
               "at audit checkpoints; "
               "transient mapping/permission changes and instruction writes "
               "are not audited"
            << std::endl;
}

void* fixture_library;
const char* released_asset_path;
uintptr_t asset_open_calls = 0;
uintptr_t asset_symbol_calls = 0;
uintptr_t asset_missing_calls = 0;
void* OpenReleasedAsset(const char* path, char** error) {
  if (released_asset_path == nullptr ||
      std::strcmp(path, released_asset_path) != 0) {
    *error = strdup("asset path is outside the released fixture mapping");
    return nullptr;
  }
  ++asset_open_calls;
  auto* result = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
  if (result == nullptr) *error = strdup(dlerror());
  return result;
}
void* AssetSymbol(void* handle, const char* symbol, char** error) {
  if (std::strcmp(symbol, "ProductAssetOnly") == 0) ++asset_symbol_calls;
  if (std::strcmp(symbol, "ProductMissingAssetSymbol") == 0)
    ++asset_missing_calls;
  dlerror();
  auto* result = dlsym(handle, symbol);
  if (const auto* message = dlerror()) *error = strdup(message);
  return result;
}
void CloseAsset(void* handle, char** error) {
  if (dlclose(handle) != 0) *error = strdup(dlerror());
}
uintptr_t resolved_add_calls = 0;
uintptr_t missing_calls = 0;
void* Resolver(const char* symbol, uintptr_t num_args) {
  if (std::strcmp(symbol, "ProductResolverAdd") == 0) {
    Expect(num_args == 2, "resolver received wrong native parameter count");
    ++resolved_add_calls;
    return dlsym(fixture_library, "ProductAdd");
  }
  if (std::strcmp(symbol, "ProductResolverMissing") == 0) {
    ++missing_calls;
    return nullptr;
  }
  return dlsym(RTLD_DEFAULT, symbol);
}
Dart_Handle Invoke(Dart_Handle library,
                   const char* name,
                   int argc = 0,
                   Dart_Handle* arguments = nullptr) {
  auto result =
      Dart_Invoke(library, Dart_NewStringFromCString(name), argc, arguments);
  Check(result);
  return result;
}
int64_t Integer(Dart_Handle value) {
  int64_t result = 0;
  Check(Dart_IntegerToInt64(value, &result));
  return result;
}

void InspectReleaseMap(const char* path) {
  using namespace dart;
  auto* thread = Thread::Current();
  auto* zone = thread->zone();
  const auto& array = Array::Handle(
      zone, thread->isolate_group()->object_store()->native_assets_map());
  Expect(!array.IsNull(), "released native-assets map missing");
  NativeAssetsMap map(array.ptr());
  const auto& value = Object::Handle(
      zone, map.GetOrNull(String::Handle(
                zone, Symbols::New(thread, "product-release-asset"))));
  map.Release();
  Expect(value.IsArray() && Array::Cast(value).Length() == 2,
         "released native-asset location missing");
  const auto& kind =
      String::Handle(zone, String::RawCast(Array::Cast(value).At(0)));
  const auto& location =
      String::Handle(zone, String::RawCast(Array::Cast(value).At(1)));
  Expect(kind.Equals("absolute") && location.Equals(path),
         "native-asset mapping differs from released baseline path");
}

void InspectOrdinaryAssets(const char* path) {
  using namespace dart;
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  auto* zone = thread->zone();
  auto* store = thread->isolate_group()->object_store();
  const auto& root = Library::Handle(zone, store->root_library());
  Expect(
      !root.is_shorebird_patchable() && store->pragma_class() == Class::null(),
      "ordinary TFA snapshot retained interpreter metadata roots");
  InspectReleaseMap(path);
  Expect(Library::LookupLibrary(thread, Symbols::vm_ffi_native_assets()) ==
             Library::null(),
         "ordinary TFA snapshot published native-assets metadata carrier");
  std::cout << "Ordinary TFA native-assets map/isolate load passed without "
               "lazy FFI retention"
            << std::endl;
}

// Exercise full publication without any application FFI declarations or FFI
// bytecodes. Existing VM map/lookup APIs verify the release-owned metadata;
// actual interpreted native calls are tested separately by the raw library.
void RunFullAssets(const std::vector<uint8_t>& bytes,
                   const char* plugin_uri,
                   const char* asset_path,
                   int64_t expected_plugin_value) {
  using namespace dart;
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  auto* zone = thread->zone();
  auto* store = thread->isolate_group()->object_store();
  const auto& map = Array::Handle(zone, store->native_assets_map());
  InspectReleaseMap(asset_path);
  const auto& descriptor =
      TypedData::Handle(zone, store->shorebird_replacement_descriptor());
  Expect(!descriptor.IsNull(), "full native baseline descriptor missing");
  std::vector<uint8_t> encoded(descriptor.Length());
  for (intptr_t i = 0; i < descriptor.Length(); ++i)
    encoded[i] = descriptor.GetUint8(i);
  ShorebirdReplacementManifest baseline;
  Expect(baseline.Parse(encoded.data(), encoded.size()),
         "invalid native baseline descriptor");
  const auto carrier = "vm:ffi:native-assets";
  Expect(std::find(baseline.base.begin(), baseline.base.end(), carrier) ==
                 baseline.base.end() &&
             std::find(baseline.updated.begin(), baseline.updated.end(),
                       carrier) == baseline.updated.end(),
         "native-assets metadata carrier entered mutable domain");
  Expect(std::find(baseline.base.begin(), baseline.base.end(), plugin_uri) !=
             baseline.base.end(),
         "package plugin was excluded from mutable domain");
  const auto& uri = String::Handle(zone, String::New(plugin_uri));
  const auto& old_library =
      Library::Handle(zone, Library::LookupLibrary(thread, uri));
  Expect(!old_library.IsNull() && old_library.is_shorebird_full_replacement(),
         "package plugin must remain a mutable library");
  const auto& old_class =
      Class::Handle(zone, old_library.LookupClass(String::Handle(
                              zone, Symbols::New(thread, "Plugin"))));
  Expect(!old_class.IsNull(), "native baseline plugin class missing");
  Expect(Library::LookupLibrary(thread, Symbols::vm_ffi_native_assets()) ==
             Library::null(),
         "metadata carrier was published as a native application library");
  const auto& asset =
      String::Handle(zone, Symbols::New(thread, "product-release-asset"));
  const auto& symbol =
      String::Handle(zone, Symbols::New(thread, "ProductAssetOnly"));
  char* error = nullptr;
  const auto target = FfiResolveInternal(asset, symbol, 1, &error);
  Expect(
      error == nullptr && target == reinterpret_cast<intptr_t>(dlsym(
                                        fixture_library, "ProductAssetOnly")),
      "released native asset failed before full publication");
  {
    TransitionVMToNative to_native(thread);
    Check(Dart_ReloadBytecodePatch(bytes.data(), bytes.size()));
    const auto value = Invoke(Dart_RootLibrary(), "probe");
    Expect(Integer(value) == expected_plugin_value,
           "full package plugin used the wrong base/update implementation");
  }
  Expect(map.ptr() == store->native_assets_map(),
         "full publication replaced the released native-assets map");
  InspectReleaseMap(asset_path);
  const auto& current_library =
      Library::Handle(zone, Library::LookupLibrary(thread, uri));
  Expect(
      !current_library.IsNull() && current_library.ptr() != old_library.ptr(),
      "full publication reused package plugin library identity");
  const auto& current_class =
      Class::Handle(zone, current_library.LookupClass(String::Handle(
                              zone, Symbols::New(thread, "Plugin"))));
  Expect(!current_class.IsNull() && current_class.ptr() != old_class.ptr(),
         "full publication reused package plugin class identity");
  Expect(Library::LookupLibrary(thread, Symbols::vm_ffi_native_assets()) ==
             Library::null(),
         "full publication exposed native-assets metadata carrier");
  error = nullptr;
  Expect(FfiResolveInternal(asset, symbol, 1, &error) == target &&
             error == nullptr,
         "released native-asset resolution changed after full publication");
  Expect(asset_open_calls == 2 && asset_symbol_calls == 2,
         "full map test did not use the released native-assets route twice");
  std::cout << "Full base/update preserves native-assets map identity/content; "
               "metadata carrier absent; package plugin identities fresh"
            << std::endl;
}
}  // namespace

int main(int argc, char** argv) {
  const bool full_assets =
      argc == 7 && std::strcmp(argv[5], "--full-assets") == 0;
  const bool ordinary_assets =
      argc == 4 && std::strcmp(argv[3], "--assets-baseline") == 0;
  if (argc != 3 && argc != 10 && !full_assets && !ordinary_assets) return 2;
  Expect(Dart_IsPrecompiledRuntime(),
         "PRODUCT test runtime must be precompiled");
  char* error = nullptr;
  Expect(DartEngine_Init(&error),
         error == nullptr ? "embedder init failed" : error);
  auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  Expect(error == nullptr, error == nullptr ? "" : error);
  Dart_InitializeParams params = {};
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  error = Dart_Initialize(&params);
  Expect(error == nullptr, error == nullptr ? "" : error);
  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  auto isolate = Dart_CreateIsolateGroup(
      snapshot.script_uri, "product-ffi-test", snapshot.snapshot_data,
      snapshot.snapshot_text, &flags, nullptr, nullptr, &error);
  Expect(error == nullptr && isolate != nullptr,
         error == nullptr ? "isolate creation failed" : error);
  released_asset_path = ordinary_assets             ? argv[2]
                        : argc == 10 || full_assets ? argv[3]
                                                    : nullptr;
  NativeAssetsApi assets = {};
  assets.dlopen_absolute = OpenReleasedAsset;
  assets.dlsym = AssetSymbol;
  assets.dlclose = CloseAsset;
  Dart_InitializeNativeAssetsResolver(&assets);
  Dart_EnterScope();
  if (!full_assets) {
    Check(dart::bin::DartUtils::SetupCoreLibraries(
        false, false, false, dart::bin::DartIoSettings{}));
  }
  // Bytecode instructions retain this external storage until group shutdown.
  std::vector<uint8_t> bytes;
  if (!full_assets && !ordinary_assets) InspectBaseline();
  if (ordinary_assets) {
    InspectOrdinaryAssets(argv[2]);
  } else if (argc == 3) {
    Expect(std::strcmp(argv[2], "--baseline") == 0, "unknown test mode");
  } else {
    fixture_library = dlopen(argv[3], RTLD_NOW | RTLD_GLOBAL);
    Expect(fixture_library != nullptr, "cannot load signed native fixture");
    const auto mappings = ExecutableMappings();
    std::ifstream input(argv[2], std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
    Expect(!bytes.empty(), "empty bytecode fixture");
    if (full_assets) {
      RunFullAssets(bytes, argv[4], argv[3],
                    std::strtoll(argv[6], nullptr, 10));
      CompareExecutableMappings(mappings);
      Dart_ExitScope();
      Dart_ShutdownIsolate();
      DartEngine_Shutdown();
      dlclose(fixture_library);
      return 0;
    }
    Expect(dlsym(RTLD_DEFAULT, "Dart_ExecuteInternalCommand") != nullptr,
           "existing internal GC command is unavailable in PRODUCT");
    auto buffer = Dart_NewExternalTypedData(Dart_TypedData_kUint8, bytes.data(),
                                            bytes.size());
    Check(buffer);
    auto library = Dart_LoadLibraryFromBytecode(buffer);
    Check(library);
    Expect(Dart_IsLibrary(library),
           "loader did not return new bytecode library");
    auto lookup = Dart_LookupLibrary(Dart_NewStringFromCString(argv[4]));
    Check(lookup);
    Expect(Dart_IdentityEquals(library, lookup) &&
               !Dart_IdentityEquals(library, Dart_RootLibrary()),
           "fixture must be a new library rather than the native baseline");
    InspectLoaded(argv[4]);
    Check(Dart_SetFfiNativeResolver(library, Resolver));
    auto core = Dart_LookupLibrary(Dart_NewStringFromCString("dart:core"));
    Check(core);
    auto string_type = Dart_GetNonNullableType(
        core, Dart_NewStringFromCString("String"), 0, nullptr);
    Check(string_type);
    auto arguments =
        Dart_NewListOfTypeFilled(string_type, Dart_NewStringFromCString(""), 7);
    Check(arguments);
    const char* values[] = {argv[3], argv[5], argv[6], argv[7],
                            "1",     argv[8], argv[9]};
    for (int i = 0; i < 7; ++i)
      Check(Dart_ListSetAt(arguments, i, Dart_NewStringFromCString(values[i])));
    Invoke(library, "main", 1, &arguments);
    Expect(Integer(Invoke(library, "runResolverCache")) == 58,
           "resolver call result mismatch");
    Expect(Integer(Invoke(library, "runResolverCache")) == 58 &&
               resolved_add_calls == 1,
           "native function target was not cached after first resolution");
    Expect(Integer(Invoke(library, "runResolverAddressOf")) == 73,
           "Native.addressOf result mismatch");
    Expect(Integer(Invoke(library, "runResolverAddressOf")) == 73 &&
               resolved_add_calls == 2,
           "Native.addressOf target was not cached independently");
    Expect(Integer(Invoke(library, "runResolverErrors")) == 2 &&
               missing_calls == 2,
           "missing resolver target did not report/retry safely");
    Expect(Integer(Invoke(library, "runNativeAsset")) == 509,
           "released native-asset result mismatch");
    Expect(
        Integer(Invoke(library, "runNativeAsset")) == 509 &&
            asset_open_calls == 1 && asset_symbol_calls == 1,
        "released native asset did not resolve/cache through baseline mapping");
    Expect(Integer(Invoke(library, "runNativeAssetErrors")) == 2 &&
               asset_missing_calls == 2 && asset_open_calls == 3,
           "native-asset symbol errors did not report/retry safely");
    std::cout
        << "Released baseline native-assets positive/cache/error route passed"
        << std::endl;
    CompareExecutableMappings(mappings);
    std::cout << "PRODUCT actual interpreted call-out/resolver/cache/error "
                 "checks passed"
              << std::endl;
  }
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  DartEngine_Shutdown();
  // Keep the fixture loaded until isolate shutdown has released every pointer.
  if (fixture_library != nullptr) dlclose(fixture_library);
  return 0;
}
