// Runtime-created callback fixture: new URI, absent from the native baseline.
// Unsupported shapes below are registration rejection probes, never invoked.
import 'dart:ffi';

typedef I64 = Int64 Function(Int64, Int64);
typedef F64 = Double Function(Double);
typedef Ptr = Pointer<Void> Function(Pointer<Void>, IntPtr);
typedef Mixed = Double Function(Int64, Double, Int8, Float, Uint16, Double);
typedef Ten = Int64 Function(Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int64, Int64);
typedef Variadic = Int64 Function(Int64, VarArgs<(Int64, Double, Int32)>);

late int Function(Pointer<NativeFunction<I64>>, int, int) callI64;
late double Function(Pointer<NativeFunction<F64>>, double) callDouble;
late Pointer<Void> Function(Pointer<NativeFunction<Ptr>>, Pointer<Void>, int) callPointer;
late double Function(Pointer<NativeFunction<Mixed>>) callMixed;
late int Function(Pointer<NativeFunction<Ten>>) callTen;
late int Function(Pointer<NativeFunction<Variadic>>, int) callVariadic;
late int Function(int, int) nativeAdd;
late int Function() gcNow;
late int Function() gcCount;
late int Function(int, int, int) frameSeen;
late int entryStart, entryEnd, imageBase;
late DynamicLibrary fixtureLib;
late int Function() nativeCallCount;

void equal(Object? expected, Object? actual) {
  if (expected != actual) throw StateError('expected $expected, got $actual');
}
void commonEntryFrame() => equal(1, frameSeen(entryStart, entryEnd, imageBase));

@pragma('vm:entry-point')
void configure(String library, int start, int end, int base) {
  final lib = DynamicLibrary.open(library);
  fixtureLib = lib;
  nativeCallCount = lib.lookupFunction<IntPtr Function(), int Function()>('CallbackCallCount');
  callI64 = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<I64>>, Int64, Int64), int Function(Pointer<NativeFunction<I64>>, int, int)>('CallI64');
  callDouble = lib.lookupFunction<Double Function(Pointer<NativeFunction<F64>>, Double), double Function(Pointer<NativeFunction<F64>>, double)>('CallDouble');
  callPointer = lib.lookupFunction<Pointer<Void> Function(Pointer<NativeFunction<Ptr>>, Pointer<Void>, IntPtr), Pointer<Void> Function(Pointer<NativeFunction<Ptr>>, Pointer<Void>, int)>('CallPointer');
  callMixed = lib.lookupFunction<Double Function(Pointer<NativeFunction<Mixed>>), double Function(Pointer<NativeFunction<Mixed>>)>('CallMixed');
  callTen = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Ten>>), int Function(Pointer<NativeFunction<Ten>>)>('CallTen');
  callVariadic = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<Variadic>>, Int64), int Function(Pointer<NativeFunction<Variadic>>, int)>('CallVariadic');
  nativeAdd = lib.lookupFunction<Int64 Function(Int64, Int64), int Function(int, int)>('NativeAdd');
  gcNow = lib.lookupFunction<IntPtr Function(), int Function()>('CallbackGcNow');
  gcCount = lib.lookupFunction<IntPtr Function(), int Function()>('CallbackGcCount');
  frameSeen = lib.lookupFunction<Int32 Function(Uint64, Uint64, Uint64), int Function(int, int, int)>('CallbackFrameSeen');
  entryStart = start; entryEnd = end; imageBase = base;
  configureExtras(lib);
}

// Static targets are genuinely absent from the native baseline. CPP must
// inspect these exact functions' HasBytecode/IsInterpreted after raw loading.
@pragma('vm:entry-point')
int staticScalar(int a, int b) {
  commonEntryFrame();
  if (a == -77) throw StateError('static callback exceptional return');
  return a * 3 + b;
}
@pragma('vm:entry-point')
double staticDouble(double value) {
  if (value == -77) throw StateError('double callback exceptional return');
  return value * 2 + 0.5;
}
@pragma('vm:entry-point')
Pointer<Void> staticPointer(Pointer<Void> value, int delta) =>
    Pointer<Void>.fromAddress(value.address + delta);
@pragma('vm:entry-point')
double staticMixed(int a, double b, int c, double d, int e, double f) =>
    a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
@pragma('vm:entry-point')
int staticTen(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, int a8, int a9) =>
    a0 + a1*2 + a2*3 + a3*4 + a4*5 + a5*6 + a6*7 + a7*8 + a8*9 + a9*10;
