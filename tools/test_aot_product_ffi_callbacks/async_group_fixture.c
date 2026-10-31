// Quiescent pthread/native consumers. No callback is invoked after close.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dart_api.h"
static void Require(int ok,const char* text) {if(!ok){fprintf(stderr,"async/group fixture: %s\n",text);abort();}}
static void* Required(const char* name) {void*p=dlsym(RTLD_DEFAULT,name);Require(p!=NULL,name);return p;}
#define API(name) ((__typeof__(&name))Required(#name))
static void (*state_probe)(int);
void SetAsyncGroupStateProbe(void(*probe)(int)){state_probe=probe;}
static void Probe(int phase){Require(state_probe!=NULL,"probe not installed");state_probe(phase);}
typedef void(*Listener)(int64_t);
typedef int64_t(*Group)(int64_t,int64_t);
typedef struct{int64_t a;double b;} AsyncPair;
typedef struct{float a,b,c;} AsyncHfa;
typedef struct __attribute__((packed)){uint8_t tag;uint64_t value;} AsyncPacked;
typedef struct{int64_t a,b,c;} AsyncLarge;
typedef struct{AsyncPair inner;int64_t values[3];} AsyncNested;
typedef union{uint64_t bits;double floating;} AsyncWord;
typedef void(*Payload)(int64_t,AsyncPair,AsyncHfa,AsyncPacked,AsyncLarge,AsyncNested,AsyncWord,int64_t*,Dart_Handle);
static int64_t pointee=500;
static _Atomic int64_t static_initializer_calls=0;
int64_t AsyncStaticInitializer(void){atomic_fetch_add(&static_initializer_calls,1);return 1234;}
int64_t AsyncStaticInitializerCount(void){return atomic_load(&static_initializer_calls);}
intptr_t AsyncPointeeAddress(void){return(intptr_t)&pointee;}
int64_t ClobberAsyncStack(void);
static void PostPayload(Payload callback,Dart_Handle handle,int64_t event){
  AsyncPair pair={11,2.5};AsyncHfa hfa={1.25f,2.5f,3.75f};AsyncPacked packed={7,123456789};
  AsyncLarge large={31,32,33};AsyncNested nested={{41,4.5},{51,52,53}};AsyncWord word={.bits=0x12345678};
  Probe(0);callback(event,pair,hfa,packed,large,nested,word,&pointee,handle);Probe(1);
  memset(&pair,0xCD,sizeof pair);memset(&hfa,0xCD,sizeof hfa);memset(&packed,0xCD,sizeof packed);
  memset(&large,0xCD,sizeof large);memset(&nested,0xCD,sizeof nested);memset(&word,0xCD,sizeof word);
}
void AsyncPostPayload(Payload callback,Dart_Handle handle,int64_t event){PostPayload(callback,handle,event);ClobberAsyncStack();}
void AsyncPostScalar(Listener callback,int64_t value){Probe(0);callback(value);Probe(1);}
int64_t AsyncCallGroup(Group callback,int64_t a,int64_t b){Probe(0);int64_t result=callback(a,b);Probe(1);return result;}
// Runs inside a group callback's nonleaf native callout; source group frames
// and API scope must survive both nested synchronous and async transitions.
int64_t AsyncNestedDriver(Group other,Listener listener,int64_t value){
  int64_t result=value;if(other){Probe(0);result=other(value,5);Probe(1);}
  if(listener){Probe(0);listener(value);Probe(1);}
  return result;
}
typedef struct{Listener listener;Payload payload;Group group;Dart_Handle handle;int count;int64_t base;int64_t sum;} Work;
static void* Worker(void* data){Work*w=data;int64_t sum=0;
  for(int i=0;i<w->count;++i){
    if(w->listener){Probe(0);w->listener(w->base+i);Probe(1);}
    if(w->payload){PostPayload(w->payload,w->handle,w->base+i);ClobberAsyncStack();}
    if(w->group){Probe(0);sum+=w->group(w->base+i,5);Probe(1);}
  }w->sum=sum;return NULL;
}
int64_t AsyncRunThreads(Listener listener,Payload payload,Group group,Dart_Handle persistent,int64_t threads,int64_t count){
  Require(threads>0&&threads<=8&&count>0,"invalid bounded pthread fixture size");
  pthread_t ids[8];Work jobs[8];
  for(int i=0;i<threads;++i){jobs[i]=(Work){listener,payload,group,persistent,(int)count,1000+i*100,0};Require(pthread_create(&ids[i],NULL,Worker,&jobs[i])==0,"pthread_create");}
  int64_t sum=0;for(int i=0;i<threads;++i){Require(pthread_join(ids[i],NULL)==0,"pthread_join");sum+=jobs[i].sum;}
  return sum; // all native invocations are quiescent before caller may close
}
// Native VarArgs types deliberately apply C's default numeric promotions.
void AsyncVarArgs(void(*empty)(int64_t,...),void(*single)(int64_t,...),
                  void(*many)(int64_t,...),void(*spill)(int64_t,...),
                  void(*compound)(int64_t,...)){
  Probe(0);empty(1);Probe(1);
  Probe(0);single(2,(int)7);Probe(1);
  Probe(0);many(3,(int)-9,(double)2.5,(int64_t)10000000000LL);Probe(1);
  Probe(0);spill(4,(int64_t)1,(double)1.5,(int64_t)2,(double)2.5,(int64_t)3,(double)3.5,
                  (int64_t)4,(double)4.5,(int64_t)5,(double)5.5,(int64_t)6,(double)6.5,
                  (int64_t)7,(double)7.5,(int64_t)8,(double)8.5,(int64_t)9,(double)9.5);Probe(1);
  Probe(0);compound(5,(AsyncPair){11,2.5},(AsyncLarge){31,32,33});Probe(1);
}
// A group Handle result's caller supplies an API scope on that SAME target
// group Thread. No scope is borrowed from a peer or an unrelated group.
Dart_Handle AsyncGroupHandle(Dart_Handle(*callback)(Dart_Handle),Dart_Handle token){
  Probe(0);Dart_Handle result=callback(token);Probe(2);
  Require(!API(Dart_IsError)(result)&&API(Dart_IdentityEquals)(result,token),"group Handle result identity");return result;
}
AsyncLarge AsyncGroupLarge(AsyncLarge(*callback)(AsyncLarge)){
  Probe(0);AsyncLarge result=callback((AsyncLarge){11,22,33});Probe(1);return result;
}
static int64_t(*context_probe)(int64_t,int64_t);
void SetAsyncGroupContextProbe(int64_t(*probe)(int64_t,int64_t)){context_probe=probe;}
int64_t AsyncCheckContext(int64_t kind,int64_t group){Require(context_probe!=NULL,"context probe absent");return context_probe(kind,group);}
static void(*self_close)(void);
void SetAsyncGroupSelfClose(void(*close)(void)){self_close=close;}
void AsyncGroupSelfClose(void){Require(self_close!=NULL,"self-close hook absent");self_close();}
__attribute__((noinline)) int64_t ClobberAsyncStack(void){volatile unsigned char bytes[16384];for(int i=0;i<16384;++i)bytes[i]=0xCD;return bytes[8192];}
typedef struct{const uintptr_t* pointers;intptr_t count,index,stride;int group;} PoolWork;
static void* PoolWorker(void* raw){PoolWork*w=raw;for(intptr_t i=w->index;i<w->count;i+=w->stride){Probe(0);if(w->group){Require(((Group)w->pointers[i])(3,5)==i+8,"concurrent group pool result");}else{((Listener)w->pointers[i])(1);}Probe(1);}return NULL;}
void AsyncRunPoolThreads(const uintptr_t* pointers,int64_t count,int64_t group){
  pthread_t threads[4];PoolWork work[4];
  for(int i=0;i<4;++i){work[i]=(PoolWork){pointers,count,i,4,(int)group};Require(pthread_create(&threads[i],NULL,PoolWorker,&work[i])==0,"pool pthread_create");}
  for(int i=0;i<4;++i)Require(pthread_join(threads[i],NULL)==0,"pool pthread_join");
}
