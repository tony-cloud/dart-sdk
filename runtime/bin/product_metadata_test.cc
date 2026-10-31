// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Internal embedder test. This executable links the shipping PRODUCT runtime;
// none of its inspection/mutation helpers are exported by the VM.
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

#include "include/dart_api.h"
#include "include/dart_engine.h"
#include "vm/canonical_tables.h"
#include "vm/compiler/ffi/native_type.h"
#include "vm/hash_table.h"
#include "vm/heap/safepoint.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#include "vm/thread_state.h"

#if !defined(PRODUCT) || !defined(DART_PRECOMPILED_RUNTIME)
#error "This test must link the PRODUCT precompiled runtime."
#endif

namespace dart {

static void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << std::endl;
    std::exit(1);
  }
}

static void Check(Dart_Handle result) {
  Expect(!Dart_IsError(result),
         Dart_IsError(result) ? Dart_GetError(result) : "");
}

static void Inspect(bool installed,
                    bool ordinary,
                    const char* full_value = nullptr) {
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  auto* zone = thread->zone();
  const auto& root = Library::Handle(
      zone, thread->isolate_group()->object_store()->root_library());
  const auto& pragma_class = Class::Handle(
      zone, thread->isolate_group()->object_store()->pragma_class());
  if (ordinary) {
    Expect(pragma_class.IsNull(), "ordinary AOT retained pragma identity");
    Expect(root.ptr()->untag()->metadata() == Array::null(),
           "ordinary AOT retained application metadata");
    return;
  }
  Expect(!pragma_class.IsNull(), "frozen pragma class missing");
  const auto& core = Library::Handle(zone, Library::CoreLibrary());
  Expect(pragma_class.ptr() == core.LookupClass(Symbols::Pragma()),
         "pragma identity is not the frozen core class");
  Expect(!Field::Handle(zone, pragma_class.LookupField(Symbols::name()))
                 .IsNull() &&
             !Field::Handle(zone, pragma_class.LookupField(Symbols::options()))
                  .IsNull(),
         "pragma field descriptors missing");
  const auto& old_function =
      Function::Handle(zone, root.LookupFunctionAllowPrivate(String::Handle(
                                 zone, Symbols::New(thread, "oldAnnotated"))));
  Expect(!old_function.IsNull(), "baseline function missing");
  if (!installed) {
    Expect(Array::Handle(zone, Array::RawCast(root.GetMetadata(old_function)))
                   .Length() == 0,
           "baseline application metadata must be dropped");
  }

  const auto& ffi = Library::Handle(zone, Library::FfiLibrary());
  const auto& intptr_class = Class::Handle(
      zone,
      ffi.LookupClass(String::Handle(zone, Symbols::New(thread, "IntPtr"))));
  auto& options = Object::Handle(zone);
  Library::FindPragma(thread, false, intptr_class,
                      Symbols::vm_ffi_abi_specific_mapping(), true, &options);
  Expect(options.IsGrowableObjectArray() &&
             GrowableObjectArray::Cast(options).Length() == 1,
         "frozen IntPtr ABI metadata missing");
  const auto& intptr_type = Type::Handle(zone, intptr_class.DeclarationType());
  const char* error = nullptr;
  const auto* native_int =
      compiler::ffi::NativeType::FromAbstractType(zone, intptr_type, &error);
  Expect(error == nullptr && native_int != nullptr && native_int->IsInt() &&
             native_int->IsSigned() && native_int->SizeInBytes() == kWordSize,
         "PRODUCT IntPtr conversion failed");
  if (!installed) return;

  options = Object::null();
  Expect(Library::FindPragma(
             thread, false, old_function,
             String::Handle(zone, Symbols::New(thread, "test:metadata")), false,
             &options) &&
             options.IsString() &&
             String::Cast(options).Equals(full_value == nullptr ? "baseline"
                                                                : full_value),
         "installed baseline declaration metadata has the wrong value");

  const auto& function =
      Function::Handle(zone, root.LookupFunctionAllowPrivate(String::Handle(
                                 zone, Symbols::New(thread, "newAnnotated"))));
  const auto& cls =
      Class::Handle(zone, root.LookupClass(String::Handle(
                              zone, Symbols::New(thread, "Annotated"))));
  Expect(!function.IsNull() && !cls.IsNull(), "bytecode declarations missing");
  const auto& field = Field::Handle(
      zone,
      cls.LookupField(String::Handle(zone, Symbols::New(thread, "value"))));
  Expect(!field.IsNull(), "bytecode field missing");
  for (const Object* declaration :
       {static_cast<const Object*>(&function), static_cast<const Object*>(&cls),
        static_cast<const Object*>(&field)}) {
    options = Object::null();
    Expect(Library::FindPragma(
               thread, false, *declaration,
               String::Handle(zone, Symbols::New(thread, "test:metadata")),
               false, &options) &&
               options.IsString() &&
               String::Cast(options).Equals(full_value == nullptr ? "update"
                                                                  : full_value),
           "evaluated function/class/field pragma missing");
    const auto& metadata =
        Array::Handle(zone, Array::RawCast(root.GetMetadata(*declaration)));
    bool actual_pragma = false;
    for (intptr_t i = 0; i < metadata.Length(); ++i) {
      if (Object::Handle(zone, metadata.At(i)).clazz() == pragma_class.ptr())
        actual_pragma = true;
    }
    Expect(actual_pragma, "bytecode pragma did not use frozen SDK identity");
    Expect(!Library::FindPragma(
               thread, false, *declaration,
               String::Handle(zone, Symbols::New(thread, "test:spoof"))),
           "non-pragma annotation spoofed pragma identity");
  }
  if (full_value != nullptr) return;

  const auto& metadata =
      Array::Handle(zone, Array::RawCast(root.GetMetadata(function)));
  const auto& clone = Function::Handle(
      zone,
      Function::New(
          FunctionType::Handle(zone, function.signature()),
          String::Handle(zone, Symbols::New(thread, "metadataOnlyNative")),
          UntaggedFunction::kRegularFunction, true, false, false, true, true,
          Class::Handle(zone, function.Owner()), TokenPosition::kNoSource));
  clone.set_has_pragma(true);
  {
    SafepointWriteRwLocker lock(thread,
                                thread->isolate_group()->program_lock());
    root.AddMetadata(clone, metadata);
  }
  const auto& annotation = Instance::Handle(zone, clone.GetNativeAnnotation());
  Expect(annotation.clazz() == ffi.LookupClass(Symbols::FfiNative()),
         "native annotation is not the frozen dart:ffi class");
  Expect(clone.FfiIsLeaf(), "native leaf annotation missing");
  const auto& signature = FunctionType::Handle(zone, clone.FfiCSignature());
  error = nullptr;
  const auto* native_signature =
      compiler::ffi::NativeFunctionTypeFromFunctionType(zone, signature,
                                                        &error);
  Expect(error == nullptr && native_signature != nullptr &&
             native_signature->argument_types().length() == 1 &&
             native_signature->argument_types().At(0)->IsInt() &&
             native_signature->argument_types().At(0)->SizeInBytes() ==
                 kWordSize &&
             native_signature->return_type().IsInt() &&
             native_signature->return_type().SizeInBytes() == kWordSize,
         "PRODUCT native FunctionType conversion failed");

  // This real transformed asFunction closure exists only in the bytecode
  // update. Inspect its SDK _FfiCall constant without invoking native code.
  auto& ffi_closure = Function::Handle(zone);
  {
    MetadataMap map(root.ptr()->untag()->metadata());
    MetadataMap::Iterator it(&map);
    auto& declaration = Object::Handle(zone);
    while (it.MoveNext()) {
      declaration = map.GetKey(it.Current());
      if (declaration.IsFunction() &&
          Function::Cast(declaration).IsFfiCallClosure()) {
        ffi_closure ^= declaration.ptr();
        break;
      }
    }
    map.Release();
  }
  Expect(!ffi_closure.IsNull(),
         "first-use asFunction closure metadata missing");
  const auto& as_function = Function::Handle(
      zone, root.LookupFunctionAllowPrivate(
                String::Handle(zone, Symbols::New(thread, "firstAsFunction"))));
  Expect(
      !as_function.IsNull() &&
          ffi_closure.parent_function() == as_function.ptr(),
      "asFunction metadata does not belong to the published new declaration");
  const auto& call_annotation =
      Instance::Handle(zone, ffi_closure.GetFfiCallClosurePragmaValue());
  Expect(
      call_annotation.clazz() == ffi.LookupClassAllowPrivate(String::Handle(
                                     zone, Symbols::New(thread, "_FfiCall"))),
      "asFunction annotation is not the frozen SDK _FfiCall class");
  Expect(ffi_closure.FfiIsLeaf(), "asFunction leaf metadata missing");
  const auto& call_signature =
      FunctionType::Handle(zone, ffi_closure.FfiCSignature());
  error = nullptr;
  const auto* call_native_signature =
      compiler::ffi::NativeFunctionTypeFromFunctionType(zone, call_signature,
                                                        &error);
  Expect(error == nullptr && call_native_signature != nullptr &&
             call_native_signature->argument_types().length() == 1 &&
             call_native_signature->argument_types().At(0)->IsInt() &&
             call_native_signature->argument_types().At(0)->SizeInBytes() ==
                 kWordSize &&
             call_native_signature->return_type().IsInt() &&
             call_native_signature->return_type().SizeInBytes() == kWordSize,
         "PRODUCT asFunction FunctionType conversion failed");

  // Exercise defensive lookup with internal malformed data. Runtime insertion
  // accepts evaluated arrays only; direct map insertion simulates a stale
  // kernel offset from a snapshot and must never enter the kernel evaluator.
  auto& invalid = Object::Handle(zone);
  for (intptr_t i = 0; i < 3; ++i) {
    invalid = i == 0   ? Object::null()
              : i == 1 ? static_cast<ObjectPtr>(Smi::New(123))
                       : static_cast<ObjectPtr>(String::New("malformed"));
    {
      SafepointWriteRwLocker lock(thread,
                                  thread->isolate_group()->program_lock());
      MetadataMap map(root.ptr()->untag()->metadata());
      map.UpdateOrInsert(clone, invalid);
      root.ptr()->untag()->set_metadata(map.Release().ptr());
    }
    Expect(!Library::FindPragma(thread, false, clone, Symbols::vm_ffi_native()),
           "invalid metadata was accepted as a pragma");
  }
  const auto& evaluation_error = LanguageError::Handle(
      zone,
      LanguageError::New(String::Handle(zone, String::New("metadata error"))));
  {
    SafepointWriteRwLocker lock(thread,
                                thread->isolate_group()->program_lock());
    root.AddMetadata(clone, evaluation_error);
  }
  Expect(
      root.GetMetadata(clone) == evaluation_error.ptr() &&
          !Library::FindPragma(thread, false, clone, Symbols::vm_ffi_native()),
      "evaluated metadata error was not preserved/ignored");
  Expect(!FindPragmaInMetadata(thread, String::Handle(zone, String::New("bad")),
                               Symbols::vm_ffi_native()),
         "malformed internal metadata was accepted");
}