@pragma('vm:entry-point')
int staticVariadic(int fixed, int integer, double floating, int promoted) {
  if (fixed == -99) throw StateError('variadic callback exceptional return');
  return fixed + integer + (floating * 2).toInt() + promoted;
}

// Keep returned static pointers live for the isolate lifetime; there is no
// close API for Pointer.fromFunction. No stale-pointer invocation is tested.
@pragma('vm:entry-point')
List<int> staticPointers() => staticPointerValues;
late final staticPointerValues = <int>[
  Pointer.fromFunction<I64>(staticScalar, -7001).address,
  Pointer.fromFunction<F64>(staticDouble, -12.5).address,
  Pointer.fromFunction<Ptr>(staticPointer).address,
  Pointer.fromFunction<Mixed>(staticMixed, -18.5).address,
  Pointer.fromFunction<Ten>(staticTen, -7002).address,
  Pointer.fromFunction<Variadic>(staticVariadic, -9023).address,
];

@pragma('vm:entry-point')
int runStatic() {
  final pointers = staticPointers();
  equal(19, callI64(Pointer.fromAddress(pointers[0]), 5, 4));
  equal(-7001, callI64(Pointer.fromAddress(pointers[0]), -77, 4));
  equal(5.5, callDouble(Pointer.fromAddress(pointers[1]), 2.5));
  equal(-12.5, callDouble(Pointer.fromAddress(pointers[1]), -77));
  equal(0x123425, callPointer(Pointer.fromAddress(pointers[2]), Pointer<Void>.fromAddress(0x123400), 37).address);
  equal(300065.0, callMixed(Pointer.fromAddress(pointers[3])));
  equal(385, callTen(Pointer.fromAddress(pointers[4])));
  equal(28, callVariadic(Pointer.fromAddress(pointers[5]), 5));
  equal(-9023, callVariadic(Pointer.fromAddress(pointers[5]), -99));
  return pointers.length;
}

@pragma('vm:entry-point')
int runCapturesNestingAndGc() {
  final captured = <int>[40];
  final before = gcCount();
  final inner = NativeCallable<I64>.isolateLocal((int a, int b) {
    commonEntryFrame();
    gcNow();
    return captured[0] + a + b;
  }, exceptionalReturn: -8101);
  final outer = NativeCallable<I64>.isolateLocal((int a, int b) {
    commonEntryFrame();
    gcNow();
    return nativeAdd(a, b) + callI64(inner.nativeFunction, a + 1, b + 1);
  }, exceptionalReturn: -8102);
  double throwingTarget(double value) {
    gcNow();
    throw StateError('capturing exceptional return');
  }
  final throwing = NativeCallable<F64>.isolateLocal(
      throwingTarget, exceptionalReturn: -37.25);
  final variadic = NativeCallable<Variadic>.isolateLocal(
      (int fixed, int integer, double floating, int promoted) =>
          captured[0] + fixed + integer + (floating * 2).toInt() + promoted,
      exceptionalReturn: -8201);
  try {
    gcNow(); // Descriptors must root closures before the first invocation too.
    equal(72, callI64(outer.nativeFunction, 7, 8));
    captured[0] = 100;
    equal(132, callI64(outer.nativeFunction, 7, 8));
    equal(-37.25, callDouble(throwing.nativeFunction, 2));
    equal(128, callVariadic(variadic.nativeFunction, 5));
    equal(before + 6, gcCount()); // before-first + 2 nested pairs + thrower
    return 4;
  } finally {
    inner.close(); outer.close(); throwing.close(); variadic.close();
  }
}

@pragma('vm:entry-point')
int runSelfClose() {
  late NativeCallable<I64> self;
  var oldAddress = 0;
  var replacementAddress = 0;
  final capture = <int>[5];
  self = NativeCallable<I64>.isolateLocal((int a, int b) {
    self.close();
    gcNow(); // Deleting registration must not collect the active descriptor.
    final replacement = NativeCallable<F64>.isolateLocal(
        (double value) => value * 2 + capture[0], exceptionalReturn: -91.5);
    replacementAddress = replacement.nativeFunction.address;
    try {
      final nested = callDouble(replacement.nativeFunction, 1.25);
      gcNow();
      return a + b + nested.toInt();
    } finally { replacement.close(); }
  }, exceptionalReturn: -9101);
  oldAddress = self.nativeFunction.address;
  equal(19, callI64(self.nativeFunction, 5, 7));
  var rejected = false;
  try { self.nativeFunction; } on StateError { rejected = true; }
  equal(true, rejected);
  equal(false, self.keepIsolateAlive);
  self.close(); // idempotent; never invoke the old pointer after close.
  return replacementAddress == oldAddress ? 1 : 0;
}

