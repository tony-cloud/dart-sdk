// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:typed_data';
import 'package:kernel/ast.dart';
import '../bytecode/full_replacement.dart';

/// Canonical baseline manifest, attached only to the program's root library.
class ShorebirdReplacementMetadataRepository
    extends MetadataRepository<Uint8List> {
  static const repositoryTag = 'vm.shorebird-replacement.metadata';
  @override
  final String tag = repositoryTag;
  @override
  final Map<TreeNode, Uint8List> mapping = {};
  @override
  void writeToBinary(Uint8List metadata, Node node, BinarySink sink) {
    if (node is! Library) {
      throw ArgumentError('Replacement metadata requires a root library.');
    }
    ShorebirdReplacementManifest.decode(metadata);
    sink.writeUInt30(metadata.length);
    for (final byte in metadata) {
      sink.writeByte(byte);
    }
  }

  @override
  Uint8List readFromBinary(Node node, BinarySource source) {
    final length = source.readUInt30();
    if (length > shorebirdReplacementMaxManifest) {
      throw const FormatException('Replacement metadata exceeds limit.');
    }
    final bytes = Uint8List.fromList(
      List.generate(length, (_) => source.readByte()),
    );
    ShorebirdReplacementManifest.decode(bytes);
    return bytes;
  }
}
