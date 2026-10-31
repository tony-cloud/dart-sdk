// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';

import 'package:dart2bytecode/dart2bytecode.dart';
import 'package:kernel/target/targets.dart';
import 'package:test/test.dart';
import 'package:vm/kernel_front_end.dart' show badUsageExitCode;
import 'package:vm/modular/target/install.dart';

class _TargetCreated implements Exception {
  final TargetFlags flags;
  _TargetCreated(this.flags);
}

void main() {
  late Directory directory;
  late File source;
  setUp(() {
    directory = Directory.systemTemp.createTempSync('bytecode-ios-marker-');
    source = File('${directory.path}/main.dart')
      ..writeAsStringSync('void main() {}');
    installAdditionalTargets();
  });
  tearDown(() => directory.deleteSync(recursive: true));

  Future<int> compile(List<String> flags) =>
      runCompilerWithCommandLineArguments([
        '--platform=unused.dill',
        '--target=flutter',
        ...flags,
        source.path,
      ]);

  for (final full in [false, true]) {
    test('dart2bytecode passes marker=$full for explicit full iOS', () async {
      final previous = targets['flutter']!;
      targets['flutter'] = (flags) => throw _TargetCreated(flags);
      addTearDown(() => targets['flutter'] = previous);
      await expectLater(
        compile([
          '--target-os=ios',
          if (full) '--shorebird-full-replacement',
          if (full) '--shorebird-host-abi=${'0' * 64}',
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
    test('dart2bytecode rejects full Flutter target OS $os', () async {
      expect(
        await compile([
          '--shorebird-full-replacement',
          '--shorebird-host-abi=${'0' * 64}',
          if (os != null) '--target-os=$os',
        ]),
        badUsageExitCode,
      );
    });
  }
}