final pool = <NativeCallable<I64>>[];
@pragma('vm:entry-point')
List<int> createPool(int count) {
  if (pool.isNotEmpty) throw StateError('previous pool not closed');
  for (var i = 0; i < count; ++i) {
    final captured = <int>[i];
    pool.add(NativeCallable<I64>.isolateLocal(
        (int a, int b) => captured[0] + a + b, exceptionalReturn: -10001));
  }
  return [for (final callback in pool) callback.nativeFunction.address];
}
@pragma('vm:entry-point')
int runPool() {
  final before = nativeCallCount();
  gcNow();
  var sum = 0;
  for (final callback in pool) sum += callI64(callback.nativeFunction, 3, 5);
  final n = pool.length;
  equal(n * (n - 1) ~/ 2 + n * 8, sum);
  equal(before + n, nativeCallCount());
  return n;
}
@pragma('vm:entry-point')
void closePool() {
  for (final callback in pool) { callback.close(); callback.close(); }
  pool.clear();
  gcNow();
}

void main() {} // Entry point required for ordinary component loading.

typedef DoubleTen = Double Function(Double,Double,Double,Double,Double,Double,Double,Double,Double,Double);
typedef B1 = Bool Function(Bool);
typedef F32 = Float Function(Float);
typedef V0 = Void Function(Int64);
typedef Abi16 = CallbackAbiInt Function(CallbackAbiInt);
typedef EmptyVar = Int64 Function(Int64, VarArgs<()>);
typedef NarrowStack = Int64 Function(Int64,Int64,Int64,Int64,Int64,Int64,Int64,Int64,Int8,Uint8,Int16,Uint16,Int32,Uint32);
@AbiSpecificIntegerMapping({Abi.macosArm64: Int16(), Abi.linuxArm64: Int16()})
final class CallbackAbiInt extends AbiSpecificInteger { const CallbackAbiInt(); }
@AbiSpecificIntegerMapping({Abi.fuchsiaArm64: Int8()})
final class CallbackMissingInt extends AbiSpecificInteger { const CallbackMissingInt(); }

late double Function(Pointer<NativeFunction<DoubleTen>>) callDoubleTen;
late bool Function(Pointer<NativeFunction<B1>>, bool) callBool;
late double Function(Pointer<NativeFunction<F32>>, double) callFloat;
late void Function(Pointer<NativeFunction<V0>>, int) callVoid;
late int Function(Pointer<NativeFunction<Abi16>>, int) callAbi;
late int Function(Pointer<NativeFunction<EmptyVar>>) callEmptyVar;
late int Function(Pointer<NativeFunction<NarrowStack>>) callNarrowStack;
void configureExtras(DynamicLibrary lib) {
  callDoubleTen = lib.lookupFunction<Double Function(Pointer<NativeFunction<DoubleTen>>), double Function(Pointer<NativeFunction<DoubleTen>>)>('CallDoubleTen');
  callBool = lib.lookupFunction<Bool Function(Pointer<NativeFunction<B1>>, Bool), bool Function(Pointer<NativeFunction<B1>>, bool)>('CallBool');
  callFloat = lib.lookupFunction<Double Function(Pointer<NativeFunction<F32>>, Float), double Function(Pointer<NativeFunction<F32>>, double)>('CallFloat');
  callVoid = lib.lookupFunction<Void Function(Pointer<NativeFunction<V0>>, Int64), void Function(Pointer<NativeFunction<V0>>, int)>('CallVoid');
  callAbi = lib.lookupFunction<CallbackAbiInt Function(Pointer<NativeFunction<Abi16>>, CallbackAbiInt), int Function(Pointer<NativeFunction<Abi16>>, int)>('CallAbi');
  callEmptyVar = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<EmptyVar>>), int Function(Pointer<NativeFunction<EmptyVar>>)>('CallEmptyVariadic');
  callNarrowStack = lib.lookupFunction<Int64 Function(Pointer<NativeFunction<NarrowStack>>), int Function(Pointer<NativeFunction<NarrowStack>>)>('CallNarrowStack');
}