// Host callbacks are registered and inspected, never invoked.
static int resolver_calls = 0;
static Dart_NativeFunction NativeResolverA(Dart_Handle, int, bool*) {
  resolver_calls += 1;
  return nullptr;
}
static Dart_NativeFunction NativeResolverB(Dart_Handle, int, bool*) {
  resolver_calls += 2;
  return nullptr;
}
static const uint8_t* NativeSymbolA(Dart_NativeFunction) {
  resolver_calls += 4;
  return nullptr;
}
static const uint8_t* NativeSymbolB(Dart_NativeFunction) {
  resolver_calls += 8;
  return nullptr;
}
static void* FfiResolverA(const char*, uintptr_t) {
  resolver_calls += 16;
  return nullptr;
}
static void* FfiResolverB(const char*, uintptr_t) {
  resolver_calls += 32;
  return nullptr;
}

static void RegisterResolvers(Dart_Handle library, bool alternative) {
  Check(Dart_SetNativeResolver(library,
                               alternative ? NativeResolverB : NativeResolverA,
                               alternative ? NativeSymbolB : NativeSymbolA));
  Check(Dart_SetFfiNativeResolver(library,
                                  alternative ? FfiResolverB : FfiResolverA));
}

static void InspectResolvers(Dart_Handle library,
                             bool registered,
                             bool alternative = false) {
  Dart_NativeEntryResolver native = nullptr;
  Dart_NativeEntrySymbol symbol = nullptr;
  Check(Dart_GetNativeResolver(library, &native));
  Check(Dart_GetNativeSymbol(library, &symbol));
  Expect(
      native == (registered ? (alternative ? NativeResolverB : NativeResolverA)
                            : nullptr),
      "native resolver address changed");
  Expect(symbol == (registered ? (alternative ? NativeSymbolB : NativeSymbolA)
                               : nullptr),
         "native symbol resolver address changed");
  auto url = Dart_LibraryUrl(library);
  Check(url);
  const char* uri = nullptr;
  Check(Dart_StringToCString(url, &uri));
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  const auto& lib = Library::Handle(
      thread->zone(),
      Library::LookupLibrary(thread,
                             String::Handle(thread->zone(), String::New(uri))));
  Expect(
      lib.ffi_native_resolver() ==
          (registered ? (alternative ? FfiResolverB : FfiResolverA) : nullptr),
      "FFI resolver address changed");
}

