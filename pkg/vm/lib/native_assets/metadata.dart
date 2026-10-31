// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:kernel/ast.dart';

/// The synthetic carrier is consumed into the native host assets map. It is
/// metadata, rather than a library in the replaceable Dart application domain.
bool isNativeAssetsMetadataLibrary(Library library) {
  if (library.importUri.toString() != 'vm:ffi:native-assets') return false;
  if (library.dependencies.isNotEmpty ||
      library.additionalExports.isNotEmpty ||
      library.parts.isNotEmpty ||
      library.typedefs.isNotEmpty ||
      library.classes.isNotEmpty ||
      library.extensions.isNotEmpty ||
      library.extensionTypeDeclarations.isNotEmpty ||
      library.fields.isNotEmpty ||
      library.procedures.isNotEmpty) {
    throw const FormatException(
      'Native assets metadata library cannot define Dart declarations or dependencies.',
    );
  }
  return true;
}
