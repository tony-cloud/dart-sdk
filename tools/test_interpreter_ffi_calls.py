#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Native-call regression for the JIT bytecode interpreter's data-only FFI plan.

Requires an interpreter-enabled dart build and vm_platform.dill. This does not
admit FFI into the PRODUCT replacement profile or exercise PRODUCT FFI execution.
The native fixture walks its C stack to verify that DRT_FfiCall actually made
an interpreted call; ordinary JIT execution is checked separately. Handle GC
coverage uses the existing internal-test Dart_ExecuteInternalCommand API, without
Dart callbacks. Borrowed TypedData/Struct addresses are tested only on leaf
@Native calls, as required by their API contract.
"""

import argparse
import contextlib
import platform
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


NATIVE = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "dart_api.h"
#include "dart_native_api.h"

static void* RequiredApi(const char* name) {
  void* address = dlsym(RTLD_DEFAULT, name);
  if (address == NULL) {
    fprintf(stderr, "required native Dart test API unavailable: %s\n", name);
    abort();
  }
  return address;
}
int MakeFixtureGlobal(void) {
  Dl_info info;
  if (dladdr((void*)&MakeFixtureGlobal, &info) == 0) return 0;
  return dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL) != NULL;
}
static intptr_t completed_gc_calls;
static void ForceGcFromNative(void) {
  typedef void* (*Command)(const char*, void*);
  Command command = (Command)RequiredApi("Dart_ExecuteInternalCommand");
  if (command("is-thread-in-native", NULL) == NULL) {
    fprintf(stderr, "GC test requires a nonleaf native transition\n");
    abort();
  }
  // This existing internal-test API synchronously calls CollectAllGarbage.
  command("gc-now", NULL);
  ++completed_gc_calls;
}
Dart_Handle EchoHandle(Dart_Handle value) { return value; }
Dart_Handle NewStringHandle(void) {
  typedef Dart_Handle (*NewString)(const char*);
  return ((NewString)RequiredApi("Dart_NewStringFromCString"))("native handle result");
}
Dart_Handle HandleAcrossGc(Dart_Handle value) {
  typedef Dart_PersistentHandle (*NewPersistent)(Dart_Handle);
  typedef Dart_Handle (*FromPersistent)(Dart_PersistentHandle);
  typedef void (*DeletePersistent)(Dart_PersistentHandle);
  Dart_PersistentHandle persistent =
      ((NewPersistent)RequiredApi("Dart_NewPersistentHandle"))(value);
  ForceGcFromNative();
  Dart_Handle result =
      ((FromPersistent)RequiredApi("Dart_HandleFromPersistent"))(persistent);
  ((DeletePersistent)RequiredApi("Dart_DeletePersistentHandle"))(persistent);
  return result;
}
Dart_Handle LocalHandleAcrossGc(Dart_Handle value) {
  ForceGcFromNative();
  return value;
}
intptr_t CompletedGcCalls(void) { return completed_gc_calls; }
int64_t SumBytes(const uint8_t* values, intptr_t count) {
  int64_t result = 0;
  for (intptr_t i = 0; i < count; ++i) result += values[i];
  return result;
}
void ChangeBytes(uint8_t* values, intptr_t count, uint8_t add) {
  for (intptr_t i = 0; i < count; ++i) values[i] += add;
}

int InterpreterFrameSeen(uint64_t start, uint64_t end, uint64_t image_base) {
  void* frames[64];
  int count = backtrace(frames, 64);
  for (int i = 0; i < count; ++i) {
    Dl_info info;
    if (dladdr(frames[i], &info) != 0) {
      uintptr_t address = (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase + image_base;
      if (start <= address && address < end) return 1;
    }
  }
  return 0;
}
int64_t NoArguments(void) { return 17; }
int8_t NarrowResult(int64_t value) { return (int8_t)value; }
bool BooleanResult(bool value) { return !value; }
void* EchoPointer(void* value) { return value; }
double Mixed(int64_t a, double b, int8_t c, float d, uint16_t e, double f) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}
int64_t Packed(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
               int64_t a4, int64_t a5, int64_t a6, int64_t a7,
               int8_t b, uint8_t c, int16_t d, uint16_t e, int32_t f, uint32_t g) {
  return a0+a1+a2+a3+a4+a5+a6+a7 + b + c + d + e + f + g;
}
typedef struct { int64_t a; double b; } Small;
typedef struct { int64_t a, b, c; } Large;
typedef struct { float a, b, c; } Floats;
Small MakeSmall(int64_t a, double b) { return (Small){a + 1, b + 2}; }
Small ChangeSmall(Small value) { return (Small){value.a * 2, value.b * 3}; }
Large MakeLarge(int64_t a) { return (Large){a, a + 1, a + 2}; }
Large ChangeLarge(Large value) { return (Large){value.c + 3, value.b + 4, value.a + 5}; }
Floats MakeFloats(float a, float b, float c) { return (Floats){a + 1, b + 2, c + 3}; }
Floats ChangeFloats(Floats value) { return (Floats){value.c, value.a, value.b}; }
double ChangeSmallPointer(Small* value) {
  double before = value->a + value->b;
  value->a += 100;
  value->b += 200;
  return before;
}
Small* AllocateSmall(int64_t a, double b) {
  Small* value = malloc(sizeof(Small));
  if (value == NULL) abort();
  *value = (Small){a, b};
  return value;
}
void FreeMemory(void* value) { free(value); }
Large LargeAcrossGc(Large value) {
  ForceGcFromNative();
  return (Large){value.c + 3, value.b + 4, value.a + 5};
}
double Variadic(int count, ...) {
  va_list values;
  va_start(values, count);
  double result = 0;
  for (int i = 0; i < count; ++i) {
    result += va_arg(values, int);
    result += va_arg(values, double);
  }
  va_end(values);
  return result;
}
'''

DART = r'''
import 'dart:ffi';
import 'dart:typed_data';

@Native<Int32 Function(Int32)>(symbol: 'abs')
external int nativeAbs(int value);
@Native<Int32 Function(Int32)>(symbol: 'abs', isLeaf: true)
external int nativeAbsLeaf(int value);
@Native<Int64 Function(Pointer<Uint8>, IntPtr)>(symbol: 'SumBytes', isLeaf: true)
external int sumBytes(Pointer<Uint8> values, int count);
@Native<Void Function(Pointer<Uint8>, IntPtr, Uint8)>(symbol: 'ChangeBytes', isLeaf: true)
external void changeBytes(Pointer<Uint8> values, int count, int add);
@Native<Double Function(Pointer<Small>)>(symbol: 'ChangeSmallPointer', isLeaf: true)
external double changeSmallPointer(Pointer<Small> value);

final class Marker {
  final int value;
  Marker(this.value);
}

final class Small extends Struct {
  @Int64()
  external int a;
  @Double()
  external double b;
}
final class Large extends Struct {
  @Int64()
  external int a;
  @Int64()
  external int b;
  @Int64()
  external int c;
}
final class Floats extends Struct {
  @Float()
  external double a;
  @Float()
  external double b;
  @Float()
  external double c;
}
void equal(Object expected, Object actual) {
  if (expected != actual) throw StateError('expected $expected, got $actual');
}
void same(Object? expected, Object? actual) {
  if (!identical(expected, actual)) throw StateError('Handle identity changed');
}
void testHandlesAndGc(DynamicLibrary lib) {
  final echo = lib.lookupFunction<Handle Function(Handle), Object? Function(Object?)>('EchoHandle');
  final persistentGc = lib.lookupFunction<Handle Function(Handle), Object? Function(Object?)>('HandleAcrossGc');
  final localGc = lib.lookupFunction<Handle Function(Handle), Object? Function(Object?)>('LocalHandleAcrossGc');
  final newString = lib.lookupFunction<Handle Function(), Object Function()>('NewStringHandle');
  final gcCount = lib.lookupFunction<IntPtr Function(), int Function()>('CompletedGcCalls');
  final values = <Object?>[null, true, 123456789, -123456789, 'handle value', <int>[1, 2, 3], Marker(99), Uint8List.fromList([4, 5, 6])];
  int expectedGc = 0;
  for (final value in values) {
    same(value, echo(value));
    same(value, persistentGc(value));
    same(value, localGc(value));
    expectedGc += 2;
  }
  for (int i = 0; i < 4; ++i) {
    final result = localGc(Marker(i)) as Marker;
    equal(i, result.value);
    ++expectedGc;
  }
  equal('1,2,3', (values[5] as List<int>).join(','));
  equal(99, (values[6] as Marker).value);
  equal('4,5,6', (values[7] as Uint8List).join(','));
  equal('native handle result', newString());
  final makeLarge = lib.lookupFunction<Large Function(Int64), Large Function(int)>('MakeLarge');
  final gcLarge = lib.lookupFunction<Large Function(Large), Large Function(Large)>('LargeAcrossGc');
  final input = makeLarge(50);
  final result = gcLarge(input);
  ++expectedGc;
  equal(55, result.a); equal(55, result.b); equal(55, result.c);
  equal(50, input.a); equal(51, input.b); equal(52, input.c);
  equal(expectedGc, gcCount());
  print('Handle arguments/results and $expectedGc nonleaf native GC calls passed');
}
void testLeafAddresses(DynamicLibrary lib) {
  // Collect before borrowing addresses, never during a leaf call.
  final gc = lib.lookupFunction<Handle Function(Handle), Object? Function(Object?)>('LocalHandleAcrossGc');
  final gcCount = lib.lookupFunction<IntPtr Function(), int Function()>('CompletedGcCalls');
  final beforeGc = gcCount();
  // The address extensions are supported only as leaf @Native arguments.
  final bytes = Uint8List.fromList([1, 2, 3, 4, 5, 6]);
  equal(21, sumBytes(bytes.address, bytes.length));
  final view = Uint8List.sublistView(bytes, 2, 5);
  equal(12, sumBytes(view.address, view.length));
  changeBytes(view.address, view.length, 10);
  equal('1,2,13,14,15,6', bytes.join(','));
  same(view, gc(view));
  equal(42, sumBytes(view.address, view.length));
  equal(14, sumBytes(bytes[3].address, 1));
  final storage = Uint8List(48)..fillRange(0, 48, 0x5a);
  final struct = Struct.create<Small>(storage, 16)
    ..a = 7
    ..b = 2.5;
  same(struct, gc(struct));
  equal(9.5, changeSmallPointer(struct.address));
  equal(107, struct.a); equal(202.5, struct.b);
  if (!storage.take(16).every((x) => x == 0x5a) ||
      !storage.skip(32).every((x) => x == 0x5a)) {
    throw StateError('Struct.address ignored its nonzero backing offset');
  }
  final allocate = lib.lookupFunction<Pointer<Small> Function(Int64, Double), Pointer<Small> Function(int, double)>('AllocateSmall');
  final free = lib.lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('FreeMemory');
  final native = allocate(8, 3.5);
  try {
    final nativeStruct = native.ref;
    same(nativeStruct, gc(nativeStruct));
    equal(11.5, changeSmallPointer(nativeStruct.address));
    equal(108, native.ref.a); equal(203.5, native.ref.b);
  } finally {
    free(native.cast<Void>());
  }
  equal(beforeGc + 3, gcCount());
  print('leaf TypedData.address/view/element and Dart/native Struct.address passed');
  print('3 address receivers survived native GC before borrowing');
}
void main(List<String> args) {
  final lib = DynamicLibrary.open(args[0]);
  final makeGlobal = lib.lookupFunction<Int32 Function(), int Function()>('MakeFixtureGlobal');
  equal(1, makeGlobal());
  final frameSeen = lib.lookupFunction<
      Int32 Function(Uint64, Uint64, Uint64), int Function(int, int, int)>(
          'InterpreterFrameSeen');
  equal(int.parse(args[4]), frameSeen(int.parse(args[1]), int.parse(args[2]), int.parse(args[3])));
  final noArguments = lib.lookupFunction<Int64 Function(), int Function()>('NoArguments');
  equal(17, noArguments());
  equal(314, nativeAbs(-314));
  equal(271, nativeAbsLeaf(-271));
  final addressOf = Native.addressOf<NativeFunction<Int32 Function(Int32)>>(nativeAbs).asFunction<int Function(int)>();
  equal(161, addressOf(-161));
  final narrow = lib.lookupFunction<Int8 Function(Int64), int Function(int)>('NarrowResult');
  equal(-1, narrow(255));
  final boolean = lib.lookupFunction<Bool Function(Bool), bool Function(bool)>('BooleanResult');
  equal(false, boolean(true));
  equal(true, boolean(false));
  final pointer = lib.lookupFunction<Pointer<Void> Function(Pointer<Void>), Pointer<Void> Function(Pointer<Void>)>('EchoPointer');
  equal(123456, pointer(Pointer<Void>.fromAddress(123456)).address);
  for (final leaf in [false, true]) {
    final mixed = leaf
      ? lib.lookupFunction<Double Function(Int64, Double, Int8, Float, Uint16, Double), double Function(int, double, int, double, int, double)>('Mixed', isLeaf: true)
      : lib.lookupFunction<Double Function(Int64, Double, Int8, Float, Uint16, Double), double Function(int, double, int, double, int, double)>('Mixed');
    equal(1 + 2.5*2 - 3*3 + 4.25*4 + 60000*5 + 8.5*6, mixed(1, 2.5, -3, 4.25, 60000, 8.5));
    final packed = leaf
      ? lib.lookupFunction<Int64 Function(Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int8, Uint8, Int16, Uint16, Int32, Uint32), int Function(int, int, int, int, int, int, int, int, int, int, int, int, int, int)>('Packed', isLeaf: true)
      : lib.lookupFunction<Int64 Function(Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int8, Uint8, Int16, Uint16, Int32, Uint32), int Function(int, int, int, int, int, int, int, int, int, int, int, int, int, int)>('Packed');
    equal(36 - 7 + 250 - 300 + 60000 - 70000 + 4000000000, packed(1,2,3,4,5,6,7,8,-7,250,-300,60000,-70000,4000000000));
    final makeSmall = leaf
      ? lib.lookupFunction<Small Function(Int64, Double), Small Function(int, double)>('MakeSmall', isLeaf: true)
      : lib.lookupFunction<Small Function(Int64, Double), Small Function(int, double)>('MakeSmall');
    final changeSmall = leaf
      ? lib.lookupFunction<Small Function(Small), Small Function(Small)>('ChangeSmall', isLeaf: true)
      : lib.lookupFunction<Small Function(Small), Small Function(Small)>('ChangeSmall');
    final small = makeSmall(20, 2.5);
    equal(21, small.a); equal(4.5, small.b);
    final changedSmall = changeSmall(small);
    equal(42, changedSmall.a); equal(13.5, changedSmall.b);
    final makeLarge = leaf
      ? lib.lookupFunction<Large Function(Int64), Large Function(int)>('MakeLarge', isLeaf: true)
      : lib.lookupFunction<Large Function(Int64), Large Function(int)>('MakeLarge');
    final changeLarge = leaf
      ? lib.lookupFunction<Large Function(Large), Large Function(Large)>('ChangeLarge', isLeaf: true)
      : lib.lookupFunction<Large Function(Large), Large Function(Large)>('ChangeLarge');
    final large = makeLarge(100);
    equal(100, large.a); equal(101, large.b); equal(102, large.c);
    final changedLarge = changeLarge(large);
    equal(105, changedLarge.a); equal(105, changedLarge.b); equal(105, changedLarge.c);
    equal(100, large.a);
    final makeFloats = leaf
      ? lib.lookupFunction<Floats Function(Float, Float, Float), Floats Function(double, double, double)>('MakeFloats', isLeaf: true)
      : lib.lookupFunction<Floats Function(Float, Float, Float), Floats Function(double, double, double)>('MakeFloats');
    final changeFloats = leaf
      ? lib.lookupFunction<Floats Function(Floats), Floats Function(Floats)>('ChangeFloats', isLeaf: true)
      : lib.lookupFunction<Floats Function(Floats), Floats Function(Floats)>('ChangeFloats');
    final floats = changeFloats(makeFloats(1.5, 2.25, 3.5));
    equal(6.5, floats.a); equal(2.5, floats.b); equal(4.25, floats.c);
    final variadic = leaf
      ? lib.lookupFunction<Double Function(Int32, VarArgs<(Int32, Double, Int32, Double, Int32, Double)>), double Function(int, int, double, int, double, int, double)>('Variadic', isLeaf: true)
      : lib.lookupFunction<Double Function(Int32, VarArgs<(Int32, Double, Int32, Double, Int32, Double)>), double Function(int, int, double, int, double, int, double)>('Variadic');
    equal(1 + 2.5 + 3 + 4.25 + 5 + 6.5, variadic(3, 1, 2.5, 3, 4.25, 5, 6.5));
    // OVERFLOW_CALL
  }
  testHandlesAndGc(lib);
  testLeafAddresses(lib);
  print('native scalar/mixed/overflow/packed/compound/variadic calls passed');
}
'''

ZERO_STACK = r'''
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
struct Arguments {
  uintptr_t stack_area, stack_area_end;
  uintptr_t cpu_registers[32], fpu_registers[32], target;
};
_Static_assert(offsetof(struct Arguments, cpu_registers) == 16, "CPU offset");
_Static_assert(offsetof(struct Arguments, fpu_registers) == 272, "FPU offset");
_Static_assert(offsetof(struct Arguments, target) == 528, "target offset");
extern void FfiCallTrampoline(struct Arguments* args);
static int64_t Target(void) { return 12345; }
int main(void) {
  long page_size = sysconf(_SC_PAGESIZE);
  void* guard = mmap(NULL, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (guard == MAP_FAILED) return 2;
  // Both endpoints are inside inaccessible memory. A zero-size copy must not
  // load from either endpoint or the preceding 16-byte block.
  struct Arguments args = {0};
  args.stack_area = args.stack_area_end = (uintptr_t)guard + 16;
  args.target = (uintptr_t)&Target;
  FfiCallTrampoline(&args);
  munmap(guard, page_size);
  if (args.cpu_registers[0] != 12345) return 3;
  puts("empty native trampoline stack passed");
  return 0;
}
'''


def interpreter_entry_range(dart):
    # No VM test exports are needed: use existing local symbols to identify a
    # real DRT_FfiCall frame in the fixture's native backtrace.
    output = subprocess.check_output(['nm', '-n', str(dart)], text=True)
    symbols = []
    image_base = 0
    start = None
    for line in output.splitlines():
        fields = line.split()
        if len(fields) != 3 or fields[1] not in ('t', 'T'):
            continue
        address = int(fields[0], 16)
        symbols.append(address)
        if fields[2] == '__mh_execute_header':
            image_base = address
        if fields[2] in ('DRT_FfiCall', '_DRT_FfiCall'):
            start = address
    if start is None:
        raise AssertionError('dart must retain its DRT_FfiCall symbol for this test')
    return start, min(address for address in symbols if address > start), image_base


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    # The dart CLI may forward to a separate VM executable. Test the VM we
    # built directly, and derive frame addresses from that same executable.
    dart = build / ('dartvm' if (build / 'dartvm').is_file() else 'dart')
    compiler = shutil.which('clang')
    if compiler is None or sys.platform not in ('darwin', 'linux'):
        raise RuntimeError('This native regression requires clang on macOS/Linux')
    context = (contextlib.nullcontext(str(args.work_dir.resolve()))
               if args.work_dir else tempfile.TemporaryDirectory(prefix='dart-interpreter-ffi-'))
    with context as directory:
        work = Path(directory)
        work.mkdir(parents=True, exist_ok=True)
        native = work / 'fixture.c'
        source = work / 'main.dart'
        dylib = work / ('libfixture.dylib' if sys.platform == 'darwin' else 'libfixture.so')
        bytecode = work / 'main.bytecode'
        parameters = ', '.join(f'int64_t i{i}, double d{i}' for i in range(12))
        total = ' + '.join(f'i{i} + d{i}' for i in range(12))
        native.write_text(NATIVE + f'\ndouble Overflow({parameters}) {{ return {total}; }}\n')
        native_types = ', '.join(['Int64, Double'] * 12)
        dart_types = ', '.join(['int, double'] * 12)
        values = ', '.join(f'{i + 1}, {i + 1}.5' for i in range(12))
        signature = f'Double Function({native_types}), double Function({dart_types})'
        overflow = (f"final overflow = leaf ? lib.lookupFunction<{signature}>('Overflow', isLeaf: true) "
                    f": lib.lookupFunction<{signature}>('Overflow');\n"
                    f'equal(162.0, overflow({values}));')
        source.write_text(DART.replace('// OVERFLOW_CALL', overflow))

        def run(*command, log=None):
            print('+', ' '.join(str(x) for x in command), flush=True)
            result = subprocess.run([str(x) for x in command], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if log:
                (work / log).write_text(result.stdout)
            if result.returncode:
                print(result.stdout, flush=True)
                raise subprocess.CalledProcessError(result.returncode, command)
            if result.stdout:
                print(result.stdout, end='', flush=True)
            return result.stdout

        run(compiler, '-shared', '-fPIC', '-O0', '-fno-omit-frame-pointer',
            '-I', root / 'runtime/include', native, '-o', dylib, *(['-ldl'] if sys.platform == 'linux' else []))
        start, end, image_base = interpreter_entry_range(dart)
        call_args = [dylib, start, end, image_base]
        jit_output = run(dart, source, *call_args, 0, log='jit.log')
        run(dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
            f'--platform={build / "vm_platform.dill"}', '-o', bytecode, source,
            log='compile.log')
        output = run(dart, '--interpreter', bytecode, *call_args, 1, log='interpreter.log')
        markers = [
            'native scalar/mixed/overflow/packed/compound/variadic calls passed',
            'Handle arguments/results and 21 nonleaf native GC calls passed',
            'leaf TypedData.address/view/element and Dart/native Struct.address passed',
            '3 address receivers survived native GC before borrowing',
        ]
        for mode, result in [('JIT control', jit_output), ('interpreter', output)]:
            for marker in markers:
                if marker not in result:
                    raise AssertionError(f'{mode} did not finish: {marker}')
        print('DRT_FfiCall native frame verified in bytecode execution', flush=True)
        if platform.machine().lower() in ('arm64', 'aarch64'):
            empty_source = work / 'empty_stack.c'
            empty_source.write_text(ZERO_STACK)
            empty_binary = work / 'empty_stack'
            run(compiler, '-DDART_BYTECODE_INTERPRETER=1', empty_source,
                root / 'runtime/vm/ffi_trampolines_arm64.S', '-o', empty_binary)
            run(empty_binary, log='empty_stack.log')
        else:
            print('ARM64 empty-stack assembly check skipped on this host', flush=True)
        print('Interpreter FFI call-plan regression passed', flush=True)


if __name__ == '__main__':
    main()
