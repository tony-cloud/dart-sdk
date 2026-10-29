// Copyright (c) 2024, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';

import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_from_binary.dart';
import 'package:kernel/binary/ast_to_binary.dart';
import 'package:kernel/core_types.dart' show CoreTypes;
import 'package:kernel/target/targets.dart';
import 'package:kernel/verifier.dart';
import 'package:test/test.dart';
import 'package:vm/modular/target/vm.dart' show VmTarget;
import 'package:vm/transformations/mixin_deduplication.dart'
    show transformComponent;

import '../common_test_utils.dart';

final Uri _pkgVmDir = Platform.script.resolve('../..');

runTestCase(Uri source) async {
  final target = VmTarget(new TargetFlags());
  final component = await compileTestCaseToKernelProgram(
    source,
    target: target,
  );
  transformComponent(component, CoreTypes(component), target);
  verifyComponent(
    target,
    VerificationStage.afterGlobalTransformations,
    component,
  );

  final actual = component.libraries
      .where(
        (l) =>
            l.importUri.path.contains('testcases') ||
            (l.importUri.scheme == 'dart' &&
                l.importUri.path.startsWith('mixin_deduplication')),
      )
      .map(kernelLibraryToString)
      .join('\n\n')
      .replaceAll(_pkgVmDir.toString(), 'file:pkg/vm/');
  compareResultWithExpectationsFile(source, actual);
}

main() {
  group('mixin-deduplication', () {
    test('remaps constant constructor coverage before serialization', () async {
      final target = VmTarget(new TargetFlags());
      final source = _pkgVmDir.resolve(
        'testcases/transformations/mixin_deduplication/'
        'constant_coverage_helper.dart',
      );
      final component = await compileTestCaseToKernelProgram(
        source,
        target: target,
      );
      final before =
          component.uriToSource[source]!.constantCoverageConstructors!;
      // Kernel coverage may reference lowered extension-type constructor
      // procedures. Seed these actual lowered members explicitly because the
      // frontend need not record coverage for every folded constant expression.
      final loweredConstructors = {
        for (final declaration
            in component.mainMethod!.enclosingLibrary.extensionTypeDeclarations)
          for (final member in declaration.memberDescriptors)
            if (member.kind == ExtensionTypeMemberKind.Constructor)
              member.memberReference!,
      };
      expect(loweredConstructors, hasLength(2));
      before.addAll(loweredConstructors);
      final originalConstructors = before
          .map((r) => r.asMember)
          .whereType<Constructor>()
          .toSet();
      expect(
        originalConstructors.where((c) => c.enclosingClass.isAnonymousMixin),
        hasLength(6),
      );

      transformComponent(component, CoreTypes(component), target);

      final liveConstructors = {
        for (final library in component.libraries)
          for (final cls in library.classes) ...cls.constructors,
      };
      final after =
          component.uriToSource[source]!.constantCoverageConstructors!;
      expect(
        after
            .map((r) => r.asMember)
            .whereType<Constructor>()
            .every(liveConstructors.contains),
        isTrue,
      );
      expect(after, containsAll(loweredConstructors));
      final canonical = after
          .map((r) => r.asMember)
          .whereType<Constructor>()
          .where((c) => c.enclosingClass.isAnonymousMixin)
          .toList();
      expect(canonical, hasLength(3));
      expect(canonical.map((c) => c.name.text).toSet(), {
        '',
        'named',
        '_private',
      });

      final sink = BytesSink();
      BinaryPrinter(sink).writeComponentFile(component);
      final restored = Component();
      BinaryBuilder(sink.builder.takeBytes()).readComponent(restored);
      expect(
        restored.uriToSource[source]!.constantCoverageConstructors,
        hasLength(after.length),
      );
      expect(
        restored.uriToSource[source]!.constantCoverageConstructors!.where(
          (r) => r.asMember is Procedure,
        ),
        hasLength(2),
      );
    });

    final testCasesDir = Directory.fromUri(
      _pkgVmDir.resolve('testcases/transformations/mixin_deduplication'),
    );

    for (var entry
        in testCasesDir
            .listSync(recursive: true, followLinks: false)
            .reversed) {
      if (entry.path.endsWith(".dart") && !entry.path.contains('helper')) {
        test(entry.path, () => runTestCase(entry.uri));
      }
    }
  });
}
