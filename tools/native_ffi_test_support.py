# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Signing and native-host requirements shared by the bounded FFI fixtures."""
import contextlib
import hashlib
import os
import struct
import stat
import json
from pathlib import Path
import platform
import plistlib
import re
import shutil
import subprocess
import sys

from native_ffi_prepared_cases import SUITES, expected_cases, signed_runtime_paths, mutation_specs


def is_macos_x64():
    return sys.platform == 'darwin' and platform.machine().lower() in ('x86_64', 'amd64')


def _sysctl_flag(name):
    # Do not pass -i: it converts an absent OID to silent success, hiding
    # the distinction between a supported zero value and unavailable metadata.
    result = subprocess.run(['/usr/sbin/sysctl', '-n', name], capture_output=True,
                            text=True, env=_safe_environment(), timeout=60)
    if result.returncode == 0 and result.stdout.strip() in ('0', '1'):
        return result.stdout.strip() == '1'
    # Intel macOS versions may omit these Apple-silicon sysctls. Other probe
    # failures do not establish a native host and must not silently pass.
    if (result.returncode == 1 and not result.stdout.strip() and
            re.fullmatch(r"sysctl: unknown oid '" + re.escape(name) + "'",
                         result.stderr.strip(), flags=re.IGNORECASE)):
        return None
    raise RuntimeError('Cannot establish native Intel host: sysctl probe failed for ' + name)


def require_native_macos_x64():
    if is_macos_x64():
        translated = _sysctl_flag('sysctl.proc_translated')
        hardware_arm64 = _sysctl_flag('hw.optional.arm64')
        if translated or hardware_arm64:
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
        from test_aot_full_replacement_ffi import preserve_original_artifacts
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
        from test_aot_full_replacement_ffi import verify_strict_macos_signature
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
        from test_aot_full_replacement_ffi import verify_strict_macos_signature
        for report in self.signatures:
            current = verify_strict_macos_signature(
                Path(report['path']), self.run, self.evidence, self.signatures[0])
            if current != report:
                raise AssertionError(f'Executed signed image changed: {report["path"]}')


# The transfer protocol deliberately preserves all absolute fixture URIs.
# These are task-owned roots, not a general-purpose serialized command runner.
PROOF_ROOT = re.compile(r'/private/tmp/shorebird-macos-x64-proof-[A-Za-z0-9_-]+$')
DRIVERS = {'callouts': 'tools/test_aot_product_ffi.py',
           'callbacks': 'tools/test_aot_product_ffi_callbacks.py',
           'full': 'tools/test_aot_full_replacement_ffi.py'}
ORIGINAL_ROLES = ('dart', 'platform', 'gen_snapshot', 'harness')
TARGET = {'os': 'macos', 'arch': 'x64', 'machine': 'x86_64'}


def add_prepared_options(parser):
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument('--prepare-only', action='store_true',
                       help='Compile/sign a complete macOS x64 suite without executing PRODUCT')
    modes.add_argument('--run-prepared', type=Path,
                       help='Execute a previously prepared immutable suite on native Intel only')
    parser.add_argument('--target-os')
    parser.add_argument('--target-arch')
    parser.add_argument('--macos-deployment-target')


def canonical_work(work, suite):
    work = Path(work).absolute()
    if work != work.resolve() or work.name != suite or not PROOF_ROOT.fullmatch(str(work.parent)):
        raise ValueError('Prepared suite requires /private/tmp/shorebird-macos-x64-proof-<nonce>/' + suite)
    return work


