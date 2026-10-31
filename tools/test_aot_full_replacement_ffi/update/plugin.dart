import 'dart:ffi';
import 'dart:typed_data';
import 'added.dart';

@pragma('vm:entry-point')
class PluginConfig {
  final int seed;
  final int increment;
  const PluginConfig(this.seed, {this.increment = 41});
  int get value => seed + increment;
}

@pragma('vm:entry-point')
class Plugin {
  int old = 2;
  final int added = 13;
  late int lazy = 14;
  int read({int delta = 1}) => old + added + lazy + delta;
}

final class PluginRecord extends Struct {
  @Int64()
  external int number;
  @Double()
  external double fraction;
  @Int64()
  external int extra;
}

@Native<Int64 Function(Int64)>(symbol: 'FullResolverValue')
external int resolvedValue(int value);
@Native<PluginRecord Function(PluginRecord)>(
    symbol: 'FullRecordV2', assetId: 'full-release-asset')
external PluginRecord changeRecord(PluginRecord value);
@Native<Int64 Function(Int64)>(
    symbol: 'FullAssetValue', assetId: 'full-release-asset')
external int assetValue(int value);
@Native<Int64 Function(Int64)>(
    symbol: 'FullProcessValue', assetId: 'full-process-fallback')
external int processValue(int value);
@Native<Int64 Function()>(symbol: 'FullResolverMissing')
external int missingValue();
@Native<Int64 Function()>(
    symbol: 'FullMissingAssetSymbol', assetId: 'full-release-asset')
external int missingAssetValue();

void equal(Object? expected, Object? actual) {
  if (expected != actual) throw StateError('$expected != $actual');
}

@pragma('vm:entry-point')
int runResolver() => resolvedValue(9);
@pragma('vm:entry-point')
int runAddressOf() => Native.addressOf<NativeFunction<Int64 Function(Int64)>>(
      resolvedValue,
    ).asFunction<int Function(int)>()(10);
@pragma('vm:entry-point')
int runAsset() => assetValue(11);
@pragma('vm:entry-point')
int runProcess() => processValue(12);
@pragma('vm:entry-point')
int runErrors() {
  var errors = 0;
  for (var i = 0; i < 2; ++i) {
    try {
      missingValue();
    } on ArgumentError {
      ++errors;
    }
    try {
      missingAssetValue();
    } on ArgumentError {
      ++errors;
    }
  }
  return errors;
}

@pragma('vm:entry-point')
int runCalls(String path) {
  final lib = DynamicLibrary.open(path);
  final dynamicCall = lib
      .lookup<NativeFunction<Int64 Function(Int64)>>('FullDynamicValue')
      .asFunction<int Function(int)>();
  equal(413, dynamicCall(13));
  final record = Struct.create<PluginRecord>()
    ..number = 20
    ..fraction = 2.5
    ..extra = 30;
  final changed = changeRecord(record);
  equal(24, sizeOf<PluginRecord>());
  equal(22, changed.number);
  equal(7.5, changed.fraction);
  equal(35, changed.extra);
  equal(20, record.number);
  runNewCalls(lib);
  return Plugin().read() + addedProbe();
}

@pragma('vm:entry-point')
int runNonFfi() {
  const a = PluginConfig(7);
  const b = PluginConfig(7, increment: 41);
  equal(true, identical(a, b));
  equal(48, a.value);
  const unequal = PluginConfig(7, increment: 42);
  const nested = <PluginConfig>[PluginConfig(7)];
  const same = <PluginConfig>[PluginConfig(7, increment: 41)];
  equal(false, identical(a, unequal));
  equal(true, identical(nested, same));
  equal(30, Plugin().read());
  final changed = Plugin()..old = 12;
  equal(40, changed.read());
  var removed = false;
  try {
    (changed as dynamic).obsolete;
  } on NoSuchMethodError {
    removed = true;
  }
  equal(true, removed);
  return <int>[1, 2, 3].map((n) => n * 2).reduce((a, b) => a + b);
}

