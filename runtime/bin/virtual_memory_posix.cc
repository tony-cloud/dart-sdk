// Copyright (c) 2021, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "platform/globals.h"
#if defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX) ||            \
    defined(DART_HOST_OS_MACOS)

#include "bin/virtual_memory.h"

#include <errno.h>
#if defined(DART_HOST_OS_MACOS)
#include <libkern/OSCacheControl.h>
#endif
#include <sys/mman.h>
#include <unistd.h>

#if defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX)
#include <sys/prctl.h>
#endif

#include "platform/assert.h"
#include "platform/utils.h"

namespace dart {
namespace bin {

// standard MAP_FAILED causes "error: use of old-style cast" as it
// defines MAP_FAILED as ((void *) -1)
#undef MAP_FAILED
#define MAP_FAILED reinterpret_cast<void*>(-1)

uword VirtualMemory::page_size_ = 0;

intptr_t VirtualMemory::CalculatePageSize() {
  const intptr_t page_size = getpagesize();
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

  const int prot = PROT_READ | PROT_WRITE | (is_executable ? PROT_EXEC : 0);

  int map_flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if (defined(DART_HOST_OS_MACOS) && !defined(DART_HOST_OS_IOS))
  if (is_executable) {
    map_flags |= MAP_JIT;
  }
#endif  // defined(DART_HOST_OS_MACOS)

  // Some 64-bit microarchitectures store only the low 32-bits of targets as
  // part of indirect branch prediction, predicting that the target's upper bits
  // will be same as the call instruction's address. This leads to misprediction
  // for indirect calls crossing a 4GB boundary. We ask mmap to place our
  // generated code near the VM binary to avoid this.
  void* hint = is_executable ? reinterpret_cast<void*>(&Allocate) : nullptr;
  void* address = mmap(hint, size, prot, map_flags, -1, 0);
#if defined(DART_HOST_OS_LINUX)
  // On WSL 1 trying to allocate memory close to the binary by supplying a hint
  // fails with ENOMEM for unclear reason. Some reports suggest that this might
  // be related to the alignment of the hint but aligning it by 64Kb does not
  // make the issue go away in our experiments. Instead just retry without any
  // hint.
  if (address == MAP_FAILED && hint != nullptr &&
      Utils::IsWindowsSubsystemForLinux()) {
    address = mmap(nullptr, size, prot, map_flags, -1, 0);
  }
#endif
  if (address == MAP_FAILED) {
    return nullptr;
  }

#if defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX)
  // PR_SET_VMA was only added to mainline Linux in 5.17, and some versions of
  // the Android NDK have incorrect headers, so we manually define it if absent.
#if !defined(PR_SET_VMA)
#define PR_SET_VMA 0x53564d41
#endif
#if !defined(PR_SET_VMA_ANON_NAME)
#define PR_SET_VMA_ANON_NAME 0
#endif
  prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, address, size, name);
#endif

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
    if (munmap(address_, size_) != 0) {
      int error = errno;
      const int kBufferSize = 1024;
      char error_buf[kBufferSize];
      FATAL("munmap error: %d (%s)", error,
            Utils::StrError(error, error_buf, kBufferSize));
    }
  }
}

void VirtualMemory::Protect(void* address, intptr_t size, Protection mode) {
  if (!TryProtect(address, size, mode)) {
    int error = errno;
    const int kBufferSize = 1024;
    char error_buf[kBufferSize];
    FATAL("mprotect error: %d (%s)", error,
          Utils::StrError(error, error_buf, kBufferSize));
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
  int prot = 0;
  switch (mode) {
    case kNoAccess:
      prot = PROT_NONE;
      break;
    case kReadOnly:
      prot = PROT_READ;
      break;
    case kReadWrite:
      prot = PROT_READ | PROT_WRITE;
      break;
    case kReadExecute:
      prot = PROT_READ | PROT_EXEC;
      break;
    case kReadWriteExecute:
      prot = PROT_READ | PROT_WRITE | PROT_EXEC;
      break;
  }
  return mprotect(reinterpret_cast<void*>(page_address),
                  end_address - page_address, prot) == 0;
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
#if defined(DART_HOST_OS_MACOS)
  sys_icache_invalidate(address, size);
#else
  __builtin___clear_cache(static_cast<char*>(address),
                          static_cast<char*>(address) + size);
#endif
  return true;
}

}  // namespace bin
}  // namespace dart

#endif  // defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX) ||     \
        // defined(DART_HOST_OS_MACOS)
