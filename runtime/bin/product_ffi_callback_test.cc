// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Internal PRODUCT embedder: genuine new-library bytecode, no fabricated
// functions/flags or public test API. Compiled controls use another snapshot.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <vector>
#include <utility>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include "bin/dartutils.h"
#include "include/dart_api.h"
#include "include/dart_embedder_api.h"
#include "include/dart_engine.h"
#include "vm/constants_kbc.h"
#include "vm/dart_api_state.h"
#include "vm/dart_entry.h"
#include "vm/ffi/callback_function.h"
#include "vm/ffi/interpreter_callback.h"
#include "vm/ffi_callback_metadata.h"
#include "vm/flags.h"
#include "vm/handle_visitor.h"
#include "vm/heap/safepoint.h"
#include "vm/heap/heap.h"
#include "vm/isolate.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/stub_code.h"
#include "vm/symbols.h"
#include "vm/thread.h"
#include "vm/virtual_memory.h"

#if !defined(PRODUCT) || !defined(DART_PRECOMPILED_RUNTIME) || \
    !defined(TARGET_ARCH_ARM64) || !defined(HOST_ARCH_ARM64)
#error "Native ARM64 PRODUCT precompiled runtime required."
#endif
namespace {
void Expect(bool condition, const char* message) {
  if (!condition) { std::cerr << message << std::endl; std::exit(1); }
}
void Check(Dart_Handle value) {
  Expect(!Dart_IsError(value), Dart_IsError(value) ? Dart_GetError(value) : "");
}
int64_t Integer(Dart_Handle value) {
  int64_t result = 0; Check(Dart_IntegerToInt64(value, &result)); return result;
}
Dart_Handle Invoke(Dart_Handle library, const char* method, int argc = 0,
                   Dart_Handle* arguments = nullptr, bool allow_unwind = false) {
  auto* t = dart::Thread::Current();
  auto* scope = t->api_top_scope();
  auto* resource = t->top_resource();
  auto* jump = t->long_jump_base();
  const auto exit = t->top_exit_frame_info();
  const auto state = t->execution_state();
  const auto tag = t->vm_tag();
  auto result = Dart_Invoke(library, Dart_NewStringFromCString(method), argc,
                            arguments);
  Expect(t == dart::Thread::Current() && t->api_top_scope() == scope &&
             t->top_resource() == resource && t->long_jump_base() == jump &&
             t->top_exit_frame_info() == exit && t->execution_state() == state &&
             t->vm_tag() == tag,
         "callback entry failed to restore API scope/exit/resource/state/tag");
  if (!allow_unwind) Check(result);
  return result;
}
std::vector<uintptr_t> Addresses(Dart_Handle values) {
  intptr_t length = 0; Check(Dart_ListLength(values, &length));
  std::vector<uintptr_t> result;
  for (intptr_t i = 0; i < length; ++i)
    result.push_back(Integer(Dart_ListGetAt(values, i)));
  return result;
}
struct Counts {
  intptr_t callbacks, roots, code;
  bool keepalive, ports;
  bool operator==(const Counts& b) const {
    return callbacks == b.callbacks && roots == b.roots && code == b.code &&
           keepalive == b.keepalive && ports == b.ports;
  }
};
intptr_t LivePersistentHandles(dart::ApiState* state) {
  class Visitor : public dart::HandleVisitor {
   public:
    explicit Visitor(const dart::PersistentHandles& handles) : handles_(handles) {}
    void VisitHandle(dart::uword address) override {
      if (!handles_.IsFreeHandle(
              reinterpret_cast<Dart_PersistentHandle>(address))) ++count;
    }
    intptr_t count = 0;
   private:
    const dart::PersistentHandles& handles_;
  };
  intptr_t live = 0;
  state->RunWithLockedPersistentHandles([&](dart::PersistentHandles& handles) {
    Visitor visitor(handles);
    handles.Visit(&visitor);
    live = visitor.count;
  });
  return live;
}
Counts Count() {
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t);
  StackZone zone(t);
  intptr_t callbacks = 0;
  for (auto* entry = t->isolate()->ffi_callback_list_head(); entry;
       entry = entry->list_next()) ++callbacks;
  const auto& code = GrowableObjectArray::Handle(
      t->zone(), t->isolate_group()->object_store()->ffi_callback_code());
  return {callbacks, LivePersistentHandles(t->isolate_group()->api_state()),
          code.IsNull() ? 0 : code.Length(),
          t->isolate()->HasOpenNativeCallables(), t->isolate()->HasLivePorts()};
}
void PrintCounts(const char* label, const Counts& value) {
  std::cerr << label << ": callbacks=" << value.callbacks
            << " roots=" << value.roots << " code=" << value.code
            << " keepalive=" << value.keepalive << " ports=" << value.ports
            << std::endl;
}
void ExpectCounts(const Counts& expected, const char* message) {
  const auto actual = Count();
  if (!(actual == expected)) {
    PrintCounts("expected", expected);
    PrintCounts("actual", actual);
  }
  Expect(actual == expected, message);
}
struct Region { uintptr_t start, end; bool read, write, execute; };
std::vector<Region> Mappings() {
  std::vector<Region> out;
#if defined(__APPLE__)
  mach_vm_address_t address = 0; natural_t depth = 0;
  while (true) {
    mach_vm_size_t size = 0; vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    const auto status = mach_vm_region_recurse(
        mach_task_self(), &address, &size, &depth,
        reinterpret_cast<vm_region_recurse_info_t>(&info), &count);
    if (status == KERN_INVALID_ADDRESS) break;
    Expect(status == KERN_SUCCESS && size > 0, "mapping audit failed");
    if (info.is_submap) { ++depth; continue; }
    out.push_back({static_cast<uintptr_t>(address),
                   static_cast<uintptr_t>(address + size),
                   (info.protection & VM_PROT_READ) != 0,
                   (info.protection & VM_PROT_WRITE) != 0,
                   (info.protection & VM_PROT_EXECUTE) != 0});
    const auto next = address + size;
    Expect(next > address, "mapping audit address did not advance");
    address = next;
  }
#else
  auto* file = std::fopen("/proc/self/maps", "r");
  Expect(file != nullptr, "cannot open /proc/self/maps"); char line[4096];
  while (std::fgets(line, sizeof(line), file)) {
    unsigned long start, end; char p[5];
    if (std::sscanf(line, "%lx-%lx %4s", &start, &end, p) == 3)
      out.push_back({start,end,p[0]=='r',p[1]=='w',p[2]=='x'});
  }
  std::fclose(file);
#endif
  for (const auto& r : out)
    Expect(!(r.write && r.execute), "writable executable mapping at checkpoint");
  return out;
}
void Permissions(const std::vector<Region>& maps, uintptr_t start,
                 uintptr_t length, bool executable) {
  uintptr_t cursor = start;
  for (const auto& r : maps) {
    if (r.start <= cursor && cursor < r.end) {
      Expect(r.read && r.execute == executable && r.write != executable,
             "callback RX/RW protection mismatch");
      cursor = std::min(start + length, r.end);
    }
    if (cursor == start + length) return;
  }
  Expect(false, "callback mapping not fully readable");
}
void Permissions(uintptr_t start, uintptr_t length, bool executable) {
  Permissions(Mappings(), start, length, executable);
}
std::set<uintptr_t> aliases;
uintptr_t source_start;
uintptr_t alias_rx_length;
std::vector<uint8_t> source_bytes, common_bytes;
uintptr_t common_entry;
void InitializeCodeProof(uint64_t start, uint64_t end, uint64_t image_base) {
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t);
  StackZone zone(t);
  const auto& code = StubCode::FfiCallbackTrampoline();
  const auto page = VirtualMemory::PageSize();
  source_start = code.EntryPoint() & ~(page-1);
  const auto size = code.EntryPoint()+code.Size()-source_start;
  alias_rx_length = (size+page-1)&~(page-1);
  Expect(size <= FfiCallbackMetadata::RXMappingSize(), "invalid source stub span");
  source_bytes.assign(reinterpret_cast<uint8_t*>(source_start),
                      reinterpret_cast<uint8_t*>(source_start+size));
  common_entry = InterpreterFfiCallbackEntryPoint();
  Dl_info image;
  Expect(dladdr(reinterpret_cast<void*>(common_entry), &image) != 0,
         "common callback entry is outside a released image");
  const auto normalized = common_entry-reinterpret_cast<uintptr_t>(image.dli_fbase)+image_base;
  Expect(start <= normalized && normalized < end,
         "metadata helper common PC differs from linked static symbol");
  common_bytes.assign(reinterpret_cast<uint8_t*>(common_entry),
                      reinterpret_cast<uint8_t*>(common_entry+end-normalized));
  Permissions(common_entry, common_bytes.size(), true);
  Permissions(source_start, source_bytes.size(), true);
}
void Inspect(uintptr_t pointer, bool runtime, bool capturing = false,
             const std::vector<Region>* checkpoint = nullptr,
             uint64_t expected_flags = 0, uint64_t expected_indirect = 0,
             int callback_kind = 0) {
  using namespace dart;
  auto* t = Thread::Current(); TransitionNativeToVM transition(t);
  StackZone zone(t); auto* z=t->zone();
  const auto metadata = FfiCallbackMetadata::Instance(pointer)
      ->LookupMetadataForTrampolineUnlocked(pointer);
  Expect(metadata.IsLive(), "callback metadata is dead before close");
  Expect(IsInterpreterFfiCallbackEntry(metadata.target_entry_point()) == runtime,
         "callback did not use expected runtime/compiled entry");
  if (runtime) {
    Expect(metadata.target_entry_point() == common_entry &&
               (callback_kind==1 ? metadata.is_isolate_group_bound() &&
                    metadata.target_isolate_group()==t->isolate_group() :
                    metadata.target_isolate()==t->isolate()),
           "runtime callback entry/isolate mismatch");
    Expect(metadata.context_handle() != nullptr, "descriptor persistent root absent");
    const auto& object=Object::Handle(z,metadata.context_handle()->ptr());
    Expect(object.IsArray(), "callback context must be a descriptor Array");
    const auto& descriptor=Array::Cast(object);
    Expect(descriptor.Length()==kInterpreterFfiCallbackDescriptorLength,
           "invalid callback descriptor length");
    const auto& function=Function::Handle(z,Function::RawCast(
        descriptor.At(kInterpreterFfiCallbackFunction)));
    Expect(IsRuntimeFfiCallbackFunction(function) && !function.HasCode() &&
               !function.HasFfiCallbackId(), "callback metadata acquired code/id");
    const auto& closure=Closure::Handle(z,Closure::RawCast(
        descriptor.At(kInterpreterFfiCallbackClosure)));
    Expect(closure.IsNull()!=capturing, "unexpected callback closure context");
    const auto& target=Function::Handle(z,capturing ? closure.function() : function.FfiCallbackTarget());
    if(callback_kind==2) {
      Expect(target.IsNull() && closure.IsNull() &&
                 metadata.trampoline_type()==FfiCallbackMetadata::TrampolineType::kAsync,
             "listener descriptor retained/executed a synchronous target");
      const auto& port=Integer::Handle(z,Integer::RawCast(descriptor.At(kInterpreterFfiCallbackSendPort)));
      Expect(!port.IsNull() && port.Value()>0,"listener descriptor lost port identity");
    } else {
      Expect(!target.IsNull() && target.HasBytecode() && target.IsInterpreted(),
             "callback target is not genuine interpreted bytecode");
    }
    const auto& fallback=TypedData::Handle(z,TypedData::RawCast(
        descriptor.At(kInterpreterFfiCallbackFallback)));
    Expect(fallback.GetClassId()==kTypedDataUint8ArrayCid &&
               fallback.Length()==sizeof(InterpreterFfiCallbackFallback),
           "invalid fallback byte descriptor");
    InterpreterFfiCallbackFallback bits;
    { NoSafepointScope no_safepoint;
      std::memcpy(&bits,fallback.DataAddr(0),sizeof(bits)); }
    Expect(bits.flags==expected_flags && bits.indirect_result_size==expected_indirect,
           "callback Handle/indirect fallback descriptor mismatch");
  }
  const auto base=static_cast<uintptr_t>(FfiCallbackMetadata::MappingStart(pointer));
  aliases.insert(base);
  const auto current = checkpoint == nullptr ? Mappings() : std::vector<Region>();
  const auto& maps = checkpoint == nullptr ? current : *checkpoint;
  Permissions(maps,base,source_bytes.size(),true);
  Permissions(maps,base+FfiCallbackMetadata::MetadataOffset(),sizeof(uword),false);
  Expect(std::memcmp(reinterpret_cast<void*>(base),source_bytes.data(),source_bytes.size())==0,
         "callback alias differs from released stub instructions");
}
void Dead(const std::vector<uintptr_t>& pointers) {
  for (auto pointer:pointers)
    Expect(!dart::FfiCallbackMetadata::Instance(pointer)
        ->LookupMetadataForTrampolineUnlocked(pointer).IsLive(),
        "closed callback retained live metadata");
}
void Audit(const std::vector<Region>& before) {
  const auto after=Mappings();
  std::vector<Region> allowed=before;
  for (auto base:aliases) allowed.push_back({base,base+alias_rx_length,true,false,true});
  std::sort(allowed.begin(),allowed.end(),[](const Region&a,const Region&b){return a.start<b.start;});
  for (const auto&r:after) if(r.execute) {
    auto cursor=r.start;
    for(const auto&a:allowed) if(a.execute && a.start<=cursor && cursor<a.end) cursor=a.end;
    Expect(cursor>=r.end,"new executable mapping outside verified released stub aliases");
  }
  Expect(std::memcmp(reinterpret_cast<void*>(source_start),source_bytes.data(),source_bytes.size())==0 &&
             std::memcmp(reinterpret_cast<void*>(common_entry),common_bytes.data(),common_bytes.size())==0,
         "released callback instruction bytes changed");
  for (auto base:aliases) {
    Permissions(after,base,source_bytes.size(),true);
    Permissions(after,base+dart::FfiCallbackMetadata::MetadataOffset(),
                sizeof(dart::uword),false);
    Expect(std::memcmp(reinterpret_cast<void*>(base),source_bytes.data(),source_bytes.size())==0,
           "released alias instruction bytes changed");
  }
  std::cout << "Callback source/alias byte identity and RX/RW checkpoints passed; "
               "transient mapping/permission changes and instruction writes are not audited" << std::endl;
}
void InspectLoaded(const char* uri) {
  using namespace dart;
  auto*t=Thread::Current(); TransitionNativeToVM transition(t); StackZone zone(t);
  const auto& library=Library::Handle(t->zone(),Library::LookupLibrary(
      t,String::Handle(t->zone(),String::New(uri))));
  const auto& function=Function::Handle(t->zone(),library.LookupFunctionAllowPrivate(Symbols::main()));
  Expect(!function.IsNull() && function.HasBytecode() && function.IsInterpreted(),
         "new URI did not load as interpreted bytecode");
}
void InspectStaticErrorBytecodes(const char* uri) {
  using namespace dart;
  auto*t=Thread::Current();TransitionNativeToVM transition(t);StackZone zone(t);
  const auto& library=Library::Handle(t->zone(),Library::LookupLibrary(
      t,String::Handle(t->zone(),String::New(uri))));
  const char* names[]={"ordinaryTarget","ordinaryStoreTarget",
                       "get:ordinaryGetter","set:ordinarySetter"};
  const KernelBytecode::Opcode expected[]={KernelBytecode::kLoadStatic,
      KernelBytecode::kStoreStaticTOS,KernelBytecode::kVMInternal_ImplicitStaticGetter,
      KernelBytecode::kVMInternal_ImplicitStaticSetter};
  for(int i=0;i<4;++i){
    const auto& function=Function::Handle(t->zone(),library.LookupFunctionAllowPrivate(
        String::Handle(t->zone(),Symbols::New(t,names[i]))));
    Expect(!function.IsNull()&&function.HasBytecode()&&function.IsInterpreted(),
           "static error probe did not execute real interpreted function");
    const auto& bytecode=Bytecode::Handle(t->zone(),function.GetBytecode());
    bool found=false,zero_locals=i>=2;
    {NoSafepointScope no_safepoint;
      const auto* pc=reinterpret_cast<const KBCInstr*>(bytecode.PayloadStart());
      const auto* end=pc+bytecode.Size();
      while(pc<end){const auto op=KernelBytecode::DecodeOpcode(pc);
        found|=op==expected[i]||(i<2&&op==expected[i]+1);
        if(op==KernelBytecode::kEntry||op==KernelBytecode::kEntry_Wide)
          zero_locals=KernelBytecode::DecodeD(pc)==0;
        pc=KernelBytecode::Next(pc);
      }
    }
    Expect(found&&zero_locals,"static error probe lacks expected zero-local bytecode form");
    std::cout<<"STATIC_FRAME_BYTECODE:"<<names[i]<<":"<<expected[i]<<std::endl;
  }
}
struct CallbackScopeState {
  dart::ApiLocalScope* scope;
  intptr_t depth, handles;
  dart::StackResource* resource;
  dart::uword exit, tag;
};
std::vector<CallbackScopeState> callback_scopes;
intptr_t callback_scope_checks = 0;
void CallbackScopeProbe(int phase, intptr_t expected_growth) {
  auto* t = dart::Thread::Current();
  Expect(t != nullptr && t->execution_state()==dart::Thread::kThreadInNative,
         "callback scope probe requires native execution state");
  auto* scope=t->api_top_scope(); intptr_t depth=0;
  for(auto* s=scope;s;s=s->previous()) ++depth;
  if(phase==0) {
    callback_scopes.push_back({scope,depth,t->CountLocalHandles(),
                              t->top_resource(),t->top_exit_frame_info(),t->vm_tag()});
  } else {
    Expect(!callback_scopes.empty(),"callback scope probe nesting underflow");
    const auto before=callback_scopes.back();callback_scopes.pop_back();
    Expect(scope==before.scope && depth==before.depth &&
               t->CountLocalHandles()==before.handles+expected_growth &&
               t->top_resource()==before.resource && t->top_exit_frame_info()==before.exit &&
               t->vm_tag()==before.tag,
           "callback leaked scopes/handles/exit/resource/tag or returned an unowned Handle");
    ++callback_scope_checks;
  }
}
void RunCallbackHandleScopes(const char* uri, bool embedding_scope) {
  using namespace dart;
  auto* t=Thread::Current();TransitionNativeToVM transition(t);StackZone stack_zone(t);
  auto* z=t->zone();
  Expect((t->api_top_scope()!=nullptr)==embedding_scope,"unexpected Handle callback embedding scope");
  auto* parent=t->api_top_scope();const auto handles=t->CountLocalHandles();
  const auto checks=callback_scope_checks;
  const auto& lib=Library::Handle(z,Library::LookupLibrary(t,String::Handle(z,String::New(uri))));
  const auto& function=Function::Handle(z,lib.LookupFunctionAllowPrivate(
      String::Handle(z,Symbols::New(t,"runHandleScopes"))));
  Expect(!function.IsNull() && function.HasBytecode() && function.IsInterpreted(),
         "Handle callback scope test is not interpreted bytecode");
  const auto& result=Object::Handle(z,DartEntry::InvokeFunction(function,Object::empty_array()));
  Expect(!result.IsError(),result.IsError()?Error::Cast(result).ToErrorCString():"");
  Expect(result.IsInteger() && Integer::Cast(result).Value()==20 &&
             t->api_top_scope()==parent && t->CountLocalHandles()==handles &&
             callback_scopes.empty() && callback_scope_checks==checks+260,
         "Handle callback scopes/counts did not restore across all normal/error/nested cases");
  std::cout << "PRODUCT callback Handle caller-scope/error/GC/nesting checks passed with "
            << (embedding_scope?"embedding scope":"no embedding scope") << std::endl;
}
void AggregateChecks(Dart_Handle library,Dart_PersistentHandle root,const char* uri,
                     void* fixture,const char* path,bool missing_scope) {
  const auto initial=Count();Expect(initial.callbacks==0 && !initial.ports && !initial.keepalive,
                                   "aggregate baseline contains application callbacks");
  using SetProbe=void(*)(void(*)(int,intptr_t));
  auto setter=reinterpret_cast<SetProbe>(dlsym(fixture,"SetCallbackScopeProbe"));
  Expect(setter!=nullptr,"native callback scope probe setter missing");setter(CallbackScopeProbe);
  Dart_Handle path_argument=Dart_NewStringFromCString(path);
  auto all=Invoke(library,"prepare",1,&path_argument);intptr_t length=0;Check(Dart_ListLength(all,&length));
  Expect(length>30,"aggregate/Handle shape descriptors incomplete");
  std::vector<uintptr_t> closable;
  const auto checkpoint=Mappings();
  for(intptr_t i=0;i<length;++i) {
    const auto row=Addresses(Dart_ListGetAt(all,i));Expect(row.size()==4,"invalid descriptor expectation row");
    const bool capture=row[1]!=0;
    Inspect(row[0],true,capture,&checkpoint,row[2],row[3]);
    if(capture)closable.push_back(row[0]);
  }
  const auto live=Count();
  Expect(live.callbacks==initial.callbacks+length && live.roots==initial.roots+length &&
             live.code==initial.code,"Handle/aggregate registration grew callback code cache or has wrong root count");
  if(missing_scope) {
    const auto pointer=Integer(Invoke(library,"missingResultPointer"));
    Dart_ExitScope();Expect(dart::Thread::Current()->api_top_scope()==nullptr,
                           "missing-result-scope death test still owns an API scope");
    reinterpret_cast<Dart_Handle(*)()>(pointer)();
    Expect(false,"Handle result callback returned without an API scope");
  }
  const auto shapes=Integer(Invoke(library,"runAggregates"));Expect(shapes>=40,"aggregate native shape calls incomplete");
  RunCallbackHandleScopes(uri,true);
  Invoke(library,"runHandleSelfClose");
  const auto direct=Addresses(Invoke(library,"directPointers"));Expect(direct.size()==2,"direct Handle callback controls absent");
  auto token=Dart_NewPersistentHandle(Dart_NewInteger(79));
  Dart_ExitScope();Expect(dart::Thread::Current()->api_top_scope()==nullptr,"direct Handle test retained parent scope");
  RunCallbackHandleScopes(uri,false);
  Expect(reinterpret_cast<int64_t(*)(Dart_Handle)>(direct[0])(
             reinterpret_cast<Dart_Handle>(token))==79,
         "input-only Handle callback wrongly required a local API scope");
  using Explicit=int64_t(*)(Dart_Handle(*)());
  auto explicit_scope=reinterpret_cast<Explicit>(dlsym(fixture,"HandleCallbackExplicitScope"));
  Expect(explicit_scope!=nullptr && explicit_scope(reinterpret_cast<Dart_Handle(*)()>(direct[1]))==83 &&
             dart::Thread::Current()->api_top_scope()==nullptr &&
             dart::Thread::Current()->CountLocalHandles()==0 && callback_scopes.empty(),
         "explicit native caller scope failed to own/restore Handle result");
  Dart_EnterScope();library=Dart_HandleFromPersistent(root);Dart_DeletePersistentHandle(token);
  Invoke(library,"close");Dead(closable);
  const auto after=Count();
  Expect(after.callbacks==initial.callbacks+2 && after.roots==initial.roots+2 &&
             after.code==initial.code && !after.keepalive && !after.ports,
         "Handle/aggregate close leaked descriptors/scopes/ports (two static pointers remain live)");
  std::cout<<"PRODUCT callback Handle+aggregate descriptor/ownership/direct-scope/cleanup checks passed"<<std::endl;
}
std::atomic<intptr_t> async_group_cleanup_count{0};
std::atomic<unsigned> async_group_cleanup_mask{0};
void AsyncGroupCleanup(void* data){++async_group_cleanup_count;if(data==reinterpret_cast<void*>(1))async_group_cleanup_mask.fetch_or(1);if(data==reinterpret_cast<void*>(2))async_group_cleanup_mask.fetch_or(2);}
Dart_PersistentHandle async_self_wrapper=nullptr,async_self_library=nullptr;
void CloseAsyncGroupSelf(){
  using namespace dart;auto*t=Thread::Current();TransitionNativeToVM native_to_vm(t);StackZone zone(t);auto*z=t->zone();
  Expect(t->isolate()==nullptr,"group self-close unexpectedly acquired isolate");
  const auto& lib=Library::Handle(z,Library::RawCast(PersistentHandle::Cast(async_self_library)->ptr()));
  const auto& function=Function::Handle(z,lib.LookupFunctionAllowPrivate(String::Handle(z,Symbols::New(t,"closeOne"))));
  const auto& wrapper=Object::Handle(z,PersistentHandle::Cast(async_self_wrapper)->ptr());
  const auto& arguments=Array::Handle(z,Array::New(1));arguments.SetAt(0,wrapper);
  const auto& result=Object::Handle(z,DartEntry::InvokeFunction(function,arguments));
  Expect(!result.IsError(),result.IsError()?Error::Cast(result).ToErrorCString():"");
}
struct AsyncState {
  dart::Thread* thread; dart::Isolate* isolate; dart::IsolateGroup* group;
  dart::ApiLocalScope* scope; dart::StackResource* resource;
  dart::uword exit,tag; intptr_t handles; bool safepoint;
};
thread_local std::vector<AsyncState> async_states;
void AsyncStateProbe(int phase) {
  auto* t=dart::Thread::Current();
  if(phase==0) {
    async_states.push_back({t,t?t->isolate():nullptr,t?t->isolate_group():nullptr,
      t?t->api_top_scope():nullptr,t?t->top_resource():nullptr,
      t?t->top_exit_frame_info():0,t?t->vm_tag():0,t?t->CountLocalHandles():0,
      t?t->IsAtSafepoint():false});return;
  }
  Expect(!async_states.empty(),"async/group native probe underflow");
  const auto before=async_states.back();async_states.pop_back();
  Expect(t==before.thread,"callback failed to restore caller Thread identity");
  const bool matches = !t || (t->execution_state()==dart::Thread::kThreadInNative &&
    t->isolate()==before.isolate && t->isolate_group()==before.group &&
    t->api_top_scope()==before.scope && t->top_resource()==before.resource &&
    t->top_exit_frame_info()==before.exit && t->vm_tag()==before.tag &&
    t->CountLocalHandles()==before.handles+(phase==2?1:0) &&
    t->IsAtSafepoint()==before.safepoint);
  if(!matches) {
    std::cerr << "ASYNC_STATE phase=" << phase
      << " state=" << t->execution_state() << "/" << dart::Thread::kThreadInNative
      << " isolate=" << t->isolate() << "/" << before.isolate
      << " group=" << t->isolate_group() << "/" << before.group
      << " scope=" << t->api_top_scope() << "/" << before.scope
      << " resource=" << t->top_resource() << "/" << before.resource
      << " exit=" << t->top_exit_frame_info() << "/" << before.exit
      << " tag=" << t->vm_tag() << "/" << before.tag
      << " handles=" << t->CountLocalHandles() << "/" << before.handles+(phase==2?1:0)
      << " safepoint=" << t->IsAtSafepoint() << "/" << before.safepoint << std::endl;
  }
  Expect(matches,"callback failed to restore native isolate/group/safepoint/scope/frame state");
  // DLRT_Get deliberately changes runtime scratch args. The async cookie is
  // private to its selected epilogue, not a preserved native caller register.
}
std::vector<std::pair<dart::IsolateGroup*,dart::Isolate*>> async_owners;
int64_t AsyncContextProbe(int64_t kind,int64_t group_address) {
  using namespace dart;auto*t=Thread::Current();
  Expect(t && t->execution_state()==Thread::kThreadInNative &&
         t->isolate_group()==reinterpret_cast<IsolateGroup*>(group_address),
         "listener/group target executed in wrong group or state");
  if(kind==1) {
    auto it=std::find_if(async_owners.begin(),async_owners.end(),[&](const auto& p){return p.first==t->isolate_group();});
    Expect(it!=async_owners.end() && t->isolate()==it->second,"listener delivered outside creating isolate");
  } else {
    Expect(kind==2 && t->isolate()==nullptr,"group target acquired an ordinary isolate");
    TransitionNativeToVM transition(t);
    t->isolate_group()->heap()->CollectAllGarbage(GCReason::kDebugging);
  }
  return 1;
}
std::vector<std::pair<uintptr_t,dart::IsolateGroup*>> async_alias_groups;
void ForgetAsyncAliases(dart::IsolateGroup* group) {
  for(const auto& item:async_alias_groups)if(item.second==group) {
    Permissions(item.first,source_bytes.size(),true);
    Expect(std::memcmp(reinterpret_cast<void*>(item.first),source_bytes.data(),source_bytes.size())==0,
           "released alias bytes changed before group shutdown");
    aliases.erase(item.first);
  }
}
Dart_PersistentHandle async_final_library=nullptr;
struct AsyncEnvironment {
  Dart_Isolate isolate;dart::IsolateGroup* group;Dart_PersistentHandle library;
  std::vector<std::vector<uintptr_t>> specs;
};
Dart_Handle AsyncLibrary(const AsyncEnvironment& env){return Dart_HandleFromPersistent(env.library);}
void LeaveAsync(){Dart_ExitScope();Dart_ExitIsolate();}
void EnterAsync(const AsyncEnvironment& env){Dart_EnterIsolate(env.isolate);Dart_EnterScope();}
void RunnableAsync(Dart_Isolate isolate){
  LeaveAsync();char*error=Dart_IsolateMakeRunnable(isolate);Expect(!error,error?error:"");
  Dart_EnterIsolate(isolate);Dart_EnterScope();
}
void PrepareAsync(AsyncEnvironment* env,const char* path,const char* uri,bool interpreted) {
  async_owners.push_back({env->group,dart::Thread::Current()->isolate()});
  Dart_Handle args[]={Dart_NewStringFromCString(path),Dart_NewIntegerFromUint64(reinterpret_cast<uintptr_t>(env->group))};
  auto result=Invoke(AsyncLibrary(*env),"prepare",2,args);intptr_t n=0;Check(Dart_ListLength(result,&n));
  const auto checkpoint=Mappings();
  for(intptr_t i=0;i<n;++i){auto row=Addresses(Dart_ListGetAt(result,i));Expect(row.size()==5,"invalid async/group descriptor spec");
    Inspect(row[0],interpreted,row[2]!=0,&checkpoint,row[3],row[4],row[1]);async_alias_groups.push_back({static_cast<uintptr_t>(dart::FfiCallbackMetadata::MappingStart(row[0])),env->group});env->specs.push_back(row);}
  Expect(n==13,"async/group cold callback shape set incomplete");
  if(interpreted){
    using namespace dart;auto*t=Thread::Current();TransitionNativeToVM transition(t);StackZone zone(t);
    const auto& library=Library::Handle(t->zone(),Library::LookupLibrary(t,String::Handle(t->zone(),String::New(uri))));
    const auto& handler=Function::Handle(t->zone(),library.LookupFunctionAllowPrivate(String::Handle(t->zone(),Symbols::New(t,"onPayload"))));
    Expect(!handler.IsNull() && handler.HasBytecode() && handler.IsInterpreted(),"listener handler is not new-URI bytecode");
  }
}
std::vector<uintptr_t> AsyncStatus(const AsyncEnvironment& env){return Addresses(Invoke(AsyncLibrary(env),"status"));}
void PumpAsync(const AsyncEnvironment& env,int index,intptr_t expected) {
  for(int i=0;i<expected+20;++i){if(static_cast<intptr_t>(AsyncStatus(env)[index])==expected)return;Check(Dart_HandleMessage());}
  Expect(false,"listener message queue failed to reach expected delivery count");
}
void CollectAsync(){using namespace dart;auto*t=Thread::Current();TransitionNativeToVM transition(t);t->isolate_group()->heap()->CollectAllGarbage(GCReason::kDebugging);}
void RunAsyncGroups(const DartEngine_SnapshotData& snapshot,AsyncEnvironment owner,
                    const std::vector<uint8_t>& bytes,const char* uri,const char* path,
                    void* fixture,bool interpreted,int route_only) {
  using namespace dart;
  auto set_state=reinterpret_cast<void(*)(void(*)(int))>(dlsym(fixture,"SetAsyncGroupStateProbe"));
  auto set_context=reinterpret_cast<void(*)(int64_t(*)(int64_t,int64_t))>(dlsym(fixture,"SetAsyncGroupContextProbe"));
  Expect(set_state&&set_context,"async/group probes absent");set_state(AsyncStateProbe);set_context(AsyncContextProbe);
  async_final_library=owner.library;
  const auto baseline=Count();PrepareAsync(&owner,path,uri,interpreted);
  const auto cold=Count();PrintCounts("cold baseline",baseline);PrintCounts("cold registered",cold);
  Expect(cold.callbacks==baseline.callbacks+7 && cold.ports &&
      cold.keepalive==baseline.keepalive && cold.code==baseline.code,
      "cold listener/group registration changed code cache or failed to retain ports");
  if(interpreted)Expect(cold.roots==baseline.roots+13,"cold runtime descriptor root count mismatch");
  LeaveAsync();char*error=nullptr;
  auto peer_isolate=Dart_CreateIsolateInGroup(owner.isolate,"async-group-peer",nullptr,nullptr,nullptr,&error);
  Expect(peer_isolate&&!error,error?error:"same-group peer creation failed");Dart_EnterScope();
  Check(bin::DartUtils::SetupCoreLibraries(false,false,false,bin::DartIoSettings{}));
  auto peer_library=interpreted?Dart_LookupLibrary(Dart_NewStringFromCString(uri)):Dart_RootLibrary();Check(peer_library);
  AsyncEnvironment peer{peer_isolate,Thread::Current()->isolate_group(),Dart_NewPersistentHandle(peer_library),{}};
  RunnableAsync(peer.isolate);LeaveAsync();
  Dart_IsolateFlags flags;Dart_IsolateFlagsInitialize(&flags);
  auto foreign_isolate=Dart_CreateIsolateGroup(snapshot.script_uri,"async-foreign",snapshot.snapshot_data,snapshot.snapshot_text,&flags,reinterpret_cast<void*>(2),nullptr,&error);
  Expect(foreign_isolate&&!error,error?error:"foreign group creation failed");Dart_EnterScope();
  Check(bin::DartUtils::SetupCoreLibraries(false,false,false,bin::DartIoSettings{}));
  Dart_Handle foreign_library=Dart_RootLibrary();
  if(interpreted){auto buffer=Dart_NewExternalTypedData(Dart_TypedData_kUint8,const_cast<uint8_t*>(bytes.data()),bytes.size());Check(buffer);foreign_library=Dart_LoadLibraryFromBytecode(buffer);Check(foreign_library);}
  AsyncEnvironment foreign{foreign_isolate,Thread::Current()->isolate_group(),Dart_NewPersistentHandle(foreign_library),{}};
  RunnableAsync(foreign.isolate);PrepareAsync(&foreign,path,uri,interpreted);LeaveAsync();EnterAsync(owner);
  auto scalar=owner.specs[0][0],payload=owner.specs[1][0],group=owner.specs[7][0],capture=owner.specs[8][0];
  auto post=reinterpret_cast<void(*)(void*,int64_t)>(dlsym(fixture,"AsyncPostScalar"));
  auto post_payload=reinterpret_cast<void(*)(void*,Dart_Handle,int64_t)>(dlsym(fixture,"AsyncPostPayload"));
  auto call=reinterpret_cast<int64_t(*)(void*,int64_t,int64_t)>(dlsym(fixture,"AsyncCallGroup"));
  auto threads=reinterpret_cast<int64_t(*)(void*,void*,void*,Dart_Handle,int64_t,int64_t)>(dlsym(fixture,"AsyncRunThreads"));
  Expect(post&&post_payload&&call&&threads,"async native helper absent");
  // Four independent, controlled nested routes run for both compiled and
  // interpreted targets. A failed inherited transition stays a test failure.
  for(int route=0;route<4;++route){if(route_only>=0&&route!=route_only)continue;
    Dart_Handle args[]={Dart_NewIntegerFromUint64(route<2?(route==0?capture:foreign.specs[8][0]):0),
                        Dart_NewIntegerFromUint64(route<2?0:(route==2?scalar:foreign.specs[0][0]))};
    auto pointer=Integer(Invoke(AsyncLibrary(owner),"createRoute",2,args));Inspect(pointer,interpreted,true,nullptr,0,0,1);
    std::cout<<"NESTED_ROUTE_BEGIN:"<<route<<std::endl;
    Expect(call(reinterpret_cast<void*>(pointer),21,5)==(route<2?64:22),"nested same/cross group/listener result mismatch");
    std::cout<<"NESTED_ROUTE_PASS:"<<route<<std::endl;
  }
  if(route_only>=0){
    // An empty source group mutator has no frames, API scope or VM zone, so
    // callback transitions must explicitly suspend it rather than pool/reset
    // its Thread. Cover same/cross group callbacks and listeners independently.
    LeaveAsync();
    Thread::EnterIsolateGroupAsMutator(owner.group, /*bypass_safepoint=*/false);
    Thread* const empty_source=Thread::Current();
    Expect(!empty_source->HasActiveState(),"empty-source test acquired active state");
    empty_source->set_execution_state(Thread::kThreadInNative);
    empty_source->EnterSafepointToNative();
    empty_source->set_vm_tag(reinterpret_cast<uword>(call));
    const intptr_t owner_mutators=owner.group->MutatorCount();
    const intptr_t foreign_mutators=foreign.group->MutatorCount();
    std::cout<<"EMPTY_SOURCE_BEGIN:"<<route_only<<std::endl;
    if(route_only<2){
      const auto target=route_only==0?capture:foreign.specs[8][0];
      Expect(call(reinterpret_cast<void*>(target),21,5)==63,"empty-source group result mismatch");
    }else{
      const auto target=route_only==2?scalar:foreign.specs[0][0];
      post(reinterpret_cast<void*>(target),22);
    }
    Expect(Thread::Current()==empty_source && !empty_source->HasActiveState(),
           "empty-source callback changed Thread identity or acquired state");
    Expect(owner.group->MutatorCount()==owner_mutators &&
           foreign.group->MutatorCount()==foreign_mutators,
           "empty-source callback unbalanced group mutator counts");
    {TransitionNativeToVM transition(empty_source);
      owner.group->heap()->CollectAllGarbage(GCReason::kDebugging);}
    empty_source->ExitSafepointFromNative();
    empty_source->set_execution_state(Thread::kThreadInVM);
    Thread::ExitIsolateGroupAsMutator(/*bypass_safepoint=*/false);
    Expect(Thread::Current()==nullptr,"empty-source cleanup retained current Thread");
    EnterAsync(owner);
    std::cout<<"EMPTY_SOURCE_PASS:"<<route_only<<std::endl;
    // No caller can race close: every native route above returned synchronously.
    Invoke(AsyncLibrary(owner),"closeListeners");auto groups=Invoke(AsyncLibrary(owner),"exportGroups");Invoke(AsyncLibrary(owner),"closeExported",1,&groups);
    LeaveAsync();EnterAsync(foreign);Invoke(AsyncLibrary(foreign),"closeListeners");auto fgroups=Invoke(AsyncLibrary(foreign),"exportGroups");Invoke(AsyncLibrary(foreign),"closeExported",1,&fgroups);
    ForgetAsyncAliases(foreign.group);Dart_DeletePersistentHandle(foreign.library);Dart_ExitScope();Dart_ShutdownIsolate();EnterAsync(peer);Dart_DeletePersistentHandle(peer.library);Dart_ExitScope();Dart_ShutdownIsolate();EnterAsync(owner);return;
  }
  // Nested listener messages were deferred; drain them before the main counts.
  PumpAsync(owner,0,1);LeaveAsync();EnterAsync(foreign);PumpAsync(foreign,0,1);LeaveAsync();EnterAsync(owner);
  const auto start=AsyncStatus(owner);
  post(reinterpret_cast<void*>(scalar),10);Expect(AsyncStatus(owner)[0]==start[0],"listener executed synchronously");
  auto token_list=Dart_NewList(1);Check(Dart_ListSetAt(token_list,0,Dart_NewStringFromCString("queued handle")));
  auto token=Dart_NewPersistentHandle(token_list);
  post_payload(reinterpret_cast<void*>(payload),reinterpret_cast<Dart_Handle>(token),1);
  LeaveAsync();EnterAsync(peer);post(reinterpret_cast<void*>(scalar),20);post_payload(reinterpret_cast<void*>(payload),reinterpret_cast<Dart_Handle>(token),2);
  LeaveAsync();EnterAsync(foreign);post(reinterpret_cast<void*>(scalar),30);post_payload(reinterpret_cast<void*>(payload),reinterpret_cast<Dart_Handle>(token),3);
  LeaveAsync();
  // No Thread is current during joined pthread invocation. Target-group
  // persistent Handle remains valid until all native callers are quiescent.
  int64_t expected_sum=0;for(int i=0;i<4;++i)for(int j=0;j<3;++j)expected_sum+=1000+i*100+j+42;
  Expect(threads(reinterpret_cast<void*>(scalar),reinterpret_cast<void*>(payload),reinterpret_cast<void*>(capture),reinterpret_cast<Dart_Handle>(token),4,3)==expected_sum,"concurrent group callbacks returned wrong values");
  Expect(Thread::Current()==nullptr,"foreign pthread work changed caller Thread");EnterAsync(owner);
  Dart_DeletePersistentHandle(token);CollectAsync();
  PumpAsync(owner,0,start[0]+15);PumpAsync(owner,1,15);
  const auto throw_result=call(reinterpret_cast<void*>(owner.specs[9][0]),1,2);
  const auto static_result=call(reinterpret_cast<void*>(owner.specs[10][0]),1,2);
  std::cout<<"GROUP_FALLBACK throw="<<throw_result<<" ordinary="<<static_result<<std::endl;
  Expect(throw_result==-1903 && static_result==-1904,"group exception/static-access fallback mismatch");
  // Exercise all forms in compiled and interpreted code. The initializer's
  // native counter detects execution without itself reading isolate globals.
  auto initializer_count=reinterpret_cast<int64_t(*)()>(dlsym(fixture,"AsyncStaticInitializerCount"));
  Expect(initializer_count!=nullptr,"static initializer counter missing");
  const auto initializations=initializer_count();
  auto errors=Addresses(Invoke(AsyncLibrary(owner),"staticErrorPointers"));
  Expect(errors.size()==3,"zero-local static error probes missing");
  for(int i=0;i<3;++i){Inspect(errors[i],interpreted,false,nullptr,0,0,1);
    const auto result=call(reinterpret_cast<void*>(errors[i]),1,2);
    std::cout<<"STATIC_FRAME_FALLBACK:"<<i<<":"<<result<<std::endl;
    Expect(result==-1911-i,"zero-local static error damaged frame or missed fallback");
  }
  Expect(initializer_count()==initializations,"rejected group access executed the static initializer");
  if(interpreted){
    InspectStaticErrorBytecodes(uri);
  }
  Expect(Integer(Invoke(AsyncLibrary(owner),"verifyStaticOwnerAccess"))==1 &&
             initializer_count()==initializations+1,
         "ordinary isolate static access or once-only initialization failed");
  std::cout<<"STATIC_ACCESS_OWNER_AND_INITIALIZER_PASS"<<std::endl;
  auto varargs=Addresses(Invoke(AsyncLibrary(owner),"varargsPointers"));
  using Varargs=void(*)(void*,void*,void*,void*,void*);auto run_varargs=reinterpret_cast<Varargs>(dlsym(fixture,"AsyncVarArgs"));Expect(run_varargs&&varargs.size()==5,"listener VarArgs controls absent");
  run_varargs(reinterpret_cast<void*>(varargs[0]),reinterpret_cast<void*>(varargs[1]),reinterpret_cast<void*>(varargs[2]),reinterpret_cast<void*>(varargs[3]),reinterpret_cast<void*>(varargs[4]));reinterpret_cast<int64_t(*)()>(dlsym(fixture,"ClobberAsyncStack"))();CollectAsync();PumpAsync(owner,4,5);
  post(reinterpret_cast<void*>(scalar),-999);PumpAsync(owner,3,1);
  for(int which=0;which<3;++which){const auto before=Count();Dart_Handle arg=Dart_NewInteger(which);Invoke(AsyncLibrary(owner),"rejectMutable",1,&arg);ExpectCounts(before,"rejected group capture leaked callback/root/port");}
  post(reinterpret_cast<void*>(scalar),-1000);PumpAsync(owner,0,start[0]+16);
  // Queued messages dropped by port closure must release message-owned roots;
  // the pointer is invoked only BEFORE close, never after it becomes stale.
  const auto before_drop=Count();
  const auto before_drop_deliveries=AsyncStatus(owner);
  post_payload(reinterpret_cast<void*>(payload),Dart_Null(),99);
  Expect(Count().roots==before_drop.roots+1,"queued listener message does not own one persistent envelope root");
  Invoke(AsyncLibrary(owner),"closeListeners");
  Expect(Count().roots==before_drop.roots-(interpreted?6:0)+1,
         "closed port discarded a queued message root before dequeue");
  Check(Dart_HandleMessage());CollectAsync();
  Expect(AsyncStatus(owner)==before_drop_deliveries,
         "closed listener delivered its queued message");
  const auto closed=Count();Expect(closed.callbacks==baseline.callbacks && closed.roots==before_drop.roots-(interpreted?6:0) && !closed.ports&&!closed.keepalive,"listener close retained ports/list entries");
  for(auto&row:owner.specs)if(row[1]==2)Dead({row[0]});
  // Start with no live ports, so the existing boolean observation can detect
  // a leaked registration port instead of being masked by other listeners.
  if(interpreted){const auto before=Count();Expect(!before.ports,"listener failure probe requires no live ports");Invoke(AsyncLibrary(owner),"rejectMissingListener");ExpectCounts(before,"failed listener registration leaked callback/root/port");}
  // Cross more than one released alias page, close only after joined workers.
  const intptr_t n=2*FfiCallbackMetadata::NumCallbackTrampolinesPerPage()+17;
  std::set<uintptr_t> previous_pools[2];size_t pool_alias_count=0;
  for(int cycle=0;cycle<2;++cycle){for(bool group_pool:{false,true}){
    const auto before=Count();Dart_Handle args[]={Dart_NewInteger(n),Dart_NewBoolean(group_pool)};
    auto pointers=Addresses(Invoke(AsyncLibrary(owner),"createPool",2,args));std::set<uintptr_t> unique(pointers.begin(),pointers.end());Expect(unique.size()==static_cast<size_t>(n),"async/group live pool pointers overlap");
    const auto live=Count();
    Expect(live.callbacks==before.callbacks+(group_pool?0:n) &&
           live.roots==before.roots+((interpreted||group_pool)?n:0) &&
           live.code==before.code && live.keepalive==before.keepalive &&
           live.ports==(group_pool?before.ports:true),"async/group live pool ownership/code-cache mismatch");
    const auto checkpoint=Mappings();for(auto p:pointers)Inspect(p,interpreted,group_pool,&checkpoint,0,0,group_pool?1:2);
    if(cycle==0&&!group_pool)pool_alias_count=aliases.size();
    else Expect(aliases.size()==pool_alias_count,"async/group closed pages did not recycle");
    if(cycle==1){bool reused=false;for(auto pointer:unique)reused|=previous_pools[group_pool].count(pointer)!=0;Expect(reused,"async/group callback entries were not reused after quiescent close");}
    previous_pools[group_pool]=unique;
    const auto deliveries=AsyncStatus(owner)[0];
    auto run_pool=reinterpret_cast<void(*)(const uintptr_t*,int64_t,int64_t)>(dlsym(fixture,"AsyncRunPoolThreads"));Expect(run_pool!=nullptr,"quiescent native pool helper absent");
    LeaveAsync();run_pool(pointers.data(),n,group_pool);Expect(Thread::Current()==nullptr,"pool pthread work changed owner Thread");EnterAsync(owner);
    if(group_pool)Invoke(AsyncLibrary(owner),"closePoolGroups");
    else{PumpAsync(owner,0,deliveries+n);Invoke(AsyncLibrary(owner),"closeListeners");}
    Dead(pointers);ExpectCounts(before,"quiescent async/group pool close leaked resources");
  }}
  // Establish the Handle result scope on the actual target group Thread. A
  // different isolate's local scope is not borrowed across a group transition.
  auto handle_token=Dart_NewPersistentHandle(Dart_NewStringFromCString("group token"));
  LeaveAsync();Thread::EnterIsolateGroupAsMutator(owner.group,false);
  {auto*t=Thread::Current();StackZone zone(t);t->EnterApiScope();
    {TransitionVMToNative native(t);
      auto handle_call=reinterpret_cast<Dart_Handle(*)(void*,Dart_Handle)>(dlsym(fixture,"AsyncGroupHandle"));
      Expect(handle_call!=nullptr,"group Handle native helper missing");
      handle_call(reinterpret_cast<void*>(owner.specs[11][0]),reinterpret_cast<Dart_Handle>(handle_token));
      struct LargeResult{int64_t a,b,c;};
      auto large_call=reinterpret_cast<LargeResult(*)(void*)>(dlsym(fixture,"AsyncGroupLarge"));
      Expect(large_call!=nullptr,"group aggregate native helper missing");const auto result=large_call(reinterpret_cast<void*>(owner.specs[12][0]));
      Expect(result.a==48&&result.b==22&&result.c==33,"group aggregate result mismatch");
    }t->ExitApiScope();}
  Thread::ExitIsolateGroupAsMutator(false);EnterAsync(owner);Dart_DeletePersistentHandle(handle_token);
  const auto before_self=Count();
  auto self=Invoke(AsyncLibrary(owner),"createSelfClose");
  auto self_pointer=Integer(Dart_ListGetAt(self,0));
  async_self_wrapper=Dart_NewPersistentHandle(Dart_ListGetAt(self,1));async_self_library=owner.library;
  auto set_close=reinterpret_cast<void(*)(void(*)())>(dlsym(fixture,"SetAsyncGroupSelfClose"));Expect(set_close!=nullptr,"group self-close setter missing");set_close(CloseAsyncGroupSelf);
  Inspect(self_pointer,interpreted,true,nullptr,0,0,1);
  Expect(call(reinterpret_cast<void*>(self_pointer),7,8)==52,"group self-close/GC return mismatch");
  Dead({static_cast<uintptr_t>(self_pointer)});Invoke(AsyncLibrary(owner),"checkSelfClosed");Dart_DeletePersistentHandle(async_self_wrapper);async_self_wrapper=nullptr;
  ExpectCounts(before_self,"group self-close leaked root or callback");
  const auto abandoned=Integer(Invoke(AsyncLibrary(owner),"abandonListener"));
  Inspect(abandoned,interpreted,false,nullptr,0,0,2);
  auto survivors=Invoke(AsyncLibrary(owner),"exportGroups");auto survivor_root=Dart_NewPersistentHandle(survivors);
  // Creator shutdown removes listener metadata but group callbacks remain owned
  // by the group while the peer is alive. No closed pointer is invoked.
  Dart_DeletePersistentHandle(owner.library);Dart_ExitScope();Dart_ShutdownIsolate();EnterAsync(peer);
  Dead({static_cast<uintptr_t>(abandoned)});
  Expect(call(reinterpret_cast<void*>(group),7,8)==29,"group callback did not survive creator shutdown");
  auto objects=Dart_HandleFromPersistent(survivor_root);Invoke(AsyncLibrary(peer),"closeExported",1,&objects);Dart_DeletePersistentHandle(survivor_root);
  for(auto&row:owner.specs)if(row[1]==1)Dead({row[0]});
  LeaveAsync();EnterAsync(foreign);Invoke(AsyncLibrary(foreign),"closeListeners");auto fgroups=Invoke(AsyncLibrary(foreign),"exportGroups");Invoke(AsyncLibrary(foreign),"closeExported",1,&fgroups);
  ForgetAsyncAliases(foreign.group);Dart_DeletePersistentHandle(foreign.library);Dart_ExitScope();Dart_ShutdownIsolate();EnterAsync(peer);
  std::cout<<"PRODUCT listener/group deferred/Zone/VarArgs/pthread/copy/GC/transitions/lifecycle checks passed"<<std::endl;
  // Main's existing shutdown path now owns the live peer, its library root.
  // owner.library was deleted above; communicate the peer root through output.
  async_final_library=peer.library;
}

