// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/compiler/aot/link_info.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "platform/text_buffer.h"
#include "platform/utils.h"
#include "vm/class_table.h"
#include "vm/compiler/assembler/object_pool_builder.h"
#include "vm/dart.h"
#include "vm/field_table.h"
#include "vm/hash_map.h"
#include "vm/heap/heap.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/os.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"
#include "vm/zone_text_buffer.h"

namespace dart {

DEFINE_FLAG(charp,
            print_link_info_to,
            nullptr,
            "Write release/patch link information (class ids, field offsets, "
            "static field ids, selector offsets, object pool slots and code "
            "placement) to the given file.");
DEFINE_FLAG(charp,
            link_base,
            nullptr,
            "Compile against the release layout recorded in the given link "
            "info file, keeping class ids, field offsets, static field ids, "
            "dispatch table offsets and object pool slots where possible.");
DEFINE_FLAG(bool,
            trace_link_info,
            false,
            "Print layout facts that could not be kept for a patch build.");

namespace {

using CStringIntMap = MallocDirectChainedHashMap<CStringIntMapKeyValueTrait>;

// Base (release) layout as read from --link_base.
struct BaseLayout {
  CStringIntMap class_cid;
  CStringIntMap class_type_args;
  CStringIntMap class_subtree_end;
  CStringIntMap field_offset;
  CStringIntMap field_unboxed_size;
  CStringIntMap static_id;
  CStringIntMap selector_offset;
  CStringIntMap pool_index;
  intptr_t num_cids = 0;
  intptr_t num_statics = 0;
  intptr_t pool_length = 0;
  intptr_t stub_pool_length = -1;
};

BaseLayout* base_layout = nullptr;
bool base_loaded = false;
TextBuffer* output = nullptr;
// Keys of global object pool slots recorded when entries were placed.
MallocGrowableArray<const char*>* pool_keys = nullptr;

intptr_t Lookup(const CStringIntMap& map, const char* key) {
  if (key == nullptr) return LinkInfo::kNone;
  const intptr_t value = map.LookupValue(key);
  return value == CStringIntMapKeyValueTrait::kNoValue ? LinkInfo::kNone
                                                        : value;
}

void InsertKey(CStringIntMap* map, const char* key, intptr_t value) {
  // First definition wins; duplicate keys are ambiguous and are dropped so
  // that they cannot pin two different things to the same place.
  if (map->LookupValue(key) != CStringIntMapKeyValueTrait::kNoValue) {
    return;
  }
  map->Insert({Utils::StrDup(key), value});
}

// Splits a mutable line into tab separated columns.
intptr_t SplitColumns(char* line, char** columns, intptr_t max_columns) {
  intptr_t count = 0;
  char* p = line;
  while (count < max_columns) {
    columns[count++] = p;
    char* tab = strchr(p, '\t');
    if (tab == nullptr) break;
    *tab = '\0';
    p = tab + 1;
  }
  return count;
}

intptr_t ParseInt(const char* s) {
  return static_cast<intptr_t>(strtoll(s, nullptr, 10));
}

void LoadBase() {
  if (base_loaded) return;
  base_loaded = true;
  if (FLAG_link_base == nullptr) return;

  auto open = Dart::file_open_callback();
  auto read = Dart::file_read_callback();
  auto close = Dart::file_close_callback();
  if (open == nullptr || read == nullptr || close == nullptr) {
    FATAL("--link_base requires file callbacks");
  }
  void* file = open(FLAG_link_base, /*write=*/false);
  if (file == nullptr) {
    FATAL("--link_base: cannot open %s", FLAG_link_base);
  }
  uint8_t* data = nullptr;
  intptr_t length = 0;
  read(&data, &length, file);
  close(file);
  if (data == nullptr) {
    FATAL("--link_base: cannot read %s", FLAG_link_base);
  }

  char* text = reinterpret_cast<char*>(malloc(length + 1));
  memmove(text, data, length);
  text[length] = '\0';
  free(data);

  auto* base = new BaseLayout();
  char* columns[16];
  char* line = text;
  bool saw_header = false;
  while (line != nullptr && *line != '\0') {
    char* next = strchr(line, '\n');
    if (next != nullptr) {
      *next = '\0';
      next++;
    }
    if (!saw_header) {
      if (strncmp(line, "#osb-link-info 1", 16) != 0) {
        FATAL("--link_base: %s is not a link info file", FLAG_link_base);
      }
      saw_header = true;
      line = next;
      continue;
    }
    const intptr_t n = SplitColumns(line, columns, 16);
    const char* kind = columns[0];
    if (strcmp(kind, "class") == 0 && n >= 8) {
      // class cid key instance_size next_offset type_args super subtree_end
      const char* key = columns[2];
      InsertKey(&base->class_cid, key, ParseInt(columns[1]));
      InsertKey(&base->class_type_args, key, ParseInt(columns[5]));
      InsertKey(&base->class_subtree_end, key, ParseInt(columns[7]));
    } else if (strcmp(kind, "field") == 0 && n >= 4) {
      // field offset unboxed_size key
      InsertKey(&base->field_offset, columns[3], ParseInt(columns[1]));
      InsertKey(&base->field_unboxed_size, columns[3], ParseInt(columns[2]));
    } else if (strcmp(kind, "static") == 0 && n >= 3) {
      InsertKey(&base->static_id, columns[2], ParseInt(columns[1]));
    } else if (strcmp(kind, "selector") == 0 && n >= 4) {
      // selector id offset key
      InsertKey(&base->selector_offset, columns[3], ParseInt(columns[2]));
    } else if (strcmp(kind, "pool") == 0 && n >= 5) {
      // pool index type patchable key
      if (columns[4][0] != '\0') {
        InsertKey(&base->pool_index, columns[4], ParseInt(columns[1]));
      }
    } else if (strcmp(kind, "meta") == 0 && n >= 3) {
      const char* name = columns[1];
      const intptr_t value = ParseInt(columns[2]);
      if (strcmp(name, "num_cids") == 0) {
        base->num_cids = value;
      } else if (strcmp(name, "num_statics") == 0) {
        base->num_statics = value;
      } else if (strcmp(name, "pool_length") == 0) {
        base->pool_length = value;
      } else if (strcmp(name, "stub_pool_length") == 0) {
        base->stub_pool_length = value;
      }
    }
    line = next;
  }
  free(text);
  base_layout = base;
}

BaseLayout* Base() {
  LoadBase();
  return base_layout;
}

// Replaces characters that would break the line/column format.
const char* Sanitize(Zone* zone, const char* s) {
  bool clean = true;
  for (const char* p = s; *p != '\0'; p++) {
    if (*p == '\t' || *p == '\n' || *p == '\r') {
      clean = false;
      break;
    }
  }
  if (clean) return s;
  const intptr_t len = strlen(s);
  char* copy = zone->Alloc<char>(len + 1);
  for (intptr_t i = 0; i < len; i++) {
    const char c = s[i];
    copy[i] = (c == '\t' || c == '\n' || c == '\r') ? '?' : c;
  }
  copy[len] = '\0';
  return copy;
}

uint64_t Fnv1a(uint64_t hash, const void* data, intptr_t length) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  for (intptr_t i = 0; i < length; i++) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

const char* LibraryUrl(Zone* zone, const Class& cls) {
  const auto& lib = Library::Handle(zone, cls.library());
  if (lib.IsNull()) return "";
  return String::Handle(zone, lib.url()).ToCString();
}

const char* ScrubbedName(Zone* zone, const String& name) {
  if (name.IsNull()) return "";
  return String::ScrubName(name);
}

void Emit(const char* format, ...) PRINTF_ATTRIBUTE(1, 2);
void Emit(const char* format, ...) {
  if (output == nullptr) return;
  va_list args;
  va_start(args, format);
  output->VPrintf(format, args);
  va_end(args);
}

}  // namespace

bool LinkInfo::HasBase() {
  return FLAG_link_base != nullptr && Base() != nullptr;
}

bool LinkInfo::IsRecording() {
  if (FLAG_print_link_info_to == nullptr) return false;
  if (output == nullptr) {
    alignas(TextBuffer) static uint8_t storage[sizeof(TextBuffer)];
    output = ::new (storage) TextBuffer(1 << 20);
    output->Printf("#osb-link-info 1\n");
  }
  return true;
}

const char* LinkInfo::ClassKey(const Class& cls) {
  Zone* zone = Thread::Current()->zone();
  if (cls.IsNull()) return "<null-class>";
  const char* url = LibraryUrl(zone, cls);
  const auto& name = String::Handle(zone, cls.Name());
  return Sanitize(zone, OS::SCreate(zone, "%s::%s", url,
                                    ScrubbedName(zone, name)));
}

const char* LinkInfo::FieldKey(const Field& field) {
  Zone* zone = Thread::Current()->zone();
  const auto& owner = Class::Handle(zone, field.Owner());
  const auto& name = String::Handle(zone, field.name());
  return Sanitize(zone,
                  OS::SCreate(zone, "%s::%s%s", ClassKey(owner),
                              field.is_static() ? "static:" : "",
                              ScrubbedName(zone, name)));
}

const char* LinkInfo::FunctionKey(const Function& function) {
  Zone* zone = Thread::Current()->zone();
  if (function.IsNull()) return "<null-function>";
  const auto& name = String::Handle(zone, function.name());
  if (function.IsClosureFunction()) {
    const auto& parent = Function::Handle(zone, function.parent_function());
    if (function.IsImplicitClosureFunction()) {
      return Sanitize(zone, OS::SCreate(zone, "%s::<tearoff>",
                                        FunctionKey(parent)));
    }
    // Closures are identified by their parent and their position relative to
    // the outermost enclosing function, so that unrelated edits elsewhere in
    // the file do not change the key.
    const auto& outer = Function::Handle(zone, function.GetOutermostFunction());
    intptr_t relative = 0;
    if (function.token_pos().IsReal() && outer.token_pos().IsReal()) {
      relative = function.token_pos().Pos() - outer.token_pos().Pos();
    }
    return Sanitize(
        zone, OS::SCreate(zone, "%s::<closure %s@%" Pd ">",
                          parent.IsNull() ? "" : FunctionKey(parent),
                          ScrubbedName(zone, name), relative));
  }
  const auto& owner = Class::Handle(zone, function.Owner());
  return Sanitize(
      zone, OS::SCreate(zone, "%s::%s#%d/%" Pd "%s", ClassKey(owner),
                        ScrubbedName(zone, name),
                        static_cast<int>(function.kind()),
                        function.NumParameters(),
                        function.is_static() ? "s" : ""));
}

const char* LinkInfo::CodeKey(const Code& code) {
  Zone* zone = Thread::Current()->zone();
  if (code.IsStubCode()) {
    const char* name = StubCode::NameOfStub(code.EntryPoint());
    return OS::SCreate(zone, "stub:%s", name == nullptr ? "?" : name);
  }
  const auto& owner = Object::Handle(
      zone, WeakSerializationReference::UnwrapIfTarget(code.owner()));
  if (owner.IsFunction()) {
    return OS::SCreate(zone, "fn:%s", FunctionKey(Function::Cast(owner)));
  }
  if (owner.IsClass()) {
    return OS::SCreate(zone, "alloc:%s", ClassKey(Class::Cast(owner)));
  }
  if (owner.IsAbstractType()) {
    const char* key = ObjectKey(owner);
    return Sanitize(zone, OS::SCreate(zone, "tts:%s",
                                      key != nullptr ? key : "?"));
  }
  return "code:?";
}

const char* LinkInfo::ObjectKey(const Object& obj, intptr_t depth) {
  Zone* zone = Thread::Current()->zone();
  if (depth > 4) return nullptr;
  if (obj.IsNull()) return "null";
  if (obj.IsSmi()) {
    return OS::SCreate(zone, "int:%" Pd, Smi::Cast(obj).Value());
  }
  if (obj.IsMint()) {
    return OS::SCreate(zone, "int:%" Pd64, Integer::Cast(obj).Value());
  }
  if (obj.IsDouble()) {
    const double value = Double::Cast(obj).value();
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return OS::SCreate(zone, "double:%" Px64, bits);
  }
  if (obj.IsBool()) {
    return Bool::Cast(obj).value() ? "true" : "false";
  }
  if (obj.ptr() == Object::sentinel().ptr()) return "<sentinel>";
  if (obj.IsString()) {
    const auto& str = String::Cast(obj);
    const char* utf8 = str.ToCString();
    const intptr_t len = strlen(utf8);
    const uint64_t hash = Fnv1a(0xcbf29ce484222325ULL, utf8, len);
    // A short readable prefix helps diagnostics; the hash makes it unique.
    char prefix[25];
    intptr_t i = 0;
    for (; i < len && i < 24; i++) {
      const char c = utf8[i];
      prefix[i] = (c < 0x20 || c > 0x7e) ? '?' : c;
    }
    prefix[i] = '\0';
    return OS::SCreate(zone, "str:%" Pd ":%016" Px64 ":%s", str.Length(),
                       hash, prefix);
  }
  if (obj.IsClass()) {
    return OS::SCreate(zone, "class:%s", ClassKey(Class::Cast(obj)));
  }
  if (obj.IsField()) {
    return OS::SCreate(zone, "field:%s", FieldKey(Field::Cast(obj)));
  }
  if (obj.IsFunction()) {
    return OS::SCreate(zone, "function:%s", FunctionKey(Function::Cast(obj)));
  }
  if (obj.IsCode()) {
    return OS::SCreate(zone, "code:%s", CodeKey(Code::Cast(obj)));
  }
  if (obj.IsType()) {
    const auto& type = Type::Cast(obj);
    const auto& cls = Class::Handle(zone, type.type_class());
    const auto& args = TypeArguments::Handle(zone, type.arguments());
    const char* args_key =
        args.IsNull() ? "" : ObjectKey(args, depth + 1);
    return Sanitize(
        zone, OS::SCreate(zone, "type:%s<%s>%d", ClassKey(cls),
                          args_key == nullptr ? "?" : args_key,
                          static_cast<int>(type.nullability())));
  }
  if (obj.IsAbstractType()) {
    return Sanitize(zone, OS::SCreate(zone, "atype:%s", obj.ToCString()));
  }
  if (obj.IsTypeArguments()) {
    const auto& args = TypeArguments::Cast(obj);
    ZoneTextBuffer buffer(zone);
    buffer.AddString("targs:[");
    auto& type = AbstractType::Handle(zone);
    for (intptr_t i = 0; i < args.Length(); i++) {
      type = args.TypeAt(i);
      const char* key = ObjectKey(type, depth + 1);
      if (key == nullptr) return nullptr;
      buffer.Printf("%s%s", i == 0 ? "" : ",", key);
    }
    buffer.AddString("]");
    return Sanitize(zone, buffer.buffer());
  }
  if (obj.IsArray()) {
    const auto& array = Array::Cast(obj);
    if (array.Length() > 64) return nullptr;
    ZoneTextBuffer buffer(zone);
    buffer.Printf("array:%" Pd "[", array.Length());
    auto& element = Object::Handle(zone);
    for (intptr_t i = 0; i < array.Length(); i++) {
      element = array.At(i);
      const char* key = ObjectKey(element, depth + 1);
      if (key == nullptr) return nullptr;
      buffer.Printf("%s%s", i == 0 ? "" : ",", key);
    }
    buffer.AddString("]");
    return Sanitize(zone, buffer.buffer());
  }
  if (obj.IsUnlinkedCall() || obj.IsICData() || obj.IsMegamorphicCache()) {
    const auto& data = CallSiteData::Cast(obj);
    const auto& name = String::Handle(zone, data.target_name());
    const auto& desc = Array::Handle(zone, data.arguments_descriptor());
    const char* desc_key = ObjectKey(desc, depth + 1);
    const char* kind = obj.IsUnlinkedCall() ? "ucall"
                       : obj.IsICData()     ? "icdata"
                                            : "mega";
    return Sanitize(zone, OS::SCreate(zone, "%s:%s:%s", kind,
                                      ScrubbedName(zone, name),
                                      desc_key == nullptr ? "?" : desc_key));
  }
  if (obj.IsClosure()) {
    const auto& fn = Function::Handle(zone, Closure::Cast(obj).function());
    return OS::SCreate(zone, "closure:%s", FunctionKey(fn));
  }
  if (obj.IsInstance() && obj.IsCanonical()) {
    // Constants of user classes: class identity plus content hash. Content
    // hashes of nested instances depend on class ids, which the patch keeps.
    const auto& cls = Class::Handle(zone, obj.clazz());
    const uint32_t hash = Instance::Cast(obj).CanonicalizeHash();
    return OS::SCreate(zone, "const:%s:%08x", ClassKey(cls), hash);
  }
  return nullptr;
}

intptr_t LinkInfo::BaseNumCids() {
  return HasBase() ? Base()->num_cids : 0;
}

intptr_t LinkInfo::BaseCid(const char* class_key) {
  return HasBase() ? Lookup(Base()->class_cid, class_key) : kNone;
}

bool LinkInfo::BaseFieldLayout(const char* field_key,
                               intptr_t* offset,
                               intptr_t* unboxed_size) {
  if (!HasBase()) return false;
  const intptr_t o = Lookup(Base()->field_offset, field_key);
  if (o == kNone) return false;
  *offset = o;
  *unboxed_size = Lookup(Base()->field_unboxed_size, field_key);
  return true;
}

intptr_t LinkInfo::BaseTypeArgumentsOffset(const char* class_key) {
  return HasBase() ? Lookup(Base()->class_type_args, class_key) : kNone;
}

intptr_t LinkInfo::BaseSubtreeEnd(const char* class_key) {
  return HasBase() ? Lookup(Base()->class_subtree_end, class_key) : kNone;
}

intptr_t LinkInfo::BaseNumStaticFields() {
  return HasBase() ? Base()->num_statics : 0;
}

intptr_t LinkInfo::BaseStaticFieldId(const char* field_key) {
  return HasBase() ? Lookup(Base()->static_id, field_key) : kNone;
}

intptr_t LinkInfo::BaseSelectorOffset(const char* selector_key) {
  return HasBase() ? Lookup(Base()->selector_offset, selector_key) : kNone;
}

intptr_t LinkInfo::BasePoolLength() {
  return HasBase() ? Base()->pool_length : 0;
}

intptr_t LinkInfo::BasePoolIndex(const char* pool_key) {
  return HasBase() ? Lookup(Base()->pool_index, pool_key) : kNone;
}

intptr_t LinkInfo::BaseStubPoolLength() {
  return HasBase() ? Base()->stub_pool_length : kNone;
}

void LinkInfo::NoteMismatch(const char* format, ...) {
  if (!FLAG_trace_link_info) return;
  va_list args;
  va_start(args, format);
  OS::VFPrint(stderr, format, args);
  va_end(args);
  OS::PrintErr("\n");
}

const char* LinkInfo::PoolEntryKey(
    const compiler::ObjectPoolBuilderEntry& entry,
    const char* context,
    intptr_t ordinal) {
  using compiler::ObjectPoolBuilderEntry;
  Zone* zone = Thread::Current()->zone();
  const bool patchable =
      entry.patchable() == ObjectPoolBuilderEntry::kPatchable;
  switch (entry.type()) {
    case ObjectPoolBuilderEntry::kImmediate:
      if (patchable || entry.snapshot_behavior() !=
                           ObjectPoolBuilderEntry::kSnapshotable) {
        break;
      }
      return OS::SCreate(zone, "imm:%" Px, entry.imm_);
    case ObjectPoolBuilderEntry::kTaggedObject:
      if (!patchable && entry.equivalence_ == entry.obj_) {
        const char* key = ObjectKey(*entry.obj_);
        if (key != nullptr) return key;
      }
      break;
    default:
      // Multi-slot immediates and native function addresses are never
      // pinned individually by content.
      break;
  }
  // Entries without a content key (call-site specific entries, unique
  // objects, native entries) are keyed by their position in the code that
  // requested them.
  if (context == nullptr) return nullptr;
  return Sanitize(zone,
                  OS::SCreate(zone, "site:%s#%" Pd, context, ordinal));
}

void LinkInfo::RemapStaticFieldIds(IsolateGroup* IG) {
  if (!HasBase()) return;
  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  FieldTable* initial = IG->initial_field_table();
  const intptr_t old_count = initial->NumFieldIds();
  const intptr_t base_count = BaseNumStaticFields();

  // Collect every static field object (including clones) from the heap, so
  // that no Field is left with a stale id.
  class FieldCollector : public ObjectVisitor {
   public:
    FieldCollector(Zone* zone, intptr_t count, GrowableArray<const Field*>* out)
        : zone_(zone), count_(count), out_(out) {}
    void VisitObject(ObjectPtr obj) override {
      if (!obj->IsField()) return;
      const auto& field = Field::Handle(zone_, Field::RawCast(obj));
      if (!field.is_static() || field.is_shared()) return;
      if (field.field_id() < 0 || field.field_id() >= count_) return;
      out_->Add(&Field::ZoneHandle(zone_, field.ptr()));
    }

   private:
    Zone* zone_;
    intptr_t count_;
    GrowableArray<const Field*>* out_;
  };
  GrowableArray<const Field*> fields;
  {
    HeapIterationScope iteration(thread);
    FieldCollector collector(zone, old_count, &fields);
    iteration.IterateObjects(&collector);
  }

  std::unique_ptr<intptr_t[]> old_to_new(new intptr_t[old_count]);
  for (intptr_t i = 0; i < old_count; i++) old_to_new[i] = -1;
  const intptr_t new_capacity = Utils::Maximum(old_count, base_count) +
                                old_count;  // Worst case: all appended.
  std::unique_ptr<bool[]> taken(new bool[new_capacity]);
  for (intptr_t i = 0; i < new_capacity; i++) taken[i] = false;

  intptr_t kept = 0;
  for (auto field : fields) {
    const intptr_t base_id = BaseStaticFieldId(FieldKey(*field));
    const intptr_t old_id = field->field_id();
    if (base_id != kNone && base_id < base_count && !taken[base_id] &&
        old_to_new[old_id] == -1) {
      old_to_new[old_id] = base_id;
      taken[base_id] = true;
      kept++;
    }
  }
  intptr_t next = base_count;
  for (intptr_t i = 0; i < old_count; i++) {
    if (old_to_new[i] == -1) {
      while (taken[next]) next++;
      old_to_new[i] = next;
      taken[next] = true;
    }
  }
  intptr_t new_count = 0;
  for (intptr_t i = 0; i < old_count; i++) {
    new_count = Utils::Maximum(new_count, old_to_new[i] + 1);
  }
  new_count = Utils::Maximum(new_count, base_count);

  IG->initial_field_table()->Remap(old_to_new.get(), old_count, new_count);
  IG->sentinel_field_table()->Remap(old_to_new.get(), old_count, new_count);
  IG->ForEachIsolate([&](Isolate* isolate) {
    if (isolate->field_table() != nullptr &&
        isolate->field_table()->NumFieldIds() == old_count) {
      isolate->field_table()->Remap(old_to_new.get(), old_count, new_count);
    }
  });
  GrowableArray<intptr_t> new_ids(fields.length());
  for (auto field : fields) {
    new_ids.Add(old_to_new[field->field_id()]);
  }
  for (intptr_t i = 0; i < fields.length(); i++) {
    fields[i]->set_field_id_unsafe(new_ids[i]);
  }
  if (FLAG_trace_link_info) {
    OS::PrintErr("link: static fields kept %" Pd " of %" Pd
                 " (release %" Pd ", now %" Pd " ids)\n",
                 kept, old_count, base_count, new_count);
  }
}

void LinkInfo::RecordSelector(intptr_t id, intptr_t offset, const char* key) {
  if (!IsRecording()) return;
  Emit("selector\t%" Pd "\t%" Pd "\t%s\n", id, offset, key);
}

void LinkInfo::RecordPoolKey(intptr_t index, const char* key) {
  if (!IsRecording() || key == nullptr) return;
  if (pool_keys == nullptr) {
    pool_keys = new MallocGrowableArray<const char*>();
  }
  while (pool_keys->length() <= index) {
    pool_keys->Add(nullptr);
  }
  if ((*pool_keys)[index] != nullptr) {
    free(const_cast<char*>((*pool_keys)[index]));
  }
  (*pool_keys)[index] = Utils::StrDup(key);
}

void LinkInfo::ResetPoolKeys() {
  if (pool_keys == nullptr) return;
  for (intptr_t i = 0; i < pool_keys->length(); i++) {
    if ((*pool_keys)[i] != nullptr) free(const_cast<char*>((*pool_keys)[i]));
  }
  pool_keys->Clear();
}

void LinkInfo::RecordPoolUses(const char* code_key,
                              const GrowableArray<intptr_t>& uses) {
  if (!IsRecording()) return;
  Emit("uses\t%s\t", code_key);
  for (intptr_t i = 0; i < uses.length(); i++) {
    Emit(i == 0 ? "%" Pd : ",%" Pd, uses[i]);
  }
  Emit("\n");
}

void LinkInfo::RecordPool(compiler::ObjectPoolBuilder* pool) {
  if (!IsRecording()) return;
  using compiler::ObjectPoolBuilderEntry;
  const intptr_t length = pool->CurrentLength();
  RecordMeta("pool_length", length);
  for (intptr_t i = 0; i < length; i++) {
    const ObjectPoolBuilderEntry& entry = pool->EntryAt(i);
    const char* key = nullptr;
    if (pool_keys != nullptr && i < pool_keys->length()) {
      key = (*pool_keys)[i];
    }
    if (key == nullptr) {
      key = PoolEntryKey(entry, nullptr, 0);
    }
    Emit("pool\t%" Pd "\t%d\t%d\t%s\n", i, static_cast<int>(entry.type()),
         static_cast<int>(entry.patchable()), key == nullptr ? "" : key);
  }
}

void LinkInfo::RecordProgramTables(IsolateGroup* IG) {
  if (!IsRecording()) return;
  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();
  ClassTable* table = IG->class_table();
  const intptr_t num_cids = table->NumCids();
  RecordMeta("num_cids", num_cids);

  // Subtree end: the largest next-field offset of a class and all of its
  // subclasses. A field added to the class in a patch is placed after it so
  // that it cannot collide with a subclass field kept at its release offset.
  std::unique_ptr<intptr_t[]> subtree_end(new intptr_t[num_cids]);
  for (intptr_t cid = 0; cid < num_cids; cid++) subtree_end[cid] = 0;
  auto& cls = Class::Handle(zone);
  auto& super = Class::Handle(zone);
  for (intptr_t cid = kNumPredefinedCids; cid < num_cids; cid++) {
    if (!table->HasValidClassAt(cid)) continue;
    cls = table->At(cid);
    if (!cls.is_finalized()) continue;
    const intptr_t end = cls.host_next_field_offset();
    super = cls.ptr();
    while (!super.IsNull()) {
      const intptr_t scid = super.id();
      if (scid >= 0 && scid < num_cids) {
        subtree_end[scid] = Utils::Maximum(subtree_end[scid], end);
      }
      super = super.SuperClass();
    }
  }

  auto& fields = Array::Handle(zone);
  auto& field = Field::Handle(zone);
  for (intptr_t cid = kNumPredefinedCids; cid < num_cids; cid++) {
    if (!table->HasValidClassAt(cid)) continue;
    cls = table->At(cid);
    super = cls.SuperClass();
    const bool finalized = cls.is_finalized();
    Emit("class\t%" Pd "\t%s\t%" Pd "\t%" Pd "\t%" Pd "\t%" Pd "\t%" Pd "\n",
         cid, ClassKey(cls), finalized ? cls.host_instance_size() : 0,
         finalized ? cls.host_next_field_offset() : 0,
         finalized ? cls.host_type_arguments_field_offset()
                   : static_cast<intptr_t>(Class::kNoTypeArguments),
         super.IsNull() ? static_cast<intptr_t>(-1) : super.id(),
         subtree_end[cid]);
    if (!finalized) continue;
    fields = cls.fields();
    if (fields.IsNull()) continue;
    for (intptr_t i = 0; i < fields.Length(); i++) {
      field ^= fields.At(i);
      if (field.is_static()) continue;
      const intptr_t size = field.is_unboxed() ? Class::UnboxedFieldSizeInBytesByCid(
                                                     field.guarded_cid())
                                               : 0;
      Emit("field\t%" Pd "\t%" Pd "\t%s\n", field.HostOffset(), size,
           FieldKey(field));
    }
  }

  // Static fields (including those of top-level classes).
  RecordMeta("num_statics", IG->initial_field_table()->NumFieldIds());
  const auto& libs =
      GrowableObjectArray::Handle(zone, IG->object_store()->libraries());
  auto& lib = Library::Handle(zone);
  for (intptr_t i = 0; i < libs.Length(); i++) {
    lib ^= libs.At(i);
    ClassDictionaryIterator it(lib, ClassDictionaryIterator::kIteratePrivate);
    while (it.HasNext()) {
      cls = it.GetNextClass();
      fields = cls.fields();
      if (fields.IsNull()) continue;
      for (intptr_t j = 0; j < fields.Length(); j++) {
        field ^= fields.At(j);
        if (!field.is_static() || field.is_shared()) continue;
        Emit("static\t%" Pd "\t%s\n", field.field_id(), FieldKey(field));
      }
    }
  }
}

void LinkInfo::RecordCode(const char* image,
                          const Code& code,
                          intptr_t text_offset,
                          intptr_t payload_size) {
  if (!IsRecording()) return;
  Zone* zone = Thread::Current()->zone();
  Emit("code\t%s\t%" Pd "\t%" Pd "\t%s\n", image, text_offset, payload_size,
       CodeKey(code));
  const auto& table = Array::Handle(zone, code.static_calls_target_table());
  if (table.IsNull()) return;
  StaticCallsTable calls(table);
  auto& target = Object::Handle(zone);
  for (auto call : calls) {
    const auto kind_and_offset = call.Get<Code::kSCallTableKindAndOffset>();
    const intptr_t value = Smi::Value(kind_and_offset);
    const auto kind = Code::KindField::decode(value);
    const auto return_pc_offset = Code::OffsetField::decode(value);
    const auto entry = Code::EntryPointField::decode(value);
    target = call.Get<Code::kSCallTableCodeOrTypeTarget>();
    const char* target_key = "?";
    if (target.IsCode()) {
      target_key = CodeKey(Code::Cast(target));
    } else {
      target = call.Get<Code::kSCallTableFunctionTarget>();
      if (target.IsFunction()) {
        target_key = OS::SCreate(zone, "fn:%s",
                                 FunctionKey(Function::Cast(target)));
      }
    }
    Emit("call\t%" Pd "\t%" Pd "\t%d\t%d\t%s\n", text_offset,
         static_cast<intptr_t>(return_pc_offset), static_cast<int>(kind),
         static_cast<int>(entry), target_key);
  }
}

void LinkInfo::RecordMeta(const char* name, intptr_t value) {
  if (!IsRecording()) return;
  Emit("meta\t%s\t%" Pd "\n", name, value);
}

void LinkInfo::Flush() {
  if (!IsRecording()) return;
  auto open = Dart::file_open_callback();
  auto write = Dart::file_write_callback();
  auto close = Dart::file_close_callback();
  if (open == nullptr || write == nullptr || close == nullptr) {
    OS::PrintErr("--print_link_info_to: no file callbacks\n");
    return;
  }
  void* file = open(FLAG_print_link_info_to, /*write=*/true);
  if (file == nullptr) {
    OS::PrintErr("--print_link_info_to: cannot open %s\n",
                 FLAG_print_link_info_to);
    return;
  }
  write(output->buffer(), output->length(), file);
  close(file);
  output->Clear();
}

}  // namespace dart
