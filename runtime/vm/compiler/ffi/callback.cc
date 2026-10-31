// Copyright (c) 2020, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/ffi/callback.h"

#include "vm/canonical_tables.h"
#include "vm/ffi/callback_function.h"
#include "vm/object_store.h"

namespace dart {

namespace compiler {

namespace ffi {

FunctionPtr NativeCallbackFunction(const FunctionType& c_signature,
                                   const Function& dart_target,
                                   const Instance& exceptional_return,
                                   FfiCallbackKind kind) {
  Thread* const thread = Thread::Current();
  Zone* const zone = thread->zone();
  Function& function =
      Function::Handle(zone, NewFfiCallbackFunction(c_signature, dart_target,
                                                    exceptional_return, kind));

  {
    // Ensure only one thread updates the cache of deduped ffi trampoline
    // functions.
    auto isolate_group = thread->isolate_group();
    SafepointWriteRwLocker ml(thread, isolate_group->program_lock());

    auto object_store = isolate_group->object_store();
    if (object_store->ffi_callback_functions() == Array::null()) {
      FfiCallbackFunctionSet set(
          HashTables::New<FfiCallbackFunctionSet>(/*initial_capacity=*/4));
      object_store->set_ffi_callback_functions(set.Release());
    }
    FfiCallbackFunctionSet set(object_store->ffi_callback_functions());

    const intptr_t entry_count_before = set.NumOccupied();
    function ^= set.InsertOrGet(function);
    const intptr_t entry_count_after = set.NumOccupied();

    object_store->set_ffi_callback_functions(set.Release());

    if (entry_count_before != entry_count_after) {
      function.AssignFfiCallbackId(entry_count_before);
    } else {
      ASSERT(function.FfiCallbackId() != -1);
    }
  }

  return function.ptr();
}

static void EnsureFfiCallbackMetadata(Thread* thread, intptr_t callback_id) {
  static constexpr intptr_t kInitialCallbackIdsReserved = 16;

  auto object_store = thread->isolate_group()->object_store();
  auto zone = thread->zone();

  auto& code_array =
      GrowableObjectArray::Handle(zone, object_store->ffi_callback_code());
  if (code_array.IsNull()) {
    code_array =
        GrowableObjectArray::New(kInitialCallbackIdsReserved, Heap::kOld);
    object_store->set_ffi_callback_code(code_array);
  }
  if (code_array.Length() <= callback_id) {
    // Ensure we've enough space in the arrays.
    while (!(callback_id < code_array.Length())) {
      code_array.Add(Code::null_object());
    }
  }

  ASSERT(callback_id < code_array.Length());
}

void SetFfiCallbackCode(Thread* thread,
                        const Function& ffi_trampoline,
                        const Code& code) {
  auto zone = thread->zone();

  const intptr_t callback_id = ffi_trampoline.FfiCallbackId();
  EnsureFfiCallbackMetadata(thread, callback_id);

  auto object_store = thread->isolate_group()->object_store();
  const auto& code_array =
      GrowableObjectArray::Handle(zone, object_store->ffi_callback_code());
  code_array.SetAt(callback_id, code);
}

}  // namespace ffi

}  // namespace compiler

}  // namespace dart
