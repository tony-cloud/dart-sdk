// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:typed_data';

import 'bytecode_serialization.dart';
import 'constant_pool.dart';
import 'declarations.dart';
import 'exceptions.dart';

/// Produces a component declaring only changed bodies of existing functions.
///
/// Both inputs are full bytecode components built with the same SDK, defines,
/// package identities and source URIs. Layout or API changes need a new release.
Uint8List createBytecodePatch(List<int> baseline, List<int> updated) {
  final base = Component.read(BufferedReader(LinkReader(), baseline));
  final patch = Component.read(BufferedReader(LinkReader(), updated));
  final baseLibraries = {for (final lib in base.libraries) lib.importUri: lib};
  final patchLibraries = {
    for (final lib in patch.libraries) lib.importUri: lib,
  };
  _sameKeys(baseLibraries, patchLibraries, 'libraries');
  final changedCodes = <Code>{};
  final keptClasses = <ClassDeclaration>{};
  final keptMembers = <Members>{};
  final keptLibraries = <LibraryDeclaration>{};

  for (final library in patch.libraries) {
    final oldLibrary = baseLibraries[library.importUri]!;
    _require(
      _same([oldLibrary.flags, oldLibrary.name], [library.flags, library.name]),
      'library ${library.importUri}',
    );
    final oldClasses = {for (final cls in oldLibrary.classes) cls.name: cls};
    final newClasses = {for (final cls in library.classes) cls.name: cls};
    _sameKeys(oldClasses, newClasses, 'classes in ${library.importUri}');
    final topLevel = library.classes.first;
    for (final cls in library.classes) {
      final oldClass = oldClasses[cls.name]!;
      final context = '${library.importUri}::${cls.name}';
      _require(_same(_classShape(oldClass), _classShape(cls)), context);
      final oldFields = {for (final f in oldClass.members.fields) f.name: f};
      final newFields = {for (final f in cls.members.fields) f.name: f};
      _sameKeys(oldFields, newFields, 'fields in $context');
      for (final field in cls.members.fields) {
        _require(
          _same(_field(oldFields[field.name]!), _field(field)),
          '$context field ${field.name}',
        );
      }
      final oldFunctions = {
        for (final f in oldClass.members.functions) f.name: f,
      };
      final newFunctions = {for (final f in cls.members.functions) f.name: f};
      _sameKeys(oldFunctions, newFunctions, 'functions in $context');
      final changedFunctions = <FunctionDeclaration>[];
      for (final function in cls.members.functions) {
        final old = oldFunctions[function.name]!;
        _require(
          _same(_functionShape(old), _functionShape(function)),
          '$context function ${function.name}',
        );
        if (!_same(_code(old.code), _code(function.code))) {
          if (function.code == null || old.code == null) {
            throw FormatException(
              'Unsupported bytecode patch: $context '
              'function ${function.name} changes its implementation kind.',
            );
          }
          changedFunctions.add(function);
          changedCodes.add(function.code!);
        }
      }
      cls.members.fields.clear();
      cls.members.functions
        ..clear()
        ..addAll(changedFunctions);
      if (changedFunctions.isNotEmpty) {
        keptClasses.add(cls);
        keptMembers.add(cls.members);
        keptLibraries.add(library);
      }
    }
    if (keptLibraries.contains(library)) {
      // The runtime identifies the top-level class by its index, even when
      // only an instance class has changes.
      keptClasses.add(topLevel);
      keptMembers.add(topLevel.members);
      library.classes.removeWhere((cls) => !keptClasses.contains(cls));
    }
  }
  if (changedCodes.isEmpty) {
    throw const FormatException('No supported function-body changes.');
  }
  patch.libraries.removeWhere((lib) => !keptLibraries.contains(lib));
  patch.classes.removeWhere((cls) => !keptClasses.contains(cls));
  patch.members.removeWhere((members) => !keptMembers.contains(members));
  patch.codes.removeWhere((code) => !changedCodes.contains(code));
  patch.dynModuleEntryPoint = null;
  final writer = BufferedWriter(
    patch.stringTable,
    patch.objectTable,
    LinkWriter(),
  );
  patch.write(writer);
  return writer.getContents();
}

void _require(bool supported, String context) {
  if (!supported) {
    throw FormatException(
      'Unsupported bytecode patch structure: $context. '
      'Publish a new release for declaration, field, or signature changes.',
    );
  }
}

void _sameKeys(
  Map<Object?, Object> old,
  Map<Object?, Object> updated,
  String context,
) {
  _require(
    old.length == updated.length && old.keys.every(updated.containsKey),
    context,
  );
}

bool _same(Object? a, Object? b) {
  if (a is List && b is List) {
    return a.length == b.length &&
        Iterable.generate(a.length).every((i) => _same(a[i], b[i]));
  }
  return a == b;
}

Object _classShape(ClassDeclaration cls) => [
  cls.flags & ~ClassDeclaration.hasSourcePositionsFlag,
  cls.typeParameters,
  cls.numTypeArguments,
  cls.superType,
  cls.interfaces,
  cls.annotations?.object,
];

Object _field(FieldDeclaration field) => [
  field.flags &
      ~(FieldDeclaration.hasSourcePositionsFlag |
          FieldDeclaration.hasCustomScriptFlag),
  field.name,
  field.type,
  field.value,
  field.getterName,
  field.setterName,
  _code(field.initializerCode),
  field.annotations?.object,
];

Object _functionShape(FunctionDeclaration function) => [
  function.flags &
      ~(FunctionDeclaration.hasSourcePositionsFlag |
          FunctionDeclaration.hasCustomScriptFlag),
  function.name,
  function.typeParameters,
  function.numRequiredParameters,
  [
    for (final p in function.parameters) [p.name, p.type],
  ],
  function.parameterFlags,
  function.returnType,
  function.nativeName,
  function.annotations?.object,
];

Object? _code(Code? code) => code == null
    ? null
    : [
        code.bytecodes,
        _pool(code.constantPool),
        _exceptions(code.exceptionsTable),
        code.nullableFields,
        code.parameterFlags,
        code.forwardingStubTargetCpIndex,
        code.defaultFunctionTypeArgsCpIndex,
        [
          for (final closure in code.closures)
            [
              closure.flags & ~ClosureDeclaration.hasSourcePositionsFlag,
              closure.parent,
              closure.name,
              closure.typeParameters,
              closure.numRequiredParams,
              closure.numNamedParams,
              [
                for (final p in closure.parameters) [p.name, p.type],
              ],
              closure.parameterFlags,
              closure.returnType,
              closure.annotations?.object,
              closure.code?.bytecodes,
              if (closure.code != null)
                _exceptions(closure.code!.exceptionsTable),
              closure.code?.capturesOnlyFinalNotLateVars,
            ],
        ],
      ];

Object _exceptions(ExceptionsTable table) => [
  for (final block in table.blocks)
    [
      block.tryIndex,
      block.outerTryIndex,
      block.startPC,
      block.endPC,
      block.handlerPC,
      block.flags,
      block.types,
    ],
];

Object _pool(ConstantPool pool) {
  final values = <Object>[];
  for (var i = 0; i < pool.entries.length; ++i) {
    final entry = pool.entries[i];
    // These entries deliberately use identity equality to prevent merging
    // runtime cache slots; their serialized semantic value is just their tag.
    values.add(switch (entry.tag) {
      ConstantTag.kEndClosureFunctionScope ||
      ConstantTag.kSubtypeTestCache ||
      ConstantTag.kExternalCall ||
      ConstantTag.kNativeFunction => entry.tag,
      _ => entry,
    });
    i += entry.numReservedEntries;
  }
  return values;
}
