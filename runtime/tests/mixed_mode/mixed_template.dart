// Mixed-mode spike program. gen.sh produces base.dart (K = 3) and
// patch.dart (K = 5) by replacing the token __K__. Every edit is a same-size
// change of an immediate, so the two compiles keep one layout and the
// runtime's identity link table can link everything that did not change.

import 'dart:async';
import 'dart:typed_data';

// ---------------------------------------------------------------- linked code
@pragma('vm:never-inline')
int linkedHelper(int x) => x * 2 + 1;

@pragma('vm:never-inline')
int linkedThrower(int x) {
  if (x > 2) throw StateError('native throw $x');
  return x;
}

@pragma('vm:never-inline')
int linkedCatcher(int Function(int) f, int n) {
  int c = 0;
  for (int i = 0; i < n; i++) {
    try {
      c += f(i);
    } on ArgumentError {
      c += 1000;
    }
  }
  return c;
}

@pragma('vm:never-inline')
int linkedPassThrough(int Function(int) f, int x) => f(x) + 1;

@pragma('vm:never-inline')
int applyN(int Function(int) f, int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += f(i);
  }
  return s;
}

@pragma('vm:never-inline')
List<Object> linkedAlloc(int n) => List<Object>.generate(n, (i) => [i, i + 1]);

abstract class Shape {
  int area();
}

class Sq extends Shape {
  final int s;
  Sq(this.s);
  @override
  @pragma('vm:never-inline')
  int area() => s * s;
}

class Rect extends Shape {
  final int w, h;
  Rect(this.w, this.h);
  @override
  @pragma('vm:never-inline')
  int area() => w * h;
}

class Circ extends Shape {
  final int r;
  Circ(this.r);
  @override
  @pragma('vm:never-inline')
  int area() => r * r + __K__; // changed
}

class Tri extends Shape {
  final int b, h;
  Tri(this.b, this.h);
  @override
  @pragma('vm:never-inline')
  int area() => (b * h) >> 1;
}

@pragma('vm:never-inline')
int sumAreas(List<Shape> shapes) {
  int t = 0;
  for (final s in shapes) {
    t += s.area(); // dispatch-table call from native code
  }
  return t;
}

@pragma('vm:never-inline')
int tiny(int x) => x + 1;

// --------------------------------------------------------------- changed code
@pragma('vm:never-inline')
int changedLeaf(int x) => x + __K__;

@pragma('vm:never-inline')
int changedCaller(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s += linkedHelper(i) + __K__; // simulated -> native
  }
  return s;
}

@pragma('vm:never-inline')
int changedThrower(int x) {
  if (x > __K__) throw ArgumentError('simulated throw $x');
  return x;
}

@pragma('vm:never-inline')
int changedCatcher(int n) {
  int c = 0;
  for (int i = 0; i < n; i++) {
    try {
      c += linkedThrower(i); // thrown in a native frame, caught here
    } on StateError {
      c += __K__;
    }
  }
  return c;
}

@pragma('vm:never-inline')
int changedOuter(int n) {
  // simulated -> native (linkedPassThrough) -> simulated (changedThrower)
  // throws; the handler is in this simulated frame.
  int c = 0;
  for (int i = 0; i < n; i++) {
    try {
      c += linkedPassThrough(changedThrower, i);
    } on ArgumentError {
      c += 100 + __K__;
    }
  }
  return c;
}

@pragma('vm:never-inline')
int changedGc(int rounds) {
  final keep = <List<int>>[];
  final local = Uint8List(16)..[0] = __K__;
  int total = 0;
  for (int r = 0; r < rounds; r++) {
    final tmp = linkedAlloc(2000); // allocates (and scavenges) natively
    final mine = List<int>.filled(100, r + __K__); // allocates while simulated
    keep.add(mine);
    total += tmp.length + mine[50] + local[0];
  }
  for (int r = 0; r < rounds; r++) {
    if (keep[r][99] != r + __K__) throw StateError('corrupt $r');
  }
  return total;
}

@pragma('vm:never-inline')
Future<int> changedAsync(int n) async {
  int s = 0;
  for (int i = 0; i < n; i++) {
    await Future<void>.delayed(Duration.zero);
    s += i + __K__;
  }
  return s;
}

// ------------------------------------------------------------------ benchmarks
// The B variants are changed in the patch (simulated); the N variants are
// identical bodies that stay linked (native) in the patch.
@pragma('vm:never-inline')
int numLoopB(int n) {
  int a = 1, b = 0;
  for (int i = 0; i < n; i++) {
    a = (a * 31 + i) & 0xffffff;
    b ^= a;
  }
  return b + __K__;
}

