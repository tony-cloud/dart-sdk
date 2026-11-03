// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';

import 'package:kernel/ast.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/target/targets.dart';
import 'package:test/test.dart';
import 'package:vm/modular/target/vm.dart' show VmTarget;
import 'package:vm/modular/transformations/pragma.dart'
    show
        dynModuleCallablePragmaName,
        dynModuleCanBeOverriddenPragmaName,
        dynModuleDynamicallyCallablePragmaName,
        dynModuleExtendablePragmaName;
import 'package:vm/transformations/dynamic_interface_annotator.dart'
    show annotateShorebirdStructuralComponent, shorebirdBytecodeSdkDependencies;

import '../common_test_utils.dart';

final Uri pkgVmDir = Platform.script.resolve('../..');

/// Names of the `dyn-module:` pragmas (ignores unrelated pragmas).
Set<String> pragmaNames(List<Expression> annotations) => {
  for (final annotation in annotations)
    if (annotation is ConstantExpression)
      if (annotation.constant case InstanceConstant constant)
        if (constant.fieldValues.values.firstOrNull case StringConstant name)
          if (name.value.startsWith('dyn-module:')) name.value,
};

main() {
  late Component component;

  setUpAll(() async {
    component = await compileTestCaseToKernelProgram(
      pkgVmDir.resolve(
        'testcases/transformations/dynamic_interface_annotator/main.dart',
      ),
      target: VmTarget(TargetFlags()),
    );
    annotateShorebirdStructuralComponent(component, CoreTypes(component));
  });

  Class coreClass(String library, String name) => component.libraries
      .firstWhere((lib) => lib.importUri.toString() == library)
      .classes
      .firstWhere((cls) => cls.name == name);

  void expectOpen(Class cls) {
    expect(
      pragmaNames(cls.annotations),
      contains(dynModuleExtendablePragmaName),
      reason: '${cls.name} should stay extendable',
    );
    for (final member in cls.members.where(
      (m) => m.isInstanceMember && !m.name.text.startsWith('_'),
    )) {
      expect(
        pragmaNames(member.annotations),
        contains(dynModuleCanBeOverriddenPragmaName),
        reason: '${cls.name}.${member.name} should stay overridable',
      );
    }
  }

  void expectClosed(Class cls) {
    expect(
      pragmaNames(cls.annotations),
      isNot(contains(dynModuleExtendablePragmaName)),
      reason: '${cls.name} cannot be subtyped by a patch',
    );
    for (final member in cls.members) {
      expect(
        pragmaNames(member.annotations),
        isNot(contains(dynModuleCanBeOverriddenPragmaName)),
        reason: '${cls.name}.${member.name} cannot be overridden by a patch',
      );
    }
  }

  test('public open dart: classes stay extendable', () {
    expectOpen(coreClass('dart:core', 'Object'));
    expectOpen(coreClass('dart:core', 'Iterable'));
    expectOpen(coreClass('dart:core', 'Exception'));
  });

  test('final and sealed dart: classes are closed', () {
    final string = coreClass('dart:core', 'String');
    expect(string.isFinal, isTrue);
    expectClosed(string);
    expectClosed(coreClass('dart:core', 'int'));
    final sealed = component.libraries
        .where((lib) => lib.importUri.isScheme('dart'))
        .expand((lib) => lib.classes)
        .where((cls) => cls.isSealed);
    for (final cls in sealed) {
      expectClosed(cls);
    }
  });

  test('private dart: classes are closed', () {
    final privateClasses = component.libraries
        .where((lib) => lib.importUri.isScheme('dart'))
        .expand((lib) => lib.classes)
        .where((cls) => cls.name.startsWith('_'))
        .toList();
    expect(privateClasses, isNotEmpty);
    for (final cls in privateClasses) {
      expectClosed(cls);
    }
  });

  test('non-dart: classes stay extendable', () {
    final user = component.libraries.where(
      (lib) => !lib.importUri.isScheme('dart') && lib.classes.isNotEmpty,
    );
    expect(user, isNotEmpty);
    for (final lib in user) {
      for (final cls in lib.classes) {
        expect(
          pragmaNames(cls.annotations),
          contains(dynModuleExtendablePragmaName),
        );
      }
    }
  });

  Member? member(Class cls, String name) =>
      cls.members.where((m) => m.name.text == name).firstOrNull;

  test('private dart: classes are not callable roots', () {
    final string = coreClass('dart:core', '_StringBase');
    expect(
      pragmaNames(string.annotations),
      isNot(contains(dynModuleCallablePragmaName)),
    );
    for (final m in string.members) {
      if (m.name.text.startsWith('_') || !m.isInstanceMember) {
        if (m.name.text == '_interpolate' ||
            m.name.text == '_interpolateSingle') {
          // Referenced by dart2bytecode (string interpolation): callable.
          expect(
            pragmaNames(m.annotations),
            contains(dynModuleCallablePragmaName),
          );
          continue;
        }
        expect(
          pragmaNames(m.annotations),
          isEmpty,
          reason: '_StringBase.${m.name} is not nameable by a patch',
        );
      }
    }
  });

  test(
    'public selectors of private dart: classes are dynamically callable only',
    () {
      final string = coreClass('dart:core', '_StringBase');
      var checked = 0;
      for (final name in [
        'toUpperCase',
        'substring',
        'length',
        'hashCode',
        '+',
      ]) {
        final m = member(string, name);
        if (m == null) continue; // inherited, not declared here
        checked++;
        final names = pragmaNames(m.annotations);
        expect(
          names,
          contains(dynModuleDynamicallyCallablePragmaName),
          reason: '_StringBase.$name implements a public selector',
        );
        expect(
          names,
          isNot(contains(dynModuleCallablePragmaName)),
          reason: 'must not be an unconditional AOT root',
        );
      }
      expect(checked, greaterThan(2));
    },
  );

  test('public-named members of private classes need a public declaration', () {
    // Instance members without any annotation exist (no public supertype
    // declares them) and are not dynamically callable.
    var unannotated = 0;
    var annotated = 0;
    for (final lib in component.libraries.where(
      (l) => l.importUri.isScheme('dart'),
    )) {
      for (final cls in lib.classes.where((c) => c.name.startsWith('_'))) {
        for (final m in cls.members) {
          if (!m.isInstanceMember || m.name.text.startsWith('_')) continue;
          if (pragmaNames(m.annotations).isEmpty) {
            unannotated++;
          } else {
            annotated++;
          }
        }
      }
    }
    expect(annotated, isPositive);
    expect(unannotated, isPositive);
  });

  test('public dart: classes keep public members, drop private ones', () {
    final redirectionTargets = <Member>{
      for (final lib in component.libraries.where(
        (l) => l.importUri.isScheme('dart'),
      ))
        for (final cls in lib.classes)
          for (final p in cls.procedures)
            if (p.isRedirectingFactory)
              ?p.function.redirectingFactoryTarget?.target,
    };
    final iterable = coreClass('dart:core', 'Iterable');
    expect(
      pragmaNames(member(iterable, 'map')!.annotations),
      containsAll([
        dynModuleCallablePragmaName,
        dynModuleDynamicallyCallablePragmaName,
      ]),
    );
    for (final cls
        in component.libraries
            .where((l) => l.importUri.isScheme('dart'))
            .expand((l) => l.classes)
            .where((c) => !c.name.startsWith('_'))) {
      final dependencies =
          shorebirdBytecodeSdkDependencies[cls.enclosingLibrary.importUri
              .toString()];
      for (final m in cls.members) {
        if (!m.name.text.startsWith('_')) continue;
        // Fields of classes with a const constructor are required by
        // constants a patch creates.
        if (m is Field &&
            m.isInstanceMember &&
            cls.constructors.any((c) => c.isConst)) {
          continue;
        }
        if (redirectionTargets.contains(m)) continue;
        if (dependencies?.contains('${cls.name}.${m.name.text}') ?? false) {
          continue;
        }
        expect(
          pragmaNames(m.annotations),
          isEmpty,
          reason: '${cls.name}.${m.name} is private to dart:',
        );
      }
    }
  });

  test('declarations the patch compilers reference by name stay callable', () {
    for (final entry in shorebirdBytecodeSdkDependencies.entries) {
      final lib = component.libraries
          .where((l) => l.importUri.toString() == entry.key)
          .firstOrNull;
      if (lib == null) continue; // not loaded by this test program
      for (final dependency in entry.value) {
        final dot = dependency.indexOf('.');
        if (dot < 0) {
          final cls = lib.classes
              .where((c) => c.name == dependency)
              .firstOrNull;
          if (cls == null) continue;
          expect(
            pragmaNames(cls.annotations),
            contains(dynModuleCallablePragmaName),
            reason: '${entry.key} $dependency',
          );
          continue;
        }
        final className = dependency.substring(0, dot);
        final name = dependency.substring(dot + 1);
        final owner = className.isEmpty
            ? null
            : lib.classes.where((c) => c.name == className).firstOrNull;
        if (className.isNotEmpty && owner == null) continue;
        final members = owner?.members ?? lib.members;
        final matches = name.endsWith('*')
            ? members.where(
                (m) =>
                    m.name.text.startsWith(name.substring(0, name.length - 1)),
              )
            : members.where((m) => m.name.text == name);
        if (matches.isEmpty) continue; // lowering target absent in this SDK
        for (final m in matches) {
          expect(
            pragmaNames(m.annotations),
            contains(dynModuleCallablePragmaName),
            reason: '${entry.key} $dependency',
          );
        }
      }
    }
  });

  test('dependency list covers every private LibraryIndex lookup', () {
    final root = pkgVmDir.resolve('../');
    final files = [
      File.fromUri(root.resolve('kernel/lib/core_types.dart')),
      for (final dir in ['vm/lib', 'dart2bytecode/lib'])
        for (final f in Directory.fromUri(
          root.resolve('$dir/'),
        ).listSync(recursive: true))
          if (f is File &&
              f.path.endsWith('.dart') &&
              !f.path.contains('/type_flow/') &&
              !f.path.endsWith('library_index.dart'))
            f,
    ];
    final lookup = RegExp(
      r'''(getTopLevelProcedure|getTopLevelMember|getProcedure|getField|getClass|getConstructor|getMember)\(\s*((?:'[^']*'|"[^"]*"|LibraryIndex\.\w+\s*\+\s*'[^']*'|\s|,)+)\)''',
    );
    final literal = RegExp(r''''([^']*)'|"([^"]*)"''');
    var count = 0;
    for (final file in files) {
      for (final m in lookup.allMatches(file.readAsStringSync())) {
        final kind = m.group(1)!;
        final args = [
          for (final a in literal.allMatches(m.group(2)!))
            a.group(1) ?? a.group(2)!,
        ];
        if (args.isEmpty || !args[0].startsWith('dart:')) continue;
        final library = args[0];
        final String className;
        String name;
        if (kind.startsWith('getTopLevel')) {
          className = '';
          name = args[1];
        } else if (kind == 'getClass') {
          className = args[1];
          name = '';
        } else {
          className = args[1];
          name = args.length > 2 ? args[2] : '';
        }
        name = name.replaceFirst(RegExp(r'^(get|set):'), '');
        if (name.contains(r'$')) name = '${name.split(r'$').first}*';
        if (!className.startsWith('_') && !name.startsWith('_')) continue;
        count++;
        final key = name.isEmpty ? className : '$className.$name';
        expect(
          shorebirdBytecodeSdkDependencies[library],
          contains(key),
          reason:
              '${file.path} looks up private $library $key; add it to '
              '_bytecodeSdkDependencies in dynamic_interface_annotator.dart',
        );
      }
    }
    expect(count, greaterThan(100));
  });

  test('redirecting factory targets and const fields stay callable', () {
    final mapEntry = coreClass('dart:core', 'MapEntry');
    final redirect = mapEntry.constructors.firstWhere(
      (c) => c.name.text == '_',
    );
    expect(
      pragmaNames(redirect.annotations),
      contains(dynModuleCallablePragmaName),
    );
    // Const instances created by a patch refer to private fields by name.
    final endian = coreClass('dart:typed_data', 'Endian');
    final field = endian.fields.firstWhere(
      (f) => f.name.text == '_littleEndian',
    );
    expect(
      pragmaNames(field.annotations),
      contains(dynModuleCallablePragmaName),
    );
  });
}
