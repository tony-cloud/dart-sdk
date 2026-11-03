// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/assembler/assembler_base.h"

#include "platform/utils.h"
#include "vm/compiler/assembler/object_pool_builder.h"
#if defined(DART_PRECOMPILER) && !defined(DART_PRECOMPILED_RUNTIME)
#include "vm/compiler/aot/link_info.h"
#endif
#include "vm/compiler/backend/slot.h"
#include "vm/cpu.h"
#include "vm/flags.h"
#include "vm/heap/heap.h"
#include "vm/memory_region.h"
#include "vm/os.h"
#include "vm/zone.h"

namespace dart {

DEFINE_FLAG(bool,
            check_code_pointer,
            false,
            "Verify instructions offset in code object."
            "NOTE: This breaks the profiler.");
#if defined(TARGET_ARCH_ARM)
DEFINE_FLAG(bool, use_far_branches, false, "Enable far branches for ARM.");
#endif

namespace compiler {

AssemblerBase::~AssemblerBase() {}

void AssemblerBase::LoadFromSlot(Register dst,
                                 Register base,
                                 const Slot& slot,
                                 MemoryOrder memory_order) {
  if (!slot.is_tagged()) {
    // The result cannot be a floating point or SIMD value.
    ASSERT(slot.representation() == kUntagged ||
           RepresentationUtils::IsUnboxedInteger(slot.representation()));
    // Since we only have a single destination register, the result value must
    // fit into a register.
    ASSERT(RepresentationUtils::ValueSize(slot.representation()) <=
           compiler::target::kWordSize);
    auto const sz = RepresentationUtils::OperandSize(slot.representation());
    if (slot.has_untagged_instance()) {
      LoadFromOffset(dst, base, slot.offset_in_bytes(), sz);
    } else {
      LoadFieldFromOffset(dst, base, slot.offset_in_bytes(), sz);
    }
  } else if (slot.has_untagged_instance()) {
    // Non-Dart objects do not contain compressed pointers.
    ASSERT(!slot.is_compressed());
    LoadFromOffset(dst, base, slot.offset_in_bytes());
  } else if (!slot.is_guarded_field() && slot.type().ToCid() == kSmiCid) {
    if (slot.is_compressed()) {
      LoadCompressedSmiFieldFromOffset(dst, base, slot.offset_in_bytes());
    } else {
      LoadSmiFieldFromOffset(dst, base, slot.offset_in_bytes());
    }
  } else {
    if (slot.is_compressed()) {
      if (memory_order == kAcquire) {
        LoadAcquireCompressedFieldFromOffset(dst, base, slot.offset_in_bytes());
      } else {
        LoadCompressedFieldFromOffset(dst, base, slot.offset_in_bytes());
      }
    } else {
      if (memory_order == kAcquire) {
        LoadAcquire(dst, FieldAddress(base, slot.offset_in_bytes()));
      } else {
        LoadFieldFromOffset(dst, base, slot.offset_in_bytes());
      }
    }
  }
}

void AssemblerBase::StoreToSlot(Register src,
                                Register base,
                                const Slot& slot,
                                MemoryOrder memory_order,
                                Register scratch) {
  auto const can_be_smi =
      slot.type().CanBeSmi() ? kValueCanBeSmi : kValueIsNotSmi;
  StoreToSlot(src, base, slot, can_be_smi, memory_order, scratch);
}

void AssemblerBase::StoreToSlot(Register src,
                                Register base,
                                const Slot& slot,
                                CanBeSmi can_be_smi,
                                MemoryOrder memory_order,
                                Register scratch) {
  if (!slot.is_tagged() || slot.has_untagged_instance()) {
    // Same as the no barrier case.
    StoreToSlotNoBarrier(src, base, slot, memory_order);
  } else if (slot.is_compressed()) {
    StoreCompressedIntoObjectOffset(base, slot.offset_in_bytes(), src,
                                    can_be_smi, memory_order, scratch);
  } else {
    StoreIntoObjectOffset(base, slot.offset_in_bytes(), src, can_be_smi,
                          memory_order, scratch);
  }
}

void AssemblerBase::StoreToSlotNoBarrier(Register src,
                                         Register base,
                                         const Slot& slot,
                                         MemoryOrder memory_order) {
  if (!slot.is_tagged()) {
    // The stored value cannot be a SIMD value.
    ASSERT(slot.representation() == kUntagged ||
           RepresentationUtils::IsUnboxedFloat(slot.representation()) ||
           RepresentationUtils::IsUnboxedInteger(slot.representation()));
    // Since we only have a single source register, the stored value must
    // fit into a register.
    ASSERT(RepresentationUtils::ValueSize(slot.representation()) <=
           compiler::target::kWordSize);
    auto const sz = RepresentationUtils::OperandSize(slot.representation());
    if (slot.has_untagged_instance()) {
      StoreToOffset(src, base, slot.offset_in_bytes(), sz);
    } else {
      StoreFieldToOffset(src, base, slot.offset_in_bytes(), sz);
    }
  } else if (slot.has_untagged_instance()) {
    // Non-Dart objects do not contain compressed pointers.
    ASSERT(!slot.is_compressed());
    StoreToOffset(src, base, slot.offset_in_bytes());
  } else if (slot.is_compressed()) {
    StoreCompressedIntoObjectOffsetNoBarrier(base, slot.offset_in_bytes(), src,
                                             memory_order);
  } else {
    StoreIntoObjectOffsetNoBarrier(base, slot.offset_in_bytes(), src,
                                   memory_order);
  }
}

void AssemblerBase::LoadFromOffset(Register dst,
                                   Register base,
                                   int32_t offset,
                                   OperandSize sz) {
  Load(dst, Address(base, offset), sz);
}

void AssemblerBase::StoreToOffset(Register src,
                                  Register base,
                                  int32_t offset,
                                  OperandSize sz) {
  Store(src, Address(base, offset), sz);
}

void AssemblerBase::LoadField(Register dst,
                              const FieldAddress& address,
                              OperandSize sz) {
  Load(dst, address, sz);
}
void AssemblerBase::LoadFieldFromOffset(Register dst,
                                        Register base,
                                        int32_t offset,
                                        OperandSize sz) {
  Load(dst, FieldAddress(base, offset), sz);
}

void AssemblerBase::StoreFieldToOffset(Register src,
                                       Register base,
                                       int32_t offset,
                                       OperandSize sz) {
  Store(src, FieldAddress(base, offset), sz);
}

void AssemblerBase::LoadSmiField(Register dst, const FieldAddress& address) {
  LoadSmi(dst, address);
}
void AssemblerBase::LoadSmiFromOffset(Register dst,
                                      Register base,
                                      int32_t offset) {
  LoadSmi(dst, Address(base, offset));
}
void AssemblerBase::LoadSmiFieldFromOffset(Register dst,
                                           Register base,
                                           int32_t offset) {
  LoadSmi(dst, FieldAddress(base, offset));
}

void AssemblerBase::LoadAcquireCompressedFromOffset(Register dst,
                                                    Register base,
                                                    int32_t offset) {
  LoadAcquireCompressed(dst, Address(base, offset));
}
void AssemblerBase::LoadAcquireCompressedFieldFromOffset(Register dst,
                                                         Register base,
                                                         int32_t offset) {
  LoadAcquireCompressed(dst, FieldAddress(base, offset));
}
void AssemblerBase::LoadCompressedField(Register dst,
                                        const FieldAddress& address) {
  LoadCompressed(dst, address);
}
void AssemblerBase::LoadCompressedFromOffset(Register dst,
                                             Register base,
                                             int32_t offset) {
  LoadCompressed(dst, Address(base, offset));
}
void AssemblerBase::LoadCompressedFieldFromOffset(Register dst,
                                                  Register base,
                                                  int32_t offset) {
  LoadCompressed(dst, FieldAddress(base, offset));
}
void AssemblerBase::LoadCompressedSmiField(Register dst,
                                           const FieldAddress& address) {
  LoadCompressedSmi(dst, address);
}
void AssemblerBase::LoadCompressedSmiFromOffset(Register dst,
                                                Register base,
                                                int32_t offset) {
  LoadCompressedSmi(dst, Address(base, offset));
}
void AssemblerBase::LoadCompressedSmiFieldFromOffset(Register dst,
                                                     Register base,
                                                     int32_t offset) {
  LoadCompressedSmi(dst, FieldAddress(base, offset));
}

void AssemblerBase::LoadAcquireFromOffset(Register dst,
                                          Register base,
                                          int32_t offset,
                                          OperandSize size) {
  LoadAcquire(dst, Address(base, offset), size);
}
void AssemblerBase::StoreReleaseToOffset(Register src,
                                         Register base,
                                         int32_t offset,
                                         OperandSize size) {
  StoreRelease(src, Address(base, offset), size);
}

void AssemblerBase::StoreIntoObject(Register object,
                                    const Address& address,
                                    Register value,
                                    CanBeSmi can_be_smi,
                                    MemoryOrder memory_order,
                                    Register scratch,
                                    OperandSize size) {
  // A write barrier should never be applied when writing a reference to an
  // object into itself.
  ASSERT(object != value);
  ASSERT(object != scratch);
  ASSERT(value != scratch);
  if (memory_order == kRelease) {
    StoreRelease(value, address, size);
  } else {
    Store(value, address, size);
  }
  StoreBarrier(object, value, can_be_smi, scratch);
}

void AssemblerBase::StoreIntoObjectNoBarrier(Register object,
                                             const Address& address,
                                             Register value,
                                             MemoryOrder memory_order,
                                             OperandSize size) {
  if (memory_order == kRelease) {
    StoreRelease(value, address, size);
  } else {
    Store(value, address, size);
  }
  DEBUG_ONLY(VerifyStoreNeedsNoWriteBarrier(object, value));
}

void AssemblerBase::StoreIntoObjectOffset(Register object,
                                          int32_t offset,
                                          Register value,
                                          CanBeSmi can_be_smi,
                                          MemoryOrder memory_order,
                                          Register scratch,
                                          OperandSize size) {
  StoreIntoObject(object, FieldAddress(object, offset), value, can_be_smi,
                  memory_order, scratch, size);
}

void AssemblerBase::StoreIntoObjectOffsetNoBarrier(Register object,
                                                   int32_t offset,
                                                   Register value,
                                                   MemoryOrder memory_order,
                                                   OperandSize size) {
  StoreIntoObjectNoBarrier(object, FieldAddress(object, offset), value,
                           memory_order, size);
}
void AssemblerBase::StoreObjectIntoObjectOffsetNoBarrier(
    Register object,
    int32_t offset,
    const Object& value,
    MemoryOrder memory_order,
    OperandSize size) {
  StoreObjectIntoObjectNoBarrier(object, FieldAddress(object, offset), value,
                                 memory_order, size);
}

void AssemblerBase::StoreIntoArray(Register object,
                                   Register slot,
                                   Register value,
                                   CanBeSmi can_be_smi,
                                   Register scratch,
                                   OperandSize size) {
  ASSERT(object != scratch);
  ASSERT(value != object);
  ASSERT(value != scratch);
  ASSERT(slot != object);
  ASSERT(slot != value);
  ASSERT(slot != scratch);
  Store(value, Address(slot, 0), size);
  ArrayStoreBarrier(object, slot, value, can_be_smi, scratch);
}

void AssemblerBase::UnrolledMemCopy(Register dst_base,
                                    intptr_t dst_offset,
                                    Register src_base,
                                    intptr_t src_offset,
                                    intptr_t size,
                                    Register temp) {
  intptr_t offset = 0;
  if (target::kWordSize >= 8) {
    while (offset + 8 <= size) {
      LoadFromOffset(temp, src_base, src_offset + offset, kEightBytes);
      StoreToOffset(temp, dst_base, dst_offset + offset, kEightBytes);
      offset += 8;
    }
  }
  while (offset + 4 <= size) {
    LoadFromOffset(temp, src_base, src_offset + offset, kUnsignedFourBytes);
    StoreToOffset(temp, dst_base, dst_offset + offset, kUnsignedFourBytes);
    offset += 4;
  }
  while (offset + 2 <= size) {
    LoadFromOffset(temp, src_base, src_offset + offset, kUnsignedTwoBytes);
    StoreToOffset(temp, dst_base, dst_offset + offset, kUnsignedTwoBytes);
    offset += 2;
  }
  while (offset + 1 <= size) {
    LoadFromOffset(temp, src_base, src_offset + offset, kUnsignedByte);
    StoreToOffset(temp, dst_base, dst_offset + offset, kUnsignedByte);
    offset += 1;
  }
  ASSERT(offset == size);
}

void AssemblerBase::LoadTypeClassId(Register dst, Register src) {
  if (dst != src) {
    EnsureHasClassIdInDEBUG(kTypeCid, src, dst);
  } else {
#if !defined(TARGET_ARCH_IA32)
    EnsureHasClassIdInDEBUG(kTypeCid, src, TMP);
#else
    // Skip check on IA32 since we don't have TMP.
#endif
  }
  LoadFromSlot(dst, src, Slot::AbstractType_flags());
  LsrImmediate(dst, compiler::target::UntaggedType::kTypeClassIdShift);
}

void AssemblerBase::LoadAbstractTypeNullability(Register dst, Register type) {
  LoadFromSlot(dst, type, Slot::AbstractType_flags());
  AndImmediate(dst, compiler::target::UntaggedAbstractType::kNullabilityMask);
}

void AssemblerBase::CompareAbstractTypeNullabilityWith(Register type,
                                                       int8_t value,
                                                       Register scratch) {
  LoadAbstractTypeNullability(scratch, type);
  CompareImmediate(scratch, value);
}

intptr_t AssemblerBase::InsertAlignedRelocation(BSS::Relocation reloc) {
  // We cannot put a relocation at the very start (it's not a valid
  // instruction)!
  ASSERT(CodeSize() != 0);

  // Align to a target word boundary.
  const intptr_t offset =
      Utils::RoundUp(CodeSize(), compiler::target::kWordSize);

  while (CodeSize() < offset) {
    Breakpoint();
  }
  ASSERT(CodeSize() == offset);

  AssemblerBuffer::EnsureCapacity ensured(&buffer_);
  buffer_.Emit<compiler::target::word>(BSS::RelocationIndex(reloc) *
                                       compiler::target::kWordSize);

  ASSERT(CodeSize() == (offset + compiler::target::kWordSize));

  return offset;
}

void AssemblerBase::MsanUnpoison(Register base, intptr_t length_in_bytes) {
  Comment("MsanUnpoison base %s length_in_bytes %" Pd,
          RegisterNames::RegisterName(base), length_in_bytes);
  LeafRuntimeScope rt(static_cast<Assembler*>(this), /*frame_size=*/0,
                      /*preserve_registers=*/true);
  MoveRegister(CallingConventions::ArgumentRegisters[0], base);
  LoadImmediate(CallingConventions::ArgumentRegisters[1], length_in_bytes);
  rt.Call(kMsanUnpoisonRuntimeEntry, /*argument_count=*/2);
}

void AssemblerBase::MsanUnpoison(Register base, Register length_in_bytes) {
  Comment("MsanUnpoison base %s length_in_bytes %s",
          RegisterNames::RegisterName(base),
          RegisterNames::RegisterName(length_in_bytes));
  LeafRuntimeScope rt(static_cast<Assembler*>(this), /*frame_size=*/0,
                      /*preserve_registers=*/true);
  const Register a0 = CallingConventions::ArgumentRegisters[0];
  const Register a1 = CallingConventions::ArgumentRegisters[1];
  if (length_in_bytes == a0) {
    if (base == a1) {
      MoveRegister(TMP, length_in_bytes);
      MoveRegister(a0, base);
      MoveRegister(a1, TMP);
    } else {
      MoveRegister(a1, length_in_bytes);
      MoveRegister(a0, base);
    }
  } else {
    MoveRegister(a0, base);
    MoveRegister(a1, length_in_bytes);
  }
  rt.Call(kMsanUnpoisonRuntimeEntry, /*argument_count=*/2);
}

#if defined(DEBUG)
static void InitializeMemoryWithBreakpoints(uword data, intptr_t length) {
#if defined(TARGET_ARCH_ARM) || defined(TARGET_ARCH_ARM64)
  ASSERT(Utils::IsAligned(data, 4));
  ASSERT(Utils::IsAligned(length, 4));
  const uword end = data + length;
  while (data < end) {
    *reinterpret_cast<int32_t*>(data) = Instr::kBreakPointInstruction;
    data += 4;
  }
#else
  memset(reinterpret_cast<void*>(data), Instr::kBreakPointInstruction, length);
#endif
}
#endif

static uword NewContents(intptr_t capacity) {
  Zone* zone = Thread::Current()->zone();
  uword result = zone->AllocUnsafe(capacity);
#if defined(DEBUG)
  // Initialize the buffer with kBreakPointInstruction to force a break
  // point if we ever execute an uninitialized part of the code buffer.
  InitializeMemoryWithBreakpoints(result, capacity);
#endif
  return result;
}

#if defined(DEBUG)
AssemblerBuffer::EnsureCapacity::EnsureCapacity(AssemblerBuffer* buffer) {
  if (buffer->cursor() >= buffer->limit()) buffer->ExtendCapacity();
  // In debug mode, we save the assembler buffer along with the gap
  // size before we start emitting to the buffer. This allows us to
  // check that any single generated instruction doesn't overflow the
  // limit implied by the minimum gap size.
  buffer_ = buffer;
  gap_ = ComputeGap();
  // Make sure that extending the capacity leaves a big enough gap
  // for any kind of instruction.
  ASSERT(gap_ >= kMinimumGap);
  // Mark the buffer as having ensured the capacity.
  ASSERT(!buffer->HasEnsuredCapacity());  // Cannot nest.
  buffer->has_ensured_capacity_ = true;
}

AssemblerBuffer::EnsureCapacity::~EnsureCapacity() {
  // Unmark the buffer, so we cannot emit after this.
  buffer_->has_ensured_capacity_ = false;
  // Make sure the generated instruction doesn't take up more
  // space than the minimum gap.
  intptr_t delta = gap_ - ComputeGap();
  ASSERT(delta <= kMinimumGap);
}
#endif

AssemblerBuffer::AssemblerBuffer()
    : pointer_offsets_(new ZoneGrowableArray<intptr_t>(16)) {
  const intptr_t kInitialBufferCapacity = 4 * KB;
  contents_ = NewContents(kInitialBufferCapacity);
  cursor_ = contents_;
  limit_ = ComputeLimit(contents_, kInitialBufferCapacity);
  fixup_ = nullptr;
#if defined(DEBUG)
  has_ensured_capacity_ = false;
  fixups_processed_ = false;
#endif

  // Verify internal state.
  ASSERT(Capacity() == kInitialBufferCapacity);
  ASSERT(Size() == 0);
}

AssemblerBuffer::~AssemblerBuffer() {}

void AssemblerBuffer::ProcessFixups(const MemoryRegion& region) {
  AssemblerFixup* fixup = fixup_;
  while (fixup != nullptr) {
    fixup->Process(region, fixup->position());
    fixup = fixup->previous();
  }
}

void AssemblerBuffer::FinalizeInstructions(const MemoryRegion& instructions) {
  // Copy the instructions from the buffer.
  MemoryRegion from(reinterpret_cast<void*>(contents()), Size());
  instructions.CopyFrom(0, from);

  // Process fixups in the instructions.
  ProcessFixups(instructions);
#if defined(DEBUG)
  fixups_processed_ = true;
#endif
}

void AssemblerBuffer::ExtendCapacity() {
  intptr_t old_size = Size();
  intptr_t old_capacity = Capacity();
  intptr_t new_capacity =
      Utils::Minimum(old_capacity * 2, old_capacity + 1 * MB);
  if (new_capacity < old_capacity) {
    FATAL("Unexpected overflow in AssemblerBuffer::ExtendCapacity");
  }

  // Allocate the new data area and copy contents of the old one to it.
  uword new_contents = NewContents(new_capacity);
  memmove(reinterpret_cast<void*>(new_contents),
          reinterpret_cast<void*>(contents_), old_size);

  // Compute the relocation delta and switch to the new contents area.
  intptr_t delta = new_contents - contents_;
  contents_ = new_contents;

  // Update the cursor and recompute the limit.
  cursor_ += delta;
  limit_ = ComputeLimit(new_contents, new_capacity);

  // Verify internal state.
  ASSERT(Capacity() == new_capacity);
  ASSERT(Size() == old_size);
}

class PatchCodeWithHandle : public AssemblerFixup {
 public:
  PatchCodeWithHandle(ZoneGrowableArray<intptr_t>* pointer_offsets,
                      const Object& object)
      : pointer_offsets_(pointer_offsets), object_(object) {}