static void RejectResolvers(Dart_Handle library) {
  Dart_NativeEntryResolver native = NativeResolverB;
  Dart_NativeEntrySymbol symbol = NativeSymbolB;
  Expect(Dart_IsError(Dart_GetNativeResolver(library, &native)) &&
             native == nullptr,
         "inactive native getter accepted or leaked a callback");
  Expect(
      Dart_IsError(Dart_GetNativeSymbol(library, &symbol)) && symbol == nullptr,
      "inactive symbol getter accepted or leaked a callback");
  Expect(Dart_IsError(
             Dart_SetNativeResolver(library, NativeResolverB, NativeSymbolB)),
         "inactive native setter accepted");
  Expect(Dart_IsError(Dart_SetFfiNativeResolver(library, FfiResolverB)),
         "inactive FFI setter accepted");
}

static void SetReplacementTestState(intptr_t value) {
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  StackZone stack_zone(thread);
  thread->isolate_group()
      ->object_store()
      ->set_shorebird_replacement_installation_state(Smi::Handle(
          thread->zone(), value == 0 ? Smi::null() : Smi::New(value)));
}

static void ExpectPoisoned() {
  auto* thread = Thread::Current();
  TransitionNativeToVM transition(thread);
  Expect(thread->isolate_group()
                 ->object_store()
                 ->shorebird_replacement_installation_state() == Smi::New(1),
         "malformed fixture did not reach permanent identity staging");
}

