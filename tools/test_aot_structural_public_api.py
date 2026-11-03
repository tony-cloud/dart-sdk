#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Structural patches that use public `dart:` APIs implemented by private classes.

The structural profile does not annotate private `dart:` classes and members
(see docs/shorebird-structural-patching.md, "Retention rule for dart:
libraries"). This test checks that everything a patch can still express keeps
working although the base program never uses it:

* List/Map/Set/String/typed_data operations, iteration, spreads and literals;
* dynamic invocations on objects whose runtime class is private;
* tear-offs of public methods of private-class instances;
* Future/Stream/async/async*/sync* basics, enums, records, patterns and other
  language features that need private SDK helpers;
* and that a patch which needs a removed private member (private `dart:` class
  or top-level) cannot be expressed: the bytecode compiler rejects it.

Expected results are computed by running the very same patch source on the host
JIT VM, so the AOT interpreter result must be identical.
Requires the same artifacts as test_aot_structural_patch.py. Use --product for
the PRODUCT artifacts.
"""

import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import tempfile


BASE = r"""
int collectionsCheck() => 1;
int stringsCheck() => 1;
int typedDataCheck() => 1;
int dynamicCheck() => 1;
int tearOffCheck() => 1;
int featuresCheck() => 1;
int miscCheck() => 1;
void startAsync() {}
int readAsync() => -1;
String readAsyncLog() => '';
"""

UPDATED = r"""
import 'dart:async';
import 'dart:collection';
import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

int mix(int h, Object? v) => (h * 31 + v.toString().hashCode) & 0x3fffffff;

int collectionsCheck() {
  var h = 7;
  final l = <int>[];
  for (var i = 0; i < 20; i++) l.add(i * 3 % 7);
  l.addAll([9, 8]);
  l.insert(2, 100);
  l.removeAt(0);
  l.removeLast();
  l.sort();
  h = mix(h, l);
  h = mix(h, l.reversed.toList());
  h = mix(h, l.sublist(2, 6));
  h = mix(h, l.indexOf(9));
  h = mix(h, l.where((x) => x.isEven).map((x) => x * 2).toList());
  h = mix(h, l.fold<int>(0, (a, b) => a + b));
  h = mix(h, l.any((x) => x > 50));
  h = mix(h, l.every((x) => x >= 0));
  h = mix(h, List<int>.filled(3, 4));
  h = mix(h, List<int>.generate(5, (i) => i * i));
  h = mix(h, List<int>.unmodifiable(l.take(3)));
  h = mix(h, l.toList(growable: false).length);
  h = mix(h, [...l.take(2), if (l.length > 3) 5, for (var i in [1, 2]) i]);
  final m = <String, int>{};
  m['a'] = 1;
  m.putIfAbsent('b', () => 2);
  m.update('a', (v) => v + 10);
  m.addAll({'c': 3, 'd': 4});
  m.remove('d');
  h = mix(h, m.keys.toList());
  h = mix(h, m.values.toList());
  h = mix(h, m.entries.map((e) => '${e.key}${e.value}').join(','));
  h = mix(h, m.containsKey('c'));
  m.forEach((k, v) => h = mix(h, '$k:$v'));
  final s = <int>{1, 2, 3};
  s.add(4);
  h = mix(h, s.union({9}).toList());
  h = mix(h, s.intersection({2, 3, 7}).toList());
  h = mix(h, s.difference({1}).toList());
  h = mix(h, s.contains(3));
  for (final x in s) h = mix(h, x);
  h = mix(h, [1, 2, 3].expand((x) => [x, x]).skip(1).followedBy([7]).join('-'));
  final lm = <int, List<int>>{};
  lm.putIfAbsent(1, () => []).add(5);
  h = mix(h, lm);
  h = mix(h, const [1, 2, 3].length + const {'a': 1}.length + const {1, 2}.length);
  final hm = HashMap<String, int>()..['q'] = 1;
  h = mix(h, hm['q']);
  final lq = ListQueue<int>()..addAll([1, 2])..addFirst(0);
  h = mix(h, lq.removeLast() + lq.length);
  final sm = SplayTreeMap<int, String>()..[3] = 'c'..[1] = 'a';
  h = mix(h, sm.keys.toList());
  final lhs = LinkedHashSet<int>.identity()..addAll([3, 1]);
  h = mix(h, lhs.toList());
  h = mix(h, (Queue<int>()..add(1)).first);
  h = mix(h, UnmodifiableListView([1, 2]).length);
  return h;
}

int stringsCheck() {
  var h = 3;
  const name = 'Wörld';
  h = mix(h, 'Hello, $name! ${1 + 2} ${[1, 2]}');
  h = mix(h, 'a,b,,c'.split(','));
  h = mix(h, 'abcdef'.substring(2, 4));
  h = mix(h, 'abc'.codeUnits);
  h = mix(h, 'abc'.toUpperCase() + 'DEF'.toLowerCase());
  h = mix(h, '7'.padLeft(3, '0'));
  h = mix(h, 'aXbXc'.replaceAll('X', '--'));
  h = mix(h, 'abc'.compareTo('abd'));
  final sb = StringBuffer()..write('x')..writeAll([1, 2], ':')..writeCharCode(65);
  h = mix(h, sb.toString());
  h = mix(h, String.fromCharCodes([72, 105]));
  h = mix(h, 'héllo'.runes.toList());
  h = mix(h, int.parse('1234') + double.parse('2.5').toInt());
  h = mix(h, 3.14159.toStringAsFixed(2));
  h = mix(h, 255.toRadixString(16));
  h = mix(h, RegExp(r'(\d+)-(\d+)').firstMatch('ab 12-34 cd')!.group(2));
  h = mix(h, 'abc'.contains('bc') && 'abc'.startsWith('a') && 'abc'.endsWith('c'));
  h = mix(h, 'a b  c'.trim().split(RegExp(r'\s+')));
  h = mix(h, BigInt.from(1) << 70);
  h = mix(h, Uri.parse('https://x.y/z?q=1').queryParameters);
  h = mix(h, 'abc'.hashCode == 'abc'.hashCode);
  return h;
}

int typedDataCheck() {
  var h = 5;
  final u8 = Uint8List(8);
  for (var i = 0; i < u8.length; i++) u8[i] = i * 40;
  h = mix(h, u8);
  h = mix(h, u8.sublist(2, 5));
  h = mix(h, Uint8List.fromList([1, 2, 3]).fold<int>(0, (a, b) => a + b));
  final i32 = Int32List(4)..setAll(0, [5, -6, 7, 8]);
  h = mix(h, i32.indexOf(7));
  h = mix(h, (i32.toList()..sort()));
  final f64 = Float64List.fromList([1.5, 2.5, 3.5]);
  h = mix(h, f64.map((x) => x * 2).toList());
  final f32 = Float32List(3)..[0] = 1.25;
  h = mix(h, f32);
  final bd = ByteData(8)..setInt32(0, 0x01020304, Endian.little);
  h = mix(h, bd.getUint8(0));
  h = mix(h, bd.getInt32(0, Endian.big));
  h = mix(h, u8.buffer.asUint16List(0, 2));
  h = mix(h, Uint8List.sublistView(u8, 1, 3));
  final u16 = Uint16List(3)..setRange(0, 2, [7, 8]);
  h = mix(h, u16.reversed.toList());
  final i64 = Int64List(2)..[1] = 1 << 40;
  h = mix(h, i64);
  for (final x in u16) h = mix(h, x);
  h = mix(h, Uint8ClampedList(2)..[0] = 300);
  return h;
}

int dynamicCheck() {
  var h = 11;
  dynamic l = <int>[];
  l.add(3);
  l.addAll([4, 5]);
  h = mix(h, l.length);
  h = mix(h, l[1]);
  h = mix(h, l.first);
  h = mix(h, l.contains(5));
  h = mix(h, l.map((x) => x + 1).toList());
  h = mix(h, l.runtimeType.toString().contains('List'));
  dynamic m = <String, int>{};
  m['a'] = 1;
  h = mix(h, m.keys.length);
  h = mix(h, m.containsKey('a'));
  h = mix(h, m.putIfAbsent('z', () => 26));
  dynamic s = 'abc';
  h = mix(h, s.length);
  h = mix(h, s.toUpperCase());
  h = mix(h, s + 'd');
  h = mix(h, s.hashCode == 'abc'.hashCode);
  dynamic t = Uint8List(3);
  t[0] = 9;
  h = mix(h, t.length);
  h = mix(h, t[0]);
  h = mix(h, t.sublist(0, 2));
  dynamic i = 5;
  h = mix(h, i.toString());
  h = mix(h, i + 1);
  h = mix(h, i.isEven);
  dynamic d = 2.5;
  h = mix(h, d.floor());
  h = mix(h, d.toStringAsFixed(1));
  dynamic st = <int>{1};
  h = mix(h, st.add(2));
  h = mix(h, st.length);
  dynamic it = [1, 2, 3].map((x) => x * 2);
  h = mix(h, it.length);
  h = mix(h, it.toList());
  dynamic fn = (int x) => x + 1;
  h = mix(h, fn(2));
  h = mix(h, Function.apply(fn, [4]));
  try {
    dynamic o = <int>[];
    o.noSuchThing();
    h = mix(h, 'no');
  } on NoSuchMethodError {
    h = mix(h, 'nsm');
  }
  try {
    dynamic o = 1;
    o.length;
    h = mix(h, 'no');
  } on NoSuchMethodError {
    h = mix(h, 'nsm2');
  }
  return h;
}

int tearOffCheck() {
  var h = 13;
  final list = <int>[1, 2];
  final add = list.add;
  add(3);
  final addAll = list.addAll;
  addAll([4]);
  h = mix(h, list);
  final upper = 'abc'.toUpperCase;
  h = mix(h, upper());
  final map = <String, int>{};
  final put = map.putIfAbsent;
  h = mix(h, put('k', () => 5));
  final bytes = Uint8List(4);
  final sub = bytes.sublist;
  h = mix(h, sub(1).length);
  final contains = list.contains;
  h = mix(h, [1, 9].where(contains).toList());
  h = mix(h, ['a', 'b'].map('xyz'.contains).toList());
  h = mix(h, list.map(list.indexOf).toList());
  dynamic dl = list;
  final dyAdd = dl.add;
  dyAdd(8);
  h = mix(h, list.length);
  final len = Function.apply(list.add, [9]);
  h = mix(h, len);
  h = mix(h, list.removeLast);
  final set = <int>{};
  final sadd = set.add;
  [1, 1, 2].forEach(sadd);
  h = mix(h, set);
  h = mix(h, (1.5).floor.runtimeType.toString().length > 0);
  return h;
}

enum Shade { light, dark, mid }

class Box<T> {
  final T value;
  Box(this.value);
  @override
  String toString() => 'Box($value)';
}

extension on int {
  int twice() => this * 2;
}

Iterable<int> gen(int n) sync* {
  for (var i = 0; i < n; i++) yield i * i;
}

int featuresCheck() {
  var h = 17;
  late final int lateLocal;
  lateLocal = 5;
  h = mix(h, lateLocal);
  h = mix(h, Shade.values.map((s) => s.name).toList());
  h = mix(h, Shade.dark.index);
  h = mix(h, Shade.mid.toString());
  h = mix(h, Box<int>(3));
  h = mix(h, Box<List<int>>([1]).value is List<int>);
  h = mix(h, 4.twice());
  h = mix(h, gen(5).toList());
  final (a, b) = (1, 'two');
  h = mix(h, '$a$b');
  h = mix(h, switch (a) { 1 => 'one', _ => 'other' });
  h = mix(h, ((int, String) r) => r.$1);
  final rec = (x: 1, y: 2);
  h = mix(h, rec.x + rec.y);
  h = mix(h, rec == (x: 1, y: 2));
  try {
    throw StateError('boom');
  } on StateError catch (e, st) {
    h = mix(h, e.message);
    h = mix(h, st.toString().isNotEmpty);
  } finally {
    h = mix(h, 'fin');
  }
  try {
    [1].elementAt(5);
  } on RangeError {
    h = mix(h, 'range');
  }
  try {
    int.parse('x');
  } on FormatException {
    h = mix(h, 'format');
  }
  try {
    Object o = 'a';
    o as int;
  } on TypeError {
    h = mix(h, 'type');
  }
  Object? n;
  h = mix(h, n?.toString());
  h = mix(h, [3, 1, 2]..sort((x, y) => y.compareTo(x)));
  h = mix(h, identical(const [1], const [1]));
  h = mix(h, <int>[] is List<num>);
  h = mix(h, <Object>[1, 'a'].whereType<String>().toList());
  void Function<T>(T) generic = <T>(T v) => h = mix(h, v);
  generic<int>(1);
  generic<String>('s');
  h = mix(h, Duration(seconds: 90).inMinutes);
  h = mix(h, const Duration(seconds: 3).inSeconds);
  h = mix(h, identical(Endian.host, Endian.little) || Endian.host == Endian.big);
  h = mix(h, DateTime.utc(2020, 1, 2).weekday);
  assert(h != 0);
  return h;
}

int miscCheck() {
  var h = 23;
  h = mix(h, jsonEncode({'a': [1, 2.5, null, true], 'b': 'x'}));
  h = mix(h, (jsonDecode('{"k":[1,{"z":2}],"s":"t"}') as Map)['k']);
  h = mix(h, utf8.encode('héllo €'));
  h = mix(h, utf8.decode([104, 195, 169]));
  h = mix(h, base64Encode([1, 2, 3, 250]));
  h = mix(h, base64Decode('AQID+g=='));
  h = mix(h, math.Random(42).nextInt(1000));
  h = mix(h, math.max(3, 9) + math.min(2, 8) + math.sqrt(16).toInt());
  h = mix(h, math.pow(2, 10));
  final sw = Stopwatch()..start();
  h = mix(h, sw.elapsedTicks >= 0);
  final e = Expando<int>('e');
  final key = Object();
  e[key] = 5;
  h = mix(h, e[key]);
  h = mix(h, Symbol('foo') == const Symbol('foo'));
  h = mix(h, runZoned(() => Zone.current.runUnary((int x) => x + 1, 4)));
  h = mix(h, <int, String>{1: 'a'}.map((k, v) => MapEntry(v, k)));
  h = mix(h, Map.fromEntries([MapEntry(1, 2)]));
  h = mix(h, List.generate(3, (i) => i).asMap());
  h = mix(h, Iterable.generate(4, (i) => i * 2).toList());
  h = mix(h, Object.hash(1, 2, 3) == Object.hash(1, 2, 3));
  h = mix(h, (List<int>.generate(50, (i) => (i * 37) % 50)..sort()).last);
  h = mix(h, 'x'.runtimeType == String);
  h = mix(h, <int>[].runtimeType.toString());
  h = mix(h, int.tryParse('z'));
  h = mix(h, (-7) ~/ 2 + (-7) % 3 + 7.remainder(3));
  h = mix(h, 1e20.toInt() > 0 && double.nan.isNaN);
  h = mix(h, 0x7fffffffffffffff + 1 < 0);
  h = mix(h, DateTime.fromMillisecondsSinceEpoch(0, isUtc: true).toIso8601String());
  h = mix(h, Uri.encodeComponent('a b&c'));
  h = mix(h, LineSplitter.split('a\nb\r\nc').toList());
  h = mix(h, StringBuffer('x').length);
  h = mix(h, Error.safeToString(null));
  h = mix(h, identityHashCode(key) == identityHashCode(key));
  h = mix(h, [1, 2, 3].reduce((a, b) => a + b));
  h = mix(h, ['b', 'a'].toList()..sort());
  h = mix(h, Duration(milliseconds: 1500).toString());
  h = mix(h, {1, 2}.toString());
  h = mix(h, 12345.6789.toStringAsExponential(2));
  h = mix(h, Uint8List.fromList([1, 2]).buffer.asByteData().getUint16(0));
  return h;
}

int asyncValue = -1;
List<String> asyncLog = [];

Future<int> twice(int x) async => x * 2;

Stream<int> countTo(int n) async* {
  for (var i = 1; i <= n; i++) {
    yield i;
  }
}

Future<void> runAsync() async {
  var h = 19;
  h = mix(h, await twice(4));
  h = mix(h, await Future.value(5));
  h = mix(h, await Future.microtask(() => 6));
  h = mix(h, await Future.wait([twice(1), twice(2)]));
  h = mix(h, await Future.value(1).then((v) => v + 1));
  h = mix(h, await Future<int>.error('e').catchError((Object e) => 7));
  final c = Completer<int>();
  scheduleMicrotask(() => c.complete(8));
  h = mix(h, await c.future);
  var sum = 0;
  await for (final v in countTo(4)) sum += v;
  h = mix(h, sum);
  h = mix(h, await Stream.fromIterable([1, 2, 3]).map((x) => x * 3).toList());
  final sc = StreamController<int>();
  final got = <int>[];
  sc.stream.listen(got.add);
  sc.add(1);
  sc.add(2);
  await sc.close();
  h = mix(h, got);
  h = mix(h, await Stream.fromIterable([1, 2, 3]).fold<int>(0, (a, b) => a + b));
  try {
    await Future<void>.error(StateError('x'));
  } on StateError {
    h = mix(h, 'caught');
  } finally {
    h = mix(h, 'finally');
  }
  asyncValue = h;
}

void startAsync() {
  runAsync().catchError((Object e, StackTrace s) {
    asyncValue = -2;
    asyncLog.add('$e\n$s');
  });
}

int readAsync() => asyncValue;
String readAsyncLog() => asyncLog.join('\n');
"""

DRIVER = r"""
import 'dart:collection';
import 'dart:convert';
import 'dart:math';
import 'body.dart';
@pragma('vm:entry-point') int probeCollections() => collectionsCheck();
@pragma('vm:entry-point') int probeStrings() => stringsCheck();
@pragma('vm:entry-point') int probeTypedData() => typedDataCheck();
@pragma('vm:entry-point') int probeDynamic() => dynamicCheck();
@pragma('vm:entry-point') int probeTearOff() => tearOffCheck();
@pragma('vm:entry-point') int probeFeatures() => featuresCheck();
@pragma('vm:entry-point') int probeMisc() => miscCheck();
@pragma('vm:entry-point') void probeStartAsync() => startAsync();
@pragma('vm:entry-point') int probeAsync() => readAsync();
@pragma('vm:entry-point') String probeAsyncLog() => readAsyncLog();
void main() {}
"""

RUNNER = r"""
import 'body.dart';
Future<void> main() async {
  print('probeCollections ${collectionsCheck()}');
  print('probeStrings ${stringsCheck()}');
  print('probeTypedData ${typedDataCheck()}');
  print('probeDynamic ${dynamicCheck()}');
  print('probeTearOff ${tearOffCheck()}');
  print('probeFeatures ${featuresCheck()}');
  print('probeMisc ${miscCheck()}');
  startAsync();
  await Future<void>.delayed(const Duration(milliseconds: 50));
  print('probeAsync ${readAsync()}');
}
"""

# Patches that need a private declaration of a dart: library. They must fail to
# compile: patches cannot add code to dart: libraries or name their privates.
REMOVED_PRIVATE_PATCHES = {
    'private-class': (
        'int entry() { final l = _GrowableList<int>(0); return l.length; }\n'),
    'private-static': (
        'int entry() => _StringBase._interpolateSingle(1).length;\n'),
    'private-toplevel': (
        'int entry() { _checkLoaded; return 1; }\n'),
    'private-member': (
        "int entry() { final l = <int>[]; return (l as dynamic)"
        "._growToNextCapacity == null ? 1 : 0; }\n"
        "extension on List<int> { int bad() => this._growToNextCapacity(); }\n"),
    'internal-library': (
        "import 'dart:_internal';\nint entry() => 1;\n"),
}

PROBES = ('Collections', 'Strings', 'TypedData', 'Dynamic', 'TearOff',
          'Features', 'Misc')

HARNESS = r"""
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "include/dart_api.h"
#include "include/dart_engine.h"

void Check(Dart_Handle value) {
  if (Dart_IsError(value)) {
    std::cerr << Dart_GetError(value) << std::endl;
    std::exit(1);
  }
}

int64_t Probe(const char* name) {
  auto value = Dart_Invoke(Dart_RootLibrary(), Dart_NewStringFromCString(name),
                           0, nullptr);
  Check(value);
  int64_t result = 0;
  Check(Dart_IntegerToInt64(value, &result));
  return result;
}

const char* configured_script_uri = nullptr;

Dart_Handle SetupCoreHooks() {
  auto isolate = Dart_LookupLibrary(Dart_NewStringFromCString("dart:isolate"));
  if (Dart_IsError(isolate)) return isolate;
  auto async = Dart_LookupLibrary(Dart_NewStringFromCString("dart:async"));
  if (Dart_IsError(async)) return async;
  auto closure = Dart_Invoke(isolate,
      Dart_NewStringFromCString("_getIsolateScheduleImmediateClosure"), 0, nullptr);
  if (Dart_IsError(closure)) return closure;
  auto result = Dart_Invoke(async,
      Dart_NewStringFromCString("_setScheduleImmediateClosure"), 1, &closure);
  if (Dart_IsError(result)) return result;
  return Dart_Invoke(isolate, Dart_NewStringFromCString("_setupHooks"), 0, nullptr);
}

bool InitializeChild(void** child_data, char** error) {
  *child_data = nullptr;
  return true;
}

int main(int argc, char** argv) {
  // harness <snapshot> <patch-or-"none"> <probes.txt>: lines "name expected".
  if (argc != 4 || !Dart_IsPrecompiledRuntime()) return 2;
  char* error = nullptr;
  if (!DartEngine_Init(&error)) {
    std::cerr << (error == nullptr ? "Embedder initialization failed" : error)
              << std::endl;
    return 1;
  }
  const auto snapshot = DartEngine_AotSnapshotFromFile(argv[1], &error);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  configured_script_uri = snapshot.script_uri;
  Dart_InitializeParams params;
  std::memset(&params, 0, sizeof(params));
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  params.initialize_isolate = InitializeChild;
  error = Dart_Initialize(&params);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  auto isolate = Dart_CreateIsolateGroup(snapshot.script_uri, "public-api",
      snapshot.snapshot_data, snapshot.snapshot_text, &flags, nullptr, nullptr,
      &error);
  if (error != nullptr || isolate == nullptr) {
    std::cerr << (error == nullptr ? "No root isolate" : error) << std::endl;
    return 1;
  }
  Dart_EnterScope();
  Check(SetupCoreHooks());
  if (std::strcmp(argv[2], "none") != 0) {
    std::ifstream input(argv[2], std::ios::binary);
    std::vector<uint8_t> patch((std::istreambuf_iterator<char>(input)), {});
    Check(Dart_ReloadBytecodePatch(patch.data(), patch.size()));
  }
  Dart_ExitScope();
  Dart_ExitIsolate();
  error = Dart_IsolateMakeRunnable(isolate);
  if (error != nullptr) { std::cerr << error << std::endl; return 1; }
  Dart_EnterIsolate(isolate);
  Dart_EnterScope();
  std::ifstream probes(argv[3]);
  std::string name;
  long long expected;
  int failures = 0;
  while (probes >> name >> expected) {
    if (name == "probeAsync") {
      Check(Dart_Invoke(Dart_RootLibrary(),
                        Dart_NewStringFromCString("probeStartAsync"), 0, nullptr));
      // There is no event loop in this embedding: drain the microtask queue
      // the way the VM does after each handled message.
      auto isolate_lib =
          Dart_LookupLibrary(Dart_NewStringFromCString("dart:isolate"));
      Check(isolate_lib);
      for (int i = 0; i < 1000; ++i) {
        Check(Dart_Invoke(isolate_lib,
            Dart_NewStringFromCString("_runPendingImmediateCallback"), 0,
            nullptr));
      }
    }
    const auto actual = Probe(name.c_str());
    if (name == "probeAsync" && actual < 0) {
      auto log = Dart_Invoke(Dart_RootLibrary(),
          Dart_NewStringFromCString("probeAsyncLog"), 0, nullptr);
      const char* text = nullptr;
      if (!Dart_IsError(log) && !Dart_IsError(Dart_StringToCString(log, &text))) {
        std::cout << "async log: " << text << std::endl;
      }
    }
    std::cout << name << ": " << actual
              << (actual == expected ? " == " : " != ") << expected << std::endl;
    if (actual != expected) ++failures;
  }
  Dart_ExitScope();
  Dart_ShutdownIsolate();
  DartEngine_Shutdown();
  Dart_Cleanup();
  return failures == 0 ? 0 : 3;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--product', action='store_true',
                        help='Use matching PRODUCT compiler, platform, and shared runtime')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    if sys.platform not in ('darwin', 'linux'):
        raise SystemExit('This native acceptance harness supports macOS/Linux hosts.')

    def run(*command, check=True, capture=False):
        return subprocess.run([str(value) for value in command], cwd=root,
                              check=check, timeout=600,
                              capture_output=capture, text=True)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else
                 tempfile.TemporaryDirectory(prefix='dart-aot-structural-api-'))
    with workspace as tmp:
        work = Path(tmp)
        dart = build / 'dart'
        platform = build / ('vm_platform_product.dill' if args.product
                            else 'vm_platform.dill')
        if not dart.is_file():
            dart = build / 'dart-sdk/bin/dart'
            if not args.product:
                platform = build / 'dart-sdk/lib/_internal/vm_platform.dill'
        snapshot_compiler = build / ('gen_snapshot_product' if args.product
                                      else 'gen_snapshot')
        runtime_name = 'dart_engine_aot_shared' + ('_product' if args.product else '')
        runtime_path = build / ('lib' + runtime_name +
                                ('.dylib' if sys.platform == 'darwin' else '.so'))
        for artifact in (dart, platform, snapshot_compiler, runtime_path):
            if not artifact.is_file():
                parser.error(f'Required artifact is missing: {artifact}')
        defines = ['-Ddart.vm.product=true'] if args.product else []
        print(f'Runtime mode: {"PRODUCT" if args.product else "non-PRODUCT"}',
              flush=True)
        bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                    *defines, f'--platform={platform}',
                    '--shorebird-structural-patching']
        packages = f'--packages={root / ".dart_tool/package_config.json"}'

        body = work / 'body.dart'
        (work / 'driver.dart').write_text(DRIVER)
        (work / 'runner.dart').write_text(RUNNER)
        harness = work / 'harness.cc'
        harness.write_text(HARNESS)

        # Expected values: the same patch source on the host JIT VM.
        body.write_text(UPDATED)
        expected = run(dart, *defines, work / 'runner.dart', capture=True).stdout
        probes = work / 'probes.txt'
        probes.write_text(expected)
        names = [line.split()[0] for line in expected.splitlines()]
        assert names == ['probe' + name for name in PROBES] + ['probeAsync'], names
        print(expected, end='', flush=True)

        # Base snapshot: the driver with trivial bodies.
        body.write_text(BASE)
        base_kernel = work / 'base.dill'
        base_snapshot = work / 'base.snapshot'
        run(dart, packages, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
            '--no-tfa', *defines, '--shorebird-structural-patching',
            f'--platform={platform}', f'--output={base_kernel}',
            work / 'driver.dart')
        if sys.platform == 'darwin':
            run(snapshot_compiler, '--snapshot-kind=app-aot-macho-dylib',
                '--shorebird-structural-patching', f'--macho={base_snapshot}',
                base_kernel)
            run('codesign', '--force', '--sign', '-', base_snapshot)
        else:
            run(snapshot_compiler, '--snapshot-kind=app-aot-elf',
                '--shorebird-structural-patching', f'--elf={base_snapshot}',
                base_kernel)
        baseline = work / 'baseline.bytecode'
        run(*bytecode, '-o', baseline, work / 'driver.dart')
        body.write_text(UPDATED)
        patch = work / 'public-api.bytecode'
        run(*bytecode, f'--patch-base={baseline}', '-o', patch,
            work / 'driver.dart')

        executable = work / 'harness'
        run(args.cxx, '-std=c++17', f'-I{root / "runtime"}',
            f'-I{root / "runtime/engine"}', harness, f'-L{build}',
            f'-l{runtime_name}', f'-Wl,-rpath,{build}', '-o', executable)
        # The unpatched base must not satisfy the probes (it returns constants).
        base_run = run(executable, base_snapshot, 'none', probes, check=False,
                       capture=True)
        if base_run.returncode == 0:
            raise AssertionError('Unpatched base unexpectedly passed the probes')
        run(executable, base_snapshot, patch, probes)
        print('Public dart: API patch probes passed.', flush=True)

        # A patch needing a removed (private) dart: member cannot be expressed.
        for name, source in REMOVED_PRIVATE_PATCHES.items():
            case = work / f'removed-{name}'
            case.mkdir(exist_ok=True)
            (case / 'main.dart').write_text(source + 'void main() {}\n')
            result = run(*bytecode, '-o', case / 'out.bytecode',
                         case / 'main.dart', check=False, capture=True)
            if result.returncode == 0:
                raise AssertionError(
                    f'Patch using a private dart: declaration compiled: {name}')
            print(f'Expected rejection ({name}): '
                  f'{(result.stdout + result.stderr).strip().splitlines()[0][:120]}',
                  flush=True)


if __name__ == '__main__':
    main()
