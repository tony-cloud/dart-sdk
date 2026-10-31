// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Standalone memory-loader test: actual loader, allocator, protection, cache
// publication and Windows unwind APIs, with refusal hooks compiled only here.
// The synthetic ELF tests loader mechanics, not execution of a Dart snapshot.
#include "bin/elf_loader.h"
#include "bin/mappable.h"
#include "platform/elf.h"
#include "platform/unwinding_records.h"

#include <map>
#include <vector>

#if defined(DART_HOST_OS_MACOS)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#elif defined(DART_HOST_OS_LINUX)
#include <sys/mman.h>
#endif

#if !defined(DART_ELF_LOADER_TESTING)
#error This target requires private ELF loader test hooks.
#endif

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    fprintf(stderr, "ELF memory test failed: %s\n", message);
    abort();
  }
}

// 1=R, 2=W, 4=X. Inspect actual page permissions, including at copy time.
int Protection(void* address) {
#if defined(DART_HOST_OS_WINDOWS)
  MEMORY_BASIC_INFORMATION info;
  Check(VirtualQuery(address, &info, sizeof(info)) != 0, "VirtualQuery");
  switch (info.Protect & 0xff) {
    case PAGE_READONLY:
      return 1;
    case PAGE_READWRITE:
      return 3;
    case PAGE_EXECUTE_READ:
      return 5;
    case PAGE_EXECUTE_READWRITE:
      return 7;
    default:
      return 0;
  }
#elif defined(DART_HOST_OS_MACOS)
  mach_vm_address_t start = reinterpret_cast<mach_vm_address_t>(address);
  mach_vm_size_t size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object = MACH_PORT_NULL;
  Check(mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &count,
                       &object) == KERN_SUCCESS,
        "mach_vm_region");
  if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
  return info.protection;
#else
  FILE* maps = fopen("/proc/self/maps", "r");
  Check(maps != nullptr, "open maps");
  char line[1024];
  int result = 0;
  while (fgets(line, sizeof(line), maps) != nullptr) {
    uintptr_t start, end;
    char mode[5];
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, mode) ==
            3 &&
        reinterpret_cast<uintptr_t>(address) >= start &&
        reinterpret_cast<uintptr_t>(address) < end) {
      result = (mode[0] == 'r' ? 1 : 0) | (mode[1] == 'w' ? 2 : 0) |
               (mode[2] == 'x' ? 4 : 0);
      break;
    }
  }
  fclose(maps);
  return result;
#endif
}

using VM = dart::bin::VirtualMemory;
std::map<void*, intptr_t> mappings;
VM::TestOperation refused_operation = VM::TestOperation::kFree;
int refuse_at = 0;
int allocation_calls = 0;
int protect_calls = 0;
int flush_calls = 0;
int registrations = 0;
int unregistrations = 0;
int active_tables = 0;
void* registered_code = nullptr;
bool refuse_registration = false;

bool MemoryHook(VM::TestOperation operation, void* address, intptr_t size) {
  int call = 0;
  switch (operation) {
    case VM::TestOperation::kAllocate:
      call = ++allocation_calls;
      break;
    case VM::TestOperation::kAllocated:
      Check(Protection(address) == 3, "allocation must initially be RW");
      Check(mappings.emplace(address, size).second, "duplicate allocation");
      break;
    case VM::TestOperation::kProtect:
      call = ++protect_calls;
      Check(Protection(address) == 3, "copy must happen in RW, non-X pages");
      break;
    case VM::TestOperation::kFlush:
      call = ++flush_calls;
      Check(Protection(address) == 5, "flush must publish RX pages");
      break;
    case VM::TestOperation::kFree:
      if (registered_code != nullptr &&
          reinterpret_cast<uintptr_t>(registered_code) >=
              reinterpret_cast<uintptr_t>(address) &&
          reinterpret_cast<uintptr_t>(registered_code) -
                  reinterpret_cast<uintptr_t>(address) <
              static_cast<uintptr_t>(size)) {
        Check(active_tables == 0, "unregister before unmapping code");
      }
      Check(mappings.erase(address) == 1, "free must own a mapping");
      break;
  }
  return operation != refused_operation || call != refuse_at;
}