static int RunResolverTest(int argc, char** argv) {
  Expect(argc == 8, "resolver test arguments missing");
  const bool base = std::strcmp(argv[7], "base") == 0;
  const bool poison = std::strcmp(argv[7], "poison") == 0;
  char* error = nullptr;
  Expect(DartEngine_Init(&error),
         error == nullptr ? "embedder init failed" : error);
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  Expect(error == nullptr, error == nullptr ? "" : error);
  Dart_InitializeParams params = {};
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  error = Dart_Initialize(&params);
  Expect(error == nullptr, error == nullptr ? "" : error);
  std::ifstream input(argv[2], std::ios::binary);
  const std::vector<uint8_t> payload((std::istreambuf_iterator<char>(input)),
                                     {});
  Expect(!payload.empty(), "empty resolver payload");
  // A second fresh group must start with null callbacks even though the first
  // group registered and subsequently overrode resolvers for these same URIs.
  for (intptr_t group = 0; group < 2; ++group) {
    Dart_IsolateFlags flags;
    Dart_IsolateFlagsInitialize(&flags);
    auto isolate = Dart_CreateIsolateGroup(
        snapshot.script_uri, "resolver-test", snapshot.snapshot_data,
        snapshot.snapshot_text, &flags, nullptr, nullptr, &error);
    Expect(error == nullptr && isolate != nullptr,
           error == nullptr ? "isolate creation failed" : error);
    Dart_EnterScope();
    auto root = Dart_RootLibrary();
    auto shared = Dart_LookupLibrary(Dart_NewStringFromCString(argv[4]));
    auto removed = Dart_LookupLibrary(Dart_NewStringFromCString(argv[5]));
    Check(shared);
    Check(removed);
    InspectResolvers(root, false);
    InspectResolvers(shared, false);
    InspectResolvers(removed, false);
    if (group == 0) {
      RegisterResolvers(root, false);
      RegisterResolvers(removed, true);
    }
    InspectResolvers(root, group == 0);
    InspectResolvers(removed, group == 0, true);
    // Simulate the synchronous staging interval without adding an embedder
    // callback to the loader; the malformed mode below leaves real poison.
    SetReplacementTestState(1);
    RejectResolvers(root);
    auto core = Dart_LookupLibrary(Dart_NewStringFromCString("dart:core"));
    Check(core);
    Dart_NativeEntryResolver core_resolver = nullptr;
    Check(Dart_GetNativeResolver(core, &core_resolver));
    SetReplacementTestState(3);
    RejectResolvers(root);
    SetReplacementTestState(0);
    InspectResolvers(root, group == 0);
    auto retired_root = Dart_NewPersistentHandle(root);
    auto retired_removed = Dart_NewPersistentHandle(removed);
    auto result = Dart_ReloadBytecodePatch(payload.data(), payload.size());
    if (poison) {
      Expect(Dart_IsError(result), "malformed resolver payload was accepted");
      ExpectPoisoned();
      RejectResolvers(Dart_HandleFromPersistent(retired_root));
      RejectResolvers(Dart_HandleFromPersistent(retired_removed));
      Check(Dart_GetNativeResolver(core, &core_resolver));
    } else {
      Check(result);
      root = Dart_RootLibrary();
      Expect(
          !Dart_IdentityEquals(root, Dart_HandleFromPersistent(retired_root)),
          "replacement reused baseline library identity");
      InspectResolvers(root, group == 0);
      shared = Dart_LookupLibrary(Dart_NewStringFromCString(argv[4]));
      Check(shared);
      InspectResolvers(shared, false);
      removed = Dart_LookupLibrary(Dart_NewStringFromCString(argv[5]));
      if (base) {
        Check(removed);
        InspectResolvers(removed, group == 0, true);
      } else {
        Expect(Dart_IsError(removed), "removed URI survived publication");
        auto added = Dart_LookupLibrary(Dart_NewStringFromCString(argv[6]));
        Check(added);
        InspectResolvers(added, false);
      }
      RejectResolvers(Dart_HandleFromPersistent(retired_root));
      RejectResolvers(Dart_HandleFromPersistent(retired_removed));
      Dart_ExitScope();
      Dart_ExitIsolate();
      error = Dart_IsolateMakeRunnable(isolate);
      Expect(error == nullptr, error == nullptr ? "" : error);
      Dart_EnterIsolate(isolate);
      Dart_EnterScope();
      root = Dart_RootLibrary();
      RegisterResolvers(root, true);
      InspectResolvers(root, true, true);
      RejectResolvers(Dart_HandleFromPersistent(retired_root));
      InspectResolvers(root, true, true);
    }
    Expect(Dart_IsError(Dart_GetNativeResolver(Dart_Null(), nullptr)) &&
               Dart_IsError(Dart_GetNativeSymbol(Dart_Null(), nullptr)),
           "getter null output validation changed");
    Dart_DeletePersistentHandle(retired_root);
    Dart_DeletePersistentHandle(retired_removed);
    Dart_ExitScope();
    Dart_ShutdownIsolate();
  }
  Expect(resolver_calls == 0, "resolver callback was unexpectedly executed");
  DartEngine_Shutdown();
  error = Dart_Cleanup();
  Expect(error == nullptr, error == nullptr ? "" : error);
  std::cout << "PRODUCT host resolver " << argv[7] << " checks passed"
            << std::endl;
  return 0;
}

}  // namespace dart

