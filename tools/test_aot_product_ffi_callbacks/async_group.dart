// The same source is loaded cold as bytecode and compiled in another snapshot.
import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';

final class AsyncPair extends Struct {
  @Int64()
  external int a;
  @Double()
  external double b;
}

final class AsyncHfa extends Struct {
  @Float()
  external double a;
  @Float()
  external double b;
  @Float()
  external double c;
}

@Packed(1)
final class AsyncPacked extends Struct {
  @Uint8()
  external int tag;
  @Uint64()
  external int value;
}

final class AsyncLarge extends Struct {
  @Int64()
  external int a;
  @Int64()
  external int b;
  @Int64()
  external int c;
}

final class AsyncNested extends Struct {
  external AsyncPair inner;
  @Array(3)
  external Array<Int64> values;
}

final class AsyncWord extends Union {
  @Uint64()
  external int bits;
  @Double()
  external double floating;
}

typedef Scalar = Void Function(Int64);
typedef Group = Int64 Function(Int64, Int64);
typedef Payload = Void Function(Int64, AsyncPair, AsyncHfa, AsyncPacked,
    AsyncLarge, AsyncNested, AsyncWord, Pointer<Int64>, Handle);
typedef GroupHandle = Handle Function(Handle);
typedef GroupLarge = AsyncLarge Function(AsyncLarge);
typedef Driver = Int64 Function(
    Pointer<NativeFunction<Group>>, Pointer<NativeFunction<Scalar>>, Int64);
typedef Context = Int64 Function(Int64, Int64);
late DynamicLibrary lib;
late NativeCallable<Scalar> scalar;
late NativeCallable<Payload> payload;
late NativeCallable<Group> groupStatic, groupCapture, groupThrow, groupOrdinary;
late NativeCallable<GroupHandle> groupHandle;
late NativeCallable<GroupLarge> groupLarge;
final groupObjects = <NativeCallable>[];
final poolGroups = <NativeCallable>[];
final listeners = <NativeCallable>[];
var received = 0,
    payloads = 0,
    sum = 0,
    zoneErrors = 0,
    expressionEvaluations = 0;
var ordinary = 91;
@Native<Int64 Function()>(symbol: 'AsyncStaticInitializer')
external int ordinaryInitializer();
late int ordinaryGetter = ordinaryInitializer();
late final int ordinarySetter;
late int groupAddress, contextAddress, driverAddress;
late int Function() gcNow;
late int Function(int, int) context;
late int pointee;
String? creator;
void equal(Object? a, Object? b) {
  if (a != b) throw StateError('$a != $b');
}

void Function(int) callbackExpression() {
  ++expressionEvaluations;
  return onScalar;
}