double staticDoubleTen(double a,double b,double c,double d,double e,double f,double g,double h,double i,double j) => a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10;
bool staticBool(bool value) { if (!value) throw StateError('Bool'); return !value; }
double staticFloat(double value) { if (value == -77) throw StateError('Float'); return value * 2 + 0.25; }
int voidCalls = 0;
void staticVoid(int value) { ++voidCalls; if (value < 0) throw StateError('Void'); }
Pointer<Void> staticPointerFailure(Pointer<Void> value, int delta) => throw StateError('Pointer');
int staticAbi(int value) { if (value == -77) throw StateError('Abi'); return value + 7; }
int staticEmptyVar(int value) => value + 2;
int staticNarrowStack(int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,int b,int c,int d,int e,int f,int g) =>
    a0+a1+a2+a3+a4+a5+a6+a7+b+c+d+e+f+g;
@pragma('vm:entry-point')
List<int> extraStaticPointers() => extraStaticPointerValues;
late final extraStaticPointerValues = <int>[
  Pointer.fromFunction<B1>(staticBool, true).address,
  Pointer.fromFunction<F32>(staticFloat, -6.25).address,
  Pointer.fromFunction<V0>(staticVoid).address,
  Pointer.fromFunction<Ptr>(staticPointerFailure).address,
  Pointer.fromFunction<Abi16>(staticAbi, -1234).address,
  Pointer.fromFunction<EmptyVar>(staticEmptyVar, -1235).address,
  Pointer.fromFunction<NarrowStack>(staticNarrowStack, -1236).address,
  Pointer.fromFunction<DoubleTen>(staticDoubleTen, -1237.5).address,
];
@pragma('vm:entry-point')
int runExtraStatic() {
  final p = extraStaticPointers();
  equal(false, callBool(Pointer.fromAddress(p[0]), true));
  equal(true, callBool(Pointer.fromAddress(p[0]), false));
  equal(3.25, callFloat(Pointer.fromAddress(p[1]), 1.5));
  equal(-6.25, callFloat(Pointer.fromAddress(p[1]), -77));
  callVoid(Pointer.fromAddress(p[2]), -1); callVoid(Pointer.fromAddress(p[2]), 1);
  equal(2, voidCalls);
  equal(0, callPointer(Pointer.fromAddress(p[3]), Pointer<Void>.fromAddress(1234), 1).address);
  equal(-293, callAbi(Pointer.fromAddress(p[4]), -300));
  equal(-1234, callAbi(Pointer.fromAddress(p[4]), -77));
  equal(21, callEmptyVar(Pointer.fromAddress(p[5])));
  equal(36-7+250-300+60000-70000+4000000000, callNarrowStack(Pointer.fromAddress(p[6])));
  equal(385.0, callDoubleTen(Pointer.fromAddress(p[7])));
  return p.length;
}

@pragma('vm:entry-point')
int runExtraCaptures() {
  final captured = <int>[100];
  final pointer = NativeCallable<Ptr>.isolateLocal((Pointer<Void> p, int d) =>
      Pointer<Void>.fromAddress(p.address + d + captured[0]));
  final mixed = NativeCallable<Mixed>.isolateLocal(
      (int a,double b,int c,double d,int e,double f) => captured[0]+a+b*2+c*3+d*4+e*5+f*6,
      exceptionalReturn: -99.25);
  final narrow = NativeCallable<NarrowStack>.isolateLocal(
      (int a0,int a1,int a2,int a3,int a4,int a5,int a6,int a7,int b,int c,int d,int e,int f,int g) =>
          captured[0]+a0+a1+a2+a3+a4+a5+a6+a7+b+c+d+e+f+g,
      exceptionalReturn: -99);
  final doubleTen = NativeCallable<DoubleTen>.isolateLocal(
      (double a,double b,double c,double d,double e,double f,double g,double h,double i,double j) => captured[0]+a+b*2+c*3+d*4+e*5+f*6+g*7+h*8+i*9+j*10,
      exceptionalReturn: -1238.5);
  final boolean = NativeCallable<B1>.isolateLocal((bool value) { if (!value) throw StateError('capture'); return value; }, exceptionalReturn: false);
  final floating = NativeCallable<F32>.isolateLocal((double value) { if (value < 0) throw StateError('capture'); return value+captured[0]; }, exceptionalReturn: -8.5);
  final abi = NativeCallable<Abi16>.isolateLocal((int value) { if (value == -77) throw StateError('capture'); return value+captured[0]; }, exceptionalReturn: -2345);
  final empty = NativeCallable<EmptyVar>.isolateLocal((int value) => value+captured[0], exceptionalReturn: -99);
  final voidCallback = NativeCallable<V0>.isolateLocal((int value) { captured[0] += 1; if (value < 0) throw StateError('capture'); });
  final pointerFailure = NativeCallable<Ptr>.isolateLocal((Pointer<Void> p, int d) => throw StateError('capture'));
  try {
    gcNow();
    equal(0x1066, callPointer(pointer.nativeFunction, Pointer<Void>.fromAddress(0x1000), 2).address);
    equal(300165.0, callMixed(mixed.nativeFunction));
    equal(485.0, callDoubleTen(doubleTen.nativeFunction));
    equal(100+36-7+250-300+60000-70000+4000000000, callNarrowStack(narrow.nativeFunction));
    equal(true, callBool(boolean.nativeFunction, true)); equal(false, callBool(boolean.nativeFunction, false));
    equal(101.5, callFloat(floating.nativeFunction, 1.5)); equal(-8.5, callFloat(floating.nativeFunction, -1));
    equal(-200, callAbi(abi.nativeFunction, -300)); equal(-2345, callAbi(abi.nativeFunction, -77));
    equal(119, callEmptyVar(empty.nativeFunction));
    callVoid(voidCallback.nativeFunction, -1); callVoid(voidCallback.nativeFunction, 1); equal(102, captured[0]);
    equal(0, callPointer(pointerFailure.nativeFunction, nullptr, 1).address);
    return 10;
  } finally { doubleTen.close(); pointer.close(); mixed.close(); narrow.close(); boolean.close(); floating.close(); abi.close(); empty.close(); voidCallback.close(); pointerFailure.close(); }
}