@Native<Int64 Function(Pointer<Int64>, Int64)>(
    symbol: 'FullBorrow', assetId: 'full-release-asset', isLeaf: true)
external int borrow(Pointer<Int64> values, int delta);

void runNewCalls(DynamicLibrary lib) {
  for (final leaf in [false, true]) {
    final word = leaf
        ? lib.lookupFunction<PluginWord Function(PluginWord),
            PluginWord Function(PluginWord)>('FullWord', isLeaf: true)
        : lib.lookupFunction<PluginWord Function(PluginWord),
            PluginWord Function(PluginWord)>('FullWord');
    final w = Union.create<PluginWord>()..bits = 80;
    equal(97, word(w).bits);
    equal(80, w.bits);
    final hfa = leaf
        ? lib.lookupFunction<PluginHfa Function(PluginHfa),
            PluginHfa Function(PluginHfa)>('FullHfa', isLeaf: true)
        : lib.lookupFunction<PluginHfa Function(PluginHfa),
            PluginHfa Function(PluginHfa)>('FullHfa');
    final h = Struct.create<PluginHfa>()
      ..a = 1
      ..b = 2
      ..c = 3;
    final hr = hfa(h);
    equal(12.0, hr.a + hr.b + hr.c);
    final packed = leaf
        ? lib.lookupFunction<PluginPacked Function(PluginPacked),
            PluginPacked Function(PluginPacked)>('FullPacked', isLeaf: true)
        : lib.lookupFunction<PluginPacked Function(PluginPacked),
            PluginPacked Function(PluginPacked)>('FullPacked');
    final p = Struct.create<PluginPacked>()
      ..tag = 7
      ..value = 100;
    equal(9, sizeOf<PluginPacked>());
    final pr = packed(p);
    equal(9, pr.tag);
    equal(103, pr.value);
    final nested = leaf
        ? lib.lookupFunction<PluginNested Function(PluginNested),
            PluginNested Function(PluginNested)>('FullNested', isLeaf: true)
        : lib.lookupFunction<PluginNested Function(PluginNested),
            PluginNested Function(PluginNested)>('FullNested');
    final n = Struct.create<PluginNested>();
    n.hfa.b = 3;
    n.word.bits = 20;
    final nr = nested(n);
    equal(8.0, nr.hfa.b);
    equal(27, nr.word.bits);
  }
  final abi =
      lib.lookupFunction<PluginInt Function(PluginInt), int Function(int)>(
          'FullAbi');
  equal(32, abi(25));
  final boolean =
      lib.lookupFunction<Bool Function(Bool), bool Function(bool)>('FullBool');
  equal(false, boolean(true));
  equal(true, boolean(false));
  final pointer = lib.lookupFunction<Pointer<Void> Function(Pointer<Void>),
      Pointer<Void> Function(Pointer<Void>)>('FullPointer');
  equal(1234, pointer(Pointer<Void>.fromAddress(1234)).address);
  final bytes = Int64List.fromList([33]);
  equal(42, borrow(bytes.address, 9));
  final echo =
      lib.lookupFunction<Handle Function(Handle), Object? Function(Object?)>(
          'FullEcho');
  final token = <Object>['full update handle', Plugin()];
  equal(true, identical(token, echo(token)));
  // The compound VarArgs signature is also absent from N/B.
  final variadic = lib.lookupFunction<
      Double Function(Int64, VarArgs<(PluginHfa, PluginHfa)>),
      double Function(int, PluginHfa, PluginHfa)>('FullHfaVarArgs');
  final h = Struct.create<PluginHfa>()
    ..a = 1
    ..b = 2
    ..c = 3;
  equal(12.0, variadic(2, h, h));
}

