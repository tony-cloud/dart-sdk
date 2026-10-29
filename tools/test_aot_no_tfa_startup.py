#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Run no-TFA AOT programs through the standalone runtime's full bootstrap.

Build dart, gen_snapshot_product, dartaotruntime_product, and vm_platform_product
first. PRODUCT dictionary pruning and the full SDK initialization (such as
_setWorkingDirectory) must both run: direct API entry point calls miss these
startup failures.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


PROGRAM = r'''
import 'dart:async';
import 'dart:convert';
import 'dart:io';

enum LicenseType { free, pro, enterprise }

LicenseType currentLicenseType() => LicenseType.free;
bool get proFeatureEnabled => currentLicenseType() == LicenseType.pro ||
    currentLicenseType() == LicenseType.enterprise;

@pragma('vm:never-inline')
@pragma('vm:entry-point')
String currentLicenseNameForPatch() => currentLicenseType().name;

@pragma('vm:never-inline')
@pragma('vm:entry-point')
bool currentProFeatureEnabledForPatch() => proFeatureEnabled;

@pragma('vm:never-inline')
String readLicense() =>
    Function.apply(currentLicenseNameForPatch, const <Object?>[]) as String;

@pragma('vm:never-inline')
bool readFeature() =>
    Function.apply(currentProFeatureEnabledForPatch, const <Object?>[]) as bool;

@pragma('vm:never-inline')
String splitAndJoin(String value) => value.split('/').join(':');

class Label {
  @override
  String toString() => 'label';
}

@pragma('vm:never-inline')
String describe(Object value) => value.toString();

void check(bool value, String name) {
  if (!value) throw StateError(name);
}

Future<void> main() async {
  check(readLicense() == 'free' && !readFeature(), 'Function.apply');
  check(Uri.directory('/tmp').path == '/tmp/', 'Uri.directory');
  check(Directory.current.path.isNotEmpty, 'Directory.current');
  check(splitAndJoin('a/b/c') == 'a:b:c', 'String.split');
  check(splitAndJoin('a/\u4e2d/\u{1f600}') == 'a:\u4e2d:\u{1f600}',
      'String.split with another receiver class');
  check(describe(Label()) == 'label', 'SDK interface to app override');
  final values = <int>[1, 2, 3];
  check(values.map((value) => value + 1).join(',') == '2,3,4', 'Iterable');
  final decoded = jsonDecode(jsonEncode({'values': values})) as Map;
  check((decoded['values'] as List).length == 3, 'JSON');
  check(StateError('probe').toString().contains('probe'), 'Error.toString');
  check(await Future<int>.value(7).then((value) => value + 1) == 8, 'Future');
  print('CORE_AND_APPLY_PASSED');
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    root = Path(__file__).resolve().parents[1]
    if sys.platform not in ('darwin', 'linux'):
        raise SystemExit('This standalone AOT test supports macOS/Linux hosts.')

    with tempfile.TemporaryDirectory(prefix='dart-no-tfa-startup-') as tmp:
        work = Path(tmp)
        empty = work / 'empty.dart'
        empty.write_text("void main() { print('EMPTY_MAIN_PASSED'); }\n")
        core = work / 'core.dart'
        core.write_text(PROGRAM)

        def run(*command):
            return subprocess.run([str(x) for x in command], cwd=root,
                                  check=True, capture_output=True, text=True,
                                  timeout=120)

        for name, source, flags, expected in (
                ('empty', empty, ['--no-tfa'], 'EMPTY_MAIN_PASSED'),
                ('body', core, ['--no-tfa'], 'CORE_AND_APPLY_PASSED'),
                ('structural', core, ['--no-tfa', '--shorebird-structural-patching'],
                 'CORE_AND_APPLY_PASSED'),
                ('tfa', core, [], 'CORE_AND_APPLY_PASSED')):
            kernel = work / f'{name}.dill'
            snapshot = work / f'{name}.aot'
            try:
                run(build / 'dart', root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                    '-Ddart.vm.product=true', *flags,
                    f'--platform={build / "vm_platform_product.dill"}',
                    f'--output={kernel}', source)
                snapshot_flags = (['--shorebird-structural-patching']
                                  if name == 'structural' else [])
                if sys.platform == 'darwin':
                    run(build / 'gen_snapshot_product',
                        '--snapshot-kind=app-aot-macho-dylib', *snapshot_flags,
                        f'--macho={snapshot}', kernel)
                    run('codesign', '--force', '--sign', '-', snapshot)
                else:
                    run(build / 'gen_snapshot_product',
                        '--snapshot-kind=app-aot-elf', *snapshot_flags,
                        f'--elf={snapshot}', kernel)
                result = run(build / 'dartaotruntime_product', snapshot)
                if result.stdout.strip() != expected:
                    raise AssertionError(f'{name}: unexpected output {result.stdout!r}')
            except subprocess.CalledProcessError as error:
                print(f'{name}: {error.stdout}{error.stderr}', file=sys.stderr)
                raise
            print(f'{name}: standalone AOT startup passed', flush=True)


if __name__ == '__main__':
    main()
