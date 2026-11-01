#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Fail-closed controls for strict macOS host signing evidence."""

import contextlib
import json
from pathlib import Path
import plistlib
import tempfile
import unittest

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


if __name__ == '__main__':
    unittest.main()
