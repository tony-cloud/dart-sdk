#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Exercise runtime-created callbacks in native ARM64 PRODUCT/precompiled VM.

A structural native baseline has no application callbacks or FFI. A new URI
ordinary bytecode library introduces its targets/signatures. Separate compiled
controls and child termination runs use the same signed native consumers. This
establishes no full-domain admission, deterministic allocation-failure injection,
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
    parser.add_argument('--transitions-only', action='store_true', help='Generate fixtures and run all eight mandatory isolated compiled/interpreted transition children')
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

        def run(*command, log=None, required=(), expect_failure=False):
            print('+', *command, flush=True)
            try:
                result = subprocess.run([str(part) for part in command], cwd=root,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=600)
            except subprocess.TimeoutExpired as error:
                output = error.stdout or ''
                if isinstance(output, bytes):
                    output = output.decode(errors='replace')
                output += '\nMANDATORY CHILD TIMED OUT\n'
                if log is not None:
                    log.write_text(output)
                print(output, end='', flush=True)
                raise
            if log is not None:
                log.write_text(result.stdout)
            print(result.stdout, end='', flush=True)
            if expect_failure:
                if result.returncode == 0:
                    raise AssertionError('Expected subprocess death did not occur')
            else:
                result.check_returncode()
            for marker in required:
                if marker not in result.stdout:
                    raise AssertionError(f'Missing acceptance marker: {marker}')
            return result.stdout

        def snapshot(kernel, output, structural=False, shared=False):
            profile = ['--shorebird-structural-patching'] if structural else []
            if shared:
                profile += ['--experimental-shared-data']
            if sys.platform == 'darwin':
                run(gen_snapshot, '--snapshot-kind=app-aot-macho-dylib', *profile,
                    f'--macho={output}', kernel)
                run('codesign', '--force', '--sign', '-', output)
            else:
                run(gen_snapshot, '--snapshot-kind=app-aot-elf', *profile,
                    f'--elf={output}', kernel)

        def kernel(source, output, structural=False, shared=False):
            profile = ['--no-tfa', '--shorebird-structural-patching'] if structural else []
            run(dart, *(['--experimental-shared-data'] if shared else []), root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                '-Ddart.vm.product=true', f'--platform={platform_file}', *profile,
                f'--output={output}', source)

        for source in fixtures.iterdir():
            if source.is_file():
                shutil.copyfile(source, work / source.name)
        library = work / ('libcallback_fixture.dylib' if sys.platform == 'darwin' else 'libcallback_fixture.so')
        run(compiler, '-shared', '-fPIC', '-O0', '-fno-omit-frame-pointer',
            '-I', root / 'runtime/include', work / 'fixture.c',
            work / 'aggregate_fixture.c', work / 'aggregate_fixture.S',
            work / 'async_group_fixture.c', '-pthread', '-o', library,
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
        for source in ('callbacks', 'kill', 'aggregates', 'handle_unwind', 'async_group'):
            run(dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                '-Ddart.vm.product=true', f'--platform={platform_file}',
                '-o', work / f'{source}.bytecode', work / f'{source}.dart')
        # Compiled controls deliberately use another snapshot, so the positive
        # raw-library baseline stays free of application callback signatures.
        kernel(work / 'compiled_controls.dart', work / 'compiled_controls.dill')
        compiled = work / 'compiled_controls.snapshot'
        snapshot(work / 'compiled_controls.dill', compiled)
        # The missing-ABI signature is deliberately a runtime preparation
        # rejection. AOT compilation rejects it before a runnable snapshot can
        # exist, so it is excluded from the otherwise identical compiled control.
        async_source = (work / 'async_group.dart').read_text()
        negative_start = async_source.index('@AbiSpecificIntegerMapping')
        negative_end = async_source.index(
            "@pragma('vm:entry-point')\nvoid closePoolGroups()", negative_start)
        control_source = async_source[:negative_start] + async_source[negative_end:]
        for entrypoint in ('prepare', 'createRoute', 'createPool',
                           'closePoolGroups', 'closeListeners', 'createSelfClose',
                           'exportGroups', 'closeExported'):
            if f'{entrypoint}(' not in control_source:
                raise AssertionError(f'Compiled control lost positive entrypoint: {entrypoint}')
        (work / 'async_group_control.dart').write_text(control_source)
        kernel(work / 'async_group_control.dart', work / 'async_group.dill')
        async_compiled = work / 'async_group.snapshot'
        snapshot(work / 'async_group.dill', async_compiled)
        # Shared access is exercised only under the existing experimental flag,
        # in independent runtime/compiled processes and matching snapshots.
        kernel(work / 'baseline.dart', work / 'shared-baseline.dill', structural=True, shared=True)
        shared_baseline = work / 'shared-baseline.snapshot'
        snapshot(work / 'shared-baseline.dill', shared_baseline, structural=True, shared=True)
        kernel(work / 'async_shared.dart', work / 'async_shared.dill', shared=True)
        shared_compiled = work / 'async_shared.snapshot'
        snapshot(work / 'async_shared.dill', shared_compiled, shared=True)
        run(dart, '--experimental-shared-data', root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
            '-Ddart.vm.product=true', f'--platform={platform_file}',
            '-o', work / 'async_shared.bytecode', work / 'async_shared.dart')
        start, end, image_base = common_entry_range(harness)
        proof = (start, end, image_base)
        # Collect ALL isolated route failures before failing preflight. A crash
        # in one inherited transition cannot suppress the independent controls.
        failures = []
        for interpreted in (False, True):
            label = 'runtime' if interpreted else 'control'
            for route in range(4):
                mode = f'async-{label}-{route}'
                try:
                    run(harness, baseline if interpreted else async_compiled,
                        work / 'async_group.bytecode' if interpreted else '-', library,
                        (work / 'async_group.dart').as_uri(), *proof, mode,
                        log=work / f'{mode}.log', required=(f'NESTED_ROUTE_PASS:{route}', f'EMPTY_SOURCE_PASS:{route}'))
                except (subprocess.SubprocessError, AssertionError) as error:
                    failures.append(f'{mode}: {error}')
        if failures:
            raise AssertionError('Mandatory callback transition preflight failed:\n' + '\n'.join(failures))
        print('All eight isolated compiled/interpreted callback transition children passed')
        if args.transitions_only:
            return
        for interpreted in (False, True):
            label = 'runtime' if interpreted else 'control'
            run(harness, baseline if interpreted else async_compiled,
                work / 'async_group.bytecode' if interpreted else '-', library,
                (work / 'async_group.dart').as_uri(), *proof, f'async-{label}',
                log=work / f'async-{label}.log', required=(
                    'PRODUCT listener/group deferred/Zone/VarArgs/pthread/copy/GC/transitions/lifecycle checks passed',
                    'Callback source/alias byte identity and RX/RW checkpoints passed',
                    'STATIC_FRAME_FALLBACK:0:-1911',
                    'STATIC_FRAME_FALLBACK:1:-1912',
                    'STATIC_FRAME_FALLBACK:2:-1913',
                    'STATIC_ACCESS_OWNER_AND_INITIALIZER_PASS',
                    *(('STATIC_FRAME_BYTECODE:ordinaryTarget',
                       'STATIC_FRAME_BYTECODE:ordinaryStoreTarget',
                       'STATIC_FRAME_BYTECODE:get:ordinaryGetter',
                       'STATIC_FRAME_BYTECODE:set:ordinarySetter') if interpreted else ()),
                ))
            run(harness, shared_baseline if interpreted else shared_compiled,
                work / 'async_shared.bytecode' if interpreted else '-', library,
                (work / 'async_shared.dart').as_uri(), *proof, f'async-shared-{label}',
                log=work / f'async-shared-{label}.log', required=(
                    'PRODUCT group callback flagged shared-field access passed',
                    'Callback source/alias byte identity and RX/RW checkpoints passed',
                ))
        run(harness, baseline, work / 'callbacks.bytecode', library,
            (work / 'callbacks.dart').as_uri(), *proof, 'runtime',
            log=work / 'runtime.log', required=(
                'PRODUCT interpreted static/capture scalar/pointer/VarArgs, GC, nesting, self-close, growth/reuse, rejection cleanup and owned-isolate callback checks passed',
                'Callback source/alias byte identity and RX/RW checkpoints passed',
            ))
        run(harness, baseline, work / 'aggregates.bytecode', library,
            (work / 'aggregates.dart').as_uri(), *proof, 'aggregate',
            log=work / 'aggregates.log', required=(
                'PRODUCT callback aggregate layouts/copy/aliases/offsets/guard pages/zero fallbacks passed',
                'PRODUCT callback Handle caller-scope/error/GC/nesting checks passed with embedding scope',
                'PRODUCT callback Handle caller-scope/error/GC/nesting checks passed with no embedding scope',
                'PRODUCT callback Handle+aggregate descriptor/ownership/direct-scope/cleanup checks passed',
                'Callback source/alias byte identity and RX/RW checkpoints passed',
            ))
        missing = run(harness, baseline, work / 'aggregates.bytecode', library,
                      (work / 'aggregates.dart').as_uri(), *proof, 'missing-scope',
                      log=work / 'missing-handle-scope.log', expect_failure=True,
                      required=('A Handle-returning native callback expects a current API scope',))
        if 'FORBIDDEN_HANDLE_TARGET_WITHOUT_SCOPE' in missing:
            raise AssertionError('Handle-result callback target executed without an owning scope')
        handle_unwind = run(harness, baseline, work / 'handle_unwind.bytecode', library,
                            (work / 'handle_unwind.dart').as_uri(), *proof, 'handle-result-kill',
                            log=work / 'handle-result-unwind.log',
                            required=('NATIVE_HANDLE_RESULT_UNWIND_EPILOGUE', 'API_UNWIND_OBSERVED'))
        if 'FORBIDDEN_OUTER_DART_AFTER_KILL' in handle_unwind:
            raise AssertionError('Handle-result callback resumed killed outer Dart continuation')
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
        print('PRODUCT runtime-created callback listener/group and local Handle/aggregate acceptance passed; '
              'full admission, deterministic allocation/OOM injection, '
              'transient instruction/permission audit and devices remain outside this proof')


if __name__ == '__main__':
    main()
