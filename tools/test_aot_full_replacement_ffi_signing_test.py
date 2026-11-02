#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Fail-closed controls for strict macOS host signing evidence."""

import contextlib
import json
import io
from pathlib import Path
import plistlib
import tempfile
import subprocess
import sys
import unittest
from unittest import mock

import native_ffi_test_support as native_support
from native_ffi_test_support import require_native_macos_x64
import test_aot_product_ffi
import test_aot_product_ffi_callbacks

from test_aot_full_replacement_ffi import (
    parse_strict_macos_signature, preserve_original_artifacts,
    publish_host_acceptance)


class StrictMacosSignatureTest(unittest.TestCase):
    details = ('CodeDirectory v=20500 size=123 flags=0x10000(runtime) hashes=1\n'
               'Authority=Developer ID Application: Example (TEAM123456)\n'
               'TeamIdentifier=TEAM123456\n')

    def test_explicit_empty_entitlements(self):
        report = parse_strict_macos_signature(
            self.details, 'Executable=/private/test\n' + plistlib.dumps({}).decode())
        self.assertEqual(report['entitlements'], {})
        self.assertEqual(report['team_identifier'], 'TEAM123456')

    def test_absent_and_malformed_entitlements_rejected(self):
        for entitlements in ('', 'Executable=/private/test\n',
                             '<?xml version="1.0"?><plist><dict></plist>',
                             plistlib.dumps([]).decode()):
            with self.subTest(entitlements=entitlements), self.assertRaises(ValueError):
                parse_strict_macos_signature(self.details, entitlements)

    def test_every_entitlement_rejected_even_when_false(self):
        for key in ('com.apple.security.cs.disable-library-validation',
                    'com.apple.security.cs.allow-jit',
                    'com.apple.security.cs.allow-unsigned-executable-memory',
                    'com.apple.security.get-task-allow', 'unexpected-entitlement'):
            for value in (True, False):
                with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                    parse_strict_macos_signature(
                        self.details, plistlib.dumps({key: value}).decode())

    def test_missing_runtime_team_and_developer_id_rejected(self):
        for details in ('', self.details.replace('0x10000(runtime)', '0x2(adhoc)'),
                        self.details.replace('TeamIdentifier=TEAM123456',
                                             'TeamIdentifier=not set'),
                        self.details.replace('Authority=Developer ID Application:',
                                             'Authority=Apple Development:')):
            with self.subTest(details=details), self.assertRaises(ValueError):
                parse_strict_macos_signature(details, plistlib.dumps({}).decode())


