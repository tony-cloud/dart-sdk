// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_FFI_INTERPRETER_CALL_PLAN_H_
#define RUNTIME_VM_FFI_INTERPRETER_CALL_PLAN_H_

#include "vm/compiler/ffi/native_calling_convention.h"
#include "vm/object.h"

namespace dart {

// The interpreter marshals boxed Dart values directly, without compiler
// Locations, Representations, or IL. Native ABI allocation remains owned by
// NativeCallingConvention; this plan adds Dart type queries and scratch space
// for compounds passed by pointer.
class InterpreterFfiCallPlan : public ZoneObject {
 public:
  static constexpr intptr_t kResultIndex = -1;

  static InterpreterFfiCallPlan* FromFunction(
      Zone* zone,
      const Function& function,
      intptr_t dart_signature_params_start_at,
      const FunctionType& c_signature,
      const char** error);

  intptr_t num_args() const {
    return native_calling_convention_.argument_locations().length();
  }
  const compiler::ffi::NativeLocation& Location(intptr_t arg_index) const {
    if (arg_index == kResultIndex) {
      return native_calling_convention_.return_location();
    }
    return *native_calling_convention_.argument_locations().At(arg_index);
  }
  bool contains_varargs() const {
    return native_calling_convention_.contains_varargs();
  }

  // Excludes the implicit native function pointer and flattens VarArgs records.
  AbstractTypePtr CType(intptr_t arg_index) const;
  bool IsPointerPointer(intptr_t arg_index) const;
  bool IsTypedDataPointer(intptr_t arg_index) const;
  bool IsCompoundPointer(intptr_t arg_index) const;
  bool IsHandleCType(intptr_t arg_index) const;
  bool IsBool(intptr_t arg_index) const;
  bool IsCompoundCType(intptr_t arg_index) const;
  bool IsVoid(intptr_t arg_index) const;

  bool ReturnsCompound() const { return IsCompoundCType(kResultIndex); }
  intptr_t CompoundReturnSizeInBytes() const;
  intptr_t PassByPointerStackOffset(intptr_t arg_index) const {
    ASSERT(arg_index >= kResultIndex && arg_index < num_args());
    return pass_by_pointer_stack_offsets_.At(arg_index + 1);
  }
  intptr_t RequiredStackSpaceInBytes() const { return required_stack_space_; }

 private:
  InterpreterFfiCallPlan(
      Zone* zone,
      const Function& dart_signature,
      intptr_t dart_signature_params_start_at,
      const FunctionType& c_signature,
      const compiler::ffi::NativeCallingConvention& native_calling_convention);

  AbstractTypePtr DartType(intptr_t arg_index) const;
  bool IsPointerCType(intptr_t arg_index) const;

  Zone* const zone_;
  const Function& dart_signature_;
  const intptr_t dart_signature_params_start_at_;
  const FunctionType& c_signature_;
  const compiler::ffi::NativeCallingConvention& native_calling_convention_;
  // Entry 0 is the result; entry i + 1 is argument i.
  ZoneGrowableArray<intptr_t> pass_by_pointer_stack_offsets_;
  intptr_t required_stack_space_;
};

}  // namespace dart

#endif  // RUNTIME_VM_FFI_INTERPRETER_CALL_PLAN_H_
