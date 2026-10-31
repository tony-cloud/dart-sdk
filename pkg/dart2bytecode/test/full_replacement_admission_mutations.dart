// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Produces deliberately malformed, digest-correct wrapped components for the
// native loader tests. This never calls the producer admission API to bypass a
// gate: it models authenticated but malformed input to the production loader.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/constant_pool.dart';
import 'package:dart2bytecode/dbc.dart';
import 'package:dart2bytecode/declarations.dart';
import 'package:dart2bytecode/disassembler.dart';
import 'package:vm/bytecode/full_replacement.dart';

void main(List<String> arguments) {
  if (arguments.length != 2) {
    throw ArgumentError(
      'Usage: full_replacement_admission_mutations.dart UPDATE.sbr OUTDIR',
    );
  }
  final original = decodeShorebirdReplacement(
    File(arguments[0]).readAsBytesSync(),
  );
  final directory = Directory(arguments[1]).absolute
    ..createSync(recursive: true);
  final cases = <Map<String, Object>>[];
  Component read() =>
      Component.read(BufferedReader(LinkReader(), original.component));
  Uint8List write(Component component) {
    final writer = BufferedWriter(
      component.stringTable,
      component.objectTable,
      LinkWriter(),
    );
    component.write(writer);
    return writer.getContents();
  }

  List<FunctionDeclaration> functions(Component c) => [
    for (final lib in c.libraries)
      for (final cls in lib.classes) ...cls.members.functions,
  ];
  FunctionDeclaration native(Component c) => functions(c).firstWhere(
    (f) =>
        (f.flags &
            (FunctionDeclaration.isNativeFlag |
                FunctionDeclaration.isExternalFlag)) ==
        (FunctionDeclaration.isNativeFlag | FunctionDeclaration.isExternalFlag),
  );
  void replace(
    Component c,
    FunctionDeclaration f, {
    int? flags,
    Code? code,
    bool oldName = false,
    AnnotationsDeclaration? annotations,
    bool omitAnnotations = false,
  }) {
    final replacement = FunctionDeclaration(
      flags ?? f.flags,
      f.name,
      f.script,
      f.position,
      f.endPosition,
      f.typeParameters,
      f.numRequiredParameters,
      f.parameters,
      f.parameterFlags,
      f.returnType,
      oldName ? c.libraries.first.importUri : null,
      code ?? f.code,
      omitAnnotations ? null : annotations ?? f.annotations,
    );
    for (final lib in c.libraries) {
      for (final cls in lib.classes) {
        final index = cls.members.functions.indexOf(f);
        if (index >= 0) cls.members.functions[index] = replacement;
      }
    }
    if (code != null) c.codes[c.codes.indexOf(f.code!)] = code;
  }

  Code withBytes(Code code, List<int> bytes) => Code(
    code.constantPool,
    Uint8List.fromList(bytes),
    code.exceptionsTable,
    code.sourcePositions,
    code.localVariables,
    code.recordedCoverage,
    code.nullableFields,
    code.closures,
    code.parameterFlags,
    code.forwardingStubTargetCpIndex,
    code.defaultFunctionTypeArgsCpIndex,
  );
  Uint8List wrap(Uint8List payload) {
    final m = original.manifest;
    return encodeShorebirdReplacement(
      ShorebirdReplacementManifest(
        hostAbi: m.hostAbi,
        baselineDigest: m.baselineDigest,
        currentDigest: replacementDigest(payload),
        rootUri: m.rootUri,
        baselineUris: m.baselineUris,
        currentUris: m.currentUris,
        removedUris: m.removedUris,
      ),
      payload,
    );
  }

  void add(
    String name,
    String diagnostic,
    void Function(Component) mutate, {
    bool poisoned = true,
  }) {
    final component = read();
    mutate(component);
    final path = '${directory.path}/$name.sbr';
    File(path).writeAsBytesSync(wrap(write(component)));
    cases.add({
      'name': name,
      'path': path,
      'diagnostic': diagnostic,
      'poisoned': poisoned,
    });
  }

  void libraryFlags(Component c, int Function(int) update) {
    for (var i = 0; i < c.libraries.length; ++i) {
      final lib = c.libraries[i];
      c.libraries[i] = LibraryDeclaration(
        lib.importUri,
        update(lib.flags),
        lib.name,
        lib.script,
        lib.classes,
      );
    }
  }

  for (final entry in {
    'missing-full': 0,
    'ffi-only': 2,
    'mirrors-flag': 8 | 1,
    'structural-flag': 8 | 4,
    'unknown-flag': 8 | 16,
  }.entries) {
    add(
      entry.key,
      'Replacement requires full-profile library flags.',
      (c) => libraryFlags(c, (_) => entry.value),
      poisoned: false,
    );
  }
  add('plain-external', 'requires lowered FFI declarations', (c) {
    final f = native(c);
    replace(c, f, flags: f.flags & ~FunctionDeclaration.isNativeFlag);
  });
  add('old-native-flags', 'requires lowered FFI declarations', (c) {
    final f = native(c);
    replace(c, f, flags: f.flags & ~FunctionDeclaration.isExternalFlag);
  });
  add('old-native-name', 'cannot define old native names', (c) {
    replace(c, native(c), oldName: true);
  });
  add('missing-ffi-annotation', 'Missing full replacement FFI pragma.', (c) {
    final f = native(c);
    replace(
      c,
      f,
      flags:
          f.flags &
          ~(FunctionDeclaration.hasAnnotationsFlag |
              FunctionDeclaration.hasPragmaFlag),
      omitAnnotations: true,
    );
  });
  add('nonarray-metadata', 'annotations must be an evaluated array', (c) {
    final annotation = AnnotationsDeclaration(c.libraries.first.importUri);
    c.annotations.add(annotation);
    replace(c, native(c), annotations: annotation);
  });
  for (final entry in {
    'fullFfiBadOption':
        'Full replacement requires SDK FFI annotation identity.',
    'fullFfiBadSignature': 'Full replacement FFI requires a function type.',
    'fullFfiBadIdentity':
        'Full replacement requires SDK FFI annotation identity.',
  }.entries) {
    add(entry.key, entry.value, (c) {
      final other = functions(
        c,
      ).singleWhere((f) => f.name.toString() == "'${entry.key}'");
      replace(c, native(c), annotations: other.annotations);
    });
  }
  add('invalid-ffi-owner', 'Invalid full replacement FFI call owner.', (c) {
    final f = native(c);
    replace(
      c,
      f,
      flags:
          f.flags &
          ~(FunctionDeclaration.isNativeFlag |
              FunctionDeclaration.isExternalFlag),
    );
  });
  for (final entry in {
    'ffi-cache-out-of-range': false,
    'ffi-cache-wrong-kind': true,
  }.entries) {
    add(entry.key, 'Invalid full replacement FFI cache operand.', (c) {
      final code = native(c).code!;
      final call = BytecodeDisassembler()
          .decode(code.bytecodes)
          .firstWhere((i) => i.opcode == Opcode.kFfiCall);
      final index = call.operands.single;
      if (entry.value) {
        code.constantPool.entries[index] = ConstantObjectRef(null);
      } else {
        // A native declaration's tiny body has no large pool indices.
        if (call.isWide || code.constantPool.entries.length > 255) {
          throw StateError('Fixture native body no longer has a small pool.');
        }
        code.bytecodes[call.pc + 1] = 255;
      }
    });
  }
  add(
    'wide-ffi-cache-out-of-range',
    'Invalid full replacement FFI cache operand.',
    (c) {
      final f = native(c);
      replace(
        c,
        f,
        code: withBytes(f.code!, [
          Opcode.kFfiCall_Wide.index,
          255,
          255,
          255,
          127,
        ]),
      );
    },
  );
  FunctionDeclaration resolver(Component c) => functions(c).firstWhere(
    (f) =>
        f.code != null &&
        BytecodeDisassembler()
            .decode(f.code!.bytecodes)
            .any((i) => i.opcode == Opcode.kResolveNativeFunction),
  );
  add(
    'resolve-cache-out-of-range',
    'Invalid full replacement FFI cache operand.',
    (c) {
      final f = resolver(c);
      replace(
        c,
        f,
        code: withBytes(f.code!, [
          Opcode.kResolveNativeFunction_Wide.index,
          255,
          255,
          255,
          127,
        ]),
      );
    },
  );
  add(
    'resolve-missing-annotation-push',
    'Invalid full replacement FFI address metadata.',
    (c) {
      final f = resolver(c);
      final instruction = BytecodeDisassembler()
          .decode(f.code!.bytecodes)
          .firstWhere((i) => i.opcode == Opcode.kResolveNativeFunction);
      final operand = instruction.operands.single;
      replace(
        c,
        f,
        code: withBytes(f.code!, [
          Opcode.kResolveNativeFunction_Wide.index,
          operand & 255,
          (operand >> 8) & 255,
          (operand >> 16) & 255,
          (operand >> 24) & 255,
        ]),
      );
    },
  );
  add('resolve-wrong-annotation', 'Invalid full replacement FFI annotation.', (
    c,
  ) {
    final code = resolver(c).code!;
    final instructions = BytecodeDisassembler().decode(code.bytecodes);
    final index = instructions.indexWhere(
      (i) => i.opcode == Opcode.kResolveNativeFunction,
    );
    final push = instructions[index - 1];
    if (push.opcode != Opcode.kPushConstant) {
      throw StateError('Unexpected address lowering.');
    }
    code.constantPool.entries[push.operands.single] = ConstantObjectRef(null);
  });
  add('external-pool', 'cannot contain native pools', (c) {
    native(c).code!.constantPool.addExternalCall();
  });
  for (final entry in {
    'external-opcode': Opcode.kExternalCall.index,
    'external-wide-opcode': Opcode.kExternalCall_Wide.index,
    'unknown-opcode': 255,
  }.entries) {
    add(entry.key, 'Unsupported or truncated replacement opcode.', (c) {
      final code = native(c).code!;
      final call = BytecodeDisassembler()
          .decode(code.bytecodes)
          .firstWhere((i) => i.opcode == Opcode.kFfiCall);
      code.bytecodes[call.pc] = entry.value;
    });
  }
  for (final entry in {
    'truncated-ffi': Opcode.kFfiCall,
    'truncated-wide-ffi': Opcode.kFfiCall_Wide,
    'truncated-resolve': Opcode.kResolveNativeFunction,
    'truncated-wide-resolve': Opcode.kResolveNativeFunction_Wide,
  }.entries) {
    add(entry.key, 'Unsupported or truncated replacement opcode.', (c) {
      final f = native(c);
      replace(c, f, code: withBytes(f.code!, [entry.value.index]));
    });
  }
  add('closure-missing-pragma', 'Missing full replacement FFI pragma.', (c) {
    final code = c.codes.firstWhere(
      (code) => code.closures.any(
        (closure) => BytecodeDisassembler()
            .decode(closure.code!.bytecodes)
            .any((i) => i.opcode == Opcode.kFfiCall),
      ),
    );
    final closure = code.closures.firstWhere(
      (closure) => BytecodeDisassembler()
          .decode(closure.code!.bytecodes)
          .any((i) => i.opcode == Opcode.kFfiCall),
    );
    closure.flags &= ~ClosureDeclaration.hasPragmaFlag;
  });
  // Forge only a manifest URI; preserve the exact component and valid digest.
  // It must fail reserved-domain parsing before checking component/domain match.
  {
    final bytes = File(arguments[0]).readAsBytesSync();
    final metadata = BytesBuilder();
    void u32(int value) => metadata.add(
      (ByteData(4)..setUint32(0, value, Endian.little)).buffer.asUint8List(),
    );
    void uri(String value) {
      final bytes = utf8.encode(value);
      u32(bytes.length);
      metadata.add(bytes);
    }

    final m = original.manifest;
    u32(1);
    metadata.add(m.hostAbi);
    metadata.add(m.baselineDigest);
    metadata.add(m.currentDigest);
    uri(m.rootUri);
    for (final uris in [m.baselineUris, m.currentUris, m.removedUris]) {
      final forged = canonicalReplacementUris([
        ...uris,
        if (identical(uris, m.currentUris)) 'vm:ffi:native-assets',
      ]);
      u32(forged.length);
      forged.forEach(uri);
    }
    final manifest = metadata.takeBytes();
    final header = Uint8List.fromList(bytes.sublist(0, 24));
    ByteData.sublistView(header).setUint32(12, manifest.length, Endian.little);
    final path = '${directory.path}/reserved-assets-carrier.sbr';
    File(
      path,
    ).writeAsBytesSync([...header, ...manifest, ...original.component]);
    cases.add({
      'name': 'reserved-assets-carrier',
      'path': path,
      'diagnostic': 'Invalid full replacement manifest',
      'poisoned': false,
    });
  }
  final cleared = read();
  libraryFlags(cleared, (flags) => flags & ~LibraryDeclaration.usesDartFfiFlag);
  final clearedPath = '${directory.path}/cleared-ffi-flag.sbr';
  File(clearedPath).writeAsBytesSync(wrap(write(cleared)));
  File('${directory.path}/cases.json').writeAsStringSync(
    const JsonEncoder.withIndent('  ').convert({
      'format_version': 1,
      'cases': cases,
      'positive_cases': [
        {'name': 'cleared-ffi-flag', 'path': clearedPath},
      ],
    }),
  );
  stdout.writeln('Generated ${cases.length} native loader rejection cases.');
}
