#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Small synthetic protocol controls; no compilation, signing or native proof."""
import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from unittest import mock

import native_ffi_test_support as support
from native_ffi_prepared_cases import (expected_cases, mutation_specs, signed_runtime_paths,
                                       MISSING_SCOPE_FATAL, SUITES)
from test_aot_full_replacement import host_abi


class PreparedProtocolTest(unittest.TestCase):
    @contextlib.contextmanager
    def bundle(self, suite='callbacks'):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            proof = Path(directory).resolve()
            work = proof / suite
            work.mkdir()
            stack.enter_context(mock.patch.object(support, 'PROOF_ROOT', re.compile(re.escape(str(proof)) + '$')))
            symbols = ({'DRT_FfiCall': [100, 200, 10]} if suite == 'callouts' else
                       {'InterpreterFfiCallbackEntry': [300, 400, 10]} if suite == 'callbacks' else {})
            for path in signed_runtime_paths(suite, work):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(('signed ' + path.name).encode())
            originals = {}
            for role in support.ORIGINAL_ROLES:
                path = work / 'original-inputs' / role
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(('original ' + role).encode())
                originals[role] = {'path': 'original-inputs/' + role, 'sha256': support._sha(path),
                                   'usage': 'provenance-only-never-executed'}
            if suite == 'full':
                mutations, positives = mutation_specs(work)
                for item in mutations + positives:
                    path = Path(item['path'])
                    path.parent.mkdir(exist_ok=True)
                    path.write_bytes(item['name'].encode())
                (work / 'admission-mutations/cases.json').write_text(json.dumps(
                    {'format_version': 1, 'cases': mutations, 'positive_cases': positives}))
                for name in ('base.sbr', 'update.sbr'):
                    (work / name).write_bytes(name.encode())
            (work / 'strict-signing/original-build-artifacts.json').write_text('{"unchanged":true}')
            def runtime_inventory(suite, work, evidence):
                return [{'path': str(path.relative_to(work)), 'sha256': support._sha(path),
                         'team_identifier': 'TEAM', 'certificate_sha1': 'A' * 40,
                         'entitlements': {}, 'hardened_runtime': True, 'machine': 'x86_64',
                         'dependencies': []} for path in signed_runtime_paths(suite, work)]
            stack.enter_context(mock.patch.object(support, '_symbols', return_value=symbols))
            stack.enter_context(mock.patch.object(support, '_source_identity', return_value=('a' * 40, {'driver.py': 'b' * 64})))
            stack.enter_context(mock.patch.object(support, '_machine', return_value='x86_64'))
            stack.enter_context(mock.patch.object(support, '_strict_runtime_inventory', side_effect=runtime_inventory))
            cases = expected_cases(suite, work, symbols)
            with contextlib.redirect_stdout(io.StringIO()):
                manifest = support.write_prepared_manifest(
                    suite, work, originals, {'machine': 'arm64', 'tools': {}},
                    [case['argv'] for case in cases], '14.0')
            yield work, manifest, cases

    def validate(self, work, suite):
        return support.validate_prepared_manifest(work / 'prepared-manifest.json', suite,
                                                  work.parent / 'verification')

    def mutate_manifest(self, work, mutate):
        path = work / 'prepared-manifest.json'
        data = json.loads(path.read_text())
        mutate(data)
        path.write_text(json.dumps(data))

    def test_complete_inventory_counts_and_pending_preparation_never_executes(self):
        for suite, count in [('callouts', 8), ('callbacks', 19), ('full', 66)]:
            with self.subTest(suite=suite), self.bundle(suite) as (work, manifest, cases), \
                 mock.patch.object(support.subprocess, 'run') as child:
                self.assertEqual(len(cases), count)
                self.assertEqual(self.validate(work, suite)['test_cases'], cases)
                self.assertEqual(manifest['status'], 'prepared_runtime_pending')
                self.assertFalse((work / 'host-acceptance.json').exists())
                args = argparse.Namespace(prepare_only=True)
                deferred = []
                for case in cases:
                    self.assertTrue(support.defer_product(args, case['argv'], case['argv'][0], deferred))
                self.assertEqual(deferred, [case['argv'] for case in cases])
                child.assert_not_called()

    def test_incomplete_argv_exit_oracle_and_provenance_mutations_rejected(self):
        changes = [lambda m: m['test_cases'].pop(),
                   lambda m: m['test_cases'].reverse(),
                   lambda m: m['test_cases'][0]['argv'].__setitem__(0, '/bin/sh'),
                   lambda m: m['test_cases'][0]['argv'].append('--unexpected'),
                   lambda m: m['test_cases'][0].__setitem__('expected_exit', 'nonzero'),
                   lambda m: m['test_cases'][0]['required_markers'].clear(),
                   lambda m: m['test_cases'][0].__setitem__('timeout_seconds', 1),
                   lambda m: m['host_abi'].__setitem__('digest', '0' * 64),
                   lambda m: m['symbol_ranges']['InterpreterFfiCallbackEntry'].__setitem__(0, 1),
                   lambda m: m['preparation_host'].__setitem__('machine', 'x86_64'),
                   lambda m: m.__setitem__('deployment_target', '11.0')]
        for index, change in enumerate(changes):
            with self.subTest(change=index), self.bundle() as (work, _, _):
                self.mutate_manifest(work, change)
                with self.assertRaises(ValueError):
                    self.validate(work, 'callbacks')

    def test_fatal_scope_case_cannot_be_generic_nonzero(self):
        with self.bundle() as (work, manifest, _):
            missing = next(case for case in manifest['test_cases'] if case['id'] == 'missing-handle-scope')
            self.assertEqual(missing['expected_exit'], 'nonzero')
            self.assertIn(MISSING_SCOPE_FATAL, missing['required_markers'])
            self.assertIn('FORBIDDEN_HANDLE_TARGET_WITHOUT_SCOPE', missing['forbidden_markers'])
            result = subprocess.CompletedProcess(missing['argv'], 1, stdout='unrelated failure')
            with mock.patch.object(support.subprocess, 'run', return_value=result):
                with self.assertRaisesRegex(AssertionError, 'Missing native case oracle'):
                    support._execute_case(missing, work.parent)

    def test_input_tamper_extra_and_symlink_rejected(self):
        for mode in ('tamper', 'extra', 'symlink'):
            with self.subTest(mode=mode), self.bundle() as (work, _, _):
                if mode == 'tamper':
                    (work / 'baseline.snapshot').write_bytes(b'changed')
                elif mode == 'extra':
                    (work / 'undeclared').write_bytes(b'extra')
                else:
                    (work / 'linked').symlink_to(work / 'baseline.snapshot')
                with self.assertRaises(ValueError):
                    self.validate(work, 'callbacks')

    def test_certificate_team_entitlement_and_machine_report_mutations_rejected(self):
        for key, value in [('certificate_sha1', 'B' * 40), ('team_identifier', 'WRONG'),
                           ('entitlements', {'com.apple.security.cs.allow-jit': True}),
                           ('hardened_runtime', False), ('machine', 'arm64')]:
            with self.subTest(key=key), self.bundle() as (work, _, _):
                self.mutate_manifest(work, lambda m: m['signed_runtime_inputs'][0].__setitem__(key, value))
                with self.assertRaises(ValueError):
                    self.validate(work, 'callbacks')

    def test_each_original_role_hash_is_required(self):
        for role in support.ORIGINAL_ROLES:
            with self.subTest(role=role), self.bundle() as (work, _, _):
                self.mutate_manifest(work, lambda m: m['original_artifacts'][role].__setitem__('sha256', '0' * 64))
                with self.assertRaises(ValueError):
                    self.validate(work, 'callbacks')

    def test_mutation_case_omission_rejected_even_with_updated_file_hash(self):
        with self.bundle('full') as (work, _, _):
            path = work / 'admission-mutations/cases.json'
            data = json.loads(path.read_text())
            data['cases'].pop()
            path.write_text(json.dumps(data))
            self.mutate_manifest(work, lambda m: m['inputs'].__setitem__('admission-mutations/cases.json', support._sha(path)))
            with self.assertRaisesRegex(ValueError, 'mutation inventory'):
                self.validate(work, 'full')

    def test_wrong_cpu_and_translation_rejected(self):
        with mock.patch.object(support, 'is_macos_x64', return_value=False):
            with self.assertRaisesRegex(ValueError, 'native macOS Intel'):
                support.require_native_intel()
        translated = subprocess.CompletedProcess([], 0, stdout='1\n')
        with mock.patch.object(support, 'is_macos_x64', return_value=True), \
             mock.patch.object(support.subprocess, 'run', return_value=translated):
            with self.assertRaisesRegex(RuntimeError, 'never Rosetta'):
                support.require_native_intel()

    def test_native_probe_uses_system_binary_scrubs_credentials_and_rejects_errors(self):
        for returncode, stdout, stderr in [(2, '', 'permission denied'), (0, 'unexpected', ''), (1, '', 'unrelated error')]:
            result = subprocess.CompletedProcess([], returncode, stdout=stdout, stderr=stderr)
            with self.subTest(returncode=returncode, stdout=stdout), \
                 mock.patch.object(support, 'is_macos_x64', return_value=True), \
                 mock.patch.dict(os.environ, {'PATH': '/attacker/bin', 'GITHUB_TOKEN': 'private'}), \
                 mock.patch.object(support.subprocess, 'run', return_value=result) as probe:
                with self.assertRaisesRegex(RuntimeError, 'sysctl probe failed'):
                    support.require_native_intel()
                self.assertEqual(probe.call_args.args[0][0], '/usr/sbin/sysctl')
                self.assertEqual(probe.call_args.kwargs['env'], support._safe_environment())

    def test_manifest_hardlink_rejected(self):
        with self.bundle() as (work, _, _):
            os.link(work / 'prepared-manifest.json', work.parent / 'hardlinked-manifest')
            with self.assertRaisesRegex(ValueError, 'hardlinks'):
                self.validate(work, 'callbacks')

    def test_provenance_git_lookup_scrubs_inherited_configuration(self):
        with mock.patch.dict(os.environ, {'GIT_DIR': '/wrong', 'GITHUB_TOKEN': 'private'}), \
             mock.patch.object(support, '_source_files', return_value=[]), \
             mock.patch.object(support.subprocess, 'check_output', return_value='a' * 40) as git:
            support._source_identity('callbacks')
            self.assertEqual(git.call_args.args[0][0], '/usr/bin/git')
            self.assertEqual(git.call_args.kwargs['env'], support._safe_environment())

    def test_failed_child_mutation_cannot_publish_success(self):
        with self.bundle() as (work, _, cases), \
             mock.patch.object(support, 'require_native_intel'), \
             mock.patch.object(support.subprocess, 'run') as child, \
             contextlib.redirect_stdout(io.StringIO()):
            by_argv = {tuple(case['argv']): case for case in cases}
            def execute(argv, **kwargs):
                self.assertEqual(kwargs['env'], support._safe_environment())
                self.assertEqual(argv[0], str(work / 'strict-signing' / SUITES['callbacks']))
                if argv == cases[0]['argv']:
                    (work / 'original-inputs/platform').write_bytes(b'changed by child')
                    return subprocess.CompletedProcess(argv, 23, stdout='failure')
                case = by_argv[tuple(argv)]
                return subprocess.CompletedProcess(argv, 1 if case['expected_exit'] == 'nonzero' else 0,
                                                   stdout='\n'.join(case['required_markers']))
            child.side_effect = execute
            with self.assertRaisesRegex(AssertionError, 'changed immutable'):
                support.run_prepared(work / 'prepared-manifest.json', 'callbacks')
            report = json.loads((work.parent / 'callbacks-results/native-results.json').read_text())
            self.assertEqual(report['status'], 'native_execution_failed')
            self.assertFalse(report['immutable_inputs_unchanged'])
            self.assertEqual(child.call_count, 19)

    def test_clean_consumer_executes_complete_trusted_inventory_and_reports_after_verification(self):
        with self.bundle() as (work, _, cases), \
             mock.patch.object(support, 'require_native_intel'), \
             mock.patch.object(support.subprocess, 'run') as child, \
             contextlib.redirect_stdout(io.StringIO()):
            by_argv = {tuple(case['argv']): case for case in cases}
            def execute(argv, **kwargs):
                case = by_argv[tuple(argv)]
                return subprocess.CompletedProcess(argv, 1 if case['expected_exit'] == 'nonzero' else 0,
                                                   stdout='\n'.join(case['required_markers']))
            child.side_effect = execute
            report = support.run_prepared(work / 'prepared-manifest.json', 'callbacks')
            self.assertEqual(report['status'], 'native_execution_passed')
            self.assertTrue(report['immutable_inputs_unchanged'])
            self.assertEqual(child.call_count, 19)
            self.assertFalse((work / 'native-results.json').exists())

    def test_runtime_inventory_accepts_only_system_dependencies_and_exact_self_id(self):
        import test_aot_full_replacement_ffi as full
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            path = work / 'libfixture.dylib'
            path.write_bytes(b'synthetic signed image')
            def report(dependencies):
                return {'path': str(path), 'sha256': support._sha(path),
                        'team_identifier': 'TEAM', 'certificate_sha1': 'A' * 40,
                        'entitlements': {}, 'hardened_runtime': True,
                        'dependencies': dependencies}
            for dependency in (str(path), path.name, '/usr/lib/libSystem.B.dylib',
                               '/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation'):
                with self.subTest(dependency=dependency), \
                     mock.patch.object(support, '_machine', return_value='x86_64'), \
                     mock.patch.object(support, 'signed_runtime_paths', return_value=[path]), \
                     mock.patch.object(full, 'verify_strict_macos_signature', return_value=report([dependency + ' (compatibility version 1.0.0)'])):
                    inventory = support._strict_runtime_inventory('callouts', work, work / 'evidence')
                    self.assertEqual(inventory[0]['path'], path.name)
            for dependency in ('/tmp/unowned.dylib', '@rpath/unowned.dylib', '../libfixture.dylib',
                               str(work / 'different/libfixture.dylib')):
                with self.subTest(dependency=dependency), \
                     mock.patch.object(support, '_machine', return_value='x86_64'), \
                     mock.patch.object(support, 'signed_runtime_paths', return_value=[path]), \
                     mock.patch.object(full, 'verify_strict_macos_signature', return_value=report([dependency])):
                    with self.assertRaisesRegex(ValueError, 'Unowned Mach-O dependency'):
                        support._strict_runtime_inventory('callouts', work, work / 'evidence')

    def test_explicit_target_machine_preserves_host_abi_descriptor(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = [Path(directory) / name for name in ('runtime', 'compiler', 'platform')]
            for path in paths:
                path.write_bytes(path.name.encode())
            with mock.patch('test_aot_full_replacement.host_platform.machine', return_value='x86_64'):
                native = host_abi(*paths, True)
            with mock.patch('test_aot_full_replacement.host_platform.machine', return_value='arm64'):
                self.assertEqual(host_abi(*paths, True, target_machine='x86_64'), native)
                self.assertNotEqual(host_abi(*paths, True), native)

class DriverPreparationIsolationTest(unittest.TestCase):
    def test_all_three_drivers_prepare_complete_inventory_without_product_execution(self):
        import test_aot_product_ffi as callouts
        import test_aot_product_ffi_callbacks as callbacks
        import test_aot_full_replacement_ffi as full
        from native_ffi_test_support import StrictMacosSigning
        for suite, driver in [('callouts', callouts), ('callbacks', callbacks), ('full', full)]:
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
                root = Path(directory).resolve()
                proof = root / 'proof'
                proof.mkdir()
                work = proof / suite
                build = root / 'build'
                build.mkdir()
                for name in ('dart', 'vm_platform_product.dill', 'gen_snapshot_product', SUITES[suite]):
                    (build / name).write_bytes(('original ' + name).encode())
                producer = root / 'envelope_producer.dart'
                producer.write_text('synthetic producer')
                producer_packages = root / 'producer-packages.json'
                producer_packages.write_text('{}')
                producer_dart = root / 'producer-dart'
                producer_dart.write_bytes(b'synthetic producer tool')
                symbols = ({'DRT_FfiCall': [100, 200, 10]} if suite == 'callouts' else
                           {'InterpreterFfiCallbackEntry': [300, 400, 10]} if suite == 'callbacks' else {})
                stack.enter_context(mock.patch.object(support, 'PROOF_ROOT', re.compile(re.escape(str(proof)) + '$')))
                stack.enter_context(mock.patch.object(support, '_source_identity', return_value=('a' * 40, {'driver': 'b' * 64})))
                stack.enter_context(mock.patch.object(support, '_symbols', return_value=symbols))
                stack.enter_context(mock.patch.object(support, '_machine', return_value='x86_64'))
                stack.enter_context(mock.patch.object(driver, 'preparation_host', return_value={'machine': 'arm64', 'tools': {}}))
                stack.enter_context(mock.patch.object(StrictMacosSigning, 'sign'))
                stack.enter_context(mock.patch.object(driver.shutil, 'which', return_value='clang'))
                stack.enter_context(mock.patch.object(driver.platform, 'machine', return_value='arm64'))
                stack.enter_context(mock.patch.object(driver.sys, 'platform', 'darwin'))
                if suite == 'callouts':
                    stack.enter_context(mock.patch.object(driver, 'interpreter_entry_range', return_value=(100, 200, 10)))
                if suite == 'callbacks':
                    stack.enter_context(mock.patch.object(driver, 'common_entry_range', return_value=(300, 400, 10)))
                def signature(path, *args, **kwargs):
                    return {'path': str(path), 'sha256': support._sha(path), 'team_identifier': 'TEAM',
                            'certificate_sha1': 'A' * 40, 'entitlements': {},
                            'hardened_runtime': True, 'dependencies': []}
                if suite == 'full':
                    stack.enter_context(mock.patch.object(driver, 'verify_strict_macos_signature', side_effect=signature))
                def inventory(suite, work, evidence):
                    return [dict(signature(path), path=str(path.relative_to(work)), machine='x86_64')
                            for path in signed_runtime_paths(suite, work)]
                stack.enter_context(mock.patch.object(support, '_strict_runtime_inventory', side_effect=inventory))
                original_is_file = Path.is_file
                stack.enter_context(mock.patch.object(Path, 'is_file', lambda path:
                    True if path.name == 'package_config.json' else original_is_file(path)))
                def compile_only(command, **kwargs):
                    command = list(map(str, command))
                    self.assertNotIn(Path(command[0]).name, SUITES.values(), 'Preparation executed PRODUCT')
                    if 'forged-carrier.snapshot' in ' '.join(command):
                        return subprocess.CompletedProcess(command, 1, stdout='Native assets metadata library cannot define fields.')
                    if any('/forbidden/' in value for value in command):
                        source = command[-1]
                        text = ('External/native replacement definition' if any(name in source for name in ('plain_external', 'old_native')) else
                                'Unsupported replacement v1 import: dart:nativewrappers')
                        return subprocess.CompletedProcess(command, 1, stdout=text)
                    if any(value.endswith('full_replacement_admission_mutations.dart') for value in command):
                        mutations, positives = mutation_specs(work)
                        for item in mutations + positives:
                            path = Path(item['path']); path.parent.mkdir(exist_ok=True); path.write_bytes(item['name'].encode())
                        (work / 'admission-mutations/cases.json').write_text(json.dumps(
                            {'format_version': 1, 'cases': mutations, 'positive_cases': positives}))
                    elif str(producer) in command:
                        out = Path(command[command.index(str(producer)) + 3]); out.mkdir()
                        for name in ('valid.json', 'tampered.json', 'expired.json'):
                            (out / name).write_text('{}')
                    else:
                        outputs = [value.split('=', 1)[1] for value in command
                                   if value.startswith(('--output=', '--macho=', '--elf='))]
                        if '-o' in command:
                            outputs.append(command[command.index('-o') + 1])
                        for value in outputs:
                            Path(value).write_bytes(('synthetic ' + Path(value).name).encode())
                    return subprocess.CompletedProcess(command, 0, stdout='')
                child = stack.enter_context(mock.patch.object(driver.subprocess, 'run', side_effect=compile_only))
                args = ['fixture', '--prepare-only', '--target-os=macos', '--target-arch=x64',
                        '--build-dir=' + str(build), '--work-dir=' + str(work), '--macos-signing-identity=test-identity']
                if suite == 'full':
                    args += ['--envelope-producer=' + str(producer), '--envelope-packages=' + str(producer_packages),
                             '--envelope-dart=' + str(producer_dart)]
                stack.enter_context(mock.patch.object(driver.sys, 'argv', args))
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    driver.main()
                self.assertIn('runtime acceptance PENDING', output.getvalue())
                for forbidden in ('execution passed', 'fixture passed', 'transition children passed'):
                    self.assertNotIn(forbidden, output.getvalue())
                manifest = json.loads((work / 'prepared-manifest.json').read_text())
                self.assertEqual(manifest['status'], 'prepared_runtime_pending')
                self.assertEqual(len(manifest['test_cases']), {'callouts': 8, 'callbacks': 19, 'full': 66}[suite])
                native_commands = [call.args[0] for call in child.call_args_list if str(call.args[0][0]) == 'clang']
                self.assertEqual(len(native_commands), 1)
                self.assertIn('x86_64', native_commands[0])
                self.assertIn('-mmacosx-version-min=14.0', native_commands[0])
                if suite != 'callbacks':
                    assets = json.loads((work / 'released-native-assets.json').read_text())
                    self.assertEqual(set(assets['native-assets']), {'macos_x64'})


if __name__ == '__main__':
    unittest.main()
