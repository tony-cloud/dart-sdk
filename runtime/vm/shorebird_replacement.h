// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
#ifndef RUNTIME_VM_SHOREBIRD_REPLACEMENT_H_
#define RUNTIME_VM_SHOREBIRD_REPLACEMENT_H_

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "vm/globals.h"

namespace dart {

// Version 1 is deliberately a complete, closed mutable domain. No declaration
// outside dart:* may be inherited from the inactive AOT application.
struct ShorebirdReplacementManifest {
  static constexpr intptr_t kMaxManifest = 4 * MB;
  static constexpr intptr_t kMaxComponent = 128 * MB;
  static constexpr intptr_t kMaxUri = 16 * KB;
  static constexpr uint32_t kMaxLibraries = 65536;
  static constexpr intptr_t kHeaderSize = 24;

  uint8_t host[32];
  uint8_t baseline[32];
  uint8_t current[32];
  std::string root;
  std::vector<std::string> base;
  std::vector<std::string> updated;
  std::vector<std::string> removed;

  static bool IsWrapper(const uint8_t* bytes, intptr_t size) {
    static const uint8_t magic[] = {'S', 'B', 'R', 'P', 'L', 'C', 'E', 0};
    return size >= 8 && memcmp(bytes, magic, sizeof(magic)) == 0;
  }

  static uint32_t UInt32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
  }

  // Reject overlong encodings, surrogates, NUL and non-Unicode scalar values.
  static bool ValidUtf8(const uint8_t* bytes, intptr_t length) {
    for (intptr_t i = 0; i < length;) {
      uint32_t scalar = bytes[i++];
      if (scalar == 0) return false;
      if (scalar < 0x80) continue;
      intptr_t extra;
      uint32_t minimum;
      if (scalar >= 0xc2 && scalar <= 0xdf) {
        scalar &= 0x1f;
        extra = 1;
        minimum = 0x80;
      } else if (scalar >= 0xe0 && scalar <= 0xef) {
        scalar &= 0x0f;
        extra = 2;
        minimum = 0x800;
      } else if (scalar >= 0xf0 && scalar <= 0xf4) {
        scalar &= 7;
        extra = 3;
        minimum = 0x10000;
      } else {
        return false;
      }
      if (extra > length - i) return false;
      for (intptr_t j = 0; j < extra; ++j) {
        if ((bytes[i] & 0xc0) != 0x80) return false;
        scalar = (scalar << 6) | (bytes[i++] & 0x3f);
      }
      if (scalar < minimum || scalar > 0x10ffff ||
          (scalar >= 0xd800 && scalar <= 0xdfff))
        return false;
    }
    return true;
  }

  class Cursor {
   public:
    Cursor(const uint8_t* bytes, intptr_t size) : bytes_(bytes), left_(size) {}
    bool Bytes(uint8_t* out, intptr_t size) {
      if (size < 0 || size > left_) return false;
      memcpy(out, bytes_, size);
      bytes_ += size;
      left_ -= size;
      return true;
    }
    bool UInt(uint32_t* value) {
      if (left_ < 4) return false;
      *value = UInt32(bytes_);
      bytes_ += 4;
      left_ -= 4;
      return true;
    }
    bool Uri(std::string* value) {
      uint32_t size;
      if (!UInt(&size) || size == 0 || size > kMaxUri ||
          static_cast<intptr_t>(size) > left_ ||
          !ValidUtf8(bytes_, size))
        return false;
      value->assign(reinterpret_cast<const char*>(bytes_), size);
      bytes_ += size;
      left_ -= size;
      return true;
    }
    bool Set(std::vector<std::string>* values) {
      uint32_t count;
      if (!UInt(&count) || count > kMaxLibraries ||
          static_cast<intptr_t>(count) > left_ / 5) {
        return false;
      }
      values->clear();
      for (uint32_t i = 0; i < count; ++i) {
        std::string value;
        if (!Uri(&value) || value.compare(0, 5, "dart:") == 0 ||
            value == "vm:ffi:native-assets" ||
            (i != 0 && values->back() >= value))
          return false;
        values->push_back(value);
      }
      return true;
    }
    bool done() const { return left_ == 0; }

   private:
    const uint8_t* bytes_;
    intptr_t left_;
  };

  bool Parse(const uint8_t* bytes, intptr_t size) {
    if (size <= 0 || size > kMaxManifest) return false;
    Cursor cursor(bytes, size);
    uint32_t policy;
    if (!cursor.UInt(&policy) || policy != 1 || !cursor.Bytes(host, 32) ||
        !cursor.Bytes(baseline, 32) || !cursor.Bytes(current, 32) ||
        !cursor.Uri(&root) || root.compare(0, 5, "dart:") == 0 ||
        !cursor.Set(&base) || !cursor.Set(&updated) || !cursor.Set(&removed) ||
        !cursor.done())
      return false;
    bool found_root = false;
    for (const auto& uri : updated)
      found_root |= uri == root;
    if (!found_root || base.empty()) return false;
    // R must be exactly B minus U. Newly introduced U entries are permitted.
    intptr_t r = 0;
    for (const auto& uri : base) {
      const bool present =
          std::binary_search(updated.begin(), updated.end(), uri);
      if (!present &&
          (r >= static_cast<intptr_t>(removed.size()) || removed[r++] != uri)) {
        return false;
      }
    }
    return r == static_cast<intptr_t>(removed.size());
  }
  bool IsBaseline() const {
    return base == updated && removed.empty() &&
           memcmp(baseline, current, 32) == 0;
  }
  bool MatchesBaseline(const ShorebirdReplacementManifest& other) const {
    return other.IsBaseline() && memcmp(host, other.host, 32) == 0 &&
           memcmp(baseline, other.baseline, 32) == 0 && root == other.root &&
           base == other.base;
  }
};

// Reject direct public API handles to inactive declarations and objects. This
// does not make arbitrary graphs retained by a native embedder safe: embedders
// must install before allowing app objects into SDK or native caches.
class Object;
bool IsInactiveShorebirdReplacementObject(const Object& object);

}  // namespace dart
#endif  // RUNTIME_VM_SHOREBIRD_REPLACEMENT_H_
