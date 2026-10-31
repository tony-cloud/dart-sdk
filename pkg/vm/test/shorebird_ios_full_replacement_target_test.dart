// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';

import 'package:kernel/ast.dart';
import 'package:kernel/target/targets.dart';
import 'package:test/test.dart';
import 'package:vm/kernel_front_end.dart';
import 'package:vm/modular/target/flutter.dart';
import 'package:vm/modular/target/install.dart';

import 'common_test_utils.dart';

const marker = '_shorebird_ios_full_replacement';

// The host VM platform does not include dart:ui. Keep the real Flutter library
// support policy while compiling the small conditional-import fixture.
class _HostPlatformFlutterTarget extends FlutterTarget {
  _HostPlatformFlutterTarget(super.flags);

  @override
  List<String> get extraRequiredLibraries => const [];
}

class _TargetCreated implements Exception {
  final TargetFlags flags;
  _TargetCreated(this.flags);
}

void main() {
  bool supported(Target target, String library) => target.dartLibrarySupport
      .computeDartLibrarySupport(library, isSupportedBySpec: false);

  test('marker flags default off and participate in target identity', () {
    const ordinary = TargetFlags();
    const enabled = TargetFlags(shorebirdIosFullReplacement: true);
    expect(ordinary.shorebirdIosFullReplacement, isFalse);
    expect(enabled, isNot(ordinary));
    expect(enabled.hashCode, isNot(ordinary.hashCode));
    expect(enabled, const TargetFlags(shorebirdIosFullReplacement: true));
    expect(supported(FlutterTarget(ordinary), marker), isFalse);
    expect(supported(FlutterTarget(enabled), marker), isTrue);
    expect(
      supported(
        createFrontEndTarget('vm', shorebirdIosFullReplacement: true)!,
        marker,
      ),
      isFalse,
    );
    expect(
      FlutterTarget(enabled).dartLibrarySupport.computeDartLibrarySupport(
        'ffi',
        isSupportedBySpec: true,
      ),
      isTrue,
    );
  });

  for (final enabled in [false, true]) {
    test(
      'conditional import and environment bool agree when marker=$enabled',
      () async {
        final directory = Directory.systemTemp.createTempSync('ios-marker-');
        addTearDown(() => directory.deleteSync(recursive: true));
        File(
          '${directory.path}/fallback.dart',
        ).writeAsStringSync('const selected = 0;');
        File(
          '${directory.path}/ios.dart',
        ).writeAsStringSync('const selected = 1;');
        final source = File('${directory.path}/main.dart')
          ..writeAsStringSync('''
import 'fallback.dart'
    if (dart.library._shorebird_ios_full_replacement) 'ios.dart';
const enabled = bool.fromEnvironment('dart.library._shorebird_ios_full_replacement');
const markerValue = String.fromEnvironment('dart.library._shorebird_ios_full_replacement', defaultValue: 'absent');
const hasMarker = bool.hasEnvironment('dart.library._shorebird_ios_full_replacement');
const unknownValue = String.fromEnvironment('dart.library._missing_test_library', defaultValue: 'absent');
const unknownDefault = bool.fromEnvironment('dart.library._missing_test_library', defaultValue: true);
const hasUnknown = bool.hasEnvironment('dart.library._missing_test_library');
const coreAvailable = bool.fromEnvironment('dart.library.core');
const mirrorsAvailable = bool.fromEnvironment('dart.library.mirrors');
const ffiAvailable = bool.fromEnvironment('dart.library.ffi');
void main() { print(selected); print(enabled); }
''');
        final component = await compileTestCaseToKernelProgram(
          source.uri,
          target: _HostPlatformFlutterTarget(
            TargetFlags(shorebirdIosFullReplacement: enabled),
          ),
          environmentDefines: {
            'dart.library.$marker': '${!enabled}',
            'dart.library._missing_test_library': 'true',
            'dart.library.mirrors': 'true',
          },
        );
        final main = component.mainMethod!.enclosingLibrary;
        final selected = main.dependencies
            .where((d) => !d.targetLibrary.importUri.isScheme("dart"))
            .single
            .targetLibrary
            .importUri;
        expect(
          selected,
          directory.uri.resolve(enabled ? 'ios.dart' : 'fallback.dart'),
        );
        Constant value(String name) =>
            (main.fields
                        .singleWhere((field) => field.name.text == name)
                        .initializer
                    as ConstantExpression)
                .constant;
        expect((value('enabled') as BoolConstant).value, enabled);
        expect(
          (value('markerValue') as StringConstant).value,
          enabled ? 'true' : 'absent',
        );
        expect((value('hasMarker') as BoolConstant).value, enabled);
        expect((value('unknownValue') as StringConstant).value, 'absent');
        expect((value('unknownDefault') as BoolConstant).value, isTrue);
        expect((value('hasUnknown') as BoolConstant).value, isFalse);
        expect((value('coreAvailable') as BoolConstant).value, isTrue);
        expect((value('mirrorsAvailable') as BoolConstant).value, isFalse);
        expect((value('ffiAvailable') as BoolConstant).value, isTrue);
      },
    );
  }

  group('gen_kernel profile propagation', () {
    late Directory directory;
    late File source;
    setUp(() {
      directory = Directory.systemTemp.createTempSync('native-ios-marker-');
      source = File('${directory.path}/main.dart')
        ..writeAsStringSync('void main() {}');
      installAdditionalTargets();
    });
    tearDown(() => directory.deleteSync(recursive: true));

    Future<int> compile(List<String> flags) {
      final options = createCompilerArgParser().parse([
        '--platform=unused.dill',
        '--target=flutter',
        '--aot',
        '--no-tfa',
        ...flags,
        source.path,
      ]);
      return runCompiler(options, '');
    }

    for (final full in [false, true]) {
      test('passes marker=$full only for explicit full iOS', () async {
        final previous = targets['flutter']!;
        targets['flutter'] = (flags) => throw _TargetCreated(flags);
        addTearDown(() => targets['flutter'] = previous);
        await expectLater(
          compile([
            '--target-os=ios',
            if (full) '--shorebird-full-replacement',
          ]),
          throwsA(
            isA<_TargetCreated>().having(
              (created) => created.flags.shorebirdIosFullReplacement,
              'iOS marker',
              full,
            ),
          ),
        );
      });
    }
    for (final os in [null, 'android', 'macos']) {
      test('rejects full Flutter without explicit iOS ($os)', () async {
        expect(
          await compile([
            '--shorebird-full-replacement',
            if (os != null) '--target-os=$os',
          ]),
          badUsageExitCode,
        );
      });
    }
  });
}