def handle_prepared_options(args, parser, suite):
    if args.run_prepared:
        conflicts = ('build_dir', 'work_dir', 'macos_signing_identity', 'envelope_producer',
                     'envelope_dart', 'envelope_packages', 'target_os', 'target_arch',
                     'baseline_only', 'body_profile', 'full_assets_only', 'transitions_only', 'check_fixtures', 'macos_deployment_target')
        if any(getattr(args, name, None) for name in conflicts):
            parser.error('--run-prepared does not accept build, preparation or partial-suite options')
        run_prepared(args.run_prepared, suite)
        return True
    if args.prepare_only:
        if sys.platform != 'darwin' or (args.target_os, args.target_arch) != ('macos', 'x64'):
            parser.error('--prepare-only requires macOS and explicit --target-os=macos --target-arch=x64')
        if not args.work_dir or not args.build_dir or not args.macos_signing_identity:
            parser.error('--prepare-only requires --build-dir, canonical --work-dir and --macos-signing-identity')
        if any(getattr(args, name, False) for name in ('baseline_only', 'body_profile', 'full_assets_only', 'transitions_only', 'check_fixtures')):
            parser.error('Prepared acceptance requires the complete suite')
        canonical_work(args.work_dir, suite)
        args.macos_deployment_target = args.macos_deployment_target or '14.0'
        if not re.fullmatch(r'[0-9]+\.[0-9]+', args.macos_deployment_target):
            parser.error('Invalid macOS deployment target')
    elif args.target_os or args.target_arch:
        parser.error('Explicit target options belong to --prepare-only')
    return False


def prepared_compile_flags(args):
    return (['-arch', 'x86_64', '-mmacosx-version-min=' + args.macos_deployment_target]
            if args.prepare_only else [])


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _safe_environment():
    # No inherited GitHub tokens, cloud credentials, signing configuration or
    # DYLD/VM flags. The native consumers need only the system tool path/locale.
    return {'PATH': '/usr/bin:/bin:/usr/sbin:/sbin', 'LANG': 'C', 'LC_ALL': 'C', 'TZ': 'UTC'}


def _inspect(*command, log=None):
    allowed = {'codesign': ('--verify', '--display'), 'otool': ('-L',),
               'lipo': ('-archs',), 'nm': ('-n',)}
    name = str(command[0])
    if name not in allowed or len(command) < 2 or str(command[1]) not in allowed[name]:
        raise ValueError('Only static signature/Mach-O inspection is permitted')
    result = subprocess.run(['/usr/bin/' + name, *map(str, command[1:])],
                            capture_output=True, text=True, env=_safe_environment(), timeout=60)
    output = result.stdout + result.stderr
    if log:
        Path(log).write_text(output)
    result.check_returncode()
    return output


def _machine(path):
    machine = _inspect('lipo', '-archs', path).strip()
    if machine != 'x86_64':
        raise ValueError('Prepared Mach-O must contain exactly native x86_64: ' + str(path))
    return machine


def preparation_host(artifacts):
    """Probe the actual x64 launch context; this is provenance, not acceptance."""
    probe = subprocess.run(['/usr/bin/arch', '-x86_64', '/usr/sbin/sysctl', '-in', 'sysctl.proc_translated'],
                           capture_output=True, text=True, env=_safe_environment(), timeout=60)
    # Intel may have no proc_translated sysctl. Do not label that a positive
    # translated result. ARM preparation must demonstrate the x64 context.
    if platform.machine().lower() in ('arm64', 'aarch64') and (probe.returncode or probe.stdout.strip() != '1'):
        raise ValueError('Cannot establish Rosetta context for x64 preparation tools')
    return {'system': sys.platform, 'machine': platform.machine(),
            'python': sys.version.split()[0],
            'x64_translation_probe': {'argv': ['arch', '-x86_64', 'sysctl', '-in', 'sysctl.proc_translated'],
                                      'returncode': probe.returncode, 'stdout': probe.stdout.strip(),
                                      'stderr': probe.stderr.strip()},
            'tools': {role: {'machine': _machine(path), 'sha256': _sha(path),
                             'translated': probe.returncode == 0 and probe.stdout.strip() == '1'}
                      for role, path in zip(ORIGINAL_ROLES, artifacts) if role != 'platform'}}


def capture_original_inputs(work, artifacts):
    directory = Path(work) / 'original-inputs'
    directory.mkdir()
    captured = {}
    for role, original in zip(ORIGINAL_ROLES, artifacts):
        destination = directory / role
        shutil.copy2(original, destination)
        if _sha(destination) != _sha(original):
            raise AssertionError('Original input changed while copying: ' + role)
        captured[role] = {'path': 'original-inputs/' + role, 'sha256': _sha(destination),
                          'usage': 'provenance-only-never-executed'}
    if set(captured) != set(ORIGINAL_ROLES):
        raise ValueError('All four original input roles are required')
    return captured


