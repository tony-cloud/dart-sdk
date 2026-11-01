// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/zone.h"

#include <initializer_list>

#if defined(HOST_ARCH_ARM64) &&                                                \
    (defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX) ||           \
     defined(DART_HOST_OS_MACOS))
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX)
#include <sys/prctl.h>
#endif
#endif

#include "platform/address_sanitizer.h"
#include "platform/assert.h"
#include "platform/memory_sanitizer.h"
#include "vm/dart.h"
#include "vm/isolate.h"
#include "vm/unit_test.h"
#include "vm/zone_memory_tagging.h"

namespace dart {

namespace {

class FakeZoneMemoryTaggingSyscalls {
 public:
  unsigned long hwcap2 = 1UL << 18;
  int set_result = 0;
  bool get_succeeds = true;
  int accepted_control = 0x7fff7;
  // Android may enable tagged pointers without supporting MTE.
  int control = 1;
  int hwcap_calls = 0;
  int set_calls = 0;
  int get_calls = 0;

  bool Enable() {
    return internal::TryEnableArm64MemoryTagging(
        [&]() {
          EXPECT_EQ(0, set_calls);
          EXPECT_EQ(0, get_calls);
          hwcap_calls++;
          return hwcap2;
        },
        [&](int option, unsigned long flags) {
          EXPECT_EQ(1, hwcap_calls);
          if (option == 55) {
            EXPECT_EQ(0, set_calls);
            EXPECT_EQ(0, get_calls);
            EXPECT_EQ(0x7fff7UL, flags);
            set_calls++;
            if (set_result == 0) {
              control = accepted_control;
            }
            return set_result;
          }
          EXPECT_EQ(56, option);
          EXPECT_EQ(0UL, flags);
          EXPECT_EQ(1, set_calls);
          EXPECT_EQ(0, get_calls);
          get_calls++;
          return get_succeeds ? control : -1;
        });
  }
};

}  // namespace

UNIT_TEST_CASE(ZoneMemoryTaggingUnsupportedPreservesTaggedPointers) {
  // The second value was reported by an Android ARM64 emulator which crashed
  // executing STG after the old check mistook its tagged address ABI for MTE.
  for (unsigned long hwcap2 : {0UL, 1065345UL}) {
    FakeZoneMemoryTaggingSyscalls syscalls;
    syscalls.hwcap2 = hwcap2;
    EXPECT(!syscalls.Enable());
    EXPECT_EQ(1, syscalls.hwcap_calls);
    EXPECT_EQ(0, syscalls.set_calls);
    EXPECT_EQ(0, syscalls.get_calls);
    EXPECT_EQ(1, syscalls.control);
  }
}

UNIT_TEST_CASE(ZoneMemoryTaggingSetFailurePreservesTaggedPointers) {
  FakeZoneMemoryTaggingSyscalls syscalls;
  syscalls.set_result = -1;
  EXPECT(!syscalls.Enable());
  EXPECT_EQ(1, syscalls.set_calls);
  EXPECT_EQ(0, syscalls.get_calls);
  EXPECT_EQ(1, syscalls.control);
}

UNIT_TEST_CASE(ZoneMemoryTaggingGetFailure) {
  FakeZoneMemoryTaggingSyscalls syscalls;
  syscalls.get_succeeds = false;
  EXPECT(!syscalls.Enable());
  EXPECT_EQ(1, syscalls.set_calls);
  EXPECT_EQ(1, syscalls.get_calls);
  EXPECT_EQ(0x7fff7, syscalls.control);
}

UNIT_TEST_CASE(ZoneMemoryTaggingIncompleteControl) {
  // Tagged pointers alone, or fault modes without the tagged address ABI, do
  // not confirm that MTE was successfully enabled.
  for (int control : {0, 1, 2, 4, 6, 0x7fff1}) {
    FakeZoneMemoryTaggingSyscalls syscalls;
    syscalls.accepted_control = control;
    EXPECT(!syscalls.Enable());
    EXPECT_EQ(1, syscalls.set_calls);
    EXPECT_EQ(1, syscalls.get_calls);
    EXPECT_EQ(control, syscalls.control);
  }
}

UNIT_TEST_CASE(ZoneMemoryTaggingSupportedFaultModes) {
  for (int control : {3, 5, 7, 0x7fff7}) {
    FakeZoneMemoryTaggingSyscalls syscalls;
    syscalls.accepted_control = control;
    EXPECT(syscalls.Enable());
    EXPECT_EQ(1, syscalls.set_calls);
    EXPECT_EQ(1, syscalls.get_calls);
    EXPECT_EQ(control, syscalls.control);
  }
}

VM_UNIT_TEST_CASE(AllocateZone) {
#if defined(DEBUG)
  FLAG_trace_zones = true;
#endif
  TestCase::CreateTestIsolate();
  Thread* thread = Thread::Current();
  EXPECT(thread->zone() == nullptr);
  {
    TransitionNativeToVM transition(thread);
    StackZone stack_zone(thread);
    EXPECT(thread->zone() != nullptr);
    Zone* zone = stack_zone.GetZone();
    uintptr_t allocated_size = 0;

    // The loop is to make sure we overflow one segment and go on
    // to the next segment.
    for (int i = 0; i < 1000; i++) {
      uword first = zone->AllocUnsafe(2 * kWordSize);
      uword second = zone->AllocUnsafe(3 * kWordSize);
      EXPECT(first != second);
      allocated_size = ((2 + 3) * kWordSize);
    }
    EXPECT_LE(allocated_size, zone->SizeInBytes());

    // Test for allocation of large segments.
    const uword kLargeSize = 1 * MB;
    const uword kSegmentSize = 64 * KB;
    ASSERT(kLargeSize > kSegmentSize);
    for (int i = 0; i < 10; i++) {
      EXPECT(zone->AllocUnsafe(kLargeSize) != 0);
      allocated_size += kLargeSize;
    }
    EXPECT_LE(allocated_size, zone->SizeInBytes());

    // Test corner cases of kSegmentSize.
    uint8_t* buffer = nullptr;
    buffer =
        reinterpret_cast<uint8_t*>(zone->AllocUnsafe(kSegmentSize - kWordSize));
    EXPECT(buffer != nullptr);
    buffer[(kSegmentSize - kWordSize) - 1] = 0;
    allocated_size += (kSegmentSize - kWordSize);
    EXPECT_LE(allocated_size, zone->SizeInBytes());

    buffer = reinterpret_cast<uint8_t*>(
        zone->AllocUnsafe(kSegmentSize - (2 * kWordSize)));
    EXPECT(buffer != nullptr);
    buffer[(kSegmentSize - (2 * kWordSize)) - 1] = 0;
    allocated_size += (kSegmentSize - (2 * kWordSize));
    EXPECT_LE(allocated_size, zone->SizeInBytes());

    buffer =
        reinterpret_cast<uint8_t*>(zone->AllocUnsafe(kSegmentSize + kWordSize));
    EXPECT(buffer != nullptr);
    buffer[(kSegmentSize + kWordSize) - 1] = 0;
    allocated_size += (kSegmentSize + kWordSize);
    EXPECT_LE(allocated_size, zone->SizeInBytes());
  }
  EXPECT(thread->zone() == nullptr);
  Dart_ShutdownIsolate();
}

VM_UNIT_TEST_CASE(AllocGeneric_Success) {
#if defined(DEBUG)
  FLAG_trace_zones = true;
#endif
  TestCase::CreateTestIsolate();
  Thread* thread = Thread::Current();
  EXPECT(thread->zone() == nullptr);
  {
    TransitionNativeToVM transition(thread);
    StackZone zone(thread);
    EXPECT(thread->zone() != nullptr);
    uintptr_t allocated_size = 0;

    const intptr_t kNumElements = 1000;
    zone.GetZone()->Alloc<uint32_t>(kNumElements);
    allocated_size += sizeof(uint32_t) * kNumElements;
    EXPECT_LE(allocated_size, zone.SizeInBytes());
  }
  EXPECT(thread->zone() == nullptr);
  Dart_ShutdownIsolate();
}

// This test is expected to crash.
VM_UNIT_TEST_CASE_WITH_EXPECTATION(AllocGeneric_Overflow, "Crash") {
#if defined(DEBUG)
  FLAG_trace_zones = true;
#endif
  TestCase::CreateTestIsolate();
  Thread* thread = Thread::Current();
  EXPECT(thread->zone() == nullptr);
  {
    StackZone zone(thread);
    EXPECT(thread->zone() != nullptr);

    const intptr_t kNumElements = (kIntptrMax / sizeof(uint32_t)) + 1;
    zone.GetZone()->Alloc<uint32_t>(kNumElements);
  }
  Dart_ShutdownIsolate();
}

VM_UNIT_TEST_CASE(ZoneRealloc) {
  TestCase::CreateTestIsolate();
  Thread* thread = Thread::Current();
  {
    TransitionNativeToVM transition(thread);
    StackZone stack_zone(thread);
    auto zone = thread->zone();

    const intptr_t kOldLen = 32;
    const intptr_t kNewLen = 16;
    const intptr_t kNewLen2 = 16;

    auto data_old = zone->Alloc<uint8_t>(kOldLen);
    auto data_new = zone->Realloc<uint8_t>(data_old, kOldLen, kNewLen);
    RELEASE_ASSERT(data_old == data_new);

    auto data_new2 = zone->Realloc<uint8_t>(data_old, kNewLen, kNewLen2);
    RELEASE_ASSERT(data_old == data_new2);
  }
  Dart_ShutdownIsolate();
}

VM_UNIT_TEST_CASE(ZoneObject) {
#if defined(DEBUG)
  FLAG_trace_zones = true;
#endif
  TestCase::CreateTestIsolate();
  Thread* thread = Thread::Current();
  EXPECT(thread->zone() == nullptr);
  static int marker;

  class SimpleZoneObject : public ZoneObject {
   public:
    SimpleZoneObject() : slot(marker++) {}
    virtual ~SimpleZoneObject() {}
    virtual int GetSlot() { return slot; }
    int slot;
  };

  // Reset the marker.
  marker = 0;

  // Create a few zone allocated objects.
  {
    TransitionNativeToVM transition(thread);
    StackZone zone(thread);
    EXPECT_EQ(0UL, zone.SizeInBytes());
    SimpleZoneObject* first = new SimpleZoneObject();
    EXPECT(first != nullptr);
    SimpleZoneObject* second = zone.GetZone()->New<SimpleZoneObject>();
    EXPECT(second != nullptr);
    EXPECT(first != second);
    uintptr_t expected_size = (2 * sizeof(SimpleZoneObject));
    EXPECT_LE(expected_size, zone.SizeInBytes());

    // Make sure the constructors were invoked.
    EXPECT_EQ(0, first->slot);
    EXPECT_EQ(1, second->slot);

    // Make sure we can write to the members of the zone objects.
    first->slot = 42;
    second->slot = 87;
    EXPECT_EQ(42, first->slot);
    EXPECT_EQ(87, second->slot);
  }
  EXPECT(thread->zone() == nullptr);
  Dart_ShutdownIsolate();
}

TEST_CASE(PrintToString) {
  TransitionNativeToVM transition(Thread::Current());
  StackZone zone(Thread::Current());
  const char* result = zone.GetZone()->PrintToString("Hello %s!", "World");
  EXPECT_STREQ("Hello World!", result);
}

#if !defined(PRODUCT) && !defined(USING_ADDRESS_SANITIZER) &&                  \
    !defined(USING_MEMORY_SANITIZER)
// RSS hooks absent in PRODUCT mode. Scudo quarantine interferes RSS
// measurements under the sanitizers. Slack to allow for limited pooling
// in the malloc implementation.
static constexpr int64_t kRssSlack = 20 * MB;
#define CHECK_RSS
#endif

// clang-format off
static const size_t kSizes[] = {
  64 * KB,
  64 * KB + 2 * kWordSize,
  64 * KB - 2 * kWordSize,
  128 * KB,
  128 * KB + 2 * kWordSize,
  128 * KB - 2 * kWordSize,
  256 * KB,
  256 * KB + 2 * kWordSize,
  256 * KB - 2 * kWordSize,
  512 * KB,
  512 * KB + 2 * kWordSize,
  512 * KB - 2 * kWordSize,
};
// clang-format on

TEST_CASE(StressMallocDirectly) {
#if defined(CHECK_RSS)
  int64_t start_rss = Service::CurrentRSS();
#endif

  void* allocations[ARRAY_SIZE(kSizes)];
  for (size_t i = 0; i < ((3u * GB) / (512u * KB)); i++) {
    for (size_t j = 0; j < ARRAY_SIZE(kSizes); j++) {
      allocations[j] = malloc(kSizes[j]);
    }
    for (size_t j = 0; j < ARRAY_SIZE(kSizes); j++) {
      free(allocations[j]);
    }
  }

#if defined(CHECK_RSS)
  int64_t stop_rss = Service::CurrentRSS();
  EXPECT_LT(stop_rss, start_rss + kRssSlack);
#endif
}

ISOLATE_UNIT_TEST_CASE(StressMallocThroughZones) {
#if defined(CHECK_RSS)
  int64_t start_rss = Service::CurrentRSS();
#endif

  for (size_t i = 0; i < ((3u * GB) / (512u * KB)); i++) {
    StackZone stack_zone(Thread::Current());
    Zone* zone = stack_zone.GetZone();
    for (size_t j = 0; j < ARRAY_SIZE(kSizes); j++) {
      zone->Alloc<uint8_t>(kSizes[j]);
    }
  }

#if defined(CHECK_RSS)
  int64_t stop_rss = Service::CurrentRSS();
  EXPECT_LT(stop_rss, start_rss + kRssSlack);
#endif
}

#if defined(DART_COMPRESSED_POINTERS)
ISOLATE_UNIT_TEST_CASE(ZonesNotLimitedByCompressedHeap) {
  StackZone stack_zone(Thread::Current());
  Zone* zone = stack_zone.GetZone();

  size_t total = 0;
  while (total <= (4u * GB)) {
    size_t chunk_size = 512u * MB;
    zone->AllocUnsafe(chunk_size);
    total += chunk_size;
  }
}
#endif  // defined(DART_COMPRESSED_POINTERS)

ISOLATE_UNIT_TEST_CASE(ZoneVerificationScaling) {
  // This ought to complete in O(n), not O(n^2).
  const intptr_t n = 1000000;

  StackZone stack_zone(thread);
  Zone* zone = stack_zone.GetZone();

  {
    HANDLESCOPE(thread);
    for (intptr_t i = 0; i < n; i++) {
      const Object& a = Object::Handle(zone);
      DEBUG_ASSERT(!a.IsNotTemporaryScopedHandle());
      USE(a);
      const Object& b = Object::ZoneHandle(zone);
      DEBUG_ASSERT(b.IsNotTemporaryScopedHandle());
      USE(b);
    }
    // Leaves lots of HandleBlocks for recycling.
  }

  for (intptr_t i = 0; i < n; i++) {
    HANDLESCOPE(thread);
    const Object& a = Object::Handle(zone);
    DEBUG_ASSERT(!a.IsNotTemporaryScopedHandle());
    USE(a);
    const Object& b = Object::ZoneHandle(zone);
    DEBUG_ASSERT(b.IsNotTemporaryScopedHandle());
    USE(b);
    // Should not visit those recyclable blocks over and over again.
  }
}

ISOLATE_UNIT_TEST_CASE(ZoneWithMemoryTagging) {
  StackZone stack_zone(thread);
  Zone* zone = stack_zone.GetZone();
  volatile char* x = zone->Alloc<char>(16);
  x[0] = 42;
  x[15] = 43;
  EXPECT_EQ(42, x[0]);
  EXPECT_EQ(43, x[15]);

  if (!Zone::memory_tagging_enabled()) {
    EXPECT(Zone::use_initial_chunk());
    // Exercise the regular segment allocation path as well as the initial
    // chunk, without accessing beyond either allocation.
    constexpr intptr_t kSize = 1 * MB;
    volatile char* segment = zone->Alloc<char>(kSize);
    segment[0] = 44;
    segment[kSize - 1] = 45;
    EXPECT_EQ(44, segment[0]);
    EXPECT_EQ(45, segment[kSize - 1]);
    EXPECT_EQ(42, x[0]);
    EXPECT_EQ(43, x[15]);
    return;
  }

#if defined(HOST_ARCH_ARM64) &&                                                \
    (defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX) ||           \
     defined(DART_HOST_OS_MACOS))
  EXPECT(!Zone::use_initial_chunk());
  // Test listings happen before VM initialization, sometimes on a different
  // host. Check the actual target's MTE state here and require a real memory
  // fault in a child, rather than accepting both Pass and Crash for this test.
  // Allocate before fork: the child must not acquire inherited VM locks.
  const pid_t child = fork();
  if (child == 0) {
    const struct rlimit no_core = {0, 0};
    struct sigaction action = {};
    action.sa_handler = SIG_DFL;
    sigset_t signals;
    if (setrlimit(RLIMIT_CORE, &no_core) != 0 ||
        sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGSEGV, &action, nullptr) != 0 ||
        sigaction(SIGBUS, &action, nullptr) != 0 ||
        sigemptyset(&signals) != 0 || sigaddset(&signals, SIGSEGV) != 0 ||
        sigaddset(&signals, SIGBUS) != 0 ||
        sigprocmask(SIG_UNBLOCK, &signals, nullptr) != 0) {
      _exit(1);
    }
#if defined(DART_HOST_OS_ANDROID) || defined(DART_HOST_OS_LINUX)
    // Zone initialization permits either synchronous or asynchronous faults.
    // Require synchronous delivery in the child before its final _exit.
    constexpr int kGetTaggedAddrCtrl = 56;
    constexpr int kSetTaggedAddrCtrl = 55;
    constexpr unsigned long kMteTcfMask = 6;
    constexpr unsigned long kMteTcfSync = 2;
    const int control = prctl(kGetTaggedAddrCtrl, 0L, 0L, 0L, 0L);
    if (control < 0 ||
        prctl(
            kSetTaggedAddrCtrl,
            (static_cast<unsigned long>(control) & ~kMteTcfMask) | kMteTcfSync,
            0L, 0L, 0L) != 0) {
      _exit(1);
    }
    const int synchronous_control = prctl(kGetTaggedAddrCtrl, 0L, 0L, 0L, 0L);
    if (synchronous_control < 0 ||
        (synchronous_control & kMteTcfMask) != kMteTcfSync ||
        (synchronous_control & 1) == 0) {
      _exit(1);
    }
#endif
    // The next granule retains the segment's unallocated tag. Volatile keeps
    // the overflowing store observable even when the child immediately exits.
    x[16] = 0;
    _exit(0);
  }
  EXPECT(child > 0);
  if (child < 0) {
    return;
  }
  int status = 0;
  pid_t result = 0;
  // A synchronous memory fault should be immediate. Bound the wait so a child
  // setup problem cannot leave the test runner waiting indefinitely.
  for (int poll = 0; poll < 1000 && result == 0; poll++) {
    do {
      result = waitpid(child, &status, WNOHANG);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
      usleep(10000);
    }
  }
  if (result == 0) {
    EXPECT(result != 0);  // Timed out; still reap the child before returning.
    kill(child, SIGKILL);
    do {
      result = waitpid(child, &status, 0);
    } while (result == -1 && errno == EINTR);
  }
  EXPECT_EQ(child, result);
  EXPECT(WIFSIGNALED(status));
  if (WIFSIGNALED(status)) {
    EXPECT(WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
  }
#else
  // No other platform currently implements zone memory tagging.
  EXPECT(!Zone::memory_tagging_enabled());
#endif
}

}  // namespace dart
