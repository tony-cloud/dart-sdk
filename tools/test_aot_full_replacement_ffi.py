#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Actual native ARM64 PRODUCT full-domain FFI replacement acceptance.

Admission must already be implemented. A closed compiler/runtime gate is a
failure, never a skip. Build product_full_ffi_test separately; this driver does
not rebuild the SDK or change a gate. It compiles ONE released native fixture,
creates full N/B, then creates original-B-bound U, and installs B/U independently
in fresh groups of the same N. Encryption uses the existing full-replacement
test's external real CLI envelope producer contract. This is host evidence;
CLI release archive comparison, server signature verification and physical iOS
no-JIT/device acceptance remain independent gates.
"""

import argparse
import contextlib
import json
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

from test_aot_full_replacement import digest_artifact, host_abi


def load_mutation_manifest(path):
    """Validate the generator contract before starting native consumers."""
    manifest = json.loads(path.read_text())
    if not isinstance(manifest, dict) or manifest.get('format_version') != 1:
        raise ValueError('Unsupported admission mutation manifest')
    cases = manifest.get('cases')
    positives = manifest.get('positive_cases', [])
    if not isinstance(cases, list) or not cases or not isinstance(positives, list):
        raise ValueError('Mutation manifest requires nonempty rejection cases')
    names = set()
    for rejected, rows in ((True, cases), (False, positives)):
        for case in rows:
            if not isinstance(case, dict):
                raise ValueError('Invalid admission case entry')
            name = case.get('name')
            payload = case.get('path')
            if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', name):
                raise ValueError('Invalid admission case name')
            if name in names:
                raise ValueError(f'Duplicate admission case: {name}')
            names.add(name)
            if not isinstance(payload, str) or not Path(payload).is_absolute() or not Path(payload).is_file():
                raise ValueError(f'Missing absolute mutation payload: {name}')
            if rejected:
                diagnostic = case.get('diagnostic')
                if not isinstance(diagnostic, str) or len(diagnostic) < 10 or '\n' in diagnostic:
                    raise ValueError(f'Missing specific rejection diagnostic: {name}')
                if not isinstance(case.get('poisoned'), bool):
                    raise ValueError(f'Missing expected poison boundary: {name}')
    return cases, positives


def check_fixtures(fixtures):
    """Source consistency only: no Dart/compiler/native execution or PASS claim."""
    base = (fixtures / 'base/plugin.dart').read_text()
    update = (fixtures / 'update/plugin.dart').read_text()
    native = (fixtures / 'released_fixture.c').read_text()
    assert "import 'removed.dart'" in base
    assert "import 'added.dart'" in update
    assert 'this.increment = 3' in base and 'this.increment = 41' in update
    assert "symbol: 'FullRecordV1'" in base
    assert "symbol: 'FullRecordV2'" in update
    for source in ('NativeCallable', 'Pointer.fromFunction', 'PluginHfa',
                   'PluginInt', 'PluginWord', 'PluginPacked'):
        assert source not in base, f'Baseline accidentally warms {source}'
        assert source in update or source in (fixtures / 'update/added.dart').read_text()
    for source in ('isolateLocal', '.listener', 'isolateGroupBound',
                   'Native.addressOf', 'DynamicLibrary.open', 'VarArgs',
                   'bytes.address', 'Handle Function'):
        assert source in update, f'Update lost required route: {source}'
    for name in ('FullRecordV1', 'FullRecordV2', 'FullHfaVarArgs', 'FullThreads',
                 'FullResolverImplementation', 'FullAssetValue',
                 'FullProcessValue', 'FullDynamicValue', 'FullAddedImplementation'):
        assert name in native, f'Released host lacks {name}'
    for entrypoint in ('prepareStaticCallback', 'runSelfClose', 'callbackOwnerCount'):
        assert entrypoint in update, f'Lost callback cleanup checkpoint: {entrypoint}'
    for source in ('fullFfiBadOption', 'fullFfiBadSignature', 'fullFfiBadIdentity',
                   "@pragma('vm:ffi:native', 42)", "Native<int>",
                   'FakeNative<Int32 Function(Int32)>()'):
        assert source in update, f'Lost inert mutation metadata source: {source}'
    harness = (fixtures.parents[1] / 'runtime/bin/product_full_ffi_test.cc').read_text()
    for oracle in ('ExpectDeadCallbacks({self_pointer})',
                   'ExpectCallbackCounts(live_counts',
                   'ExpectCallbackCounts(static_counts',
                   'publication.CheckUnpublished(poisoned)',
                   'Full replacement installation already attempted/late.',
                   'FULL_FFI_CLEANUP:dead-metadata-roots-list-ports-keepalive-static-survivor'):
        assert oracle in harness, f'Lost native callback cleanup oracle: {oracle}'
    forbidden = fixtures / 'forbidden'
    assert len(list(forbidden.glob('*.dart'))) == 3
    print('Static fixture consistency checked; nothing compiled or executed.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--envelope-producer', type=Path)
    parser.add_argument('--envelope-dart', type=Path)
    parser.add_argument('--envelope-packages', type=Path)
    parser.add_argument('--check-fixtures', action='store_true',
                        help='Only check source consistency; requires no SDK build')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixtures = root / 'tools/test_aot_full_replacement_ffi'
    check_fixtures(fixtures)
    if args.check_fixtures:
        return
    if platform.machine().lower() not in ('arm64', 'aarch64') or sys.platform not in ('darwin', 'linux'):
        parser.error('Execution requires native ARM64 macOS/Linux, never simulator/x64')
    if args.build_dir is None:
        parser.error('--build-dir is required for execution')
    for name in ('envelope_producer', 'envelope_dart', 'envelope_packages'):
        value = getattr(args, name)
        if value is None or not value.resolve().is_file():
            parser.error(f'--{name.replace("_", "-")} must name an existing real producer artifact')
    build = args.build_dir.resolve()
    dart = build / 'dart'
    platform_file = build / 'vm_platform_product.dill'
    gen_snapshot = build / 'gen_snapshot_product'
    harness = build / 'product_full_ffi_test'
    compiler = shutil.which('clang')
    if compiler is None:
        parser.error('clang required to build the once-released native fixture')
    for artifact in (dart, platform_file, gen_snapshot, harness):
        if not artifact.is_file():
            parser.error(f'Missing artifact: {artifact}; wire/build harness separately')

    def run(*command, log=None, markers=(), reject=None):
        print('+', *command, flush=True)
        try:
            result = subprocess.run([str(value) for value in command], cwd=root,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, timeout=600)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ''
            if isinstance(output, bytes):
                output = output.decode(errors='replace')
            if log is not None:
                log.write_text(output + '\nACCEPTANCE TIMED OUT\n')
            raise
        if log is not None:
            log.write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        if reject is None:
            result.check_returncode()
        elif result.returncode == 0 or not any(text in result.stdout for text in reject):
            raise AssertionError(f'Expected narrow rejection {reject}: {result.stdout}')
        for marker in markers:
            if marker not in result.stdout:
                raise AssertionError(f'Missing acceptance marker: {marker}')
        return result.stdout

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else tempfile.TemporaryDirectory(prefix='full-ffi-'))
    with workspace as directory:
        work = Path(directory)
        plugin = work / 'full_ffi_plugin/lib'
        plugin.mkdir(parents=True, exist_ok=True)
        for source in (fixtures / 'base').glob('*.dart'):
            shutil.copyfile(source, plugin / source.name)
        source = work / 'main.dart'
        shutil.copyfile(fixtures / 'main.dart', source)
        packages = work / 'packages.json'
        packages.write_text(json.dumps({'configVersion': 2, 'packages': [{
            'name': 'full_ffi_plugin', 'rootUri': plugin.parent.as_uri() + '/',
            'packageUri': 'lib/', 'languageVersion': '3.8',
        }]}))
        native = work / ('libreleased_full_ffi.dylib' if sys.platform == 'darwin'
                         else 'libreleased_full_ffi.so')
        # All exports/callback consumers exist before B/N, including update-only
        # bindings. Never rebuild/replace this native binary after compiling N.
        run(compiler, '-shared', '-fPIC', '-O0', '-fno-omit-frame-pointer',
            '-I', root / 'runtime/include', fixtures / 'released_fixture.c',
            '-pthread', '-o', native, *(['-ldl'] if sys.platform == 'linux' else []))
        if sys.platform == 'darwin':
            run('codesign', '--force', '--sign', '-', native)
            entitlements = run('codesign', '-d', '--entitlements', ':-', harness)
            if 'com.apple.security.cs.disable-library-validation' not in entitlements:
                raise AssertionError('Use isolated host-test library-validation entitlement')
            for forbidden in ('com.apple.security.cs.allow-jit',
                              'com.apple.security.cs.allow-unsigned-executable-memory',
                              'get-task-allow'):
                if forbidden in entitlements:
                    raise AssertionError(f'Forbidden harness entitlement: {forbidden}')
            run('codesign', '--verify', '--strict', harness)
        released_hash = digest_artifact(native)
        assets = work / 'released-native-assets.json'
        assets.write_text(json.dumps({'format-version': [1, 0, 0], 'native-assets': {
            'macos_arm64' if sys.platform == 'darwin' else 'linux_arm64': {
                'full-release-asset': ['absolute', str(native)],
            },
        }}))
        abi = host_abi(harness, gen_snapshot, platform_file, True)
        profile = ['--shorebird-full-replacement', f'--shorebird-host-abi={abi}']
        common = [f'--packages={packages}', '-Ddart.vm.product=true',
                  f'--platform={platform_file}', *profile]
        bytecode = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart', *common]
        base = work / 'base.sbr'
        run(*bytecode, '-o', base, source, log=work / 'compile-base.log')
        kernel = work / 'native.dill'
        kernel_command = [dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                          '--no-tfa', *common, f'--shorebird-replacement-base={base}']
        run(*kernel_command, f'--native-assets={assets}', f'--output={kernel}', source)
        from_dill = work / 'native-from-dill.dill'
        run(*kernel_command, f'--from-dill={kernel}', f'--output={from_dill}', source)
        snapshots = []
        for label, input_kernel in (('source', kernel), ('from-dill', from_dill)):
            snapshot = work / f'native-{label}.snapshot'
            if sys.platform == 'darwin':
                run(gen_snapshot, '--snapshot-kind=app-aot-macho-dylib',
                    '--shorebird-full-replacement', f'--macho={snapshot}', input_kernel)
                run('codesign', '--force', '--sign', '-', snapshot)
            else:
                run(gen_snapshot, '--snapshot-kind=app-aot-elf',
                    '--shorebird-full-replacement', f'--elf={snapshot}', input_kernel)
            snapshots.append((label, snapshot))
        (plugin / 'removed.dart').unlink()
        for changed in (fixtures / 'update').glob('*.dart'):
            shutil.copyfile(changed, plugin / changed.name)
        update = work / 'update.sbr'
        run(*bytecode, f'--patch-base={base}', '-o', update, source,
            log=work / 'compile-update.log')
        mutation_generator = root / 'pkg/dart2bytecode/test/full_replacement_admission_mutations.dart'
        mutation_packages = root / '.dart_tool/package_config.json'
        for required in (mutation_generator, mutation_packages):
            if not required.is_file():
                raise AssertionError(f'Missing mandatory mutation producer dependency: {required}')
        mutation_dir = work / 'admission-mutations'
        run(dart, f'--packages={mutation_packages}', mutation_generator, update,
            mutation_dir, log=work / 'admission-mutations.log')
        mutation_cases, positive_cases = load_mutation_manifest(mutation_dir / 'cases.json')

        # These controls remain forbidden after ordinary FFI admission. Require
        # a diagnostic for the intended mechanism, never an arbitrary failure.
        negative_command = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                            '-Ddart.vm.product=true', f'--platform={platform_file}', *profile]
        for case, messages in (
            ('plain_external', ('External/native replacement definition',
                                'rejects external/native definitions')),
            ('old_native', ('External/native replacement definition',
                            'rejects external/native definitions')),
            ('native_wrapper', ('Unsupported replacement v1 import: dart:nativewrappers',
                                'Native wrapper replacement class')),
        ):
            output = work / f'forbidden-{case}.sbr'
            output.unlink(missing_ok=True)
            run(*negative_command, '-o', output,
                fixtures / f'forbidden/{case}.dart', reject=messages,
                log=work / f'forbidden-{case}.log')
            if output.exists():
                raise AssertionError(f'Rejected {case} produced a component')

        # Direct-loader tests use valid-inner-digest mutations to reach precise
        # admission checks. They establish neither envelope authentication nor
        # upstream signature verification; the encrypted positives stay below.
        for case in mutation_cases:
            boundary = 'poisoned' if case['poisoned'] else 'preflight'
            for label, snapshot in snapshots:
                marker = f'FULL_FFI_REJECT:{case["name"]}:{boundary}:fresh-B-recovery'
                run(harness, snapshot, case['path'], native, '--reject', base,
                    case['diagnostic'], boundary, case['name'], markers=(marker,),
                    log=work / f'{label}-reject-{case["name"]}.log')
                if digest_artifact(native) != released_hash:
                    raise AssertionError('Released native binary changed during rejection checks')

        payloads = [('base', 'base', base), ('update', 'update', update)]
        payloads.extend((case['name'], 'update', Path(case['path'])) for case in positive_cases)
        for payload_name, mode, payload in payloads:
            envelopes = work / f'envelopes-{payload_name}'
            # Exactly the established test_aot_full_replacement producer API.
            run(args.envelope_dart.resolve(),
                f'--packages={args.envelope_packages.resolve()}',
                args.envelope_producer.resolve(), base, payload, envelopes)
            for name in ('valid.json', 'tampered.json', 'expired.json'):
                if not (envelopes / name).is_file():
                    raise AssertionError(f'Envelope producer omitted {name}')
            for label, snapshot in snapshots:
                markers = [f'FULL_FFI_PASS:{mode}:fresh-group-0',
                           f'FULL_FFI_PASS:{mode}:fresh-group-1']
                if mode == 'update':
                    markers.append('FULL_FFI_CALLBACKS:local-static-capture-aggregate-listener-group-gc-close-pages')
                    markers.append('FULL_FFI_CLEANUP:dead-metadata-roots-list-ports-keepalive-static-survivor')
                run(harness, snapshot, payload, native, mode, envelopes,
                    digest_artifact(base), digest_artifact(payload), markers=markers,
                    log=work / f'{label}-{payload_name}.log')
                if digest_artifact(native) != released_hash:
                    raise AssertionError('Released native plugin binary changed')
        (work / 'host-acceptance.json').write_text(json.dumps({
            'profile': 'PRODUCT full replacement', 'host_abi': abi,
            'released_native_sha256': released_hash,
            'base_sha256': digest_artifact(base), 'update_sha256': digest_artifact(update),
            'native_baselines': [label for label, _ in snapshots],
            'direct_loader_rejection_cases': [case['name'] for case in mutation_cases],
            'encrypted_additional_positive_cases': [case['name'] for case in positive_cases],
            'status': 'host_execution_passed', 'physical_ios': 'not_exercised',
            'cli_archive_differ': 'not_exercised',
            'server_signature_verification': 'not_exercised_by_this_host_harness',
        }, indent=2) + '\n')
        print('PRODUCT full N→B/N→U FFI host fixture passed; physical iOS, '
              'CLI archive comparison and server signature gates remain separate.')


if __name__ == '__main__':
    main()
