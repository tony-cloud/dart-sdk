// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Release/patch layout link information for native code reuse.
//
// A release build (`--print_link_info_to=<file>`) records the layout facts
// that compiled code depends on: class ids, instance field offsets, static
// field ids, dispatch table selector offsets, global object pool slots and the
// placement of every compiled code payload in the instructions image.
//
// A patch build (`--link_base=<file>`) reads such a file and constrains the
// compilation of the new program so that every layout fact that still exists
// in the new program keeps the release value. Facts that cannot be kept get a
// fresh value outside the release's ranges. The patch build can record its
// own link info, which a linker compares against the release's to decide
// which compiled code may run as release native code.
//
// None of the decisions made here affect the correctness of the patch
// program itself: the patch is a normal, complete AOT snapshot compiled from
// the new program; the constraints only make it more likely that its code is
// byte-identical to the release's. See docs/ios-linker/COMPILER_LINKER.md in
// the workspace for the design.

#ifndef RUNTIME_VM_COMPILER_AOT_LINK_INFO_H_
#define RUNTIME_VM_COMPILER_AOT_LINK_INFO_H_

#if defined(DART_PRECOMPILED_RUNTIME)
#error "AOT runtime should not use compiler sources (including header files)"
#endif  // defined(DART_PRECOMPILED_RUNTIME)

#include "vm/allocation.h"
#include "vm/flags.h"
#include "vm/growable_array.h"

namespace dart {

class Class;
class Code;
class Field;
class Function;
class IsolateGroup;
class Object;
class Zone;

namespace compiler {
class ObjectPoolBuilder;
struct ObjectPoolBuilderEntry;
}  // namespace compiler

DECLARE_FLAG(charp, print_link_info_to);
DECLARE_FLAG(charp, link_base);
DECLARE_FLAG(bool, trace_link_info);

class LinkInfo : public AllStatic {
 public:
  static constexpr intptr_t kNone = -1;

  // Whether a release layout was given with --link_base.
  static bool HasBase();
  // Whether --print_link_info_to was given.
  static bool IsRecording();
  static bool IsActive() { return HasBase() || IsRecording(); }

  // Stable identity keys. Keys never contain tabs or newlines. Returned
  // strings are allocated in the current zone.
  static const char* ClassKey(const Class& cls);
  static const char* FieldKey(const Field& field);
  static const char* FunctionKey(const Function& function);
  static const char* CodeKey(const Code& code);
  // Content key of an object pool constant, or nullptr if the object has no
  // stable content key.
  static const char* ObjectKey(const Object& obj, intptr_t depth = 0);

  // ---- Release layout queries (patch build) ----
  static intptr_t BaseNumCids();
  static intptr_t BaseCid(const char* class_key);
  // Offset of an instance field in the release, and its unboxed size in bytes
  // (0 for a boxed field). Returns false if the field is not known.
  static bool BaseFieldLayout(const char* field_key,
                              intptr_t* offset,
                              intptr_t* unboxed_size);
  static intptr_t BaseTypeArgumentsOffset(const char* class_key);
  // Largest next-field offset of the class and all of its release subclasses.
  static intptr_t BaseSubtreeEnd(const char* class_key);
  static intptr_t BaseNumStaticFields();
  static intptr_t BaseStaticFieldId(const char* field_key);
  static intptr_t BaseSelectorOffset(const char* selector_key);
  static intptr_t BasePoolLength();
  static intptr_t BasePoolIndex(const char* pool_key);
  static intptr_t BaseStubPoolLength();

  // Whether static calls to [function] go through a DD slot (release DD
  // selection given with --link_dd_targets).
  static bool IsDDTarget(const Function& function);

  // Remaps static field ids of the current isolate group so that fields
  // known to the release keep their release ids. Must run before any code is
  // compiled.
  static void RemapStaticFieldIds(IsolateGroup* isolate_group);

  // Computes the pool key under which [entry] would be pinned. [context] is
  // the key of the function being compiled (or nullptr), [ordinal] the number
  // of context-keyed entries added so far by that compilation.
  static const char* PoolEntryKey(const compiler::ObjectPoolBuilderEntry& entry,
                                  const char* context,
                                  intptr_t ordinal);

  // ---- Recording ----
  static void RecordSelector(intptr_t id, intptr_t offset, const char* key);
  static void RecordPoolKey(intptr_t index, const char* key);
  static void ResetPoolKeys();
  static void RecordPoolUses(const char* code_key,
                             const GrowableArray<intptr_t>& uses);
  static void RecordPool(compiler::ObjectPoolBuilder* pool);
  static void RecordProgramTables(IsolateGroup* isolate_group);
  static void RecordCode(const char* image,
                         const Code& code,
                         intptr_t text_offset,
                         intptr_t payload_size);
  static void RecordMeta(const char* name, intptr_t value);
  static void Flush();

  // Prints a warning about a layout fact that could not be kept.
  static void NoteMismatch(const char* format, ...) PRINTF_ATTRIBUTE(1, 2);
};

}  // namespace dart

#endif  // RUNTIME_VM_COMPILER_AOT_LINK_INFO_H_
