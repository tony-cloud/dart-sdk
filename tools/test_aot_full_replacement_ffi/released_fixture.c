// All native implementations ship together BEFORE the N/B build. U changes
// Dart signatures/layouts only; distinct V1/V2 exports keep each C ABI honest.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dart_api.h"

static int64_t calls;
void FullReset(void) { calls = 0; }
int64_t FullCalls(void) { return calls; }
int64_t FullResolverImplementation(int64_t n) { ++calls; return n + 100; }
// Deliberate process-name collision: null resolver must NOT fall through here.
int64_t FullResolverMissing(void) { ++calls; abort(); }
int64_t FullAssetValue(int64_t n) { ++calls; return n + 200; }
int64_t FullProcessValue(int64_t n) { ++calls; return n + 300; }
int64_t FullDynamicValue(int64_t n) { ++calls; return n + 400; }
int64_t FullAddedImplementation(int64_t n) { ++calls; return n + 500; }
typedef struct { int64_t number; double fraction; } RecordV1;
typedef struct { int64_t number; double fraction; int64_t extra; } RecordV2;
RecordV1 FullRecordV1(RecordV1 r) { ++calls; ++r.number; r.fraction *= 2; return r; }
RecordV2 FullRecordV2(RecordV2 r) { ++calls; r.number += 2; r.fraction *= 3; r.extra += 5; return r; }
typedef union { uint64_t bits; double floating; } Word;
typedef struct { float a, b, c; } Hfa;
typedef struct __attribute__((packed)) { uint8_t tag; uint64_t value; } Packed;
typedef struct { Hfa hfa; Word word; } Nested;
Word FullWord(Word r) { ++calls; r.bits += 17; return r; }
Hfa FullHfa(Hfa r) { ++calls; r.a += 1; r.b += 2; r.c += 3; return r; }
Packed FullPacked(Packed r) { ++calls; r.tag += 2; r.value += 3; return r; }
Nested FullNested(Nested r) { ++calls; r.hfa.b += 5; r.word.bits += 7; return r; }
double FullVarArgs(int64_t count, ...) {
  ++calls;
  if (count != 2) abort();
  va_list args; va_start(args, count);
  RecordV1 record = va_arg(args, RecordV1);
  Hfa hfa = va_arg(args, Hfa);
  va_end(args);
  return record.number + record.fraction + hfa.a + hfa.b + hfa.c;
}
double FullHfaVarArgs(int64_t count, ...) {
  ++calls; if (count != 2) abort();
  va_list args; va_start(args, count);
  Hfa a = va_arg(args, Hfa); Hfa b = va_arg(args, Hfa); va_end(args);
  return a.a + a.b + a.c + b.a + b.b + b.c;
}
int16_t FullAbi(int16_t value) { ++calls; return value + 7; }
uint8_t FullBool(uint8_t value) { ++calls; return !value; }
void* FullPointer(void* value) { ++calls; return value; }
int64_t FullBorrow(int64_t* values, int64_t delta) { ++calls; return values[0] + delta; }
int64_t FullGc(void) {
  typedef void* (*Command)(const char*, void*);
  Command command = (Command)dlsym(RTLD_DEFAULT, "Dart_ExecuteInternalCommand");
  if (!command || !command("is-thread-in-native", NULL)) abort();
  command("gc-now", NULL);
  return 1;
}
Dart_Handle FullEcho(Dart_Handle value) { ++calls; FullGc(); return value; }
typedef int64_t (*Scalar)(int64_t, int64_t);
typedef void (*Listener)(int64_t);
typedef void (*PayloadListener)(RecordV2);
int64_t FullCallScalar(Scalar callback, int64_t a, int64_t b) {
  ++calls; return callback(a, b);
}
RecordV2 FullCallRecord(RecordV2 (*callback)(RecordV2), RecordV2 r) {
  ++calls; return callback(r);
}
void FullPost(Listener callback, int64_t n) { ++calls; callback(n); }
void FullPostRecord(PayloadListener callback) {
  ++calls; RecordV2 r = {21, 2.5, 31}; callback(r);
  // A queued listener must own a copy after the source stack is invalidated.
  memset(&r, 0xCD, sizeof r);
}
typedef struct { Scalar callback; Listener listener; int index; int64_t result; } Work;
static void* Worker(void* data) {
  Work* w = data;
  w->result = w->callback(10 + w->index, 5);
  w->listener(100 + w->index);
  return NULL;
}
// Called only by the embedder while outside every isolate: group-bound calls
// may acquire the owner group, and queued listeners run when it is re-entered.
int64_t FullThreads(Scalar group, Listener listener) {
  pthread_t threads[2]; Work jobs[2]; int64_t sum = 0;
  for (int i = 0; i < 2; ++i) {
    jobs[i] = (Work){group, listener, i, 0};
    if (pthread_create(&threads[i], NULL, Worker, &jobs[i])) abort();
  }
  for (int i = 0; i < 2; ++i) {
    if (pthread_join(threads[i], NULL)) abort();
    sum += jobs[i].result;
  }
  return sum;
}
