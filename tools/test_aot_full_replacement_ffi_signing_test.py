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
    def test_rosetta_rejected(self):
        translated = type('Result', (), {'returncode': 0, 'stdout': '1\n'})()
        with mock.patch('native_ffi_test_support.is_macos_x64', return_value=True), \
             mock.patch('native_ffi_test_support.subprocess.run', return_value=translated):
            with self.assertRaisesRegex(RuntimeError, 'native Intel'):
                require_native_macos_x64()

    def test_native_intel_not_rejected(self):
        native = type('Result', (), {'returncode': 1, 'stdout': ''})()
        with mock.patch('native_ffi_test_support.is_macos_x64', return_value=True), \
             mock.patch('native_ffi_test_support.subprocess.run', return_value=native):
            require_native_macos_x64()


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