def defer_product(args, command, harness, deferred):
    if not args.prepare_only:
        return False
    if Path(str(command[0])).name in SUITES.values():
        if Path(str(command[0])) != Path(harness):
            raise ValueError('Preparation attempted an unowned PRODUCT executable')
        deferred.append(list(map(str, command)))
        return True
    return False


def _source_files(suite, root):
    names = {DRIVERS[suite], 'tools/native_ffi_test_support.py', 'tools/native_ffi_prepared_cases.py',
             'tools/test_aot_full_replacement.py', 'tools/test_aot_full_replacement_ffi.py',
             'tools/native_ffi_prepared_manifest.schema.json',
             'tools/test_interpreter_ffi_calls.py', 'runtime/bin/' + SUITES[suite] + '.cc'}
    directory = {'callouts': None, 'callbacks': 'tools/test_aot_product_ffi_callbacks',
                 'full': 'tools/test_aot_full_replacement_ffi'}[suite]
    if directory:
        names.update(str(path.relative_to(root)) for path in (root / directory).rglob('*')
                     if path.is_file() and path.suffix in ('.dart', '.c', '.S'))
    if suite == 'full':
        names.add('pkg/dart2bytecode/test/full_replacement_admission_mutations.dart')
    return sorted(names)


def _source_identity(suite):
    root = Path(__file__).resolve().parents[1]
    commit = subprocess.check_output(['/usr/bin/git', '-C', str(root), 'rev-parse', 'HEAD'],
                                     text=True, env=_safe_environment()).strip()
    return commit, {name: _sha(root / name) for name in _source_files(suite, root)}


def _all_files(work):
    files = {}
    for path in work.rglob('*'):
        if path.is_symlink():
            raise ValueError('Prepared input symlinks are forbidden: ' + str(path))
        mode = path.stat().st_mode
        if not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise ValueError('Prepared inputs must be regular files/directories: ' + str(path))
        if path.is_file():
            if path.stat().st_nlink != 1:
                raise ValueError('Prepared input hardlinks are forbidden: ' + str(path))
            if path != work / 'prepared-manifest.json':
                files[str(path.relative_to(work))] = _sha(path)
    return dict(sorted(files.items()))


def _symbols(suite, harness):
    if suite == 'full':
        return {}
    name = 'DRT_FfiCall' if suite == 'callouts' else 'InterpreterFfiCallbackEntry'
    symbols, start, image_base = [], None, 0
    for line in _inspect('nm', '-n', harness).splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        if fields[2] == '__mh_execute_header':
            image_base = int(fields[0], 16)
        if fields[1] in ('t', 'T'):
            address = int(fields[0], 16)
            symbols.append(address)
            if fields[2] in (name, '_' + name):
                start = address
    if start is None or not any(address > start for address in symbols):
        raise ValueError('Missing released interpreter entry symbol: ' + name)
    return {name: [start, min(address for address in symbols if address > start), image_base]}


def _abi_inputs(work, originals, harness):
    values = ['shorebird-host-abi-v1', 'darwin', 'x86_64', 'PRODUCT', 'full-replacement-v1',
              _sha(harness), _sha(work / originals['gen_snapshot']['path']),
              _sha(work / originals['platform']['path'])]
    descriptor = b''.join(struct.pack('<I', len(value.encode())) + value.encode() for value in values)
    return {'values': values, 'digest': hashlib.sha256(descriptor).hexdigest()}


def _strict_runtime_inventory(suite, work, evidence):
    from test_aot_full_replacement_ffi import verify_strict_macos_signature
    evidence.mkdir(parents=True, exist_ok=True)
    reports = []
    for path in signed_runtime_paths(suite, work):
        machine = _machine(path)
        report = verify_strict_macos_signature(path, _inspect, evidence, reports[0] if reports else None)
        for dependency in report['dependencies']:
            target = dependency.strip().split(' (', 1)[0]
            # clang's dylib ID defaults to its absolute -o path; snapshots
            # may use a basename. Permit only this image's exact self ID.
            if target not in (path.name, str(path)) and not target.startswith(('/usr/lib/', '/System/Library/')):
                raise ValueError('Unowned Mach-O dependency: ' + target)
        report.update(path=str(path.relative_to(work)), machine=machine)
        reports.append(report)
    return reports


