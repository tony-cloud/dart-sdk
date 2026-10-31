// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';

import 'package:frontend_server/frontend_server.dart';
import 'package:kernel/target/targets.dart';
import 'package:test/test.dart';
import 'package:vm/modular/target/install.dart';

class _TargetCreated implements Exception {
  final TargetFlags flags;
  new(this.flags);
}

void main() {
  late Directory directory;
  late File source;
  setUp(() {
    directory = Directory.systemTemp.createTempSync('frontend-ios-marker-');
    source = File('${directory.path}/main.dart')
      ..writeAsStringSync('void main() {}');
    installAdditionalTargets();
  });
  tearDown(() => directory.deleteSync(recursive: true));

  Future<bool> compile(List<String> flags) => FrontendCompiler(StringBuffer())
      .compile(
        source.path,
        argParser.parse([
          '--sdk-root=${directory.path}',
          '--target=flutter',
          '--aot',
          '--no-tfa',
          ...flags,
        ]),
      );

  for (final full in [false, true]) {
    test('frontend_server passes marker=$full for explicit full iOS', () async {
      final previous = targets['flutter']!;
      targets['flutter'] = (flags) => throw _TargetCreated(flags);
      addTearDown(() => targets['flutter'] = previous);
      await expectLater(
        compile(['--target-os=ios', if (full) '--shorebird-full-replacement']),
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
    test('frontend_server rejects full Flutter target OS $os', () async {
      expect(
        await compile([
          '--shorebird-full-replacement',
          if (os != null) '--target-os=$os',
        ]),
        isFalse,
      );
    });
  }
}