@pragma('vm:never-inline')
int numLoopN(int n) {
  int a = 1, b = 0;
  for (int i = 0; i < n; i++) {
    a = (a * 31 + i) & 0xffffff;
    b ^= a;
  }
  return b + 3;
}

@pragma('vm:never-inline')
int tinyB(int x) => x + __K__ - 2;

@pragma('vm:never-inline')
int tinyN(int x) => x + 3 - 2;

// Simulated loop calling a linked leaf: one sim->native->sim round trip per
// iteration.
@pragma('vm:never-inline')
int callLoopSimToNative(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = tiny(s) ^ i;
  }
  return s + __K__;
}

// Simulated loop calling a simulated leaf: no transitions.
@pragma('vm:never-inline')
int callLoopSimToSim(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = tinyB(s) ^ i;
  }
  return s + __K__;
}

// Native loop calling a native leaf (reference for both).
@pragma('vm:never-inline')
int callLoopNative(int n) {
  int s = 0;
  for (int i = 0; i < n; i++) {
    s = tinyN(s) ^ i;
  }
  return s + 3;
}

void check(String name, Object actual, Object expected) {
  final ok = actual == expected;
  print('${ok ? "PASS" : "FAIL"} $name: $actual (expected $expected)');
  if (!ok) throw StateError('check failed: $name');
}

int timeUs(int Function() f) {
  final sw = Stopwatch()..start();
  f();
  return sw.elapsedMicroseconds;
}

Future<void> main(List<String> args) async {
  const k = __K__;
  print('variant K=$k');
  // Give the native helpers more than one closure target so TFA cannot turn
  // their closure calls into direct calls (which would cascade-unlink them).
  check('linked helpers (native)',
      linkedCatcher(tinyN, 3) + linkedPassThrough(tinyN, 1), 6 + 3);
  check('changedLeaf', changedLeaf(10), 10 + k);
  check('changedCaller sim->native', changedCaller(100),
      List.generate(100, (i) => i * 2 + 1 + k).reduce((a, b) => a + b));
  final shapes = <Shape>[Sq(3), Rect(2, 5), Circ(4), Tri(4, 3)];
  check('virtual call native->sim', sumAreas(shapes), 9 + 10 + (16 + k) + 6);
  check('closure native->sim (tear-off)', applyN(changedLeaf, 10), 45 + 10 * k);
  check('closure native->sim (lambda)', applyN((x) => x * k, 10), 45 * k);
  check('exception native frame -> simulated handler', changedCatcher(6),
      0 + 1 + 2 + 3 * k);
  int expectCatcher = 0;
  for (int i = 0; i < 8; i++) {
    expectCatcher += i > k ? 1000 : i;
  }
  check('exception simulated frame -> native handler',
      linkedCatcher(changedThrower, 8), expectCatcher);
  int expectOuter = 0;
  for (int i = 0; i < 8; i++) {
    expectOuter += i > k ? 100 + k : i + 1;
  }
  check('exception sim -> native -> sim, caught in sim', changedOuter(8),
      expectOuter);
  check('GC with simulated frames', changedGc(200),
      200 * 2000 + List.generate(200, (r) => r + k).reduce((a, b) => a + b) +
          200 * k);
  check('async/await in simulated code', await changedAsync(5),
      10 + 5 * k);

  final n = args.isNotEmpty ? int.parse(args[0]) : 20000000;
  // Warm up.
  numLoopB(1000);
  numLoopN(1000);
  callLoopSimToNative(1000);
  callLoopSimToSim(1000);
  callLoopNative(1000);
  applyN(changedLeaf, 1000);
  applyN(tinyN, 1000);
  final r = <String, int>{
    'numLoopB(changed)': timeUs(() => numLoopB(n)),
    'numLoopN(same)': timeUs(() => numLoopN(n)),
    'callLoopSimToNative': timeUs(() => callLoopSimToNative(n ~/ 4)),
    'callLoopSimToSim': timeUs(() => callLoopSimToSim(n ~/ 4)),
    'callLoopNative': timeUs(() => callLoopNative(n ~/ 4)),
    'applyN(changedLeaf) native->sim': timeUs(() => applyN(changedLeaf, n ~/ 4)),
    'applyN(tinyN) native->native': timeUs(() => applyN(tinyN, n ~/ 4)),
  };
  r.forEach((key, v) => print('TIME $key ${v}us'));
}
