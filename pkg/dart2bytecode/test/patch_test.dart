// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'dart:typed_data';

import 'package:dart2bytecode/bytecode_generator.dart' show generateBytecode;
import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/dart2bytecode.dart'
    show runCompilerWithCommandLineArguments;
import 'package:dart2bytecode/declarations.dart' as dbc;
import 'package:dart2bytecode/options.dart';
import 'package:dart2bytecode/patch.dart';
import 'package:front_end/src/api_unstable/vm.dart'
    show computePlatformBinariesLocation;
import 'package:kernel/target/targets.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/class_hierarchy.dart';
import 'package:test/test.dart';
import 'package:vm/modular/target/vm.dart';

import 'bytecode_generator_test.dart' as support;

dbc.Component readComponent(List<int> bytes) =>
    dbc.Component.read(BufferedReader(LinkReader(), bytes));

void main() {
  late Directory directory;
  late File source;

  setUp(() {
    directory = Directory.systemTemp.createTempSync('bytecode-patch-test-');
    source = File('${directory.path}/main.dart');
  });
  tearDown(() => directory.deleteSync(recursive: true));

  Future<Uint8List> compile(String contents, {bool structural = false}) async {
    source.writeAsStringSync(contents);
    final target = VmTarget(TargetFlags());
    final result = await support.compileTestCaseToKernelProgram(
      source.uri,
      target: target,
    );
    final sink = support.ByteSink();
    generateBytecode(
      result.component,
      sink,
      options: BytecodeOptions(shorebirdStructuralPatching: structural),
      libraries: result.component.libraries
          .where((library) => !result.loadedLibraries.contains(library))
          .toList(),
      extraLoadedLibraries: result.loadedLibraries,
      coreTypes: result.coreTypes ?? CoreTypes(result.component),
      hierarchy:
          result.classHierarchy ??
          ClassHierarchy(
            result.component,
            result.coreTypes ?? CoreTypes(result.component),
          ),
      target: target,
    );
    return sink.builder.takeBytes();
  }

  dbc.ClassDeclaration topLevel(dbc.Component component) =>
      component.libraries.single.classes.first;

  String name(Object? handle) {
    final quoted = '$handle';
    return quoted.substring(1, quoted.length - 1);
  }

  Iterable<String> names(dbc.Members members) =>
      members.functions.map((function) => name(function.name));

  test('ordinary deltas contain only changed bodies', () async {
    final baseline = await compile('''
int unchanged() => 10;
int changed() => 1;
void main() { print(changed() + unchanged()); }
''');
    final updated = await compile('''
int unchanged() => 10;
int changed() => 2;
void main() { print(changed() + unchanged()); }
''');
    final delta = readComponent(createBytecodePatch(baseline, updated));
    expect(names(topLevel(delta).members), ['changed']);
    expect(delta.codes, hasLength(1));
    expect(topLevel(delta).members.fields, isEmpty);
    expect(delta.dynModuleEntryPoint, isNull);
  });

  test('declaration additions require explicit profile', () async {
    final baseline = await compile('void main() {}');
    final updated = await compile('int helper() => 2; void main() {}');
    expect(() => createBytecodePatch(baseline, updated), throwsFormatException);
    expect(
      () => createBytecodePatch(
        baseline,
        updated,
        shorebirdStructuralPatching: true,
      ),
      throwsA(
        isA<FormatException>().having(
          (error) => error.message,
          'message',
          contains('requires a baseline and update'),
        ),
      ),
    );
  });

  test('profile marker is required on baseline and update', () async {
    final ordinary = await compile('void main() {}');
    final structural = await compile('void main() {}', structural: true);
    final updated = await compile(
      'int helper() => 2; void main() {}',
      structural: true,
    );
    for (final pair in [(ordinary, updated), (structural, ordinary)]) {
      expect(
        () => createBytecodePatch(
          pair.$1,
          pair.$2,
          shorebirdStructuralPatching: true,
        ),
        throwsFormatException,
      );
    }
    expect(
      () => createBytecodePatch(structural, updated),
      throwsFormatException,
    );
  });

  test(
    'new helpers retain forward, recursive and tear-off references',
    () async {
      final baseline = await compile(
        'int entry() => 1; void main() { print(entry()); }',
        structural: true,
      );
      final updated = await compile('''
int entry() => even(4) ? makeAdder<int>(2)(3) : 0;
bool even(int value) => value == 0 || odd(value - 1);
bool odd(int value) => value != 0 && even(value - 1);
int Function(int) makeAdder<T>(int left) => (right) => left + right;
void main() { print(entry()); }
''', structural: true);
      final delta = readComponent(
        createBytecodePatch(
          baseline,
          updated,
          shorebirdStructuralPatching: true,
        ),
      );
      expect(names(topLevel(delta).members), [
        'entry',
        'even',
        'odd',
        'makeAdder',
      ]);
      expect(delta.codes, hasLength(4));
      expect(delta.codes.any((code) => code.closures.isNotEmpty), isTrue);
      expect(
        delta.libraries.single.flags &
            dbc.LibraryDeclaration.shorebirdStructuralPatchingFlag,
        isNot(0),
      );
    },
  );

  test(
    'new classes retain fields, initializers, constructors and methods',
    () async {
      final baseline = await compile('void main() {}', structural: true);
      final updated = await compile('''
class Added<T> {
  final T value;
  int count = DateTime.now().year;
  static int total = DateTime.now().year;
  Added(this.value);
  T read() => value;
}
void main() {}
''', structural: true);
      final delta = readComponent(
        createBytecodePatch(
          baseline,
          updated,
          shorebirdStructuralPatching: true,
        ),
      );
      expect(delta.libraries.single.classes, hasLength(2));
      expect(topLevel(delta).members.functions, isEmpty);
      final added = delta.libraries.single.classes.last;
      expect(name(added.name), 'Added');
      expect(added.members.fields, hasLength(3));
      expect(
        added.members.fields.where((field) => field.initializerCode != null),
        hasLength(2),
      );
      expect(added.members.functions, hasLength(2));
      expect(delta.codes, hasLength(4));
    },
  );

  test('added fields retain independent initializer code', () async {
    final baseline = await compile('''
class Existing {
  int old = 7;
  Existing();
}
void main() {}
''', structural: true);
    final updated = await compile('''
class Existing {
  int old = 7;
  int added = 42;
  static int addedStatic = DateTime.now().year;
  Existing();
}
void main() {}
''', structural: true);
    final delta = readComponent(
      createBytecodePatch(baseline, updated, shorebirdStructuralPatching: true),
    );
    final existing = delta.libraries.single.classes.last;
    expect(existing.members.fields.map((field) => name(field.name)), [
      'added',
      'addedStatic',
    ]);
    expect(
      existing.members.fields.every((field) => field.initializerCode != null),
      isTrue,
    );
    expect(delta.codes, hasLength(3));
  });

  test('abstract-class additions retain their constructor', () async {
    final baseline = await compile('void main() {}', structural: true);
    final updated = await compile('''
const added = 42;
abstract class Added { int read(); }
void main() {}
''', structural: true);
    final delta = readComponent(
      createBytecodePatch(baseline, updated, shorebirdStructuralPatching: true),
    );
    expect(topLevel(delta).members.fields, hasLength(1));
    expect(delta.libraries.single.classes, hasLength(2));
    expect(delta.codes, hasLength(1));
  });

  test('field-only additions do not require a changed function body', () async {
    final baseline = await compile('void main() {}', structural: true);
    final updated = await compile(
      'const added = 42; void main() {}',
      structural: true,
    );
    final delta = readComponent(
      createBytecodePatch(baseline, updated, shorebirdStructuralPatching: true),
    );
    expect(topLevel(delta).members.fields, hasLength(1));
    expect(topLevel(delta).members.functions, isEmpty);
    expect(delta.codes, isEmpty);
  });

  test(
    'existing objects require independently initialized non-nullable fields',
    () async {
      final baseline = await compile('''
class Existing { Existing(); }
void main() {}
''', structural: true);
      final updated = await compile('''
class Existing { int added; Existing() : added = 42; }
void main() {}
''', structural: true);
      expect(
        () => createBytecodePatch(
          baseline,
          updated,
          shorebirdStructuralPatching: true,
        ),
        throwsA(
          isA<FormatException>().having(
            (error) => error.message,
            'message',
            contains('declaration initializer'),
          ),
        ),
      );
      final supported = await compile('''
class Existing {
  int? nullable;
  dynamic dynamicValue;
  late int lateValue;
  Existing();
}
void main() {}
''', structural: true);
      final delta = readComponent(
        createBytecodePatch(
          baseline,
          supported,
          shorebirdStructuralPatching: true,
        ),
      );
      expect(delta.libraries.single.classes.last.members.fields, hasLength(3));
    },
  );

  test('new libraries retain their complete declarations', () async {
    final baseline = await compile('void main() {}', structural: true);
    File('${directory.path}/helper.dart').writeAsStringSync('''
int helper() => Added().value;
class Added { int value = 42; }
''');
    final updated = await compile('''
import 'helper.dart';
void main() { print(helper()); }
''', structural: true);
    final delta = readComponent(
      createBytecodePatch(baseline, updated, shorebirdStructuralPatching: true),
    );
    expect(delta.libraries, hasLength(2));
    final helper = delta.libraries.singleWhere(
      (library) => '${library.importUri}'.contains('helper.dart'),
    );
    expect(helper.classes, hasLength(2));
    expect(names(helper.classes.first.members), ['helper']);
    expect(helper.classes.last.members.fields, hasLength(1));
  });

  test(
    'existing declarations retain their compatibility constraints',
    () async {
      const original = '''
class Existing {
  int value = 7;
  int read(int argument) => value + argument;
}
void main() {}
''';
      final baseline = await compile(original, structural: true);
      for (final incompatible in [
        original
            .replaceFirst('int value = 7;', 'String value = "7";')
            .replaceFirst('value + argument', 'argument'),
        original.replaceFirst('int value = 7;', 'int value = 8;'),
        original.replaceFirst(
          'read(int argument)',
          'read(int argument, int other)',
        ),
        original.replaceFirst(
              'class Existing {',
              'class Existing extends Object with Extra {',
            ) +
            '\nmixin Extra {}',
        'void main() {}',
      ]) {
        final updated = await compile(incompatible, structural: true);
        expect(
          () => createBytecodePatch(
            baseline,
            updated,
            shorebirdStructuralPatching: true,
          ),
          throwsFormatException,
        );
      }
    },
  );

  test('command profile creates a baseline and an additive delta', () async {
    source.writeAsStringSync('''
class Existing { int value = 42; }
void main() {}
''');
    final output = File('${directory.path}/baseline.bytecode');
    final platform = computePlatformBinariesLocation()
        .resolve('vm_platform.dill')
        .toFilePath();
    final exitCode = await runCompilerWithCommandLineArguments([
      '--platform=$platform',
      '--shorebird-structural-patching',
      '-o',
      output.path,
      source.path,
    ]);
    expect(exitCode, 0);
    final component = readComponent(output.readAsBytesSync());
    expect(
      component.libraries.single.flags &
          dbc.LibraryDeclaration.shorebirdStructuralPatchingFlag,
      isNot(0),
    );
    expect(
      component
          .libraries
          .single
          .classes
          .last
          .members
          .fields
          .single
          .initializerCode,
      isNotNull,
    );
    source.writeAsStringSync('''
class Existing { int value = 42; int added = 43; }
void main() {}
''');
    final patch = File('${directory.path}/patch.bytecode');
    expect(
      await runCompilerWithCommandLineArguments([
        '--platform=$platform',
        '--shorebird-structural-patching',
        '--patch-base=${output.path}',
        '-o',
        patch.path,
        source.path,
      ]),
      0,
    );
    final delta = readComponent(patch.readAsBytesSync());
    expect(
      delta.libraries.single.classes.last.members.fields.map(
        (field) => name(field.name),
      ),
      ['added'],
    );
  });
}