#if defined(UNWINDING_RECORDS_WINDOWS_HOST)
bool UnwindHook(dart::UnwindingRecordsPlatform::TestOperation operation,
                void* value) {
  using Operation = dart::UnwindingRecordsPlatform::TestOperation;
  switch (operation) {
    case Operation::kRegister:
      registered_code = value;
      return !refuse_registration;
    case Operation::kRegistered:
      ++registrations;
      ++active_tables;
      break;
    case Operation::kUnregister:
      Check(Protection(registered_code) == 5,
            "unregister while code is mapped");
      ++unregistrations;
      --active_tables;
      break;
  }
  return true;
}
#endif

void Reset() {
  Check(mappings.empty() && active_tables == 0,
        "previous load leaked resources");
  refused_operation = VM::TestOperation::kFree;
  refuse_at = allocation_calls = protect_calls = flush_calls = 0;
  registrations = unregistrations = 0;
  registered_code = nullptr;
  refuse_registration = false;
}

std::vector<uint8_t> Image() {
  namespace elf = dart::elf;
  const size_t page = VM::PageSize();
  size_t code_size = page;
#if defined(UNWINDING_RECORDS_WINDOWS_HOST)
  code_size = dart::Utils::RoundUp(
      static_cast<size_t>(dart::UnwindingRecordsPlatform::SizeInBytes()) + 64,
      page);
#endif
  std::vector<uint8_t> image(page * 2 + code_size);
  auto* header = reinterpret_cast<elf::ElfHeader*>(image.data());
  memcpy(header->ident, elf::ELFMAG, sizeof(elf::ELFMAG));
  header->ident[4] = sizeof(void*) == 8 ? elf::ELFCLASS64 : elf::ELFCLASS32;
  header->ident[elf::EI_DATA] = elf::ELFDATA2LSB;
  header->type = elf::ET_DYN;
#if defined(TARGET_ARCH_X64)
  header->machine = elf::EM_X86_64;
#elif defined(TARGET_ARCH_ARM64)
  header->machine = elf::EM_AARCH64;
#else
#error This standalone test currently covers desktop x64/arm64.
#endif
  header->version = elf::EV_CURRENT;
  header->header_size = sizeof(*header);
  header->program_table_offset = sizeof(*header);
  header->program_table_entry_size = sizeof(elf::ProgramHeader);
  header->num_program_headers = 3;
  header->section_table_offset = 512;
  header->section_table_entry_size = sizeof(elf::SectionHeader);
  header->num_section_headers = 4;
  header->shstrtab_section_index = 1;
  auto* programs = reinterpret_cast<elf::ProgramHeader*>(
      image.data() + header->program_table_offset);
  for (int i = 0; i < 3; ++i) {
    programs[i].type = elf::ProgramHeaderType::PT_LOAD;
    programs[i].file_offset = programs[i].memory_offset =
        i < 2 ? i * page : page + code_size;
    programs[i].file_size = programs[i].memory_size = i == 1 ? code_size : page;
    programs[i].alignment = page;
    programs[i].flags = elf::PF_R | (i == 1   ? elf::PF_X
                                     : i == 2 ? elf::PF_W
                                              : 0);
  }
  const char sections[] = "\0.shstrtab\0.dynstr\0.dynsym\0";
  const char symbols[] = "\0_kDartSnapshotData\0_kDartSnapshotText\0";
  memcpy(image.data() + 1024, sections, sizeof(sections));
  memcpy(image.data() + 1152, symbols, sizeof(symbols));
  auto* table = reinterpret_cast<elf::SectionHeader*>(image.data() + 512);
  table[1].name = 1;
  table[1].file_offset = 1024;
  table[1].file_size = sizeof(sections);
  table[2].name = 11;
  table[2].file_offset = table[2].memory_offset = 1152;
  table[2].file_size = sizeof(symbols);
  table[3].name = 19;
  table[3].file_offset = table[3].memory_offset = 1280;
  table[3].file_size = 3 * sizeof(elf::Symbol);
  auto* sym = reinterpret_cast<elf::Symbol*>(image.data() + 1280);
  sym[1].name = 1;
  sym[1].value = 1536;
  sym[2].name = 20;
  sym[2].value = page;
  image[1536] = 42;
  image[page] = 0x5a;
#if defined(UNWINDING_RECORDS_WINDOWS_HOST)
  const size_t record_offset =
      code_size - dart::UnwindingRecordsPlatform::SizeInBytes();
  auto* record = new (image.data() + page + record_offset)
      dart::CodeRangeUnwindingRecord();
  record->magic = dart::kUnwindingRecordMagic;
  record->runtime_function_count = 1;
  record->runtime_function[0].BeginAddress = 0;
  record->runtime_function[0].UnwindData =
      record_offset + offsetof(dart::CodeRangeUnwindingRecord, unwind_info);
#if defined(TARGET_ARCH_X64)
  record->runtime_function[0].EndAddress = record_offset;
#else
  record->unwind_info.unwind_info.FunctionLength =
      dart::kMaxFunctionLength >> 2;
  record->unwind_info1.unwind_info.FunctionLength = record_offset >> 2;
  record->runtime_function[0].UnwindData =
      record_offset + offsetof(dart::CodeRangeUnwindingRecord, unwind_info1);
#endif
#endif
  return image;
}