int main(int argc, char** argv) {
  if (argc >= 4 && std::strcmp(argv[3], "--resolvers") == 0) {
    return dart::RunResolverTest(argc, argv);
  }
  if (argc != 3 && argc != 4) return 2;
  dart::Expect(Dart_IsPrecompiledRuntime(), "test runtime must be precompiled");
  const bool ordinary = std::strcmp(argv[2], "--ordinary") == 0;
  const bool baseline_only = std::strcmp(argv[2], "--baseline") == 0;
  const bool reject = argc == 4 && std::strcmp(argv[3], "--reject") == 0;
  const char* full_value =
      argc == 4 && std::strcmp(argv[3], "--full-base") == 0     ? "baseline"
      : argc == 4 && std::strcmp(argv[3], "--full-update") == 0 ? "update"
                                                                : nullptr;
  char* error = nullptr;
  dart::Expect(DartEngine_Init(&error),
               error == nullptr ? "embedder init failed" : error);
  auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  dart::Expect(error == nullptr, error == nullptr ? "" : error);
  Dart_InitializeParams params = {};
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  error = Dart_Initialize(&params);
  dart::Expect(error == nullptr, error == nullptr ? "" : error);
  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  auto isolate = Dart_CreateIsolateGroup(
      snapshot.script_uri, "metadata-test", snapshot.snapshot_data,
      snapshot.snapshot_text, &flags, nullptr, nullptr, &error);
  dart::Expect(error == nullptr, error == nullptr ? "" : error);
  dart::Expect(isolate != nullptr, "isolate creation failed");
  Dart_EnterScope();
  dart::Inspect(false, ordinary);
  auto retired_root = Dart_NewPersistentHandle(Dart_RootLibrary());
  if (!ordinary && !baseline_only) {
    std::ifstream input(argv[2], std::ios::binary);
    std::vector<uint8_t> patch((std::istreambuf_iterator<char>(input)), {});
    auto result = Dart_ReloadBytecodePatch(patch.data(), patch.size());
    if (reject) {
      dart::Expect(
          Dart_IsError(result) &&
              std::strstr(Dart_GetError(result),
                          "annotations must be an evaluated array") != nullptr,
          "malformed annotation container was not rejected");
    } else {
      dart::Check(result);
      if (full_value != nullptr) {
        dart::Expect(
            !Dart_IdentityEquals(Dart_RootLibrary(),
                                 Dart_HandleFromPersistent(retired_root)),
            "full replacement reused baseline library identity");
      }
      dart::Inspect(true, false, full_value);
    }
  }
  Dart_DeletePersistentHandle(retired_root);
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  DartEngine_Shutdown();
  std::cout << "PRODUCT evaluated metadata checks passed" << std::endl;
  return 0;
}