typedef ScalarCallback = Int64 Function(Int64, Int64);
typedef RecordCallback = PluginRecord Function(PluginRecord);
typedef ListenerCallback = Void Function(Int64);
typedef RecordListener = Void Function(PluginRecord);
late int Function(Pointer<NativeFunction<ScalarCallback>>, int, int) callScalar;
late int Function() gcNow;
final callbackObjects = <NativeCallable>[];
final pool = <NativeCallable<ScalarCallback>>[];
late Pointer<NativeFunction<ScalarCallback>> staticPointer;
NativeCallable<ListenerCallback>? listener;
NativeCallable<RecordListener>? recordListener;
NativeCallable<ScalarCallback>? group;
var received = 0;
var receivedSum = 0;
var payloads = 0;

@pragma('vm:entry-point')
int staticTarget(int a, int b) {
  if (a == -1) throw StateError('full static callback');
  return a * 3 + b;
}

@pragma('vm:entry-point')
int prepareStaticCallback(String path) {
  final lib = DynamicLibrary.open(path);
  callScalar = lib.lookupFunction<
      Int64 Function(Pointer<NativeFunction<ScalarCallback>>, Int64, Int64),
      int Function(
          Pointer<NativeFunction<ScalarCallback>>, int, int)>('FullCallScalar');
  gcNow = lib.lookupFunction<Int64 Function(), int Function()>('FullGc');
  staticPointer = Pointer.fromFunction<ScalarCallback>(staticTarget, -7001);
  equal(19, callScalar(staticPointer, 5, 4));
  equal(-7001, callScalar(staticPointer, -1, 4));
  return staticPointer.address;
}

// Keep the group callback's lexical context separate from prepareCallbacks'
// mutable local capture and listener/FFI closures. The runtime validates every
// value in its context chain, including slots unused by this callback body.
NativeCallable<ScalarCallback> createImmutableGroupCallback(int offset) {
  final int amount = offset;
  return NativeCallable<ScalarCallback>.isolateGroupBound(
      (int a, int b) => amount + a + b,
      exceptionalReturn: -7005);
}

@pragma('vm:entry-point')
List<int> prepareCallbacks(String path, int poolSize) {
  final lib = DynamicLibrary.open(path);
  final localStatic = NativeCallable<ScalarCallback>.isolateLocal(staticTarget,
      exceptionalReturn: -7002);
  callbackObjects.add(localStatic);
  equal(19, callScalar(localStatic.nativeFunction, 5, 4));
  final captured = <int>[40];
  final local = NativeCallable<ScalarCallback>.isolateLocal((int a, int b) {
    gcNow();
    return captured[0] + a + b;
  }, exceptionalReturn: -7003);
  callbackObjects.add(local);
  equal(52, callScalar(local.nativeFunction, 7, 5));
  captured[0] = 100;
  equal(112, callScalar(local.nativeFunction, 7, 5));
  final aggregate =
      NativeCallable<RecordCallback>.isolateLocal((PluginRecord r) {
    gcNow();
    r.number += captured[0];
    r.extra += 2;
    return r;
  });
  callbackObjects.add(aggregate);
  final invokeRecord = lib.lookupFunction<
      PluginRecord Function(
          Pointer<NativeFunction<RecordCallback>>, PluginRecord),
      PluginRecord Function(Pointer<NativeFunction<RecordCallback>>,
          PluginRecord)>('FullCallRecord');
  final r = Struct.create<PluginRecord>()
    ..number = 10
    ..fraction = 2.5
    ..extra = 20;
  final result = invokeRecord(aggregate.nativeFunction, r);
  equal(110, result.number);
  equal(22, result.extra);
  equal(10, r.number);
  listener = NativeCallable<ListenerCallback>.listener((int value) {
    gcNow();
    ++received;
    receivedSum += value;
  });
  callbackObjects.add(listener!);
  recordListener =
      NativeCallable<RecordListener>.listener((PluginRecord value) {
    gcNow();
    equal(21, value.number);
    equal(2.5, value.fraction);
    equal(31, value.extra);
    ++payloads;
  });
  callbackObjects.add(recordListener!);
  final post = lib.lookupFunction<
      Void Function(Pointer<NativeFunction<ListenerCallback>>, Int64),
      void Function(
          Pointer<NativeFunction<ListenerCallback>>, int)>('FullPost');
  final postRecord = lib.lookupFunction<
      Void Function(Pointer<NativeFunction<RecordListener>>),
      void Function(Pointer<NativeFunction<RecordListener>>)>('FullPostRecord');
  post(listener!.nativeFunction, 7);
  postRecord(recordListener!.nativeFunction);
  // Capture only deep-immutable scalars in group-bound closures.
  final int amount = Plugin().read() + 7;
  group = createImmutableGroupCallback(amount);
  callbackObjects.add(group!);
  for (var i = 0; i < poolSize; ++i) {
    final int index = i;
    pool.add(NativeCallable<ScalarCallback>.isolateLocal(
        (int a, int b) => index + a + b,
        exceptionalReturn: -7006));
  }
  for (var i = 0; i < pool.length; ++i) {
    equal(i + 8, callScalar(pool[i].nativeFunction, 3, 5));
  }
  return <int>[
    staticPointer.address,
    localStatic.nativeFunction.address,
    local.nativeFunction.address,
    aggregate.nativeFunction.address,
    listener!.nativeFunction.address,
    group!.nativeFunction.address,
    recordListener!.nativeFunction.address,
    for (final callback in pool) callback.nativeFunction.address
  ];
}