void RuntimeChecks(Dart_Handle library, Dart_PersistentHandle library_root) {
  using dart::FfiCallbackMetadata;
  const auto initial=Count();
  Expect(initial.callbacks==0 && !initial.keepalive && !initial.ports,
         "released baseline unexpectedly contains application callbacks/ports");
  auto statics=Addresses(Invoke(library,"staticPointers"));
  auto extras=Addresses(Invoke(library,"extraStaticPointers"));
  statics.insert(statics.end(),extras.begin(),extras.end());
  Expect(statics.size()==14,"static shape coverage incomplete");
  for(auto p:statics) Inspect(p,true);
  const auto static_counts=Count();
  Expect(static_counts.code==initial.code &&
             static_counts.roots==initial.roots+14 && static_counts.callbacks==14,
         "runtime static callbacks must use exactly one root each and no code cache");
  for(int phase=0;phase<2;++phase) {
    std::cout << "STAGE: static callbacks " << phase << std::endl;
    Dart_Handle argument=Dart_NewInteger(phase);
    const auto expected=phase==0?6:8;
    Expect(Integer(Invoke(library,"runCounted",1,&argument))==expected,"static callback coverage incomplete");
  }
  const intptr_t count=2*FfiCallbackMetadata::NumCallbackTrampolinesPerPage()+17;
  std::set<uintptr_t> first;
  size_t alias_count=0;
  for(int cycle=0;cycle<2;++cycle) {
    std::cout << "STAGE: callback pool create " << cycle << std::endl;
    const auto before=Count(); Dart_Handle argument=Dart_NewInteger(count);
    auto pointers=Addresses(Invoke(library,"createPool",1,&argument));
    Expect(pointers.size()==static_cast<size_t>(count),"pool size mismatch");
    std::set<uintptr_t> unique(pointers.begin(),pointers.end());
    Expect(unique.size()==pointers.size(),"live callbacks share a trampoline");
    const auto live=Count();
    Expect(live.callbacks==before.callbacks+count && live.roots==before.roots+count &&
               live.code==before.code && live.keepalive,"pool descriptor roots/cache/keepalive mismatch");
    // All descriptors and alias bytes are checked against one mapping
    // checkpoint, avoiding thousands of identical kernel map enumerations.
    const auto checkpoint = Mappings();
    for(auto p:pointers) Inspect(p,true,true,&checkpoint);
    std::cout << "STAGE: callback pool invoke " << cycle << std::endl;
    Expect(Integer(Invoke(library,"runPool"))==count,"pool callbacks were not all invoked");
    std::cout << "STAGE: callback pool close " << cycle << std::endl;
    Invoke(library,"closePool"); Dead(pointers);
    ExpectCounts(before,"closing pool leaked roots/list/keepalive/ports");
    if(cycle==0) {first=unique; alias_count=aliases.size();}
    else {
      bool reused=false; for(auto p:unique) reused|=first.count(p)!=0;
      Expect(reused && aliases.size()==alias_count,"callback pages were not reused after close");
    }
  }
  for(int phase=2;phase<5;++phase) {
    std::cout << "STAGE: capturing callbacks " << phase << std::endl;
    const auto before=Count(); Dart_Handle argument=Dart_NewInteger(phase);
    const auto value=Integer(Invoke(library,"runCounted",1,&argument));
    Expect(phase==4 ? value==0||value==1 : value==(phase==2?4:10),"capturing callback coverage incomplete");
    ExpectCounts(before,"capture/nesting/self-close leaked registration state");
  }
  for(int repeat=0;repeat<3;++repeat) for(int which : {4,6}) {
    const auto before=Count(); Dart_Handle argument=Dart_NewInteger(which);
    Expect(Integer(Invoke(library,"rejectCase",1,&argument))==which,"registration rejection incomplete");
    ExpectCounts(before,"rejected callback leaked root/list/keepalive/listener port");
  }
  const auto before_owned=Count();
  const uintptr_t owned=Integer(Invoke(library,"createOwned")); Inspect(owned,true,true);
  auto isolate=Dart_CurrentIsolate();
  Dart_SetCurrentThreadOwnsIsolate();
  Dart_ExitScope(); Dart_ExitIsolate();
  Expect(dart::Thread::Current()==nullptr,"ownership control still has current Thread");
  using Callback=int64_t(*)(int64_t,int64_t);
  const int64_t result=reinterpret_cast<Callback>(owned)(7,8);
  Expect(result==315 && dart::Thread::Current()==nullptr,
         "owned-isolate callback failed to exit its temporary Thread entry");
  Dart_EnterIsolate(isolate); Dart_EnterScope();
  // The old local handle belonged to the exited scope; its existing persistent
  // root keeps the actual library alive across the no-current-Thread call.
  auto callback_library=Dart_HandleFromPersistent(library_root);
  Invoke(callback_library,"closeOwned");
  Dead({owned}); ExpectCounts(before_owned,"owned-isolate callback close leaked metadata");
  std::cout << "PRODUCT interpreted static/capture scalar/pointer/VarArgs, GC, nesting, self-close, growth/reuse, rejection cleanup and owned-isolate callback checks passed" << std::endl;
}
}  // namespace

