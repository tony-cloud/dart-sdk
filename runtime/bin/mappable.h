// Copyright (c) 2025, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef RUNTIME_BIN_MAPPABLE_H_
#define RUNTIME_BIN_MAPPABLE_H_

#include "platform/globals.h"

#include "bin/file.h"
#include "bin/virtual_memory.h"

namespace dart {
namespace bin {

class Mappable {
 public:
  static Mappable* FromPath(const char* path);
#if defined(DART_HOST_OS_FUCHSIA) || defined(DART_HOST_OS_LINUX)
  static Mappable* FromFD(int fd);
#endif
  static Mappable* FromMemory(const uint8_t* memory, size_t size);

  virtual MappedMemory* Map(File::MapType type,
                            uint64_t position,
                            uint64_t length,
                            void* start = nullptr) = 0;

  virtual bool SetPosition(uint64_t position) = 0;
  virtual bool ReadFully(void* dest, int64_t length) = 0;

  virtual ~Mappable() {}

 protected:
  Mappable() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(Mappable);
};

class FileMappable : public Mappable {
 public:
  explicit FileMappable(File* file) : Mappable(), file_(file) {}

  ~FileMappable() override { file_->Release(); }

  MappedMemory* Map(File::MapType type,
                    uint64_t position,
                    uint64_t length,
                    void* start = nullptr) override {
    return file_->Map(type, position, length, start);
  }

  bool SetPosition(uint64_t position) override {
    return file_->SetPosition(position);
  }

  bool ReadFully(void* dest, int64_t length) override {
    return file_->ReadFully(dest, length);
  }

 private:
  File* const file_;
  DISALLOW_COPY_AND_ASSIGN(FileMappable);
};

class MemoryMappable : public Mappable {
 public:
  MemoryMappable(const uint8_t* memory, size_t size)
      : Mappable(), memory_(memory), size_(size), position_(0) {}

  ~MemoryMappable() override {}

  MappedMemory* Map(File::MapType type,
                    uint64_t position,
                    uint64_t length,
                    void* start = nullptr) override {
    const uint64_t page_mask = VirtualMemory::PageSize() - 1;
    if (!HasValidSourceRange() || position > size_ || length == 0 ||
        length > static_cast<uint64_t>(kWordMax) - page_mask) {
      return nullptr;
    }
    const intptr_t map_size = Utils::RoundUp(length, VirtualMemory::PageSize());
    if (start != nullptr &&
        (reinterpret_cast<uword>(start) > kUwordMax - map_size ||
         !Utils::IsAligned(start, VirtualMemory::PageSize()))) {
      return nullptr;
    }
    MappedMemory* result = nullptr;
    if (start == nullptr) {
      // Copy into non-executable memory even for an eventual RX mapping.
      auto* memory = VirtualMemory::Allocate(
          map_size, false, "dart-compiled-image",
          /*executable_capable=*/type == File::kReadExecute);
      if (memory == nullptr) return nullptr;
      result = new MappedMemory(memory->address(), memory->size());
      memory->release();
      delete memory;
    } else {
      result = new MappedMemory(start, map_size, /*should_unmap=*/false);
    }

    const size_t copy_size = static_cast<size_t>(
        Utils::Minimum(length, static_cast<uint64_t>(size_ - position)));
    memcpy(result->address(), memory_ + position, copy_size);  // NOLINT
    memset(static_cast<uint8_t*>(result->address()) + copy_size, 0,
           static_cast<size_t>(length) - copy_size);

    auto mode = VirtualMemory::kReadOnly;
    switch (type) {
      case File::kReadExecute:
        mode = VirtualMemory::kReadExecute;
        break;
      case File::kReadWrite:
        mode = VirtualMemory::kReadWrite;
        break;
      case File::kReadOnly:
        mode = VirtualMemory::kReadOnly;
        break;
      default:
        UNREACHABLE();
    }

    if (!VirtualMemory::TryProtect(result->address(), result->size(), mode) ||
        (type == File::kReadExecute &&
         !VirtualMemory::FlushInstructionCache(result->address(),
                                               result->size()))) {
      delete result;
      return nullptr;
    }

    return result;
  }

  bool SetPosition(uint64_t position) override {
    if (!HasValidSourceRange() || position > size_) return false;
    position_ = static_cast<size_t>(position);
    return true;
  }

  bool ReadFully(void* dest, int64_t length) override {
    if (!HasValidSourceRange() || length < 0 ||
        static_cast<uint64_t>(length) > size_ - position_ ||
        (length != 0 && (memory_ == nullptr || dest == nullptr))) {
      return false;
    }
    if (length != 0) {
      memcpy(dest, memory_ + position_, static_cast<size_t>(length));
    }
    position_ += static_cast<size_t>(length);
    return true;
  }

 private:
  bool HasValidSourceRange() const {
    return memory_ != nullptr && size_ <= static_cast<size_t>(kWordMax) &&
           reinterpret_cast<uword>(memory_) <= kUwordMax - size_;
  }

  const uint8_t* const memory_;
  const size_t size_;
  size_t position_;
  DISALLOW_COPY_AND_ASSIGN(MemoryMappable);
};

}  // namespace bin
}  // namespace dart

#endif  // RUNTIME_BIN_MAPPABLE_H_
