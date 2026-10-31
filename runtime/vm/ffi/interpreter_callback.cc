// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/ffi/interpreter_callback.h"

#include <cstddef>
#include <cstring>

#if defined(HOST_ARCH_ARM64E)
#include <ptrauth.h>
#endif

#include "platform/utils.h"
#include "vm/dart_api_state.h"
#include "vm/dart_entry.h"
#include "vm/ffi/interpreter_call_plan.h"
#include "vm/ffi_callback_metadata.h"
#include "vm/handles.h"
#include "vm/longjump.h"
#include "vm/object_store.h"
#include "vm/runtime_entry.h"
#include "vm/simulator.h"
#include "vm/thread.h"
#include "vm/zone.h"

#if defined(DART_PRECOMPILED_RUNTIME) && defined(DART_BYTECODE_INTERPRETER) && \
    defined(DART_SHOREBIRD_INTERPRETER) && defined(HOST_ARCH_ARM64) &&         \
    defined(TARGET_ARCH_ARM64) && !defined(DART_HOST_OS_WINDOWS) &&            \
    !defined(DART_TARGET_OS_WINDOWS)
#define INTERPRETER_FFI_CALLBACK 1
#endif

namespace dart {

static_assert(sizeof(InterpreterFfiCallbackNativeResult) == 48);
static_assert(offsetof(InterpreterFfiCallbackNativeResult, cpu) == 0);
static_assert(offsetof(InterpreterFfiCallbackNativeResult, fpu) == 16);
static_assert(sizeof(InterpreterFfiCallbackFallback) == 64);
static_assert(offsetof(InterpreterFfiCallbackFallback, registers) == 0);
static_assert(offsetof(InterpreterFfiCallbackFallback, indirect_result_size) ==
              48);
static_assert(offsetof(InterpreterFfiCallbackFallback, flags) == 56);
#if defined(HOST_ARCH_ARM64) && defined(TARGET_ARCH_ARM64)
// Keep in sync with the release-linked assembly, including the caller's
// six-word trampoline frame between our entry SP and the native arguments.
static_assert(offsetof(InterpreterFfiCallbackFrame, cpu) == 0);
static_assert(offsetof(InterpreterFfiCallbackFrame, fpu) == 72);
static_assert(offsetof(InterpreterFfiCallbackFrame, stack_args) == 136);
static_assert(offsetof(InterpreterFfiCallbackFrame, result) == 144);
static_assert(offsetof(InterpreterFfiCallbackFrame, result) +
                  offsetof(InterpreterFfiCallbackNativeResult, fpu) ==
              160);
static_assert(sizeof(InterpreterFfiCallbackFrame) == 192);
static_assert(alignof(InterpreterFfiCallbackFrame) == 16);
static_assert(FfiCallbackMetadata::kNativeCallbackTrampolineStackDelta == 6);
#endif

bool InterpreterFfiCallbacksSupported() {
#if defined(INTERPRETER_FFI_CALLBACK)
#if defined(DART_INCLUDE_SIMULATOR)
  if (FLAG_use_simulator) return false;
#endif
  return true;
#else
  return false;
#endif
}

uword InterpreterFfiCallbackEntryPoint() {
#if defined(INTERPRETER_FFI_CALLBACK)
  if (!InterpreterFfiCallbacksSupported()) return 0;
  auto entry = &InterpreterFfiCallbackEntry;
#if defined(HOST_ARCH_ARM64E)
  entry = reinterpret_cast<decltype(entry)>(
      ptrauth_strip(entry, ptrauth_key_function_pointer));
#endif
  return reinterpret_cast<uword>(entry);
#else
  return 0;
#endif
}

bool IsInterpreterFfiCallbackEntry(uword entry_point) {
  return entry_point != 0 && entry_point == InterpreterFfiCallbackEntryPoint();
}

#if defined(INTERPRETER_FFI_CALLBACK)
namespace {

using compiler::ffi::NativeLocation;
using compiler::ffi::PrimitiveType;

int64_t TruncateInteger(uint64_t value, PrimitiveType type) {
  switch (type) {
    case compiler::ffi::kInt8:
      return static_cast<int8_t>(value);
    case compiler::ffi::kUint8:
      return static_cast<uint8_t>(value);
    case compiler::ffi::kInt16:
      return static_cast<int16_t>(value);
    case compiler::ffi::kUint16:
      return static_cast<uint16_t>(value);
    case compiler::ffi::kInt32:
      return static_cast<int32_t>(value);
    case compiler::ffi::kUint32:
      return static_cast<uint32_t>(value);
    case compiler::ffi::kInt64:
    case compiler::ffi::kUint64:
      return static_cast<int64_t>(value);
    default:
      UNREACHABLE();
  }
}

bool IsPointer(Zone* zone, const InterpreterFfiCallPlan& plan, intptr_t index) {
  return AbstractType::Handle(zone, plan.CType(index)).type_class_id() ==
         kPointerCid;
}

bool IsSupportedScalarLocation(const NativeLocation& location, bool is_result) {
  const auto& type = location.payload_type();
  if (!type.IsPrimitive() || type.SizeInBytes() > kWordSize) {
    return false;
  }
  if (type.IsVoid()) return is_result;
  const auto primitive = type.AsPrimitive().representation();
  if (!(type.IsInt() || primitive == compiler::ffi::kFloat ||
        primitive == compiler::ffi::kDouble)) {
    return false;
  }
  // The native classifier can express larger or composite containers. The
  // capture/reader below deliberately implements only scalar containers.
  if (!location.container_type().IsPrimitive() ||
      location.container_type().SizeInBytes() > kWordSize) {
    return false;
  }
  if (location.IsRegisters()) {
    const auto& regs = location.AsRegisters();
    return regs.num_regs() == 1 && regs.reg_at(0) >= 0 &&
           regs.reg_at(0) < (is_result ? 2 : 9);
  }
  if (location.IsFpuRegisters()) {
    const auto& regs = location.AsFpuRegisters();
    return regs.fpu_reg_kind() == compiler::ffi::kQuadFpuReg &&
           regs.fpu_reg() >= 0 && regs.fpu_reg() < (is_result ? 4 : 8);
  }
  return !is_result && location.IsStack() &&
         location.AsStack().offset_in_bytes() >= 0;
}

// Aggregate chunks are byte sequences, including 3/5/6/7-byte tails. They
// must not go through scalar integer extension or rounded container copies.
bool IsSupportedCompoundLocation(const NativeLocation& location,
                                 bool is_result) {
  const intptr_t size = location.payload_type().SizeInBytes();
  if (size <= 0) return false;
  if (location.IsMultiple()) {
    intptr_t offset = 0;
    for (const auto* part : location.AsMultiple().locations()) {
      const intptr_t part_size = part->payload_type().SizeInBytes();
      if (part->IsPointerToMemory() || part_size > size - offset ||
          !IsSupportedCompoundLocation(*part, is_result)) {
        return false;
      }
      offset += part_size;
    }
    return offset == size;
  }
  if (location.IsPointerToMemory()) {
    const auto& pointer = location.AsPointerToMemory().pointer_location();
    if (!pointer.payload_type().IsInt() ||
        pointer.payload_type().SizeInBytes() != kWordSize ||
        !IsSupportedScalarLocation(pointer, false)) {
      return false;
    }
    if (!is_result) return true;
    // ARM64's indirect result is supplied in captured x8. It does not belong
    // in the x0/x1 result bank restored by the shared callback trampoline.
    const auto& returned =
        location.AsPointerToMemory().pointer_return_location();
    return pointer.IsRegisters() &&
           pointer.AsRegisters().reg_at(0) ==
               CallingConventions::kPointerToReturnStructRegisterCall &&
           pointer.AsRegisters().reg_at(0) == 8 && returned.IsRegisters() &&
           returned.AsRegisters().num_regs() == 1 &&
           returned.AsRegisters().reg_at(0) == 8;
  }
  if (location.IsStack()) {
    const intptr_t offset = location.AsStack().offset_in_bytes();
    return !is_result && offset >= 0 && offset <= kIntptrMax - size;
  }
  if (!location.payload_type().IsPrimitive() || size > kWordSize) {
    return false;
  }
  if (location.IsRegisters()) {
    const auto& regs = location.AsRegisters();
    return regs.num_regs() == 1 && regs.reg_at(0) >= 0 &&
           regs.reg_at(0) < (is_result ? 2 : 9);
  }
  if (location.IsFpuRegisters()) {
    const auto& regs = location.AsFpuRegisters();
    return regs.fpu_reg_kind() == compiler::ffi::kQuadFpuReg &&
           regs.fpu_reg() >= 0 && regs.fpu_reg() < (is_result ? 4 : 8);
  }
  return false;
}

// Conversion failures keep the preencoded exceptional return. No allocation or
// managed throw is needed to turn a bad result into the native fallback.
bool EncodeResult(Zone* zone,
                  const InterpreterFfiCallPlan& plan,
                  const Object& value,
                  InterpreterFfiCallbackNativeResult* result,
                  bool exceptional) {
  memset(result, 0, sizeof(*result));
  constexpr intptr_t index = InterpreterFfiCallPlan::kResultIndex;
  if (plan.IsVoid(index)) return true;
  const auto& location = plan.Location(index);
  uint64_t bits;
  if (IsPointer(zone, plan, index)) {
    if (exceptional) return value.IsNull();
    if (!value.IsPointer()) return false;
    bits = Pointer::Cast(value).NativeAddress();
  } else if (plan.IsBool(index)) {
    if (!value.IsBool()) return false;
    bits = Bool::Cast(value).value() ? 1 : 0;
  } else {
    const auto& type = location.payload_type();
    const auto primitive = type.AsPrimitive().representation();
    if (type.IsInt()) {
      if (!value.IsInteger()) return false;
      bits = TruncateInteger(Integer::Cast(value).Value(), primitive);
    } else {
      if (!value.IsDouble()) return false;
      const double number = Double::Cast(value).value();
      bits = primitive == compiler::ffi::kFloat
                 ? bit_cast<uint32_t, float>(static_cast<float>(number))
                 : bit_cast<uint64_t, double>(number);
    }
  }
  if (location.IsRegisters()) {
    result->cpu[location.AsRegisters().reg_at(0)] = bits;
  } else {
    ASSERT(location.IsFpuRegisters());
    result->fpu[location.AsFpuRegisters().fpu_reg()] = bits;
  }
  return true;
}

uint64_t ReadArgument(const InterpreterFfiCallbackFrame& frame,
                      const NativeLocation& location) {
  if (location.IsRegisters()) {
    return frame.cpu[location.AsRegisters().reg_at(0)];
  }
  if (location.IsFpuRegisters()) {
    return frame.fpu[location.AsFpuRegisters().fpu_reg()];
  }
  ASSERT(location.IsStack());
  uint64_t bits = 0;
  // Darwin packs narrow arguments. Read only their ABI container, allowing
  // unaligned bytes and avoiding the callout plan's separate scratch space.
  memcpy(&bits,
         reinterpret_cast<const void*>(frame.stack_args +
                                       location.AsStack().offset_in_bytes()),
         location.container_type().SizeInBytes());
  return bits;
}

// Called only with native-owned storage while safepoints are disabled. The
// validated layout advances by payload sizes, even for a packed trailing chunk
// whose ABI container is a full word.
void CopyCompoundArgument(const InterpreterFfiCallbackFrame& frame,
                          const NativeLocation& location,
                          uint8_t* destination) {
  const intptr_t size = location.payload_type().SizeInBytes();
  if (location.IsMultiple()) {
    intptr_t offset = 0;
    for (const auto* part : location.AsMultiple().locations()) {
      CopyCompoundArgument(frame, *part, destination + offset);
      offset += part->payload_type().SizeInBytes();
    }
    ASSERT(offset == size);
  } else if (location.IsPointerToMemory()) {
    const uword source =
        ReadArgument(frame, location.AsPointerToMemory().pointer_location());
    memcpy(destination, reinterpret_cast<const void*>(source), size);
  } else if (location.IsStack()) {
    memcpy(destination,
           reinterpret_cast<const void*>(frame.stack_args +
                                         location.AsStack().offset_in_bytes()),
           size);
  } else if (location.IsRegisters()) {
    memcpy(destination, &frame.cpu[location.AsRegisters().reg_at(0)], size);
  } else {
    ASSERT(location.IsFpuRegisters());
    memcpy(destination, &frame.fpu[location.AsFpuRegisters().fpu_reg()], size);
  }
}

void CopyCompoundResult(const NativeLocation& location,
                        const uint8_t* source,
                        InterpreterFfiCallbackNativeResult* result) {
  const intptr_t size = location.payload_type().SizeInBytes();
  if (location.IsMultiple()) {
    intptr_t offset = 0;
    for (const auto* part : location.AsMultiple().locations()) {
      CopyCompoundResult(*part, source + offset, result);
      offset += part->payload_type().SizeInBytes();
    }
    ASSERT(offset == size);
  } else if (location.IsRegisters()) {
    memcpy(&result->cpu[location.AsRegisters().reg_at(0)], source, size);
  } else {
    ASSERT(location.IsFpuRegisters());
    memcpy(&result->fpu[location.AsFpuRegisters().fpu_reg()], source, size);
  }
}

ObjectPtr BoxCompoundArgument(Thread* thread,
                              const InterpreterFfiCallPlan& plan,
                              intptr_t index,
                              const uint8_t* snapshot) {
  Zone* const zone = thread->zone();
  const intptr_t size = plan.Location(index).payload_type().SizeInBytes();
  const auto& data =
      TypedData::Handle(zone, TypedData::New(kTypedDataUint8ArrayCid, size));
  {
    NoSafepointScope no_safepoint;
    memcpy(data.DataAddr(0), snapshot, size);
  }
  const auto& type = AbstractType::Handle(zone, plan.CType(index));
  const auto& cls = Class::Handle(zone, type.type_class());
  auto* const store = thread->isolate_group()->object_store();
  const auto& data_field =
      Field::Handle(zone, store->compound_typed_data_base_field());
  const auto& offset_field =
      Field::Handle(zone, store->compound_offset_in_bytes_field());
  const auto& wrapper = Instance::Handle(zone, Instance::New(cls));
  wrapper.SetField(data_field, data);
  wrapper.SetField(offset_field, Smi::Handle(zone, Smi::New(0)));
  return wrapper.ptr();
}

bool EncodeCompoundResult(Thread* thread,
                          const InterpreterFfiCallPlan& plan,
                          const Object& value,
                          InterpreterFfiCallbackFrame* frame) {
  Zone* const zone = thread->zone();
  constexpr intptr_t index = InterpreterFfiCallPlan::kResultIndex;
  const auto& type = AbstractType::Handle(zone, plan.CType(index));
  if (!value.IsInstance() || value.IsNull() ||
      value.clazz() != type.type_class()) {
    return false;
  }
  auto* const store = thread->isolate_group()->object_store();
  const auto& data_field =
      Field::Handle(zone, store->compound_typed_data_base_field());
  const auto& offset_field =
      Field::Handle(zone, store->compound_offset_in_bytes_field());
  const auto& backing =
      Object::Handle(zone, Instance::Cast(value).GetField(data_field));
  const auto& offset_value =
      Object::Handle(zone, Instance::Cast(value).GetField(offset_field));
  if (!offset_value.IsInteger()) return false;
  const int64_t offset = Integer::Cast(offset_value).Value();
  const auto& location = plan.Location(index);
  const intptr_t size = location.payload_type().SizeInBytes();
  if (offset < 0) return false;
  if (backing.IsTypedDataBase()) {
    const intptr_t length = TypedDataBase::Cast(backing).LengthInBytes();
    if (size > length || offset > length - size) return false;
  } else if (backing.IsPointer()) {
    if (static_cast<uint64_t>(offset) >
        kUwordMax - Pointer::Cast(backing).NativeAddress()) {
      return false;
    }
  } else {
    return false;
  }
  InterpreterFfiCallbackNativeResult native_result = {};
  {
    // All managed lookups/validation are complete. TypedData views already
    // include their base offset in DataAddr; add only the compound offset.
    NoSafepointScope no_safepoint;
    const auto* source =
        backing.IsTypedDataBase()
            ? reinterpret_cast<const uint8_t*>(
                  TypedDataBase::Cast(backing).DataAddr(offset))
            : reinterpret_cast<const uint8_t*>(
                  Pointer::Cast(backing).NativeAddress() + offset);
    if (location.IsPointerToMemory()) {
      // Native backing can alias the result destination. Do not zero or copy
      // the destination until after successful validation of the whole result.
      memmove(reinterpret_cast<void*>(frame->cpu[8]), source, size);
    } else {
      CopyCompoundResult(location, source, &native_result);
    }
  }
  // No allocation or potentially failing conversion follows the native write.
  frame->result = native_result;
  return true;
}

ObjectPtr BoxArgument(Zone* zone,
                      const InterpreterFfiCallPlan& plan,
                      intptr_t index,
                      uint64_t bits) {
  if (IsPointer(zone, plan, index)) return Pointer::New(bits);
  const auto primitive =
      plan.Location(index).payload_type().AsPrimitive().representation();
  if (plan.IsBool(index)) {
    return Bool::Get(TruncateInteger(bits, primitive) != 0).ptr();
  }
  if (plan.Location(index).payload_type().IsInt()) {
    return Integer::New(TruncateInteger(bits, primitive));
  }
  if (primitive == compiler::ffi::kFloat) {
    return Double::New(static_cast<double>(
        bit_cast<float, uint32_t>(static_cast<uint32_t>(bits))));
  }
  ASSERT(primitive == compiler::ffi::kDouble);
  return Double::New(bit_cast<double, uint64_t>(bits));
}

const InterpreterFfiCallPlan* CallbackCallPlan(Zone* zone,
                                               const Function& function) {
  const auto& signature =
      FunctionType::ZoneHandle(zone, function.FfiCSignature());
  const char* error = nullptr;
  const auto* plan = InterpreterFfiCallPlan::FromFunction(zone, function, 0,
                                                          signature, &error);
  // Preparation already checked this immutable signature before publication.
  RELEASE_ASSERT(error == nullptr && plan != nullptr);
  return plan;
}

ArrayPtr BuildCallbackArguments(Thread* thread,
                                const InterpreterFfiCallPlan& plan,
                                const Closure& closure,
                                const InterpreterFfiCallbackFrame& frame) {
  Zone* const zone = thread->zone();
  const Object** const handle_arguments =
      zone->Alloc<const Object*>(plan.num_args());
  auto* compound_offsets = zone->Alloc<intptr_t>(plan.num_args());
  intptr_t snapshot_size = 0;
  for (intptr_t i = 0; i < plan.num_args(); ++i) {
    handle_arguments[i] = nullptr;
    compound_offsets[i] = -1;
    if (plan.IsHandleCType(i)) {
      const uword address = ReadArgument(frame, plan.Location(i));
      if (address == 0) {
        FATAL(
            "A native callback Handle argument must be a valid Dart_Handle, "
            "not a null C pointer.");
      }
      // Local, persistent and read-only API handles share their ptr slot. Root
      // every referent before any later argument boxing can collect the heap.
      handle_arguments[i] = &Object::ZoneHandle(
          zone, Api::UnwrapHandle(reinterpret_cast<Dart_Handle>(address)));
    } else if (plan.IsCompoundCType(i)) {
      const intptr_t size = plan.Location(i).payload_type().SizeInBytes();
      if (Utils::WillAddOverflow(snapshot_size, size)) return Array::null();
      compound_offsets[i] = snapshot_size;
      snapshot_size += size;
    }
  }
  auto* snapshots = zone->Alloc<uint8_t>(snapshot_size);
  {
    // Native input storage must be stable throughout the native call. Snapshot
    // all by-value arguments together before allocating managed wrappers or
    // entering Dart; the target may retain wrappers or overwrite input memory.
    NoSafepointScope no_safepoint;
    for (intptr_t i = 0; i < plan.num_args(); ++i) {
      if (compound_offsets[i] >= 0) {
        CopyCompoundArgument(frame, plan.Location(i),
                             snapshots + compound_offsets[i]);
      }
    }
  }
  const intptr_t receiver_count = closure.IsNull() ? 0 : 1;
  const auto& arguments =
      Array::Handle(zone, Array::New(plan.num_args() + receiver_count));
  if (receiver_count != 0) arguments.SetAt(0, closure);
  auto& argument = Object::Handle(zone);
  for (intptr_t i = 0; i < plan.num_args(); ++i) {
    if (handle_arguments[i] != nullptr) {
      argument = handle_arguments[i]->ptr();
    } else if (compound_offsets[i] >= 0) {
      argument =
          BoxCompoundArgument(thread, plan, i, snapshots + compound_offsets[i]);
    } else {
      argument =
          BoxArgument(zone, plan, i, ReadArgument(frame, plan.Location(i)));
    }
    arguments.SetAt(i + receiver_count, argument);
  }
  return arguments.ptr();
}

// Called below setjmp so no modified automatic variable from this worker is
// inspected after a VM longjmp. All managed state stays in local VM handles.
DART_NOINLINE bool InvokeCallback(Thread* thread,
                                  const Function& function,
                                  const Closure& closure,
                                  InterpreterFfiCallbackFrame* frame,
                                  LocalHandle* handle_result) {
  Zone* const zone = thread->zone();
  const auto* plan = CallbackCallPlan(zone, function);
  const auto& arguments = Array::Handle(
      zone, BuildCallbackArguments(thread, *plan, closure, *frame));
  if (arguments.IsNull()) return false;
  auto& result = Object::Handle(zone);
  if (closure.IsNull()) {
    const auto& target = Function::Handle(zone, function.FfiCallbackTarget());
    result = DartEntry::InvokeFunction(target, arguments);
  } else {
    result = DartEntry::InvokeClosure(thread, arguments);
  }
  if (handle_result != nullptr) {
    // Keep null and Error values intact, including UnhandledException's stack
    // trace and UnwindError. The preallocated handle belongs to the caller's
    // API scope and survives this helper's local roots and a callback self-close.
    handle_result->set_ptr(result.ptr());
    return true;
  }
  if (result.IsError()) return false;
  if (plan->IsCompoundCType(InterpreterFfiCallPlan::kResultIndex)) {
    return EncodeCompoundResult(thread, *plan, result, frame);
  }
  InterpreterFfiCallbackNativeResult native_result;
  if (EncodeResult(zone, *plan, result, &native_result, false)) {
    frame->result = native_result;
    return true;
  }
  return false;
}

DART_NOINLINE void PostCallback(Thread* thread,
                                const Function& function,
                                Dart_Port target_port,
                                const InterpreterFfiCallbackFrame& frame) {
  Zone* const zone = thread->zone();
  const auto* plan = CallbackCallPlan(zone, function);
  const auto& arguments = Array::Handle(
      zone,
      BuildCallbackArguments(thread, *plan, Object::null_closure(), frame));
  if (!arguments.IsNull()) {
    FfiAsyncCallbackSendMessage(thread, target_port, arguments);
  }
}

void ApplyIndirectFallback(const InterpreterFfiCallbackFallback& fallback,
                           const InterpreterFfiCallbackFrame& frame) {
  if (fallback.indirect_result_size != 0) {
    // Deliberately delayed until failure: x8 can alias an input buffer read by
    // the target. Use the descriptor's exact size even if plan creation failed.
    NoSafepointScope no_safepoint;
    memset(reinterpret_cast<void*>(frame.cpu[8]), 0,
           fallback.indirect_result_size);
  }
}

LocalHandle* ReserveHandleResult(Thread* thread) {
  auto* const scope = thread->api_top_scope();
  if (scope == nullptr) {
    FATAL(
        "A Handle-returning native callback expects a current "
        "API scope. Did you forget to call Dart_EnterScope?");
  }
  auto* const result = scope->local_handles()->AllocateHandle();
  result->set_ptr(Object::unhandled_oom_exception().ptr());
  return result;
}

}  // namespace
#endif  // defined(INTERPRETER_FFI_CALLBACK)

InterpreterFfiCallbackPreparation PrepareInterpreterFfiCallback(
    Zone* zone,
    const Function& function,
    InterpreterFfiCallbackFallback* fallback,
    const char** error) {
  memset(fallback, 0, sizeof(*fallback));
  const auto kind = function.GetFfiCallbackKind();
  if (kind != FfiCallbackKind::kIsolateLocalStaticCallback &&
      kind != FfiCallbackKind::kIsolateLocalClosureCallback &&
      kind != FfiCallbackKind::kIsolateGroupBoundStaticCallback &&
      kind != FfiCallbackKind::kIsolateGroupBoundClosureCallback &&
      kind != FfiCallbackKind::kAsyncCallback) {
    *error = "The native callback kind is not supported by the interpreter.";
    return InterpreterFfiCallbackPreparation::kUnsupportedKind;
  }
#if defined(INTERPRETER_FFI_CALLBACK)
  if (InterpreterFfiCallbacksSupported()) {
    const auto& plan_function = Function::ZoneHandle(zone, function.ptr());
    const auto& signature =
        FunctionType::ZoneHandle(zone, function.FfiCSignature());
    *error = nullptr;
    const auto* plan = InterpreterFfiCallPlan::FromFunction(
        zone, plan_function, 0, signature, error);
    if (*error != nullptr || plan == nullptr) {
      return InterpreterFfiCallbackPreparation::kInvalidSignature;
    }
    if (kind == FfiCallbackKind::kAsyncCallback &&
        !plan->IsVoid(InterpreterFfiCallPlan::kResultIndex)) {
      *error = "NativeCallable.listener callbacks must return Void.";
      return InterpreterFfiCallbackPreparation::kInvalidSignature;
    }
    intptr_t snapshot_size = 0;
    for (intptr_t i = InterpreterFfiCallPlan::kResultIndex;
         i < plan->num_args(); ++i) {
      const auto& location = plan->Location(i);
      const bool is_result = i == InterpreterFfiCallPlan::kResultIndex;
      const bool compound = plan->IsCompoundCType(i);
      if (!(compound ? IsSupportedCompoundLocation(location, is_result)
                     : IsSupportedScalarLocation(location, is_result))) {
        *error =
            "The native callback ABI location is not supported by the "
            "ARM64 interpreter capture.";
        return InterpreterFfiCallbackPreparation::kUnsupportedSignature;
      }
      if (compound && !is_result) {
        const intptr_t size = location.payload_type().SizeInBytes();
        // This is the VM's existing TypedData representation limit, not an
        // additional FFI size policy. Reject before publication instead of
        // reaching TypedData::New's fatal invalid-length check during entry.
        if (size > TypedData::MaxElements(kTypedDataUint8ArrayCid) ||
            Utils::WillAddOverflow(snapshot_size, size)) {
          *error =
              "The native callback argument snapshots exceed the VM's "
              "representable TypedData or scratch size.";
          return InterpreterFfiCallbackPreparation::kUnsupportedSignature;
        }
        snapshot_size += size;
      }
    }
    const auto& exceptional =
        Instance::Handle(zone, function.FfiCallbackExceptionalReturn());
    if (kind == FfiCallbackKind::kAsyncCallback && !exceptional.IsNull()) {
      *error = "NativeCallable.listener does not accept an exceptional return.";
      return InterpreterFfiCallbackPreparation::kInvalidSignature;
    }
    constexpr intptr_t result_index = InterpreterFfiCallPlan::kResultIndex;
    if (plan->IsHandleCType(result_index) ||
        plan->IsCompoundCType(result_index)) {
      if (!exceptional.IsNull()) {
        *error =
            "Handle and compound callbacks do not accept an exceptional "
            "return value.";
        return InterpreterFfiCallbackPreparation::kInvalidSignature;
      }
      if (plan->IsHandleCType(result_index)) {
        fallback->flags = kInterpreterFfiCallbackReturnsHandle;
      } else if (plan->Location(result_index).IsPointerToMemory()) {
        fallback->indirect_result_size =
            plan->Location(result_index).payload_type().SizeInBytes();
      }
    } else if (!EncodeResult(zone, *plan, exceptional, &fallback->registers,
                             true)) {
      *error =
          "The callback exceptional return does not match its native "
          "result type.";
      return InterpreterFfiCallbackPreparation::kInvalidSignature;
    }
    return InterpreterFfiCallbackPreparation::kReady;
  }
#endif
  *error =
      "Runtime-created callbacks require the native ARM64 AOT interpreter.";
  return InterpreterFfiCallbackPreparation::kUnsupportedSignature;
}

#if defined(INTERPRETER_FFI_CALLBACK)
extern "C" void DLRT_InterpreterFfiCallback(
    Thread* thread,
    InterpreterFfiCallbackFrame* frame) {
  ASSERT(thread == Thread::Current());
  ASSERT(thread->execution_state() == Thread::kThreadInVM);
  ASSERT(!thread->IsAtSafepoint());
  // Async's tail epilogue needs this native original-isolate cookie even when
  // argument work or a nested Dart/native invocation uses the scratch slot.
  const int64_t saved_second_arg = thread->unboxed_int64_runtime_second_arg();
  StackZone zone(thread);
  HANDLESCOPE(thread);
  // Read the registration root once. The callback may close itself and delete
  // that persistent handle during invocation; these local roots survive it.
  const auto* persistent =
      reinterpret_cast<PersistentHandle*>(thread->unboxed_int64_runtime_arg());
  const auto& descriptor =
      Array::Handle(zone.GetZone(), Array::RawCast(persistent->ptr()));
  ASSERT(descriptor.Length() == kInterpreterFfiCallbackDescriptorLength);
  const auto& function = Function::ZoneHandle(
      zone.GetZone(),
      Function::RawCast(descriptor.At(kInterpreterFfiCallbackFunction)));
  const auto& closure = Closure::Handle(
      zone.GetZone(),
      Closure::RawCast(descriptor.At(kInterpreterFfiCallbackClosure)));
  const auto kind = function.GetFfiCallbackKind();
  bool is_async = false;
  switch (kind) {
    case FfiCallbackKind::kIsolateLocalStaticCallback:
    case FfiCallbackKind::kIsolateLocalClosureCallback:
    case FfiCallbackKind::kIsolateGroupBoundStaticCallback:
    case FfiCallbackKind::kIsolateGroupBoundClosureCallback:
      break;
    case FfiCallbackKind::kAsyncCallback:
      is_async = true;
      break;
    default:
      FATAL("Invalid interpreter native callback kind.");
  }
  const auto& send_port = Integer::Handle(
      zone.GetZone(),
      Integer::RawCast(descriptor.At(kInterpreterFfiCallbackSendPort)));
  const Dart_Port target_port = send_port.Value();
  RELEASE_ASSERT(is_async ? closure.IsNull() && target_port > ILLEGAL_PORT
                          : target_port == ILLEGAL_PORT);
  const auto& fallback_data = TypedData::Handle(
      zone.GetZone(),
      TypedData::RawCast(descriptor.At(kInterpreterFfiCallbackFallback)));
  ASSERT(fallback_data.GetClassId() == kTypedDataUint8ArrayCid);
  ASSERT(fallback_data.Length() ==
         static_cast<intptr_t>(sizeof(InterpreterFfiCallbackFallback)));
  InterpreterFfiCallbackFallback fallback;
  {
    NoSafepointScope no_safepoint;
    memcpy(&fallback, fallback_data.DataAddr(0), sizeof(fallback));
  }
  ASSERT((fallback.flags & ~kInterpreterFfiCallbackReturnsHandle) == 0);
  RELEASE_ASSERT(!is_async ||
                 (fallback.flags == 0 && fallback.indirect_result_size == 0));
  frame->result = fallback.registers;
  LocalHandle* const handle_result =
      (fallback.flags & kInterpreterFfiCallbackReturnsHandle) != 0
          ? ReserveHandleResult(thread)
          : nullptr;
  if (handle_result != nullptr) {
    frame->result.cpu[0] = reinterpret_cast<uword>(handle_result);
  }

  // Keep the outer Dart exit chain visible while boxing. DartEntry establishes
  // its own managed entry chain and suspends this C++ error boundary while Dart
  // executes, restoring both before returning here.
  LongJumpScope jump(thread);
  DEBUG_ONLY(const auto outer_exit_frame = thread->top_exit_frame_info());
  DEBUG_ONLY(const auto outer_vm_tag = thread->vm_tag());
  DEBUG_ONLY(auto* const outer_api_scope = thread->api_top_scope());
  DEBUG_ONLY(auto* const outer_resource = thread->top_resource());
  if (DART_SETJMP(*jump.Set()) == 0) {
    if (is_async) {
      // Only post copied arguments. The port's Zone-bound handler executes on
      // its owning isolate later, never on this native calling thread.
      PostCallback(thread, function, target_port, *frame);
    } else if (!InvokeCallback(thread, function, closure, frame,
                               handle_result)) {
      ApplyIndirectFallback(fallback, *frame);
    }
  } else {
    const auto error = thread->StealStickyError();
    if (handle_result != nullptr && error != Error::null()) {
      handle_result->set_ptr(error == Object::out_of_memory_error().ptr()
                                 ? Object::unhandled_oom_exception().ptr()
                                 : static_cast<ObjectPtr>(error));
    }
    ApplyIndirectFallback(fallback, *frame);
  }
  thread->set_unboxed_int64_runtime_second_arg(saved_second_arg);
  DEBUG_ASSERT(thread->top_exit_frame_info() == outer_exit_frame);
  DEBUG_ASSERT(thread->vm_tag() == outer_vm_tag);
  DEBUG_ASSERT(thread->api_top_scope() == outer_api_scope);
  DEBUG_ASSERT(thread->top_resource() == outer_resource);
  ASSERT(thread->execution_state() == Thread::kThreadInVM);
}
#endif  // defined(INTERPRETER_FFI_CALLBACK)

}  // namespace dart
