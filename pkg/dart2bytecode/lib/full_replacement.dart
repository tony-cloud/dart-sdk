// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:typed_data';
import 'package:vm/bytecode/full_replacement.dart';
import 'bytecode_serialization.dart';
import 'constant_pool.dart';
import 'object_table.dart';
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
    final ffiOwners = <Code>{};
    for (final library in component.libraries) {
      final uri = library.importUri?.stringConstantValue;
      if (uri == null) {
        throw const FormatException(
          'Replacement library URI must be a string.',
        );
      }
      if (uri.startsWith('dart:') ||
          uri == 'vm:ffi:native-assets' ||
          (library.flags & LibraryDeclaration.shorebirdFullReplacementFlag) ==
              0 ||
          (library.flags &
                  ~(LibraryDeclaration.shorebirdFullReplacementFlag |
                      LibraryDeclaration.usesDartFfiFlag)) !=
              0) {
        throw const FormatException(
          'Replacement requires exclusively full-profile mutable libraries.',
        );
      }
      uris.add(uri);
      for (final cls in library.classes) {
        for (final function in cls.members.functions) {
          final flags = function.flags;
          const nativeFlags =
              FunctionDeclaration.isExternalFlag |
              FunctionDeclaration.isNativeFlag;
          if ((flags & nativeFlags) != 0) {
            if ((flags & nativeFlags) != nativeFlags ||
                (flags & FunctionDeclaration.isStaticFlag) == 0 ||
                (flags &
                        (FunctionDeclaration.isAbstractFlag |
                            FunctionDeclaration.isConstructorFlag |
                            FunctionDeclaration.isFactoryFlag)) !=
                    0 ||
                function.nativeName != null ||
                (flags & FunctionDeclaration.hasPragmaFlag) == 0 ||
                !hasValidFfiPragma(
                  function.annotations?.object,
                  native: true,
                ) ||
                function.code == null) {
              throw const FormatException(
                'Replacement requires genuinely lowered FFI-native definitions.',
              );
            }
            ffiOwners.add(function.code!);
          }
        }
      }
    }
    void validateCode(Uint8List bytes, ConstantPool pool, bool ffiOwner) {
      if (pool.entries.any((entry) => entry is ConstantExternalCall)) {
        throw const FormatException('Replacement rejects ExternalCall pools.');
      }
      Instruction? previous;
      try {
        for (final instruction in BytecodeDisassembler().decode(bytes)) {
          if (instruction.opcode == Opcode.kExternalCall) {
            throw const FormatException('Unsupported replacement opcode.');
          }
          final call = instruction.opcode == Opcode.kFfiCall;
          final resolve = instruction.opcode == Opcode.kResolveNativeFunction;
          if (call || resolve) {
            final index = instruction.operands.single;
            if (index >= pool.entries.length ||
                pool.entries[index] is! ConstantNativeFunction ||
                (call && !ffiOwner)) {
              throw const FormatException(
                'Invalid replacement FFI owner/cache operand.',
              );
            }
            if (resolve) {
              if (previous?.opcode != Opcode.kPushConstant) {
                throw const FormatException(
                  'Invalid replacement FFI address metadata.',
                );
              }
              final annotationIndex = previous!.operands.single;
              if (annotationIndex >= pool.entries.length) {
                throw const FormatException(
                  'Invalid replacement FFI annotation operand.',
                );
              }
              final entry = pool.entries[annotationIndex];
              if (entry is! ConstantObjectRef ||
                  !isValidFfiAnnotation(
                    entry.object,
                    native: true,
                    requireFunctionType: false,
                  )) {
                throw const FormatException(
                  'Invalid replacement FFI address annotation.',
                );
              }
            }
          }
          previous = instruction;
        }
      } catch (error) {
        throw FormatException('Invalid replacement instructions: $error');
      }
    }

    for (final code in component.codes) {
      validateCode(code.bytecodes, code.constantPool, ffiOwners.contains(code));
      for (final closure in code.closures) {
        validateCode(
          closure.code!.bytecodes,
          code.constantPool,
          (closure.flags & ClosureDeclaration.hasPragmaFlag) != 0 &&
              hasValidFfiPragma(closure.annotations?.object, native: false),
        );
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
