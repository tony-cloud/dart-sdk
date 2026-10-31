// Native consumers for runtime-created Handle and by-value callback tests.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "dart_api.h"

static void Require(int ok, const char* message) {
  if (!ok) { fprintf(stderr, "callback fixture: %s\n", message); abort(); }
}
static void* Required(const char* name) {
  void* value = dlsym(RTLD_DEFAULT, name);
  Require(value != NULL, name); return value;
}
#define API(name) ((__typeof__(&name))Required(#name))
extern intptr_t CallbackGcNow(void);
static void (*scope_probe)(int phase, intptr_t result_handles);
void SetCallbackScopeProbe(void (*probe)(int, intptr_t)) { scope_probe = probe; }
static void Probe(int phase, intptr_t handles) {
  Require(scope_probe != NULL, "scope probe not installed"); scope_probe(phase, handles);
}
static intptr_t aggregate_calls;
intptr_t AggregateCallCount(void) { return aggregate_calls; }

typedef Dart_Handle (*HandleCallback)(Dart_Handle);
typedef Dart_Handle (*HandleResult)(void);
Dart_Handle HandleCallbackRoundtrip(HandleCallback cb, Dart_Handle token, int64_t mode) {
  Dart_PersistentHandle persistent = NULL;
  Dart_Handle argument = token;
  if (mode == 1) argument = API(Dart_Null)();
  if (mode == 2) argument = API(Dart_True)();
  if (mode == 3) {
    persistent = API(Dart_NewPersistentHandle)(token);
    CallbackGcNow(); argument = API(Dart_HandleFromPersistent)(persistent);
  }
  Probe(0, 0); ++aggregate_calls;
  Dart_Handle result = cb(argument); Probe(1, 1);
  Require(!API(Dart_IsError)(result), "normal Handle callback returned error");
  CallbackGcNow();
  Require(API(Dart_IdentityEquals)(argument, result), "returned Handle lost identity across GC");
  if (persistent) API(Dart_DeletePersistentHandle)(persistent);
  return result;
}
int64_t HandleCallbackInspectError(HandleCallback cb, Dart_Handle token) {
  Probe(0, 0); ++aggregate_calls;
  Dart_Handle result = cb(token); Probe(1, 1);
  Require(API(Dart_IsUnhandledExceptionError)(result), "thrown callback lost UnhandledException");
  Dart_Handle exception = API(Dart_ErrorGetException)(result);
  Dart_Handle trace = API(Dart_ErrorGetStackTrace)(result);
  Require(!API(Dart_IsError)(trace) && !API(Dart_IsNull)(trace), "error Handle lost stacktrace");
  Require(API(Dart_IdentityEquals)(exception, token), "error Handle lost thrown object identity");
  CallbackGcNow();
  Require(API(Dart_IdentityEquals)(exception, API(Dart_ErrorGetException)(result)) &&
          API(Dart_IdentityEquals)(trace, API(Dart_ErrorGetStackTrace)(result)),
          "exception/stacktrace Handle moved without a root");
  return 1;
}
void HandleCallbackPropagate(HandleCallback cb, Dart_Handle token) {
  API(Dart_EnterScope)(); ++aggregate_calls;
  Dart_Handle result = cb(token);
  Require(API(Dart_IsError)(result), "propagation callback did not throw");
  API(Dart_PropagateError)(result);
  abort();
}
// No Handle in this native function signature. It explicitly owns the local
// Handle lifetime; the harness also calls it directly with no outer API scope.
int64_t HandleCallbackExplicitScope(HandleResult cb) {
  API(Dart_EnterScope)(); Probe(0, 0); ++aggregate_calls;
  Dart_Handle result = cb(); Probe(1, 1);
  CallbackGcNow(); int64_t value = 0;
  Require(!API(Dart_IsError)(API(Dart_IntegerToInt64)(result, &value)), "explicit-scope Handle was invalid");
  API(Dart_ExitScope)(); return value;
}
Dart_Handle HandleCallbackKill(Dart_Handle (*cb)(int64_t, int64_t)) {
  ++aggregate_calls;
  Dart_Handle result = cb(10, 20);
  Require(API(Dart_IsFatalError)(result), "Handle callback swallowed UnwindError");
  puts("NATIVE_HANDLE_RESULT_UNWIND_EPILOGUE"); fflush(stdout);
  return result;
}