int main(int argc,char**argv) {
  // snapshot, bytecode-or-dash, dylib, URI, common-symbol-start/end/imagebase, mode
  if(argc!=9) return 2;
  Expect(Dart_IsPrecompiledRuntime(),"PRODUCT callback test must be precompiled");
  const bool shared_runtime=std::strcmp(argv[8],"async-shared-runtime")==0;
  const bool shared_control=std::strcmp(argv[8],"async-shared-control")==0;
  const bool shared_mode=shared_runtime||shared_control;
  char* error = nullptr;
  if (shared_mode) {
    // DartEngine_Init installs its own flags. Use the normal embedder setup
    // here so both required flags are provided in the single allowed call.
    Expect(dart::embedder::InitOnce(&error), error ? error : "embedder init failed");
    const char* vm_flags[] = {"--precompilation", "--experimental-shared-data"};
    error = Dart_SetVMFlags(2, vm_flags);
    Expect(error == nullptr, error ? error : "");
  } else {
    Expect(DartEngine_Init(&error), error ? error : "engine init failed");
  }
  const auto snapshot=DartEngine_AotSnapshotFromFile(argv[1],&error);
  Expect(error==nullptr,error?error:"");
  Dart_InitializeParams params={};params.version=DART_INITIALIZE_PARAMS_CURRENT_VERSION;params.cleanup_group=AsyncGroupCleanup;
  error=Dart_Initialize(&params);Expect(error==nullptr,error?error:"");
  Dart_IsolateFlags flags;Dart_IsolateFlagsInitialize(&flags);
  auto isolate=Dart_CreateIsolateGroup(snapshot.script_uri,"product-ffi-callback-test",
      snapshot.snapshot_data,snapshot.snapshot_text,&flags,reinterpret_cast<void*>(1),nullptr,&error);
  Expect(isolate!=nullptr&&error==nullptr,error?error:"isolate creation failed");
  Dart_EnterScope();
  Check(dart::bin::DartUtils::SetupCoreLibraries(false,false,false,dart::bin::DartIoSettings{}));
  auto*fixture=dlopen(argv[3],RTLD_NOW|RTLD_GLOBAL);
  Expect(fixture!=nullptr,"cannot load signed native fixture");
  const bool async_runtime=std::strncmp(argv[8],"async-runtime",13)==0;
  const bool async_control=std::strncmp(argv[8],"async-control",13)==0;
  const bool async_mode=async_runtime||async_control;
  const int route_only=std::strlen(argv[8])>13 ? std::atoi(argv[8]+14) : -1;
  const bool control=std::strcmp(argv[8],"control")==0;
  const bool aggregate=std::strcmp(argv[8],"aggregate")==0;
  const bool missing_scope=std::strcmp(argv[8],"missing-scope")==0;
  const bool handle_result_kill=std::strcmp(argv[8],"handle-result-kill")==0;
  const bool kill=std::strcmp(argv[8],"kill")==0;
  const bool handle_kill=std::strcmp(argv[8],"handle-kill")==0;
  Expect(shared_mode||async_mode||control||aggregate||missing_scope||handle_result_kill||kill||handle_kill||std::strcmp(argv[8],"runtime")==0,"unknown test mode");
  const auto before=Mappings();
  InitializeCodeProof(std::strtoull(argv[5],nullptr,10),std::strtoull(argv[6],nullptr,10),std::strtoull(argv[7],nullptr,10));
  std::vector<uint8_t> bytes;
  Dart_Handle library=Dart_RootLibrary();
  if(!control&&!async_control&&!shared_control) {
    Expect(Count().callbacks==0,"native baseline already has app callbacks");
    std::ifstream input(argv[2],std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
    Expect(!bytes.empty(),"empty raw bytecode fixture");
    auto buffer=Dart_NewExternalTypedData(Dart_TypedData_kUint8,bytes.data(),bytes.size());Check(buffer);
    library=Dart_LoadLibraryFromBytecode(buffer);Check(library);
    auto lookup=Dart_LookupLibrary(Dart_NewStringFromCString(argv[4]));Check(lookup);
    Expect(Dart_IdentityEquals(library,lookup)&&!Dart_IdentityEquals(library,Dart_RootLibrary()),"callback fixture is not a new URI");
    InspectLoaded(argv[4]);
  }
  // Make runnable before callback kill/message/owned-isolate entry checks.
  auto root=Dart_NewPersistentHandle(library);
  Dart_ExitScope();Dart_ExitIsolate();
  error=Dart_IsolateMakeRunnable(isolate);Expect(error==nullptr,error?error:"");
  Dart_EnterIsolate(isolate);Dart_EnterScope();library=Dart_HandleFromPersistent(root);
  if(shared_mode) {
    // This is a handler flag; Dart_IsVMFlagSet only queries boolean flags.
    Expect(dart::FLAG_experimental_shared_data,"shared group callback flag is disabled");
    auto initializer_count = reinterpret_cast<int64_t(*)()>(
        dlsym(fixture, "AsyncStaticInitializerCount"));
    Expect(initializer_count != nullptr && initializer_count() == 0,
           "shared initializer ran before prepare");
    auto pointer=Integer(Invoke(library,"prepare"));Inspect(pointer,shared_runtime,false,nullptr,0,0,1);
    Expect(initializer_count() == 1, "shared initialization must run exactly once");
    auto call=reinterpret_cast<int64_t(*)(void*,int64_t,int64_t)>(dlsym(fixture,"AsyncCallGroup"));
    auto setter=reinterpret_cast<void(*)(void(*)(int))>(dlsym(fixture,"SetAsyncGroupStateProbe"));setter(AsyncStateProbe);
    Expect(call(reinterpret_cast<void*>(pointer),3,4)==1241,"group callback could not access flagged shared field");
    Expect(initializer_count() == 1, "group callback reinitialized shared field");
    Invoke(library,"close");Dead({static_cast<uintptr_t>(pointer)});
    std::cout<<"PRODUCT group callback flagged shared-field access passed"<<std::endl;
  } else if(async_mode) {
    AsyncEnvironment owner{isolate,dart::Thread::Current()->isolate_group(),root,{}};
    RunAsyncGroups(snapshot,owner,bytes,argv[4],argv[3],fixture,async_runtime,route_only);
    root=async_final_library;
  } else if(control) {
    Dart_Handle path=Dart_NewStringFromCString(argv[3]);
    auto pointers=Addresses(Invoke(library,"prepare",1,&path));
    Expect(pointers.size()==6,"compiled control shape coverage incomplete");
    for(auto p:pointers) Inspect(p,false);
    Invoke(library,"run");Check(Dart_HandleMessage());Invoke(library,"finish");
    Dead({pointers[1],pointers[2],pointers[3],pointers[4],pointers[5]});
    std::cout<<"Compiled static/capture/Handle/aggregate/listener/group-bound callback controls passed"<<std::endl;
  } else if(aggregate||missing_scope) {
    AggregateChecks(library,root,argv[4],fixture,argv[3],missing_scope);
  } else if(handle_result_kill) {
    Dart_Handle path=Dart_NewStringFromCString(argv[3]);
    auto pointer=Integer(Invoke(library,"prepare",1,&path));
    Inspect(pointer,true,true,nullptr,dart::kInterpreterFfiCallbackReturnsHandle,0);
    const auto result=Invoke(library,"trigger",0,nullptr,true);
    Expect(Dart_IsError(result)&&Dart_IsFatalError(result),"Handle result callback swallowed UnwindError");
    std::cout<<"API_UNWIND_OBSERVED"<<std::endl;
  } else if(kill||handle_kill) {
    Dart_Handle kind=Dart_NewBoolean(handle_kill);
    const auto pointer=Integer(Invoke(library,"prepare",1,&kind));
    Inspect(pointer,true,handle_kill);
    Dart_Handle arguments[]={Dart_NewStringFromCString(argv[3]),kind};
    const auto result=Invoke(library,"trigger",2,arguments,true);
    Expect(Dart_IsError(result)&&Dart_IsFatalError(result),"callback swallowed isolate termination");
    std::cout<<"API_UNWIND_OBSERVED"<<std::endl;
  } else {
    Dart_Handle arguments[]={Dart_NewStringFromCString(argv[3]),Dart_NewIntegerFromUint64(std::strtoull(argv[5],nullptr,10)),Dart_NewIntegerFromUint64(std::strtoull(argv[6],nullptr,10)),Dart_NewIntegerFromUint64(std::strtoull(argv[7],nullptr,10))};
    Invoke(library,"configure",4,arguments);
    RuntimeChecks(library,root);
  }
  Audit(before);
  Dart_DeletePersistentHandle(root);Dart_ExitScope();Dart_ShutdownIsolate();DartEngine_Shutdown();
  if(async_mode)Expect(async_group_cleanup_mask.load()==3,"async/group owner or foreign cleanup callback missing after VM shutdown");
  dlclose(fixture);return 0;
}