  void Process(const MemoryRegion& region, intptr_t position) {
    // Patch the handle into the code. Once the instructions are installed into
    // a raw code object and the pointer offsets are setup, the handle is
    // resolved.
    region.StoreUnaligned<const Object*>(position, &object_);
    pointer_offsets_->Add(position);
  }

  virtual bool IsPointerOffset() const { return true; }

 private:
  ZoneGrowableArray<intptr_t>* pointer_offsets_;
  const Object& object_;
};

intptr_t AssemblerBuffer::CountPointerOffsets() const {
  intptr_t count = 0;
  AssemblerFixup* current = fixup_;
  while (current != nullptr) {
    if (current->IsPointerOffset()) ++count;
    current = current->previous_;
  }
  return count;
}

#if defined(TARGET_ARCH_IA32)
void AssemblerBuffer::EmitObject(const Object& object) {
  // Since we are going to store the handle as part of the fixup information
  // the handle needs to be a zone handle.
  DEBUG_ASSERT(IsNotTemporaryScopedHandle(object));
  ASSERT(IsInOldSpace(object));
  EmitFixup(new PatchCodeWithHandle(pointer_offsets_, object));
  cursor_ += target::kWordSize;  // Reserve space for pointer.
}
#endif

// Shared macros are implemented here.
void AssemblerBase::Unimplemented(const char* message) {
  const char* format = "Unimplemented: %s";
  const intptr_t len = Utils::SNPrint(nullptr, 0, format, message);
  char* buffer = reinterpret_cast<char*>(malloc(len + 1));
  Utils::SNPrint(buffer, len + 1, format, message);
  Stop(buffer);
}

void AssemblerBase::Untested(const char* message) {
  const char* format = "Untested: %s";
  const intptr_t len = Utils::SNPrint(nullptr, 0, format, message);
  char* buffer = reinterpret_cast<char*>(malloc(len + 1));
  Utils::SNPrint(buffer, len + 1, format, message);
  Stop(buffer);
}

void AssemblerBase::Unreachable(const char* message) {
  const char* format = "Unreachable: %s";
  const intptr_t len = Utils::SNPrint(nullptr, 0, format, message);
  char* buffer = reinterpret_cast<char*>(malloc(len + 1));
  Utils::SNPrint(buffer, len + 1, format, message);
  Stop(buffer);
}

void AssemblerBase::Comment(const char* format, ...) {
  if (EmittingComments()) {
    char buffer[1024];

    va_list args;
    va_start(args, format);
    Utils::VSNPrint(buffer, sizeof(buffer), format, args);
    va_end(args);

    comments_.Add(
        new CodeComment(buffer_.GetPosition(), AllocateString(buffer)));
  }
}

bool AssemblerBase::EmittingComments() {
  return FLAG_code_comments || FLAG_disassemble || FLAG_disassemble_optimized ||
         FLAG_disassemble_stubs;
}

void AssemblerBase::Stop(const char* message) {
  Comment("Stop: %s", message);
  Breakpoint();
}

uword ObjIndexPair::Hash(Key key) {
  switch (key.type()) {
    case ObjectPoolBuilderEntry::kImmediate128:
      return HashBytes(&key.imm128_, sizeof(key.imm128_));
#if defined(TARGET_ARCH_IS_32_BIT)
    case ObjectPoolBuilderEntry::kImmediate64:
      return HashBytes(&key.imm64_, sizeof(key.imm64_));
#endif
    case ObjectPoolBuilderEntry::kImmediate:
    case ObjectPoolBuilderEntry::kNativeFunction:
      return HashBytes(&key.imm_, sizeof(key.imm_));
    case ObjectPoolBuilderEntry::kTaggedObject:
      return ObjectHash(*key.obj_);
  }

  UNREACHABLE();
}

void ObjectPoolBuilder::Reset() {
  // Null out the handles we've accumulated.
  for (intptr_t i = 0; i < object_pool_.length(); ++i) {
    if (object_pool_[i].type() == ObjectPoolBuilderEntry::kTaggedObject) {
      SetToNull(const_cast<Object*>(object_pool_[i].obj_));
      SetToNull(const_cast<Object*>(object_pool_[i].equivalence_));
    }
  }

  object_pool_.Clear();
  object_pool_index_table_.Clear();
  link_reserved_start_ = link_reserved_end_ = 0;
  link_slot_used_.Clear();
  link_pins_.Clear();
}

void ObjectPoolBuilder::ReserveLinkBaseSlots() {
#if defined(DART_PRECOMPILER) && !defined(DART_PRECOMPILED_RUNTIME)
  ASSERT(parent_ == nullptr);
  if (!LinkInfo::HasBase()) return;
  const intptr_t start = CurrentLength();
  const intptr_t end = LinkInfo::BasePoolLength();
  if (LinkInfo::BaseStubPoolLength() != start) {
    LinkInfo::NoteMismatch("link: stub pool length %" Pd " differs from "
                           "release %" Pd "; object pool is not pinned",
                           start, LinkInfo::BaseStubPoolLength());
    return;
  }
  for (intptr_t i = start; i < end; i++) {
    object_pool_.Add(ObjectPoolBuilderEntry(
        0, ObjectPoolBuilderEntry::kImmediate,
        ObjectPoolBuilderEntry::kNotPatchable));
    link_slot_used_.Add(false);
  }
  link_reserved_start_ = start;
  link_reserved_end_ = end > start ? end : 0;
#endif
}

bool ObjectPoolBuilder::IsFreeLinkSlot(intptr_t index) const {
  return index >= link_reserved_start_ && index < link_reserved_end_ &&
         !link_slot_used_[index - link_reserved_start_];
}

void ObjectPoolBuilder::PlaceAtLinkSlot(intptr_t index,
                                        const ObjectPoolBuilderEntry& entry) {
  ASSERT(parent_ == nullptr);
  ASSERT(IsFreeLinkSlot(index));
  ObjectPoolBuilderEntry copy = entry;
  if (copy.type() == ObjectPoolBuilderEntry::kTaggedObject && zone_ != nullptr) {
    copy.obj_ = &NewZoneHandle(zone_, *copy.obj_);
    if (copy.equivalence_ != nullptr) {
      copy.equivalence_ = &NewZoneHandle(zone_, *copy.equivalence_);
    }
  }
  object_pool_[index] = copy;
  link_slot_used_[index - link_reserved_start_] = true;
  link_pins_.Add(index);
  if (copy.patchable() == ObjectPoolBuilderEntry::kNotPatchable) {
    object_pool_index_table_.Insert(ObjIndexPair(copy, index));
  }
}

intptr_t ObjectPoolBuilder::TryPlaceAtLinkSlot(
    const ObjectPoolBuilderEntry& entry) {
#if defined(DART_PRECOMPILER) && !defined(DART_PRECOMPILED_RUNTIME)
  ObjectPoolBuilder* root = link_root();
  if (root->link_reserved_end_ == 0) return -1;
  if (entry.type() != ObjectPoolBuilderEntry::kImmediate &&
      entry.type() != ObjectPoolBuilderEntry::kTaggedObject &&
      entry.type() != ObjectPoolBuilderEntry::kNativeFunction) {
    return -1;
  }
  const bool patchable =
      entry.patchable() == ObjectPoolBuilderEntry::kPatchable;
  const char* key =
      LinkInfo::PoolEntryKey(entry, link_context_, link_ordinal_);
  auto place = [&](intptr_t index) {
    root->PlaceAtLinkSlot(index, entry);
    // The per-code pool built from this builder records which global entries
    // the code uses (snapshot reachability), so include placed entries.
    if (parent_ != nullptr) used_from_parent_.Add(index);
    LinkInfo::RecordPoolKey(index, key);
    return index;
  };
  if (patchable) {
    // Patchable entries may be allocated in adjacent pairs that are loaded
    // with a single instruction (switchable calls, native calls, type
    // tests). A pair must be placed together or appended together, so an
    // entry is only placed if the following slot is free as well, and a
    // patchable request directly following a placed one goes next to it.
    if (link_context_ == nullptr) return -1;
    if (link_pair_index_ >= 0 && link_pair_ordinal_ == link_ordinal_ - 1) {
      const intptr_t index = link_pair_index_ + 1;
      link_pair_index_ = -1;
      ASSERT(root->IsFreeLinkSlot(index));
      return place(index);
    }
    link_pair_index_ = -1;
    if (link_appended_patchable_ordinal_ == link_ordinal_ - 1) {
      // Possibly the second half of an appended pair.
      link_appended_patchable_ordinal_ = link_ordinal_;
      return -1;
    }
    const intptr_t index = LinkInfo::BasePoolIndex(key);
    if (index == LinkInfo::kNone || !root->IsFreeLinkSlot(index) ||
        !root->IsFreeLinkSlot(index + 1)) {
      link_appended_patchable_ordinal_ = link_ordinal_;
      return -1;
    }
    link_pair_index_ = index;
    link_pair_ordinal_ = link_ordinal_;
    return place(index);
  }
  if (entry.type() == ObjectPoolBuilderEntry::kNativeFunction) return -1;
  const intptr_t index = LinkInfo::BasePoolIndex(key);
  if (index == LinkInfo::kNone || !root->IsFreeLinkSlot(index)) return -1;
  return place(index);
#else
  return -1;
#endif
}

void ObjectPoolBuilder::RecordLinkKey(intptr_t index,
                                      const ObjectPoolBuilderEntry& entry) {
#if defined(DART_PRECOMPILER) && !defined(DART_PRECOMPILED_RUNTIME)
  if (!LinkInfo::IsRecording()) return;
  LinkInfo::RecordPoolKey(
      index, LinkInfo::PoolEntryKey(entry, link_context_, link_ordinal_));
#endif
}

intptr_t ObjectPoolBuilder::AddObject(
    const Object& obj,
    ObjectPoolBuilderEntry::Patchability patchable,
    ObjectPoolBuilderEntry::SnapshotBehavior snapshot_behavior) {
  DEBUG_ASSERT(IsNotTemporaryScopedHandle(obj));
  LinkUseScope scope(this);
  return scope.Record(AddObjectImpl(
      ObjectPoolBuilderEntry(&obj, patchable, snapshot_behavior), true));
}

intptr_t ObjectPoolBuilder::AddImmediate(
    uword imm,
    ObjectPoolBuilderEntry::Patchability patchable,
    ObjectPoolBuilderEntry::SnapshotBehavior snapshotability) {
  LinkUseScope scope(this);
  return scope.Record(AddObjectImpl(
      ObjectPoolBuilderEntry(imm, ObjectPoolBuilderEntry::kImmediate,
                             patchable, snapshotability),
      true));
}

intptr_t ObjectPoolBuilder::AddImmediate64(uint64_t imm) {
#if defined(TARGET_ARCH_IS_32_BIT)
  return AddObject(
      ObjectPoolBuilderEntry(imm, ObjectPoolBuilderEntry::kImmediate64,
                             ObjectPoolBuilderEntry::kNotPatchable));
#else
  return AddImmediate(imm);
#endif
}

intptr_t ObjectPoolBuilder::AddImmediate128(simd128_value_t imm) {
  LinkUseScope scope(this);
  return scope.Record(AddObjectImpl(
      ObjectPoolBuilderEntry(imm, ObjectPoolBuilderEntry::kImmediate128,
                             ObjectPoolBuilderEntry::kNotPatchable),
      true));
}

intptr_t ObjectPoolBuilder::AddObject(ObjectPoolBuilderEntry entry) {
  return AddObjectImpl(entry, /*allow_link=*/false);
}

intptr_t ObjectPoolBuilder::AddObjectImpl(ObjectPoolBuilderEntry entry,
                                          bool allow_link) {
  DEBUG_ASSERT((entry.type() != ObjectPoolBuilderEntry::kTaggedObject) ||
               (IsNotTemporaryScopedHandle(*entry.obj_) &&
                (entry.equivalence_ == nullptr ||
                 IsNotTemporaryScopedHandle(*entry.equivalence_))));

  if (entry.type() == ObjectPoolBuilderEntry::kTaggedObject) {
    // If the owner of the object pool wrapper specified a specific zone we
    // should use we'll do so.
    if (zone_ != nullptr) {
      entry.obj_ = &NewZoneHandle(zone_, *entry.obj_);
      if (entry.equivalence_ != nullptr) {
        entry.equivalence_ = &NewZoneHandle(zone_, *entry.equivalence_);
      }
    }
  }

#if defined(TARGET_ARCH_IS_32_BIT)
  if (entry.type() == ObjectPoolBuilderEntry::kImmediate64) {
    ASSERT(entry.patchable() == ObjectPoolBuilderEntry::kNotPatchable);
    uint64_t imm = entry.imm64_;
    intptr_t idx = AddImmediate(Utils::Low32Bits(imm));
    AddImmediate(Utils::High32Bits(imm));
    object_pool_index_table_.Insert(ObjIndexPair(entry, idx));
    return idx;
  }
  if (entry.type() == ObjectPoolBuilderEntry::kImmediate128) {
    ASSERT(entry.patchable() == ObjectPoolBuilderEntry::kNotPatchable);
    intptr_t idx = AddImmediate(entry.imm128_.int_storage[0]);
    AddImmediate(entry.imm128_.int_storage[1]);
    AddImmediate(entry.imm128_.int_storage[2]);
    AddImmediate(entry.imm128_.int_storage[3]);
    object_pool_index_table_.Insert(ObjIndexPair(entry, idx));
    return idx;
  }
#else
  if (entry.type() == ObjectPoolBuilderEntry::kImmediate128) {
    ASSERT(entry.patchable() == ObjectPoolBuilderEntry::kNotPatchable);
    uword lo64 =
        (static_cast<uword>(entry.imm128_.int_storage[0]) & 0xffffffff) |
        (static_cast<uword>(entry.imm128_.int_storage[1]) << 32);
    uword hi64 =
        (static_cast<uword>(entry.imm128_.int_storage[2]) & 0xffffffff) |
        (static_cast<uword>(entry.imm128_.int_storage[3]) << 32);
    intptr_t idx = AddObjectImpl(
        ObjectPoolBuilderEntry(lo64, ObjectPoolBuilderEntry::kImmediate,
                               ObjectPoolBuilderEntry::kNotPatchable),
        false);
    AddObjectImpl(
        ObjectPoolBuilderEntry(hi64, ObjectPoolBuilderEntry::kImmediate,
                               ObjectPoolBuilderEntry::kNotPatchable),
        false);
    object_pool_index_table_.Insert(ObjIndexPair(entry, idx));
    return idx;
  }
#endif

  if (allow_link) {
    const intptr_t pinned = TryPlaceAtLinkSlot(entry);
    if (pinned >= 0) return pinned;
  }
  const intptr_t idx = base_index_ + object_pool_.length();
  object_pool_.Add(entry);
  if (allow_link) RecordLinkKey(idx, entry);
  if (entry.patchable() == ObjectPoolBuilderEntry::kNotPatchable) {
    // The object isn't patchable. Record the index for fast lookup.
    object_pool_index_table_.Insert(ObjIndexPair(entry, idx));
  }
  return idx;
}

intptr_t ObjectPoolBuilder::FindObject(ObjectPoolBuilderEntry entry) {
  LinkUseScope scope(this);
  // If the object is not patchable, check if we've already got it in the
  // object pool.
  if (entry.patchable() == ObjectPoolBuilderEntry::kNotPatchable) {
    // First check in the parent pool if we have one.
    if (parent_ != nullptr) {
      const intptr_t idx = parent_->object_pool_index_table_.LookupValue(entry);
      if (idx != ObjIndexPair::kNoIndex) {
        used_from_parent_.Add(idx);
        return scope.Record(idx);
      }
    }

    const intptr_t idx = object_pool_index_table_.LookupValue(entry);
    if (idx != ObjIndexPair::kNoIndex) {
      return scope.Record(idx);
    }
  }
  return scope.Record(AddObjectImpl(entry, /*allow_link=*/true));
}

intptr_t ObjectPoolBuilder::FindObject(
    const Object& obj,
    ObjectPoolBuilderEntry::Patchability patchable,
    ObjectPoolBuilderEntry::SnapshotBehavior snapshot_behavior) {
  return FindObject(ObjectPoolBuilderEntry(&obj, patchable, snapshot_behavior));
}

intptr_t ObjectPoolBuilder::FindObject(const Object& obj,
                                       const Object& equivalence) {
  return FindObject(ObjectPoolBuilderEntry(
      &obj, &equivalence, ObjectPoolBuilderEntry::kNotPatchable));
}

intptr_t ObjectPoolBuilder::FindImmediate(uword imm) {
  return FindObject(
      ObjectPoolBuilderEntry(imm, ObjectPoolBuilderEntry::kImmediate,
                             ObjectPoolBuilderEntry::kNotPatchable));
}

intptr_t ObjectPoolBuilder::FindImmediate64(uint64_t imm) {
#if defined(TARGET_ARCH_IS_32_BIT)
  return FindObject(
      ObjectPoolBuilderEntry(imm, ObjectPoolBuilderEntry::kImmediate64,
                             ObjectPoolBuilderEntry::kNotPatchable));
#else
  return FindImmediate(imm);
#endif
}

intptr_t ObjectPoolBuilder::FindImmediate128(simd128_value_t imm) {
  return FindObject(
      ObjectPoolBuilderEntry(imm, ObjectPoolBuilderEntry::kImmediate128,
                             ObjectPoolBuilderEntry::kNotPatchable));
}

intptr_t ObjectPoolBuilder::FindNativeFunction(
    const ExternalLabel* label,
    ObjectPoolBuilderEntry::Patchability patchable) {
  return FindObject(ObjectPoolBuilderEntry(
      label->address(), ObjectPoolBuilderEntry::kNativeFunction, patchable));
}

bool ObjectPoolBuilder::TryCommitToParent() {
  ASSERT(parent_ != nullptr);
  if (parent_->CurrentLength() != base_index_) {
    return false;
  }
  for (intptr_t i = 0; i < object_pool_.length(); i++) {
    intptr_t idx = parent_->AddObject(object_pool_[i]);
    ASSERT(idx == (base_index_ + i));
  }
  return true;
}

}  // namespace compiler

}  // namespace dart
