// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_VM_ZONE_MEMORY_TAGGING_H_
#define RUNTIME_VM_ZONE_MEMORY_TAGGING_H_

namespace dart {
namespace internal {

// Keep the Linux ARM64 UAPI constants local so this check can also be tested on
// hosts without MTE. The callbacks do not execute any MTE instructions.
template <typename GetHwcap2, typename Prctl>
bool TryEnableArm64MemoryTagging(GetHwcap2 get_hwcap2, Prctl prctl) {
  constexpr unsigned long kHwcap2Mte = 1UL << 18;
  if ((get_hwcap2() & kHwcap2Mte) == 0) {
    return false;
  }

  constexpr int kSetTaggedAddrCtrl = 55;
  constexpr int kGetTaggedAddrCtrl = 56;
  constexpr unsigned long kTaggedAddrEnable = 1UL << 0;
  constexpr unsigned long kMteTcfSync = 1UL << 1;
  constexpr unsigned long kMteTcfAsync = 1UL << 2;
  constexpr unsigned long kMteTagShift = 3;
  constexpr unsigned long kFaultModes = kMteTcfSync | kMteTcfAsync;
  // Enable sync or async failures and allow every randomly generated tag but 0.
  constexpr unsigned long kControl =
      kTaggedAddrEnable | kFaultModes | (0xfffeUL << kMteTagShift);
  if (prctl(kSetTaggedAddrCtrl, kControl) != 0) {
    return false;
  }

  const int result = prctl(kGetTaggedAddrCtrl, 0UL);
  // PR_TAGGED_ADDR_ENABLE alone only permits tagged pointers in syscalls. It
  // neither advertises MTE instructions nor confirms MTE fault checking.
  return result >= 0 && (result & kTaggedAddrEnable) != 0 &&
         (result & kFaultModes) != 0;
}

}  // namespace internal
}  // namespace dart

#endif  // RUNTIME_VM_ZONE_MEMORY_TAGGING_H_
