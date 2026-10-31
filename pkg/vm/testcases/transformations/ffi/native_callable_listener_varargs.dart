// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:ffi';

final class Pair extends Struct {
  @Int64()
  external int first;
  @Double()
  external double second;
}

typedef Empty = Void Function(Int32, VarArgs<()>);
typedef Single = Void Function(Int64, VarArgs<(Double,)>);
typedef Multiple =
    Void Function(Int32, VarArgs<(Int32, Double, Pointer<Void>)>);
typedef Compounds = Void Function(Int64, VarArgs<(Pair, Pair)>);
typedef Spilled =
    Void Function(
      Int64,
      Int64,
      Int64,
      Int64,
      Int64,
      Int64,
      Int64,
      Int64,
      VarArgs<(Int32, Double, Pair)>,
    );

void empty(int value, [int extra = 99]) => print([value, extra]);

// A compatible target can have broader parameter types and extra optional
// parameters. Its invocation type differs from the converted native signature.
void single(num fixed, Object value, [Object? extra]) =>
    print([fixed, value, extra]);

void multiple(
  int fixed,
  int promotedInteger,
  double promotedFloat,
  Pointer<Void> pointer,
) => print([fixed, promotedInteger, promotedFloat, pointer]);

void compounds(int fixed, Pair first, Pair second) =>
    print([fixed, first.first, second.second]);

void spilled(
  int a0,
  int a1,
  int a2,
  int a3,
  int a4,
  int a5,
  int a6,
  int a7,
  int integer,
  double floating,
  Pair pair,
) => print([a0, a1, a2, a3, a4, a5, a6, a7, integer, floating, pair.first]);

int evaluations = 0;
void Function(int, [int]) evaluateTarget() {
  evaluations++;
  return empty;
}

void main() {
  final callbacks = <NativeCallable>[
    NativeCallable<Empty>.listener(evaluateTarget()),
    NativeCallable<Single>.listener(single),
    NativeCallable<Multiple>.listener(multiple),
    NativeCallable<Compounds>.listener(compounds),
    NativeCallable<Spilled>.listener(spilled),
  ];
  print(evaluations);
  for (final callback in callbacks) {
    print(callback.nativeFunction);
    callback.close();
  }
}
