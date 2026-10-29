// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

mixin M {}

class B {
  const B();
  const B.named();
  const B._private();
}

class C extends B with M {
  const C();
  const C.named() : super.named();
  const C._private() : super._private();
}

class D extends B with M {
  const D();
  const D.named() : super.named();
  const D._private() : super._private();
}

void main() {
  print(const C());
  print(const C.named());
  print(const C._private());
  print(const D());
  print(const D.named());
  print(const D._private());
}