late NativeCallable<I64> ownedCallback;
@pragma('vm:entry-point')
int createOwned() {
  final capture = <int>[300];
  ownedCallback = NativeCallable<I64>.isolateLocal((int a, int b) {
    commonEntryFrame(); gcNow(); return a + b + capture[0];
  }, exceptionalReturn: -17101);
  return ownedCallback.nativeFunction.address;
}
@pragma('vm:entry-point')
void closeOwned() { ownedCallback.close(); }

typedef HandleCallback = Handle Function(Handle);
typedef BoxCallback = RejectedBox Function(RejectedBox);
typedef MissingCallback = CallbackMissingInt Function(CallbackMissingInt);
typedef UnionCallback = RejectedUnion Function(RejectedUnion);
final class RejectedUnion extends Union { @Int64() external int value; @Double() external double floating; }
RejectedUnion unionTarget(RejectedUnion value) => value;
final class RejectedBox extends Struct { @Int64() external int value; }
Object? handleTarget(Object? value) => value;
RejectedBox boxTarget(RejectedBox value) => value;
int missingTarget(int value) => value;
@pragma('vm:entry-point')
int rejectCase(int which) {
  var caught = false;
  try {
    switch (which) {
      case 0: NativeCallable<HandleCallback>.isolateLocal(handleTarget); break;
      case 1: Pointer.fromFunction<BoxCallback>(boxTarget); break;
      case 2: NativeCallable<V0>.listener((int value) {}); break;
      case 3: NativeCallable<I64>.isolateGroupBound((int a, int b) => a + b, exceptionalReturn: -1); break;
      case 4: Pointer.fromFunction<MissingCallback>(missingTarget, -11); break;
      case 5: NativeCallable<UnionCallback>.isolateLocal(unionTarget); break;
      case 6: NativeCallable<MissingCallback>.isolateLocal(missingTarget, exceptionalReturn: -11); break;
      default: throw StateError('unknown rejection case');
    }
  } on UnsupportedError { if (which == 4 || which == 6) rethrow; caught = true; }
    on ArgumentError catch (error) { if (which != 4 && which != 6) rethrow; if (!error.toString().contains('CallbackMissingInt') || !error.toString().contains('missing mapping')) rethrow; caught = true; }
  if (!caught) throw StateError('unsupported callback returned a native pointer');
  return which;
}

@pragma('vm:entry-point')
int runCounted(int phase) {
  final before = nativeCallCount();
  final int result;
  final int expected;
  switch (phase) {
    case 0: result = runStatic(); expected = 9; break;
    case 1: result = runExtraStatic(); expected = 12; break;
    case 2: result = runCapturesNestingAndGc(); expected = 6; break;
    case 3: result = runExtraCaptures(); expected = 14; break;
    case 4: result = runSelfClose(); expected = 2; break;
    default: throw StateError('unknown phase');
  }
  equal(before + expected, nativeCallCount());
  return result;
}
