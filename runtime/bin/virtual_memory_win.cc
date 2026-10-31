// Copyright (c) 2021, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/globals.h"
#if defined(DART_HOST_OS_WINDOWS)

#include "bin/virtual_memory.h"

#include "platform/assert.h"
#include "platform/utils.h"

namespace dart {
namespace bin {

uword VirtualMemory::page_size_ = 0;

intptr_t VirtualMemory::CalculatePageSize() {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const intptr_t page_size = info.dwPageSize;
  ASSERT(page_size != 0);
  ASSERT(Utils::IsPowerOfTwo(page_size));
  return page_size;
}

VirtualMemory* VirtualMemory::Allocate(intptr_t size,
                                       bool is_executable,
                                       const char* name,
                                       bool executable_capable) {
  USE(executable_capable);
  ASSERT(Utils::IsAligned(size, PageSize()));
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr &&
      !TestHookForTesting()(TestOperation::kAllocate, nullptr, size)) {
    return nullptr;
  }
#endif
  int prot = is_executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
  void* address = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, prot);
  if (address == nullptr) {
    return nullptr;
  }
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr) {
    TestHookForTesting()(TestOperation::kAllocated, address, size);
  }
#endif
  return new VirtualMemory(address, size);
}

VirtualMemory::~VirtualMemory() {
  if (address_ != nullptr) {
#if defined(DART_ELF_LOADER_TESTING)
    if (TestHookForTesting() != nullptr) {
      TestHookForTesting()(TestOperation::kFree, address_, size_);
    }
#endif
    if (VirtualFree(address_, 0, MEM_RELEASE) == 0) {
      FATAL("VirtualFree failed: Error code %d\n", GetLastError());
    }
  }
}

void VirtualMemory::Protect(void* address, intptr_t size, Protection mode) {
  if (!TryProtect(address, size, mode)) {
    int error = GetLastError();
    char buffer[1024];
    FATAL("VirtualProtect failed %d (%s)\n", error,
          Utils::StrError(error, buffer, sizeof(buffer)));
  }
}

bool VirtualMemory::TryProtect(void* address, intptr_t size, Protection mode) {
  const uword address_value = reinterpret_cast<uword>(address);
  if (address == nullptr || size <= 0 ||
      address_value > kUwordMax - static_cast<uword>(size)) {
    return false;
  }
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr &&
      !TestHookForTesting()(TestOperation::kProtect, address, size)) {
    return false;
  }
#endif
  uword start_address = reinterpret_cast<uword>(address);
  uword end_address = start_address + size;
  uword page_address = Utils::RoundDown(start_address, PageSize());
  DWORD prot = 0;
  switch (mode) {
    case kNoAccess:
      prot = PAGE_NOACCESS;
      break;
    case kReadOnly:
      prot = PAGE_READONLY;
      break;
    case kReadWrite:
      prot = PAGE_READWRITE;
      break;
    case kReadExecute:
      prot = PAGE_EXECUTE_READ;
      break;
    case kReadWriteExecute:
      prot = PAGE_EXECUTE_READWRITE;
      break;
  }
  DWORD old_prot = 0;
  return VirtualProtect(reinterpret_cast<void*>(page_address),
                        end_address - page_address, prot, &old_prot) != 0;
}

bool VirtualMemory::FlushInstructionCache(void* address, intptr_t size) {
  if (address == nullptr || size <= 0 ||
      reinterpret_cast<uword>(address) > kUwordMax - static_cast<uword>(size)) {
    return false;
  }
#if defined(DART_ELF_LOADER_TESTING)
  if (TestHookForTesting() != nullptr &&
      !TestHookForTesting()(TestOperation::kFlush, address, size)) {
    return false;
  }
#endif
  return ::FlushInstructionCache(GetCurrentProcess(), address, size) != 0;
}

}  // namespace bin
}  // namespace dart

#endif  // defined(DART_HOST_OS_WINDOWS)
