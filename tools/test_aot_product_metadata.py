#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Execute evaluated pragma and FFI signature conversion in a PRODUCT VM.

Build product_metadata_test and gen_snapshot_product with the Shorebird
interpreter first. This prerequisite test introduces annotated Dart declarations,
not executable native calls; the production FFI rejection gates remain closed.
"""

import argparse
import contextlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

BASE = r'''
@pragma('vm:entry-point')
@pragma('test:metadata', 'baseline')
int oldAnnotated() => 1;
void main() {}
'''

UPDATED = BASE + r'''
@pragma('test:metadata', 'update')
class Annotated {
  @pragma('test:metadata', 'update')
  @FakePragma()
  int value = 42;
}
class FakePragma {
  final String name = 'test:spoof';
  final Object? options = 'spoof';
  const FakePragma();
}
@pragma('test:metadata', 'update')
@pragma('vm:ffi:native', Native<IntPtr Function(IntPtr)>(isLeaf: true))
@FakePragma()
int newAnnotated(int value) => value;
int firstAsFunction(Pointer<NativeFunction<IntPtr Function(IntPtr)>> pointer,
                    int value) =>
    pointer.asFunction<int Function(int)>(isLeaf: true)(value);
'''

FULL_BASE = BASE + r'''
@pragma('test:metadata', 'baseline')
class Annotated {
  @pragma('test:metadata', 'baseline')
  int value = 42;
}
@pragma('test:metadata', 'baseline')
int newAnnotated(int value) => value;
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    dart = build / 'dart'
    platform = build / 'vm_platform_product.dill'
    gen_snapshot = build / 'gen_snapshot_product'
    test = build / 'product_metadata_test'
    for artifact in (dart, platform, gen_snapshot, test):
        if not artifact.is_file():
            parser.error(f'Missing required artifact: {artifact}')

    def run(*command):
        print('+', *command, flush=True)
        subprocess.run([str(part) for part in command], check=True, cwd=root)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else tempfile.TemporaryDirectory(
                     prefix='aot-product-metadata-'))
    with workspace as directory:
        work = Path(directory)
        source = work / 'main.dart'
        source.write_text(BASE)

        def snapshot(name, profile):
            kernel = work / f'{name}.dill'
            output = work / f'{name}.snapshot'
            run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                '-Ddart.vm.product=true', f'--platform={platform}',
                *profile, f'--output={kernel}', source)
            native_profile = [option for option in profile if option in (
                '--shorebird-structural-patching', '--shorebird-full-replacement')]
            if sys.platform == 'darwin':
                run(gen_snapshot, '--snapshot-kind=app-aot-macho-dylib',
                    *native_profile, f'--macho={output}', kernel)
                run('codesign', '--force', '--sign', '-', output)
            else:
                run(gen_snapshot, '--snapshot-kind=app-aot-elf',
                    *native_profile, f'--elf={output}', kernel)
            return output

        ordinary = snapshot('ordinary', [])
        run(test, ordinary, '--ordinary')
        # The body/raw patch profile has no blanket structural SDK export.
        # Required frozen class/field lookups must survive PRODUCT pruning on
        # their explicit roots alone.
        raw_baseline = snapshot('raw-baseline', ['--no-tfa'])
        run(test, raw_baseline, '--baseline')
        baseline = snapshot('baseline', ['--no-tfa', '--shorebird-structural-patching'])
        source.write_text("import 'dart:ffi';\n" + UPDATED)
        patch = work / 'update.bytecode'
        run(dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
            '-Ddart.vm.product=true', f'--platform={platform}',
            '--shorebird-structural-patching', '-o', patch, source)
        run(test, baseline, patch)

        # The annotations section is the final section. Replace every packed
        # object header with the valid null reference (1), preserving offsets
        # and all other declarations. Any annotated declaration must reject it.
        data = bytearray(patch.read_bytes())
        annotations_count, annotations_offset = struct.unpack_from('<II', data, 8 + 13 * 8)
        if annotations_count == 0 or not 0 < annotations_offset < len(data):
            raise AssertionError('Fixture has no annotation section')
        data[annotations_offset:] = bytes([1]) * (len(data) - annotations_offset)
        malformed = work / 'malformed.bytecode'
        malformed.write_bytes(data)
        run(test, baseline, malformed, '--reject')

        # Full replacement installs either bundle into a new isolate group.
        # Keep the fixture free of FFI imports so the existing admission gates
        # remain effective while exercising fresh application identities.
        source.write_text(FULL_BASE)
        full_profile = ['--shorebird-full-replacement',
                        '--shorebird-host-abi=' + 'a' * 64]
        full_bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                         '-Ddart.vm.product=true', f'--platform={platform}',
                         *full_profile]
        full_base = work / 'base.sbr'
        run(*full_bytecode, '-o', full_base, source)
        full_snapshot = snapshot('full', ['--no-tfa', *full_profile,
                                         f'--shorebird-replacement-base={full_base}'])
        run(test, full_snapshot, full_base, '--full-base')
        source.write_text(FULL_BASE.replace("'baseline'", "'update'"))
        full_update = work / 'update.sbr'
        run(*full_bytecode, f'--patch-base={full_base}', '-o', full_update, source)
        run(test, full_snapshot, full_update, '--full-update')
        print('PRODUCT metadata, frozen SDK identity, IntPtr/signature conversion, '
              'ordinary AOT pruning, malformed annotation rejection and full '
              'fresh-domain baseline/update pragmas passed.')


if __name__ == '__main__':
    main()
