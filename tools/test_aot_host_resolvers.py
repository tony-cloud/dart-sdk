#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Verify embedder resolver ownership across full replacement in PRODUCT.

Build product_metadata_test and gen_snapshot_product first. This test registers
and inspects callback addresses only; it never executes native/FFI functions or
opens the full-replacement FFI admission gates.
"""

import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import tempfile

from test_aot_full_replacement import MALFORMED_PRODUCER


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
        subprocess.run([str(part) for part in command], check=True, cwd=root,
                       timeout=300)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else tempfile.TemporaryDirectory(
                     prefix='aot-host-resolvers-'))
    with workspace as directory:
        work = Path(directory)
        source = work / 'main.dart'
        shared = work / 'shared.dart'
        removed = work / 'removed.dart'
        added = work / 'added.dart'
        shared.write_text('library shared;\n')
        removed.write_text('library removed;\n')
        source.write_text("import 'shared.dart';\nimport 'removed.dart';\n"
                          'void main() {}\n')
        profile = ['--shorebird-full-replacement',
                   '--shorebird-host-abi=' + 'a' * 64]
        bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                    '-Ddart.vm.product=true', f'--platform={platform}', *profile]
        baseline = work / 'base.sbr'
        run(*bytecode, '-o', baseline, source)
        kernel = work / 'base.dill'
        snapshot = work / 'base.snapshot'
        run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot', '--no-tfa',
            '-Ddart.vm.product=true', f'--platform={platform}', *profile,
            f'--shorebird-replacement-base={baseline}', f'--output={kernel}',
            source)
        if sys.platform == 'darwin':
            run(gen_snapshot, '--snapshot-kind=app-aot-macho-dylib',
                '--shorebird-full-replacement', f'--macho={snapshot}', kernel)
            run('codesign', '--force', '--sign', '-', snapshot)
        else:
            run(gen_snapshot, '--snapshot-kind=app-aot-elf',
                '--shorebird-full-replacement', f'--elf={snapshot}', kernel)
        added.write_text('library added;\n')
        removed.unlink()
        source.write_text("import 'shared.dart';\nimport 'added.dart';\n"
                          'void main() {}\n')
        update = work / 'update.sbr'
        run(*bytecode, f'--patch-base={baseline}', '-o', update, source)
        producer = work / 'malformed.dart'
        producer.write_text(MALFORMED_PRODUCER)
        malformed = work / 'malformed.sbr'
        run(dart, f'--packages={root / ".dart_tool/package_config.json"}',
            producer, update, malformed)
        for payload, mode in ((baseline, 'base'), (update, 'update'),
                              (malformed, 'poison')):
            run(test, snapshot, payload, '--resolvers', shared.as_uri(),
                removed.as_uri(), added.as_uri(), mode)
        print('PRODUCT host resolver baseline/update, stale handles, poisoned '
              'group and isolate-group isolation checks passed.')


if __name__ == '__main__':
    main()
