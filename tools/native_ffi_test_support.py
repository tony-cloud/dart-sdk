# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Signing and native-host requirements shared by the bounded FFI fixtures."""
import contextlib
import json
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys

from test_aot_full_replacement_ffi import (preserve_original_artifacts,
                                           verify_strict_macos_signature)


def is_macos_x64():
    return sys.platform == 'darwin' and platform.machine().lower() in ('x86_64', 'amd64')


def require_native_macos_x64():
    if is_macos_x64():
        translated = subprocess.run(['sysctl', '-in', 'sysctl.proc_translated'],
                                    capture_output=True, text=True)
        if translated.returncode == 0 and translated.stdout.strip() == '1':
            raise RuntimeError('macOS x64 acceptance requires native Intel, never Rosetta')


class StrictMacosSigning:
    """Sign only private test copies; use the full fixture's strict verifier."""
    def __init__(self, work, build, artifacts, identity, run):
        work, build = Path(work).resolve(), Path(build).resolve()
        self.artifacts = tuple(Path(artifact).absolute() for artifact in artifacts)
        if work == build or build in work.parents:
            raise ValueError('Strict host work directory must be outside original build outputs')
        if any(work.iterdir()):
            raise ValueError('Strict host mode requires a fresh EMPTY work directory')
        if any(work == artifact.resolve() or work in artifact.resolve().parents
               for artifact in self.artifacts):
            raise ValueError('Strict host work directory must not contain original inputs')
        self.identity, self.run = identity, run
        self.evidence = work / 'strict-signing'
        self.entitlements = self.evidence / 'empty-entitlements.plist'
        self.signatures = []

    @contextlib.contextmanager
    def _protected(self):
        # The fresh workspace prevents existing output symlinks from directing
        # fixture/compiler/signing writes into original build inputs. The guard
        # covers every original, including failed signing and child processes.
        self.evidence.mkdir()
        with preserve_original_artifacts(self.artifacts, self.evidence):
            self.entitlements.write_bytes(plistlib.dumps({}))
            yield self
            self.verify()

    def __enter__(self):
        self._context = self._protected()
        return self._context.__enter__()

    def __exit__(self, *error):
        return self._context.__exit__(*error)

    def sign(self, artifact):
        artifact = Path(artifact)
        self.run('codesign', '--force', '--sign', self.identity,
                 '--options', 'runtime', '--timestamp=none', '--entitlements',
                 self.entitlements, '--force-library-entitlements', artifact)
        report = verify_strict_macos_signature(
            artifact, self.run, self.evidence,
            self.signatures[0] if self.signatures else None)
        if (re.fullmatch(r'[A-Fa-f0-9]{40}', self.identity) and
                report['certificate_sha1'] != self.identity.upper()):
            raise AssertionError('Signed certificate does not match requested identity')
        self.signatures.append(report)
        (self.evidence / 'signatures.json').write_text(
            json.dumps(self.signatures, indent=2) + '\n')

    def copy_harness(self, original):
        original = Path(original)
        destination = self.evidence / original.name
        shutil.copy2(original, destination)
        self.sign(destination)
        return destination

    def verify(self):
        for report in self.signatures:
            current = verify_strict_macos_signature(
                Path(report['path']), self.run, self.evidence, self.signatures[0])
            if current != report:
                raise AssertionError(f'Executed signed image changed: {report["path"]}')
