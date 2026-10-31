import 'dart:ffi';
import 'removed.dart';

@pragma('vm:entry-point')
class PluginConfig {
  final int seed;
  final int increment;
  const PluginConfig(this.seed, {this.increment = 3});
  int get value => seed + increment;
}

@pragma('vm:entry-point')
class Plugin {
  int old = 2;
  int obsolete = 5;
  int read() => old + obsolete;
}

final class PluginRecord extends Struct {
  @Int64()
  external int number;
  @Double()
  external double fraction;
}

@Native<Int64 Function(Int64)>(symbol: 'FullResolverValue')
external int resolvedValue(int value);
@Native<PluginRecord Function(PluginRecord)>(
    symbol: 'FullRecordV1', assetId: 'full-release-asset')
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
    ..fraction = 2.5;
  final changed = changeRecord(record);
  equal(16, sizeOf<PluginRecord>());
  equal(21, changed.number);
  equal(5.0, changed.fraction);
  equal(20, record.number);
  return Plugin().read() + removedValue();
}

@pragma('vm:entry-point')
int runNonFfi() {
  const a = PluginConfig(7);
  const b = PluginConfig(7, increment: 3);
  equal(true, identical(a, b));
  equal(10, a.value);
  equal(7, Plugin().read());
  return <int>[1, 2, 3].map((n) => n * 2).reduce((a, b) => a + b);
}