typedef struct { int64_t a, b; } Pair;
typedef struct { float a, b, c; } HfaF;
typedef struct { double a, b, c, d; } HfaD;
typedef struct { int64_t a, b, c; } Large;
typedef union { uint64_t bits; double floating; } Word;
typedef struct { Pair inner; int64_t items[3]; } Nested;
typedef struct { int64_t* pointer; int64_t value; } Node;
typedef struct __attribute__((packed)) { uint8_t tag; uint16_t value; } Odd3;
typedef struct __attribute__((packed)) { uint8_t tag; uint32_t value; } Odd5;
typedef struct __attribute__((packed)) { uint8_t tag; uint16_t middle; uint32_t value; } Odd7;
typedef struct __attribute__((packed)) { uint8_t tag; uint64_t value; } Odd9;
typedef struct __attribute__((packed)) { uint8_t tag; uint64_t a, b; } Odd17;
static int64_t pointed = 90;
static Pair retained_source;
intptr_t CallbackAliasAddress(void) { return (intptr_t)&pointed; }
int64_t CheckPair(Pair (*cb)(Pair), int64_t zero) {
  Pair argument = {11,22}; Probe(0,0); ++aggregate_calls;
  Pair result = cb(argument); Probe(1,0);
  Require(argument.a==11 && argument.b==22, "by-value Pair mutated native input");
  Require(result.a==(zero?0:111) && result.b==(zero?0:22), "Pair register result/fallback mismatch");
  return 1;
}
int64_t CheckRetainedPair(Pair (*cb)(Pair)) {
  retained_source=(Pair){11,22}; ++aggregate_calls;
  Pair result=cb(retained_source);
  Require(retained_source.a==11 && retained_source.b==22 && result.a==111 && result.b==22,
          "retained Pair did not own an input copy");
  memset(&retained_source,0xCD,sizeof(retained_source)); return 1;
}
int64_t CheckNode(Node (*cb)(Node)) {
  Node input={&pointed,31}; pointed=90; ++aggregate_calls; Node result=cb(input);
  Require(input.value==31 && input.pointer==&pointed && result.pointer==&pointed &&
          result.value==32 && pointed==95, "Pointer field lost shallow alias semantics"); return 1;
}
int64_t CheckHfaF(HfaF (*cb)(HfaF), int64_t zero) {
  const uint32_t bits[]={0x80000000U,0x7FC12345U,0x3FC00000U};
  HfaF input; memcpy(&input,bits,sizeof(input)); ++aggregate_calls; HfaF result=cb(input);
  const HfaF zeros={0}; Require(memcmp(&result,zero?&zeros:&input,sizeof(result))==0,"float HFA bit/result mismatch"); return 1;
}
int64_t CheckHfaD(HfaD (*cb)(HfaD), int64_t zero) {
  const uint64_t bits[]={0x8000000000000000ULL,0x7FF8123456789ABCULL,0x7FF0000000000000ULL,0x3FF8000000000000ULL};
  HfaD input; memcpy(&input,bits,sizeof(input)); ++aggregate_calls; HfaD result=cb(input);
  const HfaD zeros={0}; Require(memcmp(&result,zero?&zeros:&input,sizeof(result))==0,"double HFA bit/result mismatch"); return 1;
}
int64_t CheckLarge(Large (*cb)(Large), int64_t zero) {
  Large input={11,22,33}; ++aggregate_calls; Large result=cb(input);
  Require(input.a==11 && input.b==22 && input.c==33,"indirect argument aliased target wrapper");
  Require(result.a==(zero?0:111) && result.b==(zero?0:222) && result.c==(zero?0:333),"large indirect result mismatch"); return 1;
}
int64_t CheckWord(Word (*cb)(Word), int64_t zero) {
  Word input={.bits=0x1020304050607080ULL}; ++aggregate_calls; Word result=cb(input);
  Require(result.bits==(zero?0:input.bits+1),"Union result mismatch"); return 1;
}
int64_t CheckNested(Nested (*cb)(Nested), int64_t zero) {
  Nested input={{11,22},{31,32,33}}; ++aggregate_calls; Nested result=cb(input);
  Require(input.inner.a==11 && input.items[1]==32,"nested input was not copied");
  Require(result.inner.a==(zero?0:12) && result.inner.b==(zero?0:24) &&
          result.items[0]==(zero?0:41) && result.items[1]==(zero?0:52) &&
          result.items[2]==(zero?0:63),"nested/inline-array result mismatch"); return 1;
}
Dart_Handle HandleCallbackResult(HandleCallback cb, Dart_Handle token) {
  Probe(0,0); ++aggregate_calls; Dart_Handle result=cb(token); Probe(1,1);
  Require(!API(Dart_IsError)(result),"allocated/self-closed Handle result failed");
  CallbackGcNow(); return result;
}
Pair CheckSpilled(Pair (*cb)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,
                            Pair,HfaF,double,double,double,double,double,double,double,double,HfaD)) {
  ++aggregate_calls;
  return cb(1,2,3,4,5,6,7,8,(Pair){11,22},(HfaF){1.25f,2.5f,3.75f},
            1,2,3,4,5,6,7,8,(HfaD){2,4,6,8});
}
int64_t CheckSpills(void* cb) {
  Pair result=CheckSpilled(cb);
  Require(result.a==105 && result.b==20,"register-exhausted aggregate stack arguments mismatch"); return 1;
}
int64_t CheckCompoundVarArgs(Large (*cb)(int64_t,...)) {
  ++aggregate_calls;
  Large result=cb(7,(Pair){11,22},(Large){31,32,33},(HfaF){1.25f,2.5f,3.75f});
  Require(result.a==49 && result.b==54 && result.c==40,"compound VarArgs placements mismatch"); return 1;
}
int64_t CheckCompoundEmptyVarArgs(Pair (*cb)(Pair,...)) {
  ++aggregate_calls; Pair result=cb((Pair){11,22});
  Require(result.a==111 && result.b==22,"empty compound VarArgs placement mismatch"); return 1;
}
int64_t CheckManyHandles(Large (*cb)(Dart_Handle,Dart_Handle,Dart_Handle,Dart_Handle,Dart_Handle,
                                    Dart_Handle,Dart_Handle,Dart_Handle,Dart_Handle,Dart_Handle,Large,Pair),
                         Dart_Handle token) {
  Dart_Handle null=API(Dart_Null)(), yes=API(Dart_True)();
  Dart_PersistentHandle persistent=API(Dart_NewPersistentHandle)(token);
  Dart_Handle retained=API(Dart_HandleFromPersistent)(persistent);
  typedef void*(*Command)(const char*,void*);
  ((Command)Required("Dart_ExecuteInternalCommand"))("gc-on-nth-allocation",(void*)1);
  Probe(0,0); ++aggregate_calls;
  Large result=cb(token,null,yes,retained,token,null,yes,retained,token,retained,
                  (Large){11,22,33},(Pair){5,7});
  Probe(1,0); API(Dart_DeletePersistentHandle)(persistent);
  Require(result.a==16 && result.b==29 && result.c==42,"mixed Handle/compound boxing lost roots"); return 1;
}
static intptr_t input_read_checks;
int64_t MarkAggregateInputRead(int64_t sum) {
  Require(sum==66 || sum==310,"aliased callback did not read original native input");
  ++input_read_checks; return 1;
}
extern void CallbackIndirectAt(void* callback,const void* argument,void* result);
static void* Guarded(size_t size, void** mapping) {
  const size_t page=(size_t)sysconf(_SC_PAGESIZE);
  Require(size+32<=page,"guard fixture payload too large");
  unsigned char* base=mmap(NULL,page*3,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  Require(base!=MAP_FAILED,"guard mmap failed");
  Require(mprotect(base+page,page,PROT_READ|PROT_WRITE)==0,"guard mprotect failed");
  memset(base+page,0xA7,page); *mapping=base; return base+page*2-size;
}
int64_t CheckGuardedIndirect(void* cb,int64_t size,int64_t zero,int64_t alias) {
  void* in_mapping; unsigned char* input=Guarded(size,&in_mapping);
  if(size==24) {const Large value={11,22,33}; memcpy(input,&value,24);}
  else {Require(size==17,"unexpected exact-size guard shape"); const Odd17 value={7,101,202};memcpy(input,&value,17);}
  unsigned char expected[24];memcpy(expected,input,size);
  void* out_mapping=NULL; unsigned char* output=alias?input:Guarded(size,&out_mapping);
  const intptr_t reads_before=input_read_checks;
  ++aggregate_calls; CallbackIndirectAt(cb,input,output);
  Require(input_read_checks==reads_before+1,"guard callback did not confirm original input before fallback");
  for(intptr_t i=0;i<size;++i) Require(output[i]==(zero?0:expected[i]),"exact-size indirect result mismatch");
  for(intptr_t i=1;i<=32;++i) Require(output[-i]==0xA7,"indirect result overwrote leading sentinel");
  if(!alias) Require(memcmp(input,expected,size)==0,"indirect input copy mutated native storage");
  const size_t page=(size_t)sysconf(_SC_PAGESIZE);
  munmap(in_mapping,page*3);if(out_mapping)munmap(out_mapping,page*3);return 1;
}
static unsigned char overlap_storage[80] __attribute__((aligned(16)));
intptr_t CallbackNativeBacking(void) { return (intptr_t)overlap_storage; }
int64_t CheckOverlap(void* cb) {
  memset(overlap_storage,0xA7,sizeof(overlap_storage));
  const Large original={111,222,333}; memcpy(overlap_storage+8,&original,sizeof(original));
  // Returned wrapper uses native backing +8; output starts +16 and overlaps
  // that source. memmove must read the original 24 bytes before overwriting.
  const Large argument={11,22,33}; ++aggregate_calls;
  CallbackIndirectAt(cb,&argument,overlap_storage+16);
  Require(memcmp(overlap_storage+16,&original,24)==0,"overlapping native return copy corrupted payload");
  Require(overlap_storage[15]==((unsigned char*)&original)[7] && overlap_storage[40]==0xA7,
          "overlap result wrote beyond its exact payload");return 1;
}

int64_t CheckOdd3(Odd3 (*cb)(Odd3), int64_t zero) {
  const Odd3 input={7,1234}; ++aggregate_calls; const Odd3 result=cb(input);
  const Odd3 zeros={0};
  Require(sizeof(input)==3 && memcmp(&result,zero?&zeros:&input,3)==0,"packed Odd3 payload mismatch"); return 1;
}

int64_t CheckOdd5(Odd5 (*cb)(Odd5), int64_t zero) {
  const Odd5 input={7,123456}; ++aggregate_calls; const Odd5 result=cb(input);
  const Odd5 zeros={0};
  Require(sizeof(input)==5 && memcmp(&result,zero?&zeros:&input,5)==0,"packed Odd5 payload mismatch"); return 1;
}

int64_t CheckOdd7(Odd7 (*cb)(Odd7), int64_t zero) {
  const Odd7 input={7,1234,123456}; ++aggregate_calls; const Odd7 result=cb(input);
  const Odd7 zeros={0};
  Require(sizeof(input)==7 && memcmp(&result,zero?&zeros:&input,7)==0,"packed Odd7 payload mismatch"); return 1;
}

int64_t CheckOdd9(Odd9 (*cb)(Odd9), int64_t zero) {
  const Odd9 input={7,123456789}; ++aggregate_calls; const Odd9 result=cb(input);
  const Odd9 zeros={0};
  Require(sizeof(input)==9 && memcmp(&result,zero?&zeros:&input,9)==0,"packed Odd9 payload mismatch"); return 1;
}

int64_t CheckOdd17(Odd17 (*cb)(Odd17), int64_t zero) {
  const Odd17 input={7,101,202}; ++aggregate_calls; const Odd17 result=cb(input);
  const Odd17 zeros={0};
  Require(sizeof(input)==17 && memcmp(&result,zero?&zeros:&input,17)==0,"packed Odd17 payload mismatch"); return 1;
}
intptr_t ResetCallbackNativeBacking(void) {
  memset(overlap_storage,0xA7,sizeof(overlap_storage));
  const Large value={111,222,333};memcpy(overlap_storage+8,&value,sizeof(value));
  return (intptr_t)overlap_storage;
}
Dart_Handle HandleCallbackVarArgs(Dart_Handle (*cb)(int64_t,...),Dart_Handle token) {
  Probe(0,0);++aggregate_calls;
  Dart_Handle result=cb(7,token,(Pair){11,22},(Large){31,32,33});Probe(1,1);
  Require(!API(Dart_IsError)(result),"Handle/compound VarArgs callback failed");
  CallbackGcNow();Require(API(Dart_IdentityEquals)(result,token),"VarArgs Handle lost identity/root");return result;
}

static intptr_t guarded_return_address;
intptr_t CallbackGuardReturnAddress(void) { return guarded_return_address; }

int64_t CheckGuardedReturnOdd3(Odd3 (*cb)(void)) {
  void* mapping;unsigned char* source=Guarded(sizeof(Odd3),&mapping);
  const Odd3 expected={7,1234};memcpy(source,&expected,sizeof(expected));
  guarded_return_address=(intptr_t)source;++aggregate_calls;
  const Odd3 result=cb();
  Require(memcmp(&result,&expected,sizeof(expected))==0,"guarded Odd3 return overread/wrong payload");
  guarded_return_address=0;munmap(mapping,(size_t)sysconf(_SC_PAGESIZE)*3);return 1;
}

int64_t CheckGuardedReturnOdd5(Odd5 (*cb)(void)) {
  void* mapping;unsigned char* source=Guarded(sizeof(Odd5),&mapping);
  const Odd5 expected={7,123456};memcpy(source,&expected,sizeof(expected));
  guarded_return_address=(intptr_t)source;++aggregate_calls;
  const Odd5 result=cb();
  Require(memcmp(&result,&expected,sizeof(expected))==0,"guarded Odd5 return overread/wrong payload");
  guarded_return_address=0;munmap(mapping,(size_t)sysconf(_SC_PAGESIZE)*3);return 1;
}

int64_t CheckGuardedReturnOdd7(Odd7 (*cb)(void)) {
  void* mapping;unsigned char* source=Guarded(sizeof(Odd7),&mapping);
  const Odd7 expected={7,1234,123456};memcpy(source,&expected,sizeof(expected));
  guarded_return_address=(intptr_t)source;++aggregate_calls;
  const Odd7 result=cb();
  Require(memcmp(&result,&expected,sizeof(expected))==0,"guarded Odd7 return overread/wrong payload");
  guarded_return_address=0;munmap(mapping,(size_t)sysconf(_SC_PAGESIZE)*3);return 1;
}

int64_t CheckGuardedReturnOdd9(Odd9 (*cb)(void)) {
  void* mapping;unsigned char* source=Guarded(sizeof(Odd9),&mapping);
  const Odd9 expected={7,123456789};memcpy(source,&expected,sizeof(expected));
  guarded_return_address=(intptr_t)source;++aggregate_calls;
  const Odd9 result=cb();
  Require(memcmp(&result,&expected,sizeof(expected))==0,"guarded Odd9 return overread/wrong payload");
  guarded_return_address=0;munmap(mapping,(size_t)sysconf(_SC_PAGESIZE)*3);return 1;
}

int64_t CheckGuardedReturnHfaF(HfaF (*cb)(void)) {
  void* mapping;unsigned char* source=Guarded(sizeof(HfaF),&mapping);
  const HfaF expected={1.25f,2.5f,3.75f};memcpy(source,&expected,sizeof(expected));
  guarded_return_address=(intptr_t)source;++aggregate_calls;
  const HfaF result=cb();
  Require(memcmp(&result,&expected,sizeof(expected))==0,"guarded HfaF return overread/wrong payload");
  guarded_return_address=0;munmap(mapping,(size_t)sysconf(_SC_PAGESIZE)*3);return 1;
}

int64_t CheckGuardedReturnHfaD(HfaD (*cb)(void)) {
  void* mapping;unsigned char* source=Guarded(sizeof(HfaD),&mapping);
  const HfaD expected={1.5,2.5,3.5,4.5};memcpy(source,&expected,sizeof(expected));
  guarded_return_address=(intptr_t)source;++aggregate_calls;
  const HfaD result=cb();
  Require(memcmp(&result,&expected,sizeof(expected))==0,"guarded HfaD return overread/wrong payload");
  guarded_return_address=0;munmap(mapping,(size_t)sysconf(_SC_PAGESIZE)*3);return 1;
}
