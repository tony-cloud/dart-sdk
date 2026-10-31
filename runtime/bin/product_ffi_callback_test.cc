// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Internal PRODUCT embedder: genuine new-library bytecode, no fabricated
// functions/flags or public test API. Compiled controls use another snapshot.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <vector>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif
#include "bin/dartutils.h"
#include "include/dart_api.h"
#include "include/dart_engine.h"
#include "vm/dart_api_state.h"
#include "vm/dart_entry.h"
#include "vm/ffi/callback_function.h"
#include "vm/ffi/interpreter_callback.h"
#include "vm/ffi_callback_metadata.h"
#include "vm/handle_visitor.h"
#include "vm/heap/safepoint.h"
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
void ExpectCounts(const Counts& expected, const char* message) {
  const auto actual = Count();
  if (!(actual == expected)) {
    const auto print = [](const char* label, const Counts& value) {
      std::cerr << label << ": callbacks=" << value.callbacks
                << " roots=" << value.roots << " code=" << value.code
                << " keepalive=" << value.keepalive << " ports=" << value.ports
                << std::endl;
    };
    print("expected", expected);
    print("actual", actual);
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
             uint64_t expected_flags = 0, uint64_t expected_indirect = 0) {
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
               metadata.target_isolate() == t->isolate(),
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
    Expect(!target.IsNull() && target.HasBytecode() && target.IsInterpreted(),
           "callback target is not genuine interpreted bytecode");
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
  for(int repeat=0;repeat<3;++repeat) for(int which : {2,3,4,6}) {
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
  char*error=nullptr; Expect(DartEngine_Init(&error),error?error:"engine init failed");
  const auto snapshot=DartEngine_AotSnapshotFromFile(argv[1],&error);
  Expect(error==nullptr,error?error:"");
  Dart_InitializeParams params={};params.version=DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  error=Dart_Initialize(&params);Expect(error==nullptr,error?error:"");
  Dart_IsolateFlags flags;Dart_IsolateFlagsInitialize(&flags);
  auto isolate=Dart_CreateIsolateGroup(snapshot.script_uri,"product-ffi-callback-test",
      snapshot.snapshot_data,snapshot.snapshot_text,&flags,nullptr,nullptr,&error);
  Expect(isolate!=nullptr&&error==nullptr,error?error:"isolate creation failed");
  Dart_EnterScope();
  Check(dart::bin::DartUtils::SetupCoreLibraries(false,false,false,dart::bin::DartIoSettings{}));
  auto*fixture=dlopen(argv[3],RTLD_NOW|RTLD_GLOBAL);
  Expect(fixture!=nullptr,"cannot load signed native fixture");
  const bool control=std::strcmp(argv[8],"control")==0;
  const bool aggregate=std::strcmp(argv[8],"aggregate")==0;
  const bool missing_scope=std::strcmp(argv[8],"missing-scope")==0;
  const bool handle_result_kill=std::strcmp(argv[8],"handle-result-kill")==0;
  const bool kill=std::strcmp(argv[8],"kill")==0;
  const bool handle_kill=std::strcmp(argv[8],"handle-kill")==0;
  Expect(control||aggregate||missing_scope||handle_result_kill||kill||handle_kill||std::strcmp(argv[8],"runtime")==0,"unknown test mode");
  const auto before=Mappings();
  InitializeCodeProof(std::strtoull(argv[5],nullptr,10),std::strtoull(argv[6],nullptr,10),std::strtoull(argv[7],nullptr,10));
  std::vector<uint8_t> bytes;
  Dart_Handle library=Dart_RootLibrary();
  if(!control) {
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
  if(control) {
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
  dlclose(fixture);return 0;
}
