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
    show dynModuleCanBeOverriddenPragmaName, dynModuleExtendablePragmaName;
import 'package:vm/transformations/dynamic_interface_annotator.dart'
    show annotateShorebirdStructuralComponent;

import '../common_test_utils.dart';

final Uri pkgVmDir = Platform.script.resolve('../..');

Set<String> pragmaNames(List<Expression> annotations) => {
  for (final annotation in annotations)
    if (annotation is ConstantExpression)
      if (annotation.constant case InstanceConstant constant)
        if (constant.fieldValues.values.firstOrNull case StringConstant name)
          name.value,
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
    for (final member in cls.members.where((m) => m.isInstanceMember)) {
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
}