@pragma('vm:entry-point')
void onScalar(int value) {
  equal('listener-zone', Zone.current[#callbackZone]);
  equal(creator, Isolate.current.debugName);
  equal(1, context(1, groupAddress));
  gcNow();
  if (value == -999) throw StateError('guarded Zone error');
  ++received;
  sum += value;
  if (value == -1000) scalar.close();
}

@pragma('vm:entry-point')
void onPayload(
    int event,
    AsyncPair pair,
    AsyncHfa hfa,
    AsyncPacked packed,
    AsyncLarge large,
    AsyncNested nested,
    AsyncWord word,
    Pointer<Int64> pointer,
    Object? token) {
  equal('listener-zone', Zone.current[#callbackZone]);
  equal(creator, Isolate.current.debugName);
  equal(1, context(1, groupAddress));
  gcNow();
  equal(11, pair.a);
  equal(2.5, pair.b);
  equal(7.5, hfa.a + hfa.b + hfa.c);
  equal(7, packed.tag);
  equal(123456789, packed.value);
  equal(31, large.a);
  equal(32, large.b);
  equal(33, large.c);
  equal(41, nested.inner.a);
  equal(4.5, nested.inner.b);
  equal(156, nested.values[0] + nested.values[1] + nested.values[2]);
  equal(0x12345678, word.bits);
  equal(pointee, pointer.address);
  equal(500, pointer.value);
  equal('queued handle', (token as List)[0]);
  ++payloads;
  sum += event;
}

int staticTarget(int a, int b) => a * 3 + b;
int ordinaryTarget(int a, int b) => ordinary + a + b;
int ordinaryStoreTarget(int a, int b) {
  ordinary = a + b;
  return 1;
}

int ordinaryGetterTarget(int a, int b) => ordinaryGetter + a + b;
int ordinarySetterTarget(int a, int b) {
  ordinarySetter = a + b;
  return 1;
}

@pragma('vm:entry-point')
List<int> staticErrorPointers() {
  final callbacks = [
    NativeCallable<Group>.isolateGroupBound(ordinaryStoreTarget,
        exceptionalReturn: -1911),
    NativeCallable<Group>.isolateGroupBound(ordinaryGetterTarget,
        exceptionalReturn: -1912),
    NativeCallable<Group>.isolateGroupBound(ordinarySetterTarget,
        exceptionalReturn: -1913),
  ];
  groupObjects.addAll(callbacks);
  return [for (final callback in callbacks) callback.nativeFunction.address];
}

@pragma('vm:entry-point')
int verifyStaticOwnerAccess() {
  equal(1, ordinaryStoreTarget(20, 22));
  equal(45, ordinaryTarget(1, 2));
  equal(1237, ordinaryGetterTarget(1, 2));
  equal(1239, ordinaryGetterTarget(3, 2));
  equal(1, ordinarySetterTarget(21, 22));
  equal(43, ordinarySetter);
  return 1;
}

@pragma('vm:entry-point')
List<List<int>> prepare(String path, int ownerGroup) {
  // A runtime write prevents TFA from replacing the ordinary field read.
  ordinary = ownerGroup;
  lib = DynamicLibrary.open(path);
  groupAddress = ownerGroup;
  creator = Isolate.current.debugName;
  contextAddress =
      lib.lookup<NativeFunction<Context>>('AsyncCheckContext').address;
  driverAddress =
      lib.lookup<NativeFunction<Driver>>('AsyncNestedDriver').address;
  context = Pointer<NativeFunction<Context>>.fromAddress(contextAddress)
      .asFunction<int Function(int, int)>();
  gcNow =
      lib.lookupFunction<IntPtr Function(), int Function()>('CallbackGcNow');
  pointee = lib.lookupFunction<IntPtr Function(), int Function()>(
      'AsyncPointeeAddress')();
  // Cold first construction: no local callback/listener/group warm-up.
  runZonedGuarded(() {
    scalar = NativeCallable<Scalar>.listener(callbackExpression());
    listeners.add(scalar);
    payload = NativeCallable<Payload>.listener(onPayload);
    listeners.add(payload);
    prepareVarArgs();
  }, (error, trace) {
    ++zoneErrors;
  }, zoneValues: {#callbackZone: 'listener-zone'});
  equal(1, expressionEvaluations);
  equal(0, received);
  equal(0, payloads);
  groupStatic = NativeCallable<Group>.isolateGroupBound(staticTarget,
      exceptionalReturn: -1901);
  groupObjects.add(groupStatic);
  final int captured = 37, ctx = contextAddress, owner = ownerGroup;
  groupCapture = NativeCallable<Group>.isolateGroupBound((int a, int b) {
    final probe = Pointer<NativeFunction<Context>>.fromAddress(ctx)
        .asFunction<int Function(int, int)>();
    equal(1, probe(2, owner));
    final values = <int>[captured, a, b];
    return values.fold(0, (a, b) => a + b);
  }, exceptionalReturn: -1902);
  groupObjects.add(groupCapture);
  int groupThrowTarget(int a, int b) {
    if (captured != 37) throw StateError('capture');
    throw StateError('group exception');
  }

  groupThrow = NativeCallable<Group>.isolateGroupBound(groupThrowTarget,
      exceptionalReturn: -1903);
  groupObjects.add(groupThrow);
  groupOrdinary = NativeCallable<Group>.isolateGroupBound(ordinaryTarget,
      exceptionalReturn: -1904);
  groupObjects.add(groupOrdinary);
  groupHandle = NativeCallable<GroupHandle>.isolateGroupBound((Object? token) {
    final probe = Pointer<NativeFunction<Context>>.fromAddress(ctx)
        .asFunction<int Function(int, int)>();
    equal(1, probe(2, owner));
    return token;
  });
  groupObjects.add(groupHandle);
  groupLarge = NativeCallable<GroupLarge>.isolateGroupBound((AsyncLarge value) {
    final probe = Pointer<NativeFunction<Context>>.fromAddress(ctx)
        .asFunction<int Function(int, int)>();
    equal(1, probe(2, owner));
    value.a += captured;
    return value;
  });
  groupObjects.add(groupLarge);
  equal(false, groupCapture.keepIsolateAlive);
  var rejected = false;
  try {
    groupCapture.keepIsolateAlive = true;
  } on ArgumentError {
    rejected = true;
  }
  equal(true, rejected);
  return [
    ...listenerSpecs(),
    [groupStatic.nativeFunction.address, 1, 0, 0, 0],
    [groupCapture.nativeFunction.address, 1, 1, 0, 0],
    [groupThrow.nativeFunction.address, 1, 1, 0, 0],
    [groupOrdinary.nativeFunction.address, 1, 0, 0, 0],
    [groupHandle.nativeFunction.address, 1, 1, 1, 0],
    [groupLarge.nativeFunction.address, 1, 1, 0, 24]
  ];
}

@pragma('vm:entry-point')
int createRoute(int other, int listener) {
  final int driver = driverAddress,
      destination = other,
      port = listener,
      ctx = contextAddress,
      owner = groupAddress;
  final callback = NativeCallable<Group>.isolateGroupBound((int a, int b) {
    final call = Pointer<NativeFunction<Driver>>.fromAddress(driver).asFunction<
        int Function(Pointer<NativeFunction<Group>>,
            Pointer<NativeFunction<Scalar>>, int)>();
    final result =
        call(Pointer.fromAddress(destination), Pointer.fromAddress(port), a);
    final probe = Pointer<NativeFunction<Context>>.fromAddress(ctx)
        .asFunction<int Function(int, int)>();
    equal(1, probe(2, owner));
    return result + 1;
  }, exceptionalReturn: -1905);
  groupObjects.add(callback);
  return callback.nativeFunction.address;
}

@pragma('vm:entry-point')
List<NativeCallable> exportGroups() => groupObjects;
@pragma('vm:entry-point')
List<int> status() =>
    [received, payloads, sum, zoneErrors, varargsSeen, expressionEvaluations];
@pragma('vm:entry-point')
void rejectMutable(int which) {
  var mutable = 7;
  late final int lateCapture = 8;
  final mutableObject = <int>[9];
  var rejected = false;
  try {
    switch (which) {
      case 0:
        final c = NativeCallable<Group>.isolateGroupBound(
            (int a, int b) => ++mutable + a + b,
            exceptionalReturn: -1);
        c.close();
        break;
      case 1:
        final c = NativeCallable<Group>.isolateGroupBound(
            (int a, int b) => lateCapture + a + b,
            exceptionalReturn: -1);
        c.close();
        break;
      case 2:
        final c = NativeCallable<Group>.isolateGroupBound(
            (int a, int b) => mutableObject[0] + a + b,
            exceptionalReturn: -1);
        c.close();
        break;
    }
  } on ArgumentError {
    rejected = true;
  }
  equal(true, rejected);
}

@pragma('vm:entry-point')
void closeListeners() {
  for (final c in listeners) c.close();
  listeners.clear();
}

@pragma('vm:entry-point')
void closeExported(List<NativeCallable> callbacks) {
  for (final c in callbacks) {
    c.close();
    c.close();
  }
}

@pragma('vm:entry-point')
List<int> createPool(int count, bool group) {
  final result = <int>[];
  for (var i = 0; i < count; ++i) {
    final int captured = i;
    if (group) {
      final c = NativeCallable<Group>.isolateGroupBound(
          (int a, int b) => captured + a + b,
          exceptionalReturn: -1);
      poolGroups.add(c);
      result.add(c.nativeFunction.address);
    } else {
      final c = NativeCallable<Scalar>.listener((int value) {
        ++received;
        sum += value + captured;
      });
      listeners.add(c);
      result.add(c.nativeFunction.address);
    }
  }
  return result;
}

void main() {}

typedef Empty = Void Function(Int64, VarArgs<()>);
typedef Single = Void Function(Int64, VarArgs<(Int32,)>);
typedef Multiple = Void Function(Int64, VarArgs<(Int32, Double, Int64)>);
typedef Spilled = Void Function(
    Int64,
    VarArgs<
        (
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double,
          Int64,
          Double
        )>);
typedef Compound = Void Function(Int64, VarArgs<(AsyncPair, AsyncLarge)>);
late NativeCallable<Empty> empty;
late NativeCallable<Single> single;
late NativeCallable<Multiple> multiple;
late NativeCallable<Spilled> spilled;
late NativeCallable<Compound> compound;
var varargsSeen = 0;
void varargDelivery() {
  equal('listener-zone', Zone.current[#callbackZone]);
  equal(1, context(1, groupAddress));
  gcNow();
  ++varargsSeen;
}

void optionalTarget(int a, [int extra = 17]) {
  equal(1, a);
  equal(17, extra);
  varargDelivery();
}

void prepareVarArgs() {
  empty = NativeCallable<Empty>.listener(optionalTarget);
  listeners.add(empty);
  single = NativeCallable<Single>.listener((Object? a, Object? value) {
    equal(2, a);
    equal(7, value);
    varargDelivery();
  });
  listeners.add(single);
  multiple = NativeCallable<Multiple>.listener((int a, int b, double c, int d) {
    equal(3, a);
    equal(-9, b);
    equal(2.5, c);
    equal(10000000000, d);
    varargDelivery();
  });
  listeners.add(multiple);
  spilled = NativeCallable<Spilled>.listener((int a,
      int i1,
      double d1,
      int i2,
      double d2,
      int i3,
      double d3,
      int i4,
      double d4,
      int i5,
      double d5,
      int i6,
      double d6,
      int i7,
      double d7,
      int i8,
      double d8,
      int i9,
      double d9) {
    equal(4, a);
    equal(
        94.5,
        i1 +
            i2 +
            i3 +
            i4 +
            i5 +
            i6 +
            i7 +
            i8 +
            i9 +
            d1 +
            d2 +
            d3 +
            d4 +
            d5 +
            d6 +
            d7 +
            d8 +
            d9);
    varargDelivery();
  });
  listeners.add(spilled);
  compound =
      NativeCallable<Compound>.listener((int a, AsyncPair p, AsyncLarge l) {
    equal(5, a);
    equal(11, p.a);
    equal(2.5, p.b);
    equal(96, l.a + l.b + l.c);
    varargDelivery();
  });
  listeners.add(compound);
}

List<List<int>> listenerSpecs() => [
      for (final c in listeners) [c.nativeFunction.address, 2, 0, 0, 0]
    ];
@pragma('vm:entry-point')
List<int> varargsPointers() => [
      empty.nativeFunction.address,
      single.nativeFunction.address,
      multiple.nativeFunction.address,
      spilled.nativeFunction.address,
      compound.nativeFunction.address
    ];

@AbiSpecificIntegerMapping({Abi.fuchsiaArm64: Int8()})
final class MissingMapping extends AbiSpecificInteger {
  const MissingMapping();
}

typedef Missing = Void Function(MissingMapping);
@pragma('vm:entry-point')
void rejectMissingListener() {
  var caught = false;
  try {
    final c = NativeCallable<Missing>.listener((int value) {});
    c.close();
  } on ArgumentError {
    caught = true;
  }
  equal(true, caught);
}

@pragma('vm:entry-point')
void closePoolGroups() {
  for (final c in poolGroups) c.close();
  poolGroups.clear();
}

late NativeCallable<Group> selfGroup;
@pragma('vm:entry-point')
List<Object> createSelfClose() {
  final int close = lib
      .lookup<NativeFunction<Void Function()>>('AsyncGroupSelfClose')
      .address;
  final int ctx = contextAddress, owner = groupAddress, capture = 37;
  selfGroup = NativeCallable<Group>.isolateGroupBound((int a, int b) {
    Pointer<NativeFunction<Void Function()>>.fromAddress(close)
        .asFunction<void Function()>()();
    final probe = Pointer<NativeFunction<Context>>.fromAddress(ctx)
        .asFunction<int Function(int, int)>();
    equal(1, probe(2, owner));
    return <int>[a, b, capture].fold(0, (a, b) => a + b);
  }, exceptionalReturn: -1906);
  groupObjects.add(selfGroup);
  return [selfGroup.nativeFunction.address, selfGroup];
}

@pragma('vm:entry-point')
void closeOne(NativeCallable callback) {
  callback.close();
}

@pragma('vm:entry-point')
void checkSelfClosed() {
  var rejected = false;
  try {
    selfGroup.nativeFunction;
  } on StateError {
    rejected = true;
  }
  equal(true, rejected);
  selfGroup.close();
}

@pragma('vm:entry-point')
int abandonListener() {
  final c = NativeCallable<Scalar>.listener((int value) {
    received += value;
  });
  listeners.add(c);
  return c.nativeFunction.address;
}
