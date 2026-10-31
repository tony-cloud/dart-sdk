// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_FFI_INTERPRETER_CALLBACK_H_
#define RUNTIME_VM_FFI_INTERPRETER_CALLBACK_H_

#include "vm/object.h"

namespace dart {

enum class InterpreterFfiCallbackPreparation {
  kReady,
  kUnsupportedKind,
  kUnsupportedSignature,
  kInvalidSignature,
};

enum InterpreterFfiCallbackDescriptorSlot : intptr_t {
  kInterpreterFfiCallbackFunction = 0,
  kInterpreterFfiCallbackClosure = 1,
  kInterpreterFfiCallbackSendPort =
      2,  // Integer port for async; zero for sync.
  kInterpreterFfiCallbackFallback = 3,
  kInterpreterFfiCallbackDescriptorLength = 4,
};

struct InterpreterFfiCallbackNativeResult {
  uint64_t cpu[2];
  uint64_t fpu[4];
};

struct InterpreterFfiCallbackFallback {
  InterpreterFfiCallbackNativeResult registers;
  // Exact bytes zeroed at captured x8 only after an indirect-result failure.
  uint64_t indirect_result_size;
  // Handle results reserve one LocalHandle in the caller's current API scope.
  uint64_t flags;
};

constexpr uint64_t kInterpreterFfiCallbackReturnsHandle = 1;

// Native bits only; managed roots belong in the helper's VM handles.
struct alignas(16) InterpreterFfiCallbackFrame {
  uint64_t cpu[9];  // x0-x8, including the indirect result address.
  uint64_t fpu[8];  // d0-d7.
  uword stack_args;
  InterpreterFfiCallbackNativeResult result;
};

bool InterpreterFfiCallbacksSupported();
// Raw instruction PC consumed by the existing callback stub's BLR.
uword InterpreterFfiCallbackEntryPoint();
bool IsInterpreterFfiCallbackEntry(uword entry_point);

// Validates and encodes a fallback without publishing a callback or retaining
// a Zone plan. Failure supplies a literal or Zone-lifetime error message.
InterpreterFfiCallbackPreparation PrepareInterpreterFfiCallback(
    Zone* zone,
    const Function& function,
    InterpreterFfiCallbackFallback* fallback,
    const char** error);

// Assembly entry address only: its arguments have the callback's native ABI.
extern "C" void InterpreterFfiCallbackEntry();
extern "C" void DLRT_InterpreterFfiCallback(Thread* thread,
                                            InterpreterFfiCallbackFrame* frame);

}  // namespace dart

#endif  // RUNTIME_VM_FFI_INTERPRETER_CALLBACK_H_
