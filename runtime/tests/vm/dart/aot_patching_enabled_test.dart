// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Verifies that the compact AOT patching API is exported and callable without
// enabling DART_DYNAMIC_MODULES or the bytecode interpreter.

import 'dart:ffi';

import 'package:expect/expect.dart';

typedef DartAotPatchingEnabledNative = Bool Function();
typedef DartAotPatchingEnabled = bool Function();

void main() {
  final enabled = DynamicLibrary.executable()
      .lookupFunction<DartAotPatchingEnabledNative, DartAotPatchingEnabled>(
        'Dart_AotPatchingEnabled',
      )();

  // The value depends on the build flag, but a successful call proves the API
  // is present in both enabled and default builds.
  Expect.isTrue(enabled == true || enabled == false);
}
