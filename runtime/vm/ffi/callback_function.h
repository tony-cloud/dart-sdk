// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_FFI_CALLBACK_FUNCTION_H_
#define RUNTIME_VM_FFI_CALLBACK_FUNCTION_H_

#include "vm/object.h"

namespace dart {

// Constructs callback Function/FfiTrampolineData metadata, including its
// canonical Dart signature and old-space exceptional return. Does not dedupe,
// assign a callback ID, generate Code, or publish a native trampoline.
FunctionPtr NewFfiCallbackFunction(const FunctionType& c_signature,
                                   const Function& dart_target,
                                   const Instance& exceptional_return,
                                   FfiCallbackKind kind);

// Runtime-only callbacks retain their unassigned ID and have no generated Code.
bool IsRuntimeFfiCallbackFunction(const Function& function);

}  // namespace dart

#endif  // RUNTIME_VM_FFI_CALLBACK_FUNCTION_H_
