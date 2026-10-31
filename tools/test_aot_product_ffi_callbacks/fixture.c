// Native callback consumers; all nested calls use nonleaf transitions.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// All consumers of these callbacks must be invoked from nonleaf Dart call-outs.
// Every call remains on the current isolate's owning mutator thread.
static intptr_t callback_calls;
intptr_t CallbackCallCount(void) { return callback_calls; }

typedef int64_t (*CallbackI64)(int64_t, int64_t);
typedef double (*CallbackDouble)(double);
typedef void* (*CallbackPointer)(void*, intptr_t);
typedef double (*CallbackMixed)(int64_t, double, int8_t, float, uint16_t, double);
typedef int64_t (*CallbackTen)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*CallbackVariadic)(int64_t, ...);

int64_t CallI64(CallbackI64 callback, int64_t a, int64_t b) { ++callback_calls; return callback(a, b); }
double CallDouble(CallbackDouble callback, double value) { ++callback_calls; return callback(value); }
void* CallPointer(CallbackPointer callback, void* value, intptr_t delta) { ++callback_calls;
  return callback(value, delta);
}
double CallMixed(CallbackMixed callback) { ++callback_calls;
  return callback(1, 2.5, -3, 4.25f, 60000, 8.5);
}
int64_t CallTen(CallbackTen callback) { ++callback_calls; return callback(1,2,3,4,5,6,7,8,9,10); }
int64_t CallVariadic(CallbackVariadic callback, int64_t first) { ++callback_calls;
  // Explicit default promotions: Int64, Double, Int32. On Apple ARM64 the
  // variadic arguments use stack layout; Linux ARM64 differs.
  return callback(first, (int64_t)7, (double)2.5, (int)11);
}
int64_t NativeAdd(int64_t a, int64_t b) { return a + b; }

static intptr_t completed_gc;
intptr_t CallbackGcNow(void) {
  typedef void* (*Command)(const char*, void*);
  Command command = (Command)dlsym(RTLD_DEFAULT, "Dart_ExecuteInternalCommand");
  if (command == NULL || command("is-thread-in-native", NULL) == NULL) {
    fputs("callback GC helper requires existing PRODUCT API and nonleaf transition\n", stderr);
    abort();
  }
  command("gc-now", NULL);
  return ++completed_gc;
}
intptr_t CallbackGcCount(void) { return completed_gc; }

int CallbackFrameSeen(uint64_t start, uint64_t end, uint64_t image_base) {
  void* frames[96];
  const int count = backtrace(frames, 96);
  for (int i = 0; i < count; ++i) {
    Dl_info info;
    if (dladdr(frames[i], &info) == 0) continue;
    const uintptr_t normalized =
        (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase + image_base;
    if (start <= normalized && normalized < end) return 1;
  }
  return 0;
}

#include <stdbool.h>
typedef bool (*CallbackBool)(bool);
typedef float (*CallbackFloat)(float);
typedef void (*CallbackVoid)(int64_t);
typedef int16_t (*CallbackAbi)(int16_t);
typedef int64_t (*CallbackEmptyVariadic)(int64_t, ...);
typedef int64_t (*CallbackNarrowStack)(int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int64_t,int8_t,uint8_t,int16_t,uint16_t,int32_t,uint32_t);
bool CallBool(CallbackBool callback, bool value) { ++callback_calls; return callback(value); }
double CallFloat(CallbackFloat callback, float value) { ++callback_calls; return callback(value); }
void CallVoid(CallbackVoid callback, int64_t value) { ++callback_calls; callback(value); }
int16_t CallAbi(CallbackAbi callback, int16_t value) { ++callback_calls; return callback(value); }
int64_t CallEmptyVariadic(CallbackEmptyVariadic callback) { ++callback_calls; return callback(19); }
int64_t CallNarrowStack(CallbackNarrowStack callback) { ++callback_calls;
  return callback(1,2,3,4,5,6,7,8,-7,250,-300,60000,-70000,4000000000U);
}
int64_t CallKill(CallbackI64 callback) { ++callback_calls;
  const int64_t result = callback(10, 20);
  printf("NATIVE_CALLBACK_EPILOGUE:%lld\n", (long long)result);
  fflush(stdout);
  if (result != -19001) abort();
  return 0;
}
// Compiled-only controls for shapes rejected by the runtime-created first slice.
typedef void* (*CallbackHandle)(void*);
void* CallHandle(CallbackHandle callback, void* value) { ++callback_calls; return callback(value); }
typedef struct { int64_t value; } ControlBox;
typedef ControlBox (*CallbackBox)(ControlBox);
int64_t CallBox(CallbackBox callback, int64_t value) { ++callback_calls;
  return callback((ControlBox){value}).value;
}

void* CallHandleKill(void* token, void (*callback)(void)) {
  ++callback_calls;
  callback();
  puts("NATIVE_HANDLE_CALLBACK_EPILOGUE"); fflush(stdout);
  return token;
}
typedef double (*CallbackDoubleTen)(double,double,double,double,double,double,double,double,double,double);
double CallDoubleTen(CallbackDoubleTen callback) {
  ++callback_calls;
  return callback(1,2,3,4,5,6,7,8,9,10);
}