def _validate_mutations(suite, work):
    if suite == 'full':
        from test_aot_full_replacement_ffi import load_mutation_manifest
        actual = load_mutation_manifest(work / 'admission-mutations/cases.json')
        expected = mutation_specs(work)
        if actual != expected:
            raise ValueError('Incomplete or changed full admission mutation inventory')


def write_prepared_manifest(suite, work, originals, host, deferred, deployment_target):
    work = canonical_work(work, suite)
    harness = work / 'strict-signing' / SUITES[suite]
    _validate_mutations(suite, work)
    symbols = _symbols(suite, harness)
    cases = expected_cases(suite, work, symbols)
    if deferred != [case['argv'] for case in cases]:
        raise ValueError('Preparation did not enumerate the complete trusted case inventory')
    # Signature inspection writes only new preparation evidence. Original guards
    # must already have closed successfully before this pending manifest exists.
    preservation = json.loads((work / 'strict-signing/original-build-artifacts.json').read_text())
    if preservation.get('unchanged') is not True:
        raise ValueError('Preparation has no completed original-artifact preservation proof')
    runtime = _strict_runtime_inventory(suite, work, work / 'prepared-signature-evidence')
    commit, sources = _source_identity(suite)
    (work / 'preparation-provenance.json').write_text(json.dumps(
        {'preparation_host': host, 'deployment_target': deployment_target}, indent=2) + '\n')
    manifest = {'format_version': 1, 'status': 'prepared_runtime_pending', 'suite': suite,
                'target': TARGET, 'canonical_root': str(work), 'proof_root': str(work.parent),
                'sdk_source_commit': commit, 'driver_source_sha256': sources,
                'preparation_host': host, 'deployment_target': deployment_target,
                'original_artifacts': originals, 'signed_runtime_inputs': runtime,
                'host_abi': _abi_inputs(work, originals, harness), 'symbol_ranges': symbols,
                'test_cases': cases, 'inputs': _all_files(work)}
    (work / 'prepared-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('Prepared immutable ' + suite + ' inputs; native Intel runtime acceptance PENDING.')
    return manifest


def require_native_intel():
    if not is_macos_x64():
        raise ValueError('Prepared runtime acceptance requires native macOS Intel x86_64')
    require_native_macos_x64()


def _json_equal(left, right):
    return json.dumps(left, sort_keys=True) == json.dumps(right, sort_keys=True)


def validate_prepared_manifest(path, suite, evidence):
    path = Path(path).absolute()
    work = canonical_work(path.parent, suite)
    if path != work / 'prepared-manifest.json' or path.is_symlink():
        raise ValueError('Prepared manifest path must be the canonical owned file')
    data = json.loads(path.read_text())
    keys = {'format_version', 'status', 'suite', 'target', 'canonical_root', 'proof_root',
            'sdk_source_commit', 'driver_source_sha256', 'preparation_host', 'deployment_target',
            'original_artifacts', 'signed_runtime_inputs', 'host_abi', 'symbol_ranges', 'test_cases', 'inputs'}
    if not isinstance(data, dict) or set(data) != keys or type(data['format_version']) is not int:
        raise ValueError('Unexpected or missing prepared manifest fields')
    if (data.get('format_version'), data.get('suite'), data.get('target'), data.get('status')) != (1, suite, TARGET, 'prepared_runtime_pending'):
        raise ValueError('Unsupported prepared manifest version, suite, target or status')
    if data.get('canonical_root') != str(work) or data.get('proof_root') != str(work.parent):
        raise ValueError('Prepared roots cannot be relocated')
    commit, sources = _source_identity(suite)
    if data.get('sdk_source_commit') != commit or data.get('driver_source_sha256') != sources:
        raise ValueError('Prepared SDK/driver/fixture source identity mismatch')
    if data.get('inputs') != _all_files(work):
        raise ValueError('Prepared immutable file inventory/hash mismatch')
    provenance = json.loads((work / 'preparation-provenance.json').read_text())
    if provenance != {'preparation_host': data.get('preparation_host'),
                      'deployment_target': data.get('deployment_target')}:
        raise ValueError('Prepared host/tool/deployment provenance mismatch')
    originals = data.get('original_artifacts', {})
    if set(originals) != set(ORIGINAL_ROLES):
        raise ValueError('Missing original-artifact ABI role')
    for role in ORIGINAL_ROLES:
        expected = {'path': 'original-inputs/' + role, 'sha256': _sha(work / 'original-inputs' / role),
                    'usage': 'provenance-only-never-executed'}
        if originals[role] != expected:
            raise ValueError('Original-artifact hash/role mismatch: ' + role)
        if role != 'platform':
            _machine(work / expected['path'])
    harness = work / 'strict-signing' / SUITES[suite]
    if not _json_equal(data.get('signed_runtime_inputs'), _strict_runtime_inventory(suite, work, evidence)):
        raise ValueError('Prepared signed runtime certificate/entitlement/hash mismatch')
    if data.get('host_abi') != _abi_inputs(work, originals, harness):
        raise ValueError('Prepared native Intel host ABI mismatch')
    symbols = _symbols(suite, harness)
    if not _json_equal(data.get('symbol_ranges'), symbols):
        raise ValueError('Prepared released entry symbol range mismatch')
    _validate_mutations(suite, work)
    cases = expected_cases(suite, work, symbols)
    if not _json_equal(data.get('test_cases'), cases):
        raise ValueError('Prepared test inventory/argv/oracles differ from complete trusted cases')
    return data


def _execute_case(case, results):
    # case came from expected_cases(), never from an untrusted dispatcher.
    command = case['argv']
    print('+ native Intel case ' + case['id'], flush=True)
    output = ''
    try:
        child = subprocess.run(command, cwd=results, env=_safe_environment(),
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, timeout=case['timeout_seconds'])
        output = child.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ''
        if isinstance(output, bytes):
            output = output.decode(errors='replace')
        output += '\nNATIVE CASE TIMED OUT\n'
        raise
    finally:
        (results / (case['id'] + '.log')).write_text(output)
    if case['expected_exit'] == 'nonzero':
        if child.returncode == 0:
            raise AssertionError('Expected native fatal termination: ' + case['id'])
    elif child.returncode != case['expected_exit']:
        raise subprocess.CalledProcessError(child.returncode, command, output)
    for marker in case['required_markers']:
        if marker not in output:
            raise AssertionError('Missing native case oracle ' + case['id'] + ': ' + marker)
    for marker in case['forbidden_markers']:
        if marker in output:
            raise AssertionError('Forbidden native continuation ' + case['id'] + ': ' + marker)
    return {'id': case['id'], 'returncode': child.returncode, 'status': 'passed'}


def run_prepared(path, suite):
    require_native_intel()
    path = Path(path).absolute()
    work = canonical_work(path.parent, suite)
    results = work.parent / (suite + '-results')
    results.mkdir()  # Refuse stale success, existing symlinks and reused outputs.
    report = {'suite': suite, 'target': TARGET, 'status': 'native_execution_failed', 'cases': []}
    before_manifest = _sha(path)
    before_inputs = _all_files(work)
    try:
        data = validate_prepared_manifest(path, suite, results / 'verification-before')
        failures = []
        # Run every mandatory child, including all eight isolated transitions.
        for case in expected_cases(suite, work, data['symbol_ranges']):
            try:
                report['cases'].append(_execute_case(case, results))
            except (subprocess.SubprocessError, AssertionError, OSError) as error:
                failures.append(case['id'] + ': ' + str(error))
                report['cases'].append({'id': case['id'], 'status': 'failed', 'error': str(error)})
        if failures:
            raise AssertionError('Native prepared cases failed:\n' + '\n'.join(failures))
        validate_prepared_manifest(path, suite, results / 'verification-after')
        report['status'] = 'native_execution_passed'
    except BaseException as error:
        report['error'] = str(error)
        raise
    finally:
        try:
            unchanged = _sha(path) == before_manifest and _all_files(work) == before_inputs
        except (OSError, ValueError) as error:
            unchanged = False
            report['immutable_input_error'] = str(error)
        report['immutable_inputs_unchanged'] = unchanged
        if not unchanged:
            report['status'] = 'native_execution_failed'
        (results / 'native-results.json').write_text(json.dumps(report, indent=2) + '\n')
        if not unchanged:
            raise AssertionError('Native consumer changed immutable prepared inputs')
    print('Native Intel ' + suite + ' execution passed; final hashes/signatures/ABI verified.')
    return report