class HostAcceptancePublicationTest(unittest.TestCase):
    def test_changed_original_cannot_leave_success(self):
        self.check_failed_preservation(lambda original: original.write_bytes(b'changed'),
                                       AssertionError)

    def test_unreadable_original_cannot_leave_success(self):
        self.check_failed_preservation(lambda original: original.unlink(), FileNotFoundError)

    def check_failed_preservation(self, mutate, expected_error):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as guards:
            work = Path(directory)
            original = work / 'original'
            original.write_bytes(b'original')
            guards.enter_context(preserve_original_artifacts((original,), work))
            acceptance = {'status': 'host_execution_passed'}
            destination = work / 'host-acceptance.json'
            destination.write_text(json.dumps(acceptance))
            mutate(original)
            with self.assertRaises(expected_error):
                publish_host_acceptance(work, acceptance, guards)
            self.assertFalse(destination.exists())

    def test_preserved_original_publishes_after_verification(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as guards:
            work = Path(directory)
            original = work / 'original'
            original.write_bytes(b'original')
            guards.enter_context(preserve_original_artifacts((original,), work))
            acceptance = {'status': 'host_execution_passed'}
            publish_host_acceptance(work, acceptance, guards)
            self.assertEqual(json.loads((work / 'host-acceptance.json').read_text()),
                             acceptance)
            self.assertTrue(json.loads(
                (work / 'original-build-artifacts.json').read_text())['unchanged'])


class NativeIntelSigningGateTest(unittest.TestCase):
    @staticmethod
    def result(code=0, output='', error=''):
        return subprocess.CompletedProcess([], code, output, error)

    def test_rosetta_and_hardware_arm_rejected(self):
        for translated, hardware_arm in ((True, False), (False, True),
                                        (None, True), (True, None)):
            with self.subTest(translated=translated, hardware_arm=hardware_arm), \
                 mock.patch.object(native_support, 'is_macos_x64', return_value=True), \
                 mock.patch.object(native_support, '_sysctl_flag', side_effect=[translated, hardware_arm]):
                with self.assertRaisesRegex(RuntimeError, 'native Intel'):
                    require_native_macos_x64()

    def test_native_intel_missing_oids_are_unsuppressed_and_not_rejected(self):
        def probe(command, **kwargs):
            self.assertEqual(command[:2], ['/usr/sbin/sysctl', '-n'])
            self.assertEqual(kwargs['env'], native_support._safe_environment())
            self.assertEqual(kwargs['env']['LC_ALL'], 'C')
            self.assertEqual(kwargs['timeout'], 60)
            # Real -i instead produces (0, '', ''), which is not native proof.
            return self.result(1, '', "sysctl: unknown oid '" + command[2] + "'\n")
        with mock.patch.object(native_support, 'is_macos_x64', return_value=True), \
             mock.patch.object(native_support.subprocess, 'run', side_effect=probe) as run:
            require_native_macos_x64()
        self.assertEqual([call.args[0][2] for call in run.call_args_list],
                         ['sysctl.proc_translated', 'hw.optional.arm64'])

    def test_supported_zero_and_one_flags(self):
        for output, expected in (('0\n', False), ('1\n', True)):
            with self.subTest(output=output), \
                 mock.patch.object(native_support.subprocess, 'run', return_value=self.result(0, output)):
                self.assertIs(native_support._sysctl_flag('sysctl.proc_translated'), expected)

    def test_genuine_probe_errors_and_ambiguous_empty_success_fail_closed(self):
        name = 'sysctl.proc_translated'
        absent = "sysctl: unknown oid '" + name + "'\n"
        results = [self.result(0, '', ''), self.result(0, '2\n', ''),
                   self.result(1, '', 'sysctl: permission denied\n'),
                   self.result(2, '', absent), self.result(1, '0\n', absent),
                   self.result(1, '', "sysctl: unknown oid 'another.oid'\n"),
                   self.result(1, '', 'sysctl: unknown oid'),
                   self.result(1, '', "sysctl: unknown oid 'sysctlXproc_translated'\n"),
                   self.result(1, '', 'sysctl: unknown oid \u2018sysctl.proc_translated\u2019\n'),
                   self.result(1, '', absent + 'sysctl: permission denied\n')]
        for result in results:
            with self.subTest(result=result), \
                 mock.patch.object(native_support.subprocess, 'run', return_value=result):
                with self.assertRaisesRegex(RuntimeError, 'sysctl probe failed'):
                    native_support._sysctl_flag(name)

    def test_probe_execution_failure_or_timeout_is_not_native_proof(self):
        for error in (OSError('probe cannot execute'), subprocess.TimeoutExpired('sysctl', 60)):
            with self.subTest(error=error), \
                 mock.patch.object(native_support.subprocess, 'run', side_effect=error):
                with self.assertRaises(type(error)):
                    native_support._sysctl_flag('sysctl.proc_translated')

    def test_prepared_acceptance_rejects_foreign_process_without_sysctl(self):
        with mock.patch.object(native_support, 'is_macos_x64', return_value=False), \
             mock.patch.object(native_support.subprocess, 'run') as run:
            with self.assertRaisesRegex(ValueError, 'native macOS Intel'):
                native_support.require_native_intel()
            run.assert_not_called()

    @unittest.skipUnless(sys.platform == 'darwin' and Path('/usr/sbin/sysctl').is_file(),
                         'Actual unknown-OID reproduction requires macOS sysctl')
    def test_actual_host_unknown_oid_is_visible_without_ignore_flag(self):
        # Read-only synthetic missing OID: this is probe behavior, not Intel
        # runtime acceptance on an Apple-silicon development machine.
        name = 'shorebird.native_ffi_test_missing_oid_20261102'
        ignored = subprocess.run(['/usr/sbin/sysctl', '-in', name], capture_output=True,
                                 text=True, env=native_support._safe_environment(), timeout=60)
        self.assertEqual((ignored.returncode, ignored.stdout, ignored.stderr), (0, '', ''))
        self.assertIsNone(native_support._sysctl_flag(name))


class StrictRunnerOriginalPreservationTest(unittest.TestCase):
    runners = ((test_aot_product_ffi, 'product_ffi_test'),
               (test_aot_product_ffi_callbacks, 'product_ffi_callback_test'))

    def run_fixture(self, runner, harness, work_kind, mutate=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            build = root / 'build'
            build.mkdir()
            originals = tuple(build / name for name in (
                'dart', 'vm_platform_product.dill', 'gen_snapshot_product', harness))
            for original in originals:
                original.write_bytes(('original ' + original.name).encode())
            before = {str(path): path.read_bytes() for path in originals}
            if work_kind == 'build':
                work = build
            elif work_kind == 'build-child':
                work = build / 'empty-work'
                work.mkdir()
            elif work_kind == 'build-alias':
                work = root / 'build-alias'
                work.symlink_to(build, target_is_directory=True)
            else:
                work = root / 'work'
                work.mkdir()
                if work_kind == 'output-symlink':
                    (work / 'baseline.dart').symlink_to(originals[2])
            command = ['fixture', '--build-dir', str(build), '--work-dir', str(work),
                       '--macos-signing-identity=test-identity']

            def fail_child(*args, **kwargs):
                if mutate is not None:
                    originals[mutate].write_bytes(b'changed during failed child')
                raise subprocess.CalledProcessError(23, args[0])

            with mock.patch.object(sys, 'argv', command), \
                 mock.patch.object(sys, 'platform', 'darwin'), \
                 mock.patch.object(runner.platform, 'machine', return_value='arm64'), \
                 mock.patch.object(runner.shutil, 'which', return_value='clang'), \
                 mock.patch.object(runner.subprocess, 'run', side_effect=fail_child) as child, \
                 contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                if work_kind != 'fresh':
                    with self.assertRaisesRegex(ValueError, 'outside original|fresh EMPTY'):
                        runner.main()
                    child.assert_not_called()
                    self.assertEqual({str(path): path.read_bytes() for path in originals}, before)
                    return
                expected_error = subprocess.CalledProcessError if mutate is None else AssertionError
                with self.assertRaises(expected_error):
                    runner.main()
                child.assert_called_once()
            report = json.loads((work / 'strict-signing/original-build-artifacts.json').read_text())
            self.assertEqual(set(report['before_sha256']), set(before))
            self.assertEqual(report['unchanged'], mutate is None)
            self.assertFalse((work / 'host-acceptance.json').exists())
            self.assertFalse((work / 'baseline.dart').exists())
            if mutate is None:
                self.assertEqual({str(path): path.read_bytes() for path in originals}, before)

    def test_build_nonempty_and_symlink_workspaces_rejected_before_writes(self):
        for runner, harness in self.runners:
            for kind in ('build', 'build-child', 'build-alias', 'output-symlink'):
                with self.subTest(runner=runner.__name__, workspace=kind):
                    self.run_fixture(runner, harness, kind)

    def test_failed_signing_child_still_records_all_four_unchanged_originals(self):
        for runner, harness in self.runners:
            with self.subTest(runner=runner.__name__):
                self.run_fixture(runner, harness, 'fresh')

    def test_each_changed_original_is_detected_even_after_child_failure(self):
        for runner, harness in self.runners:
            for original in range(4):
                with self.subTest(runner=runner.__name__, original=original):
                    self.run_fixture(runner, harness, 'fresh', mutate=original)


if __name__ == '__main__':
    unittest.main()