void LoadFailure(const std::vector<uint8_t>& image) {
  const char* error = nullptr;
  const uint8_t* data = nullptr;
  const uint8_t* text = nullptr;
  auto* loaded =
      Dart_LoadELF_Memory(image.data(), image.size(), &error, &data, &text);
  Check(loaded == nullptr && error != nullptr, "load must refuse with error");
  Check(data == nullptr && text == nullptr,
        "failure published snapshot pointers");
  Check(mappings.empty() && active_tables == 0, "failure leaked resources");
  Check(registrations == unregistrations, "failure leaked unwind tables");
}

void TestLoadAndUnload() {
  Reset();
  auto image = Image();
  const char* error = nullptr;
  const uint8_t* data = nullptr;
  const uint8_t* text = nullptr;
  auto* loaded =
      Dart_LoadELF_Memory(image.data(), image.size(), &error, &data, &text);
  Check(loaded != nullptr, error == nullptr ? "load failed" : error);
  Check(*data == 42 && *text == 0x5a, "wrong symbol mapping");
  Check(Protection(const_cast<uint8_t*>(data)) == 1, "data must be R");
  Check(Protection(const_cast<uint8_t*>(text)) == 5, "code must be RX");
  auto* header = reinterpret_cast<dart::elf::ElfHeader*>(image.data());
  auto* programs = reinterpret_cast<dart::elf::ProgramHeader*>(
      image.data() + header->program_table_offset);
  Check(Protection(const_cast<uint8_t*>(text) + programs[1].memory_size) == 3,
        "mutable segment must be RW");
  Check(flush_calls == 1, "executable copy must flush exactly once");
  memset(image.data(), 0, image.size());
  image.clear();
  image.shrink_to_fit();
  Check(*data == 42 && *text == 0x5a, "caller buffer was not borrowed only");
#if defined(UNWINDING_RECORDS_WINDOWS_HOST)
  Check(registrations == 1 && active_tables == 1,
        "missing unwind registration");
#endif
  Dart_UnloadELF(loaded);
  Check(mappings.empty() && active_tables == 0, "unload leaked resources");
  Check(registrations == unregistrations, "unload leaked unwind tables");
}

