// Copyright (c) 2023, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/unwinding_records.h"

#include "platform/assert.h"
#include "platform/globals.h"

namespace dart {

#if defined(NEED_WINDOWS_UNWINDING_RECORDS)

#if defined(TARGET_ARCH_X64)
const intptr_t kReservedUnwindingRecordsSizeBytes = 64;
#elif defined(TARGET_ARCH_ARM64)
const intptr_t kReservedUnwindingRecordsSizeBytes = 4 * KB;
#else
#error Unhandled Windows architecture.
#endif

intptr_t UnwindingRecordsPlatform::SizeInBytes() {
  return kReservedUnwindingRecordsSizeBytes;
}

#endif  // defined(NEED_WINDOWS_UNWINDING_RECORDS)

// Only use these definitions when the ELF loader may be used on 64-bit Windows,
// as it is the only client of these methods (e.g., _not_ in gen_snapshot).
#if defined(UNWINDING_RECORDS_WINDOWS_HOST)

void UnwindingRecordsPlatform::RegisterExecutableMemory(
    void* start,
    intptr_t size,
    void** pp_dynamic_table) {
  intptr_t unwinding_record_offset = size - kReservedUnwindingRecordsSizeBytes;
  uint8_t* record_ptr = static_cast<uint8_t*>(start) + unwinding_record_offset;
  RegisterExecutableMemory(start, size, record_ptr, pp_dynamic_table);
}

void UnwindingRecordsPlatform::RegisterExecutableMemory(
    void* start,
    intptr_t size,
    void* records_start,
    void** pp_dynamic_table) {
  CodeRangeUnwindingRecord* record =
      reinterpret_cast<CodeRangeUnwindingRecord*>(records_start);
  RELEASE_ASSERT(record->magic == kUnwindingRecordMagic);
  uword start_num = reinterpret_cast<intptr_t>(start);
  uword end_num = start_num + size;
  DWORD status = RtlAddGrowableFunctionTable(
      pp_dynamic_table,
      /*FunctionTable=*/
      reinterpret_cast<PRUNTIME_FUNCTION>(record->runtime_function),
      /*EntryCount=*/record->runtime_function_count,
      /*MaximumEntryCount=*/record->runtime_function_count,
      /*RangeBase=*/start_num,
      /*RangeEnd=*/end_num);
  if (status != 0) {
    FATAL("Failed to add growable function table: 0x%x\n", status);
  }
}

bool UnwindingRecordsPlatform::TryRegisterExecutableMemory(
    void* start,
    intptr_t size,
    void** pp_dynamic_table) {
  if (pp_dynamic_table == nullptr) return false;
  *pp_dynamic_table = nullptr;
  const uword start_num = reinterpret_cast<uword>(start);
  if (start == nullptr || size < kReservedUnwindingRecordsSizeBytes ||
      start_num > kUwordMax - static_cast<uword>(size)) {
    return false;
  }
  const intptr_t record_offset = size - kReservedUnwindingRecordsSizeBytes;
  auto* record = reinterpret_cast<CodeRangeUnwindingRecord*>(
      static_cast<uint8_t*>(start) + record_offset);
  const size_t table_offset =
      offsetof(CodeRangeUnwindingRecord, runtime_function);
  const size_t max_count = (kReservedUnwindingRecordsSizeBytes - table_offset) /
                           sizeof(TargetRuntimeFunction);
  if (record->magic != kUnwindingRecordMagic ||
      record->runtime_function_count == 0 ||
      record->runtime_function_count > max_count) {
    return false;
  }
#if defined(TARGET_ARCH_X64)
  const GeneratedCodeUnwindInfo expected_info;
  if (record->runtime_function_count != 1 ||
      memcmp(&record->unwind_info, &expected_info, sizeof(expected_info)) !=
          0) {
    return false;
  }
#elif defined(TARGET_ARCH_ARM64)
  if ((record_offset & kFunctionLengthMask) != 0) return false;
  UnwindData<> expected_full;
  expected_full.unwind_info.FunctionLength = kMaxFunctionLength >> 2;
  UnwindData<> expected_remainder;
  expected_remainder.unwind_info.FunctionLength =
      (record_offset % kMaxFunctionLength) >> 2;
  if (memcmp(&record->unwind_info, &expected_full, sizeof(expected_full)) !=
          0 ||
      memcmp(&record->unwind_info1, &expected_remainder,
             sizeof(expected_remainder)) != 0 ||
      record->runtime_function_count !=
          (static_cast<uword>(record_offset) + kMaxFunctionLength - 1) /
              kMaxFunctionLength) {
    return false;
  }
#endif
  // Accept the compiler's full unwind records only. In particular, never let
  // the OS follow an out-of-range table entry or an unbounded xdata pointer.
  uword previous_end = 0;
  for (uint32_t i = 0; i < record->runtime_function_count; ++i) {
    const auto& function = record->runtime_function[i];
    uword function_end;
#if defined(TARGET_ARCH_X64)
    function_end = function.EndAddress;
    if (function.UnwindData !=
        record_offset + offsetof(CodeRangeUnwindingRecord, unwind_info)) {
      return false;
    }
    if (function.BeginAddress != 0 ||
        function.EndAddress != static_cast<uword>(record_offset)) {
      return false;
    }
#elif defined(TARGET_ARCH_ARM64)
    const UnwindData<>* data;
    if (function.UnwindData ==
        record_offset + offsetof(CodeRangeUnwindingRecord, unwind_info)) {
      data = &record->unwind_info;
    } else if (function.UnwindData ==
               record_offset +
                   offsetof(CodeRangeUnwindingRecord, unwind_info1)) {
      data = &record->unwind_info1;
    } else {
      return false;
    }
    const auto& info = data->unwind_info;
    const uword expected_begin = static_cast<uword>(i) * kMaxFunctionLength;
    const bool full_chunk =
        static_cast<uword>(record_offset) - expected_begin >=
        kMaxFunctionLength;
    const uword expected_unwind =
        record_offset +
        (full_chunk ? offsetof(CodeRangeUnwindingRecord, unwind_info)
                    : offsetof(CodeRangeUnwindingRecord, unwind_info1));
    if (function.BeginAddress != expected_begin ||
        function.UnwindData != expected_unwind) {
      return false;
    }
    function_end = static_cast<uword>(function.BeginAddress) +
                   (static_cast<uword>(info.FunctionLength) << 2);
#endif
    if (function.BeginAddress < previous_end ||
        function.BeginAddress >= function_end ||
        function_end > static_cast<uword>(record_offset)) {
      return false;
    }
    previous_end = function_end;
  }
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr &&
      !TestHookForTesting()(TestOperation::kRegister, start)) {
    return false;
  }
#endif
  void* table = nullptr;
  const DWORD status = RtlAddGrowableFunctionTable(
      &table, reinterpret_cast<PRUNTIME_FUNCTION>(record->runtime_function),
      record->runtime_function_count, record->runtime_function_count, start_num,
      start_num + size);
  if (status != 0) return false;
  *pp_dynamic_table = table;
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr) {
    TestHookForTesting()(TestOperation::kRegistered, table);
  }
#endif
  return true;
}

void UnwindingRecordsPlatform::UnregisterDynamicTable(void* p_dynamic_table) {
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr) {
    TestHookForTesting()(TestOperation::kUnregister, p_dynamic_table);
  }
#endif
  RtlDeleteGrowableFunctionTable(p_dynamic_table);
}

#endif  // defined(UNWINDING_RECORDS_WINDOWS_HOST)

}  // namespace dart
