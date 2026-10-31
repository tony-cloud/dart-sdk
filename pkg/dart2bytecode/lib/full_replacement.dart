// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:typed_data';
import 'package:vm/bytecode/full_replacement.dart';
import 'bytecode_serialization.dart';
import 'declarations.dart';
import 'dbc.dart';
import 'disassembler.dart';

/// Retains the entire component. No declarations or constant pools are pruned.
Uint8List createShorebirdFullReplacement(
  List<int> updated, {
  required List<int> hostAbi,
  required String rootUri,
  List<int>? baseline,
}) {
  List<String> validate(List<int> bytes) {
    if (bytes.length > shorebirdReplacementMaxComponent) {
      throw const FormatException('Replacement component exceeds limit.');
    }
    final Component component;
    try {
      component = Component.read(BufferedReader(LinkReader(), bytes));
    } catch (error) {
      throw FormatException('Invalid replacement bytecode component: $error');
    }
    final uris = <String>[];
    for (final library in component.libraries) {
      final uri = library.importUri?.stringConstantValue;
      if (uri == null) {
        throw const FormatException(
          'Replacement library URI must be a string.',
        );
      }
      if (uri.startsWith('dart:') ||
          library.flags != LibraryDeclaration.shorebirdFullReplacementFlag) {
        throw const FormatException(
          'Replacement requires exclusively full-profile mutable libraries.',
        );
      }
      uris.add(uri);
      for (final cls in library.classes) {
        for (final function in cls.members.functions) {
          if ((function.flags &
                  (FunctionDeclaration.isExternalFlag |
                      FunctionDeclaration.isNativeFlag)) !=
              0) {
            throw const FormatException(
              'Replacement v1 rejects external/native definitions.',
            );
          }
        }
      }
    }
    for (final code in component.codes) {
      for (final instructions in [
        code.bytecodes,
        ...code.closures.map((closure) => closure.code!.bytecodes),
      ]) {
        for (final instruction in BytecodeDisassembler().decode(instructions)) {
          if (const {
            Opcode.kExternalCall,
            Opcode.kFfiCall,
            Opcode.kResolveNativeFunction,
          }.contains(instruction.opcode)) {
            throw const FormatException(
              'Replacement v1 rejects native/FFI opcodes.',
            );
          }
        }
      }
    }
    return canonicalReplacementUris(uris);
  }

  final uris = validate(updated);
  final digest = replacementDigest(updated);
  ShorebirdReplacementManifest manifest;
  if (baseline == null) {
    manifest = ShorebirdReplacementManifest(
      hostAbi: hostAbi,
      baselineDigest: digest,
      currentDigest: digest,
      rootUri: rootUri,
      baselineUris: uris,
      currentUris: uris,
      removedUris: const [],
    );
  } else {
    final base = decodeShorebirdReplacement(baseline);
    final actualBaseUris = validate(base.component);
    final m = base.manifest;
    if (hostAbi.join(',') != m.hostAbi.join(',') ||
        rootUri != m.rootUri ||
        actualBaseUris.join('\u0000') != m.currentUris.join('\u0000')) {
      throw const FormatException(
        'Replacement baseline host ABI, root or payload domain mismatch.',
      );
    }
    final currentSet = uris.toSet();
    manifest = ShorebirdReplacementManifest(
      hostAbi: m.hostAbi,
      baselineDigest: m.baselineDigest,
      currentDigest: digest,
      rootUri: m.rootUri,
      baselineUris: m.baselineUris,
      currentUris: uris,
      removedUris: m.baselineUris
          .where((uri) => !currentSet.contains(uri))
          .toList(),
    );
  }
  return encodeShorebirdReplacement(manifest, updated);
}