void TestFailures() {
  auto image = Image();
  for (int i = 1; i <= 4; ++i) {
    Reset();
    refused_operation = VM::TestOperation::kAllocate;
    refuse_at = i;
    LoadFailure(image);
  }
  for (int i = 1; i <= 6; ++i) {
    Reset();
    refused_operation = VM::TestOperation::kProtect;
    refuse_at = i;
    LoadFailure(image);
  }
  Reset();
  refused_operation = VM::TestOperation::kFlush;
  refuse_at = 1;
  LoadFailure(image);
  Reset();
  auto bad_range = image;
  auto* header = reinterpret_cast<dart::elf::ElfHeader*>(bad_range.data());
  auto* programs = reinterpret_cast<dart::elf::ProgramHeader*>(
      bad_range.data() + header->program_table_offset);
  programs[0].memory_offset = UINT64_MAX;
  LoadFailure(bad_range);
  Reset();
  bad_range = image;
  header = reinterpret_cast<dart::elf::ElfHeader*>(bad_range.data());
  header->program_table_offset = UINT64_MAX;
  LoadFailure(bad_range);
#if defined(UNWINDING_RECORDS_WINDOWS_HOST)
  Reset();
  refuse_registration = true;
  LoadFailure(image);
  auto* record = reinterpret_cast<dart::CodeRangeUnwindingRecord*>(
      image.data() + image.size() - VM::PageSize() -
      dart::UnwindingRecordsPlatform::SizeInBytes());
  Reset();
  record->magic = 0;
  LoadFailure(image);
  record->magic = dart::kUnwindingRecordMagic;
  Reset();
  record->runtime_function_count = UINT32_MAX;
  LoadFailure(image);
  record->runtime_function_count = 1;
  Reset();
  const auto unwind_data = record->runtime_function[0].UnwindData;
  record->runtime_function[0].UnwindData = UINT32_MAX;
  LoadFailure(image);
  record->runtime_function[0].UnwindData = unwind_data;
  Reset();
  record->runtime_function[0].BeginAddress = UINT32_MAX;
  LoadFailure(image);
  record->runtime_function[0].BeginAddress = 0;
#if defined(TARGET_ARCH_X64)
  Reset();
  const auto original_info = record->unwind_info;
  // UWOP_ALLOC_LARGE with OpInfo=1 requires three slots, but CountOfCodes=2.
  record->unwind_info.unwind_info.UnwindCode[0].UnwindOp = 1;
  record->unwind_info.unwind_info.UnwindCode[0].OpInfo = 1;
  LoadFailure(image);
  record->unwind_info = original_info;
  Reset();
  record->unwind_info.unwind_info.FrameRegister = 0;
  LoadFailure(image);
  record->unwind_info = original_info;
  Reset();
  record->unwind_info.unwind_info.SizeOfProlog = UINT8_MAX;
  LoadFailure(image);
  record->unwind_info = original_info;
#else
  Reset();
  const auto original_info = record->unwind_info1;
  record->unwind_info1.unwind_codes[0] = 0xffffffff;
  LoadFailure(image);
  record->unwind_info1 = original_info;
  Reset();
  record->unwind_info1.unwind_info.CodeWords = 2;
  LoadFailure(image);
  record->unwind_info1 = original_info;
#endif
  void* table = reinterpret_cast<void*>(1);
  Check(!dart::UnwindingRecordsPlatform::TryRegisterExecutableMemory(
            image.data(), 1, &table) &&
            table == nullptr,
        "truncated unwind records must clear output and refuse");
#endif
}

#if defined(DART_HOST_OS_WINDOWS)
// Run only in a separate --acg-denial process. Never opt a thread out or relax
// the process policy, including after the expected load refusal.
void TestAcgDenial() {
  Reset();
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY requested = {};
  requested.ProhibitDynamicCode = 1;
  Check(SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &requested,
                                   sizeof(requested)) != 0,
        "cannot enable ACG; this negative control must not skip");
  PROCESS_MITIGATION_DYNAMIC_CODE_POLICY actual = {};
  Check(
      GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy,
                                 &actual, sizeof(actual)) != 0 &&
          actual.Flags == requested.Flags,
      "ACG must prohibit dynamic code with all opt-out/audit bits zero");
  auto image = Image();
  Check(refuse_at == 0 && !refuse_registration, "ACG test injected a refusal");
  LoadFailure(image);
  Check(allocation_calls == 2 && protect_calls == 3 && flush_calls == 0 &&
            registrations == 0 && unregistrations == 0 && active_tables == 0,
        "ACG must refuse actual RX protection before cache/unwind publication");
  actual.Flags = 0;
  Check(
      GetProcessMitigationPolicy(GetCurrentProcess(), ProcessDynamicCodePolicy,
                                 &actual, sizeof(actual)) != 0 &&
          actual.Flags == requested.Flags,
      "ELF refusal changed the ACG policy");
  puts("ELF memory loader ACG denial test passed");
}
#endif

