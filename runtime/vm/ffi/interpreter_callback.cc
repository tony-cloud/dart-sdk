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
#include "vm/simulator.h"
#include "vm/thread.h"
#include "vm/zone.h"

#if defined(DART_PRECOMPILED_RUNTIME) && defined(DART_BYTECODE_INTERPRETER) && \
    defined(DART_SHOREBIRD_INTERPRETER) && defined(HOST_ARCH_ARM64) &&       \
    defined(TARGET_ARCH_ARM64) && !defined(DART_HOST_OS_WINDOWS) &&         \
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
  return entry_point != 0 &&
         entry_point == InterpreterFfiCallbackEntryPoint();
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

bool IsSupportedScalarLocation(const NativeLocation& location,
                               bool is_result) {
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

// Called below setjmp so no modified automatic variable from this worker is
// inspected after a VM longjmp. All managed state stays in local VM handles.
DART_NOINLINE void InvokeCallback(Thread* thread,
                                 const Function& function,
                                 const Closure& closure,
                                 InterpreterFfiCallbackFrame* frame) {
  Zone* const zone = thread->zone();
  const auto& signature =
      FunctionType::ZoneHandle(zone, function.FfiCSignature());
  const char* error = nullptr;
  const auto* plan = InterpreterFfiCallPlan::FromFunction(
      zone, function, 0, signature, &error);
  // Preparation already checked this immutable signature before publication.
  RELEASE_ASSERT(error == nullptr && plan != nullptr);
  const intptr_t receiver_count = closure.IsNull() ? 0 : 1;
  const auto& arguments =
      Array::Handle(zone, Array::New(plan->num_args() + receiver_count));
  if (receiver_count != 0) arguments.SetAt(0, closure);
  auto& argument = Object::Handle(zone);
  for (intptr_t i = 0; i < plan->num_args(); ++i) {
    argument = BoxArgument(zone, *plan, i, ReadArgument(*frame, plan->Location(i)));
    arguments.SetAt(i + receiver_count, argument);
  }
  auto& result = Object::Handle(zone);
  if (closure.IsNull()) {
    const auto& target = Function::Handle(zone, function.FfiCallbackTarget());
    result = DartEntry::InvokeFunction(target, arguments);
  } else {
    result = DartEntry::InvokeClosure(thread, arguments);
  }
  if (result.IsError()) return;
  InterpreterFfiCallbackNativeResult native_result;
  if (EncodeResult(zone, *plan, result, &native_result, false)) {
    frame->result = native_result;
  }
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
      kind != FfiCallbackKind::kIsolateLocalClosureCallback) {
    *error = "Runtime-created callbacks currently require isolate-local "
             "synchronous invocation.";
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
    for (intptr_t i = InterpreterFfiCallPlan::kResultIndex;
         i < plan->num_args(); ++i) {
      if (plan->IsHandleCType(i) || plan->IsCompoundCType(i) ||
          !IsSupportedScalarLocation(plan->Location(i),
                                     i == InterpreterFfiCallPlan::kResultIndex)) {
        *error = "Runtime-created callbacks currently support only scalar and "
                 "pointer arguments and results.";
        return InterpreterFfiCallbackPreparation::kUnsupportedSignature;
      }
    }
    const auto& exceptional =
        Instance::Handle(zone, function.FfiCallbackExceptionalReturn());
    if (!EncodeResult(zone, *plan, exceptional, &fallback->registers, true)) {
      *error = "The callback exceptional return does not match its native "
               "result type.";
      return InterpreterFfiCallbackPreparation::kInvalidSignature;
    }
    return InterpreterFfiCallbackPreparation::kReady;
  }
#endif
  *error = "Runtime-created callbacks require the native ARM64 AOT interpreter.";
  return InterpreterFfiCallbackPreparation::kUnsupportedSignature;
}

#if defined(INTERPRETER_FFI_CALLBACK)
extern "C" void DLRT_InterpreterFfiCallback(
    Thread* thread, InterpreterFfiCallbackFrame* frame) {
  ASSERT(thread == Thread::Current());
  ASSERT(thread->execution_state() == Thread::kThreadInVM);
  ASSERT(!thread->IsAtSafepoint());
  StackZone zone(thread);
  HANDLESCOPE(thread);
  // Read the registration root once. The callback may close itself and delete
  // that persistent handle during invocation; these local roots survive it.
  const auto* persistent = reinterpret_cast<PersistentHandle*>(
      thread->unboxed_int64_runtime_arg());
  const auto& descriptor =
      Array::Handle(zone.GetZone(), Array::RawCast(persistent->ptr()));
  ASSERT(descriptor.Length() == kInterpreterFfiCallbackDescriptorLength);
  const auto& function = Function::ZoneHandle(
      zone.GetZone(),
      Function::RawCast(descriptor.At(kInterpreterFfiCallbackFunction)));
  const auto& closure = Closure::Handle(
      zone.GetZone(),
      Closure::RawCast(descriptor.At(kInterpreterFfiCallbackClosure)));
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
  ASSERT(fallback.indirect_result_size == 0 && fallback.flags == 0);
  frame->result = fallback.registers;

  // Keep the outer Dart exit chain visible while boxing. DartEntry establishes
  // its own managed entry chain and suspends this C++ error boundary while Dart
  // executes, restoring both before returning here.
  LongJumpScope jump(thread);
  DEBUG_ONLY(const auto outer_exit_frame = thread->top_exit_frame_info());
  DEBUG_ONLY(const auto outer_vm_tag = thread->vm_tag());
  DEBUG_ONLY(auto* const outer_api_scope = thread->api_top_scope());
  DEBUG_ONLY(auto* const outer_resource = thread->top_resource());
  if (DART_SETJMP(*jump.Set()) == 0) {
    InvokeCallback(thread, function, closure, frame);
  } else {
    USE(thread->StealStickyError());
  }
  DEBUG_ASSERT(thread->top_exit_frame_info() == outer_exit_frame);
  DEBUG_ASSERT(thread->vm_tag() == outer_vm_tag);
  DEBUG_ASSERT(thread->api_top_scope() == outer_api_scope);
  DEBUG_ASSERT(thread->top_resource() == outer_resource);
  ASSERT(thread->execution_state() == Thread::kThreadInVM);
}
#endif  // defined(INTERPRETER_FFI_CALLBACK)

}  // namespace dart
