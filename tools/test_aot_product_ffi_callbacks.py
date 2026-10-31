#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Exercise runtime-created callbacks in native ARM64 PRODUCT/precompiled VM.

A structural native baseline has no application callbacks or FFI. A new URI
ordinary bytecode library introduces its targets/signatures. Separate compiled
controls and child termination runs use the same signed native consumers. This
establishes no full-domain admission, listener/Handle/aggregate runtime callback,
iOS, ARM64e, or transient executable permission/write-audit claim.
"""
import argparse
import contextlib
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile


def common_entry_range(binary):
    symbols = []
    start = None
    image_base = 0
    for line in subprocess.check_output(['nm', '-n', str(binary)], text=True).splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        if fields[2] == '__mh_execute_header':
            image_base = int(fields[0], 16)
        if fields[1] not in ('t', 'T'):
            continue
        address = int(fields[0], 16)
        symbols.append(address)
        if fields[2] in ('InterpreterFfiCallbackEntry', '_InterpreterFfiCallbackEntry'):
            start = address
    if start is None:
        raise AssertionError('PRODUCT test must retain its linked static callback entry symbol')
    return start, min(address for address in symbols if address > start), image_base


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--work-dir', type=Path)
    args = parser.parse_args()
    if platform.machine().lower() not in ('arm64', 'aarch64') or sys.platform not in ('darwin', 'linux'):
        parser.error('Native ARM64 macOS/Linux required')
    root = Path(__file__).resolve().parents[1]
    fixtures = root / 'tools/test_aot_product_ffi_callbacks'
    build = args.build_dir.resolve()
    dart = build / 'dart'
    gen_snapshot = build / 'gen_snapshot_product'
    platform_file = build / 'vm_platform_product.dill'
    harness = build / 'product_ffi_callback_test'
    compiler = shutil.which('clang')
    if compiler is None:
        parser.error('clang is required for the local native fixture')
    for required in (dart, gen_snapshot, platform_file, harness):
        if not required.is_file():
            parser.error(f'Missing artifact: {required}')
    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else tempfile.TemporaryDirectory(prefix='product-ffi-callbacks-'))
    with workspace as directory:
        work = Path(directory)

        def run(*command, log=None, required=()):
            print('+', *command, flush=True)
            result = subprocess.run([str(part) for part in command], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=600)
            if log is not None:
                log.write_text(result.stdout)
            print(result.stdout, end='', flush=True)
            result.check_returncode()
            for marker in required:
                if marker not in result.stdout:
                    raise AssertionError(f'Missing acceptance marker: {marker}')
            return result.stdout

        def snapshot(kernel, output, structural=False):
            profile = ['--shorebird-structural-patching'] if structural else []
            if sys.platform == 'darwin':
                run(gen_snapshot, '--snapshot-kind=app-aot-macho-dylib', *profile,
                    f'--macho={output}', kernel)
                run('codesign', '--force', '--sign', '-', output)
            else:
                run(gen_snapshot, '--snapshot-kind=app-aot-elf', *profile,
                    f'--elf={output}', kernel)

        def kernel(source, output, structural=False):
            profile = ['--no-tfa', '--shorebird-structural-patching'] if structural else []
            run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                '-Ddart.vm.product=true', f'--platform={platform_file}', *profile,
                f'--output={output}', source)

        for source in fixtures.iterdir():
            if source.is_file():
                shutil.copyfile(source, work / source.name)
        library = work / ('libcallback_fixture.dylib' if sys.platform == 'darwin' else 'libcallback_fixture.so')
        run(compiler, '-shared', '-fPIC', '-O0', '-fno-omit-frame-pointer',
            work / 'fixture.c', '-o', library,
            *(['-ldl'] if sys.platform == 'linux' else []))
        if sys.platform == 'darwin':
            run('codesign', '--force', '--sign', '-', library)
            entitlements = run('codesign', '-d', '--entitlements', ':-', harness)
            if 'com.apple.security.cs.disable-library-validation' not in entitlements:
                raise AssertionError('Host harness must carry its isolated library-validation entitlement')
            for forbidden in ('com.apple.security.cs.allow-jit', 'com.apple.security.cs.allow-unsigned-executable-memory', 'get-task-allow'):
                if forbidden in entitlements:
                    raise AssertionError(f'Unexpected harness entitlement: {forbidden}')
            run('codesign', '--verify', '--strict', harness)
        baseline = work / 'baseline.snapshot'
        kernel(work / 'baseline.dart', work / 'baseline.dill', structural=True)
        snapshot(work / 'baseline.dill', baseline, structural=True)
        for source in ('callbacks', 'kill'):
            run(dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                '-Ddart.vm.product=true', f'--platform={platform_file}',
                '-o', work / f'{source}.bytecode', work / f'{source}.dart')
        # Compiled controls deliberately use another snapshot, so the positive
        # raw-library baseline stays free of application callback signatures.
        kernel(work / 'compiled_controls.dart', work / 'compiled_controls.dill')
        compiled = work / 'compiled_controls.snapshot'
        snapshot(work / 'compiled_controls.dill', compiled)
        start, end, image_base = common_entry_range(harness)
        proof = (start, end, image_base)
        run(harness, baseline, work / 'callbacks.bytecode', library,
            (work / 'callbacks.dart').as_uri(), *proof, 'runtime',
            log=work / 'runtime.log', required=(
                'PRODUCT interpreted static/capture scalar/pointer/VarArgs, GC, nesting, self-close, growth/reuse, rejection cleanup and owned-isolate callback checks passed',
                'Callback source/alias byte identity and RX/RW checkpoints passed',
            ))
        run(harness, compiled, '-', library, '-', *proof, 'control',
            log=work / 'compiled-controls.log', required=(
                'Compiled static/capture/Handle/aggregate/listener/group-bound callback controls passed',
                'Callback source/alias byte identity and RX/RW checkpoints passed',
            ))
        for mode, marker in [('kill', 'NATIVE_CALLBACK_EPILOGUE:-19001'),
                             ('handle-kill', 'NATIVE_HANDLE_CALLBACK_EPILOGUE')]:
            output = run(harness, baseline, work / 'kill.bytecode', library,
                         (work / 'kill.dart').as_uri(), *proof, mode,
                         log=work / f'{mode}.log',
                         required=(marker, 'API_UNWIND_OBSERVED'))
            if 'FORBIDDEN_OUTER_DART_AFTER_KILL' in output:
                raise AssertionError('Killed callback resumed the outer Dart continuation')
        print('PRODUCT runtime-created callback first-slice acceptance passed; '
              'full admission, runtime Handle/aggregate/listener/group-bound callbacks, '
              'transient instruction/permission audit and devices remain outside this proof')


if __name__ == '__main__':
    main()