void TestBoundsAndOwnedCopy() {
  Reset();
  const uint8_t source[] = {1, 2, 3};
  dart::bin::MemoryMappable memory(source, sizeof(source));
  uint8_t dest[4] = {};
  Check(!memory.ReadFully(dest, -1), "negative read");
  Check(!memory.ReadFully(dest, INT64_MAX), "oversized read");
  Check(memory.ReadFully(dest, 2) && dest[0] == 1 && dest[1] == 2,
        "read prefix");
  Check(memory.ReadFully(dest, 1) && dest[0] == 3,
        "read must advance position");
  Check(!memory.ReadFully(dest, 1), "read past end");
  Check(memory.SetPosition(sizeof(source)) && memory.ReadFully(nullptr, 0),
        "empty read at end");
  Check(!memory.SetPosition(UINT64_MAX), "oversized position");
  dart::bin::MemoryMappable invalid(
      reinterpret_cast<const uint8_t*>(UINTPTR_MAX), 2);
  Check(!invalid.SetPosition(0) && !invalid.ReadFully(dest, 1) &&
            invalid.Map(dart::bin::File::kReadOnly, 0, 1) == nullptr,
        "source address overflow");
  Check(memory.Map(dart::bin::File::kReadOnly, 0, UINT64_MAX) == nullptr,
        "round-up overflow");
  Check(memory.Map(dart::bin::File::kReadOnly, UINT64_MAX, 1) == nullptr,
        "copy position overflow");
  for (auto operation :
       {VM::TestOperation::kProtect, VM::TestOperation::kFlush}) {
    Reset();
    refused_operation = operation;
    refuse_at = 1;
    Check(memory.Map(dart::bin::File::kReadExecute, 0, 7) == nullptr,
          "owned copy must refuse");
    Check(mappings.empty(), "owned copy refusal must unmap allocation");
  }
  Reset();
  auto* mapped = memory.Map(dart::bin::File::kReadOnly, 1, 7);
  Check(mapped != nullptr, "owned copy");
  auto* bytes = static_cast<uint8_t*>(mapped->address());
  Check(bytes[0] == 2 && bytes[1] == 3 && bytes[2] == 0 && bytes[6] == 0,
        "copy must zero-fill beyond source");
  delete mapped;
  Check(mappings.empty(), "owned copy unload");
  Check(!VM::TryProtect(nullptr, 1, VM::kReadOnly), "null protect");
  Check(!VM::TryProtect(reinterpret_cast<void*>(UINTPTR_MAX), 2, VM::kReadOnly),
        "protect address overflow");
}

}  // namespace

// The standalone target deliberately excludes the VM and disk-file machinery.
// These unused factories refuse file inputs; the memory factory is identical to
// mappable.cc. MappedMemory uses the platform unmap operation and observes frees.
namespace dart {
namespace bin {
Mappable* Mappable::FromPath(const char* path) {
  return nullptr;
}
#if defined(DART_HOST_OS_LINUX)
Mappable* Mappable::FromFD(int fd) {
  return nullptr;
}
#endif
Mappable* Mappable::FromMemory(const uint8_t* memory, size_t size) {
  return new MemoryMappable(memory, size);
}
void MappedMemory::Unmap() {
  MemoryHook(VM::TestOperation::kFree, address(), size());
#if defined(DART_HOST_OS_WINDOWS)
  Check(VirtualFree(address(), 0, MEM_RELEASE) != 0, "unmap copy");
#else
  Check(munmap(address(), size()) == 0, "unmap copy");
#endif
}
}  // namespace bin
}  // namespace dart

DART_EXPORT void Dart_DumpNativeStackTrace(void* context) {}
DART_EXPORT void Dart_PrepareToAbort() {}

int main(int argc, char** argv) {
  VM::Init();
  VM::TestHookForTesting() = MemoryHook;
#if defined(UNWINDING_RECORDS_WINDOWS_HOST)
  dart::UnwindingRecordsPlatform::TestHookForTesting() = UnwindHook;
#endif
  if (argc == 2 && strcmp(argv[1], "--acg-denial") == 0) {
#if defined(DART_HOST_OS_WINDOWS)
    TestAcgDenial();
    return 0;
#else
    Check(false, "--acg-denial requires Windows");
#endif
  }
  Check(argc == 1, "usage: elf_loader_memory_test [--acg-denial]");
  TestLoadAndUnload();
  TestFailures();
  TestBoundsAndOwnedCopy();
  puts("ELF memory loader tests passed");
  return 0;
}