// Separate checkpoint: no later callback allocation can reuse this slot before
// the embedder checks its dead metadata and restored registration counts.
@pragma('vm:entry-point')
int runSelfClose() {
  late NativeCallable<ScalarCallback> self;
  self = NativeCallable<ScalarCallback>.isolateLocal((int a, int b) {
    self.close();
    gcNow();
    return a + b;
  }, exceptionalReturn: -7004);
  final pointer = self.nativeFunction;
  equal(12, callScalar(pointer, 7, 5));
  equal(false, self.keepIsolateAlive);
  return pointer.address;
}

@pragma('vm:entry-point')
int listenerDone() => received == 3 && payloads == 1 ? 1 : 0;
@pragma('vm:entry-point')
void finishCallbacks() {
  equal(208, receivedSum);
  equal(1, payloads);
  // Re-entering the owner after foreign group calls must retain its state.
  equal(30, Plugin().read());
  equal(1, gcNow());
  for (final c in callbackObjects) {
    c.close();
    equal(false, c.keepIsolateAlive);
  }
  for (final c in pool) {
    c.close();
    equal(false, c.keepIsolateAlive);
  }
  callbackObjects.clear();
  pool.clear();
  listener = null;
  recordListener = null;
  group = null;
}

@pragma('vm:entry-point')
int callbackOwnerCount() =>
    callbackObjects.length +
    pool.length +
    (listener == null ? 0 : 1) +
    (recordListener == null ? 0 : 1) +
    (group == null ? 0 : 1);

// Inert metadata sources for the admission mutation generator. These ordinary
// Dart bodies are never called or marked native; mutations copy only their
// evaluated annotations onto a genuine lowered FFI declaration.
@pragma('vm:ffi:native', 42)
int fullFfiBadOption(int value) => value;

@pragma('vm:ffi:native', Native<int>(symbol: 'scalar', assetId: 'asset'))
int fullFfiBadSignature(int value) => value;

class FakeNative<T> {
  final String symbol;
  final String assetId;
  final bool isLeaf;
  const FakeNative({
    this.symbol = 'scalar',
    this.assetId = 'asset',
    this.isLeaf = false,
  });
}

@pragma('vm:ffi:native', FakeNative<Int32 Function(Int32)>())
int fullFfiBadIdentity(int value) => value;
