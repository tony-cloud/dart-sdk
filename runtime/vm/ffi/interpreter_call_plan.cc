// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/ffi/interpreter_call_plan.h"

#include "vm/object_store.h"
#include "vm/symbols.h"

namespace dart {

InterpreterFfiCallPlan* InterpreterFfiCallPlan::FromFunction(
    Zone* zone,
    const Function& function,
    intptr_t dart_signature_params_start_at,
    const FunctionType& c_signature,
    const char** error) {
  DEBUG_ASSERT(function.IsNotTemporaryScopedHandle());
  DEBUG_ASSERT(c_signature.IsNotTemporaryScopedHandle());
  const auto* signature = compiler::ffi::NativeFunctionTypeFromFunctionType(
      zone, c_signature, error);
  if (*error != nullptr) {
    return nullptr;
  }
  const auto& calling_convention =
      compiler::ffi::NativeCallingConvention::FromSignature(zone, *signature);
  return new (zone)
      InterpreterFfiCallPlan(zone, function, dart_signature_params_start_at,
                             c_signature, calling_convention);
}

InterpreterFfiCallPlan::InterpreterFfiCallPlan(
    Zone* zone,
    const Function& dart_signature,
    intptr_t dart_signature_params_start_at,
    const FunctionType& c_signature,
    const compiler::ffi::NativeCallingConvention& native_calling_convention)
    : zone_(zone),
      dart_signature_(dart_signature),
      dart_signature_params_start_at_(dart_signature_params_start_at),
      c_signature_(c_signature),
      native_calling_convention_(native_calling_convention),
      pass_by_pointer_stack_offsets_(zone, num_args() + 1) {
  // Match CallMarshaller's scratch layout after the ABI-governed arguments.
  intptr_t stack_offset = native_calling_convention_.StackTopInBytes();
#if (defined(DART_TARGET_OS_MACOS_IOS) || defined(DART_TARGET_OS_MACOS)) &&    \
    defined(TARGET_ARCH_ARM64)
  // Preserve the padding used by the compiler's stack moves.
  stack_offset += 4;
#endif
  stack_offset = Utils::RoundUp(stack_offset, compiler::target::kWordSize);
  pass_by_pointer_stack_offsets_.Add(stack_offset);
  const auto& result = Location(kResultIndex);
  if (result.IsPointerToMemory()) {
    stack_offset += result.payload_type().SizeInBytes();
    stack_offset = Utils::RoundUp(stack_offset, compiler::target::kWordSize);
  }
  for (intptr_t i = 0; i < num_args(); ++i) {
    pass_by_pointer_stack_offsets_.Add(stack_offset);
    const auto& argument = Location(i);
    if (argument.IsPointerToMemory()) {
      stack_offset += argument.payload_type().SizeInBytes();
      stack_offset = Utils::RoundUp(stack_offset, compiler::target::kWordSize);
    }
  }
  required_stack_space_ = stack_offset;
}

AbstractTypePtr InterpreterFfiCallPlan::CType(intptr_t arg_index) const {
  if (arg_index == kResultIndex) {
    return c_signature_.result_type();
  }
  const auto& parameters = Array::Handle(zone_, c_signature_.parameter_types());
  const intptr_t last_index = parameters.Length() - 1;
  const auto& last_type =
      AbstractType::Handle(zone_, c_signature_.ParameterTypeAt(last_index));
  auto* store = Thread::Current()->isolate_group()->object_store();
  const intptr_t real_index = arg_index + 1;  // Skip the function pointer.
  if (last_type.type_class() == store->ffi_varargs_class() &&
      real_index >= last_index) {
    const auto& varargs = AbstractType::Handle(
        zone_, TypeArguments::Handle(zone_, Type::Cast(last_type).arguments())
                   .TypeAt(0));
    if (varargs.IsRecordType()) {
      const auto& record = RecordType::Cast(varargs);
      const intptr_t field_index = real_index - last_index;
      ASSERT(field_index < record.NumFields());
      return record.FieldTypeAt(field_index);
    }
    ASSERT(!varargs.IsNull());
    return varargs.ptr();
  }
  return c_signature_.ParameterTypeAt(real_index);
}

AbstractTypePtr InterpreterFfiCallPlan::DartType(intptr_t arg_index) const {
  if (arg_index == kResultIndex) {
    return dart_signature_.result_type();
  }
  return dart_signature_.ParameterTypeAt(arg_index +
                                         dart_signature_params_start_at_);
}

bool InterpreterFfiCallPlan::IsPointerCType(intptr_t arg_index) const {
  return AbstractType::Handle(zone_, CType(arg_index)).type_class_id() ==
         kPointerCid;
}

bool InterpreterFfiCallPlan::IsPointerPointer(intptr_t arg_index) const {
  if (dart_signature_.parameter_types() == Array::null()) {
    return IsPointerCType(arg_index);
  }
  return IsPointerCType(arg_index) &&
         AbstractType::Handle(zone_, DartType(arg_index)).type_class_id() ==
             kPointerCid;
}

bool InterpreterFfiCallPlan::IsTypedDataPointer(intptr_t arg_index) const {
  if (!IsPointerCType(arg_index) ||
      dart_signature_.parameter_types() == Array::null()) {
    return false;
  }
  const auto& type = AbstractType::Handle(zone_, DartType(arg_index));
  auto* store = Thread::Current()->isolate_group()->object_store();
  auto& typed_data_class = Class::Handle(zone_, store->typed_data_class());
  if (typed_data_class.IsNull()) {
    const auto& library = Library::Handle(zone_, Library::TypedDataLibrary());
    typed_data_class = library.LookupClass(Symbols::TypedData());
    ASSERT(!typed_data_class.IsNull());
    store->set_typed_data_class(typed_data_class);
  }
  return type.type_class() == typed_data_class.ptr();
}

static bool IsCompoundType(Zone* zone, const AbstractType& type) {
  auto* store = Thread::Current()->isolate_group()->object_store();
  auto& cls = Class::Handle(zone, type.type_class());
  if (cls.ptr() == store->ffi_compound_class() ||
      cls.ptr() == store->ffi_array_class()) {
    return true;
  }
  cls ^= cls.SuperClass();
  return cls.ptr() == store->ffi_struct_class() ||
         cls.ptr() == store->ffi_union_class();
}

bool InterpreterFfiCallPlan::IsCompoundPointer(intptr_t arg_index) const {
  if (!IsPointerCType(arg_index) ||
      dart_signature_.parameter_types() == Array::null()) {
    return false;
  }
  return IsCompoundType(zone_,
                        AbstractType::Handle(zone_, DartType(arg_index)));
}

bool InterpreterFfiCallPlan::IsHandleCType(intptr_t arg_index) const {
  return AbstractType::Handle(zone_, CType(arg_index)).type_class_id() ==
         kFfiHandleCid;
}

bool InterpreterFfiCallPlan::IsBool(intptr_t arg_index) const {
  return AbstractType::Handle(zone_, CType(arg_index)).type_class_id() ==
         kFfiBoolCid;
}

bool InterpreterFfiCallPlan::IsCompoundCType(intptr_t arg_index) const {
  return IsCompoundType(zone_, AbstractType::Handle(zone_, CType(arg_index)));
}

bool InterpreterFfiCallPlan::IsVoid(intptr_t arg_index) const {
  return AbstractType::Handle(zone_, CType(arg_index)).type_class_id() ==
         kFfiVoidCid;
}

intptr_t InterpreterFfiCallPlan::CompoundReturnSizeInBytes() const {
  ASSERT(ReturnsCompound());
  return Utils::RoundUp(Location(kResultIndex).payload_type().SizeInBytes(),
                        compiler::target::kWordSize);
}

}  // namespace dart
