# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Trusted, complete native Intel case inventory; no serialized command dispatch."""
import hashlib
from pathlib import Path

SUITES = {'callouts': 'product_ffi_test', 'callbacks': 'product_ffi_callback_test',
          'full': 'product_full_ffi_test'}
CALL_OUT_MARKERS = [
    'Handle arguments/results and 21 nonleaf native GC calls passed',
    'leaf TypedData.address/view/element and Dart/native Struct.address passed',
    'PRODUCT ABI-specific args/results and four memory variants/missing mappings passed',
    'PRODUCT Union/nested/packed/inline-array leaf and nonleaf calls passed',
    'PRODUCT native DRT_FfiCall frame and signed static trampoline caller verified',
    'PRODUCT actual interpreted call-out/resolver/cache/error checks passed',
    'Released baseline native-assets positive/cache/error route passed',
    'PRODUCT per-call Handle scopes restored across success/throw/returned-error/propagate/nesting/VarArgs with embedding scope',
    'PRODUCT per-call Handle scopes restored across success/throw/returned-error/propagate/nesting/VarArgs with no embedding scope',
]
CALLBACK_RX = 'Callback source/alias byte identity and RX/RW checkpoints passed'
MISSING_SCOPE_FATAL = 'A Handle-returning native callback expects a current API scope'


def mutation_specs(work):
    """Keep synchronized with the pinned admission mutation producer (30 + 1)."""
    rows = []
    def add(names, diagnostic, poisoned=True):
        for name in names.split():
            rows.append({'name': name, 'path': str(work / 'admission-mutations' / (name + '.sbr')),
                         'diagnostic': diagnostic, 'poisoned': poisoned})
    add('missing-full ffi-only mirrors-flag structural-flag unknown-flag',
        'Replacement requires full-profile library flags.', False)
    add('plain-external old-native-flags', 'requires lowered FFI declarations')
    add('old-native-name', 'cannot define old native names')
    add('missing-ffi-annotation', 'Missing full replacement FFI pragma.')
    add('nonarray-metadata', 'annotations must be an evaluated array')
    add('fullFfiBadOption', 'Full replacement requires SDK FFI annotation identity.')
    add('fullFfiBadSignature', 'Full replacement FFI requires a function type.')
    add('fullFfiBadIdentity', 'Full replacement requires SDK FFI annotation identity.')
    add('invalid-ffi-owner', 'Invalid full replacement FFI call owner.')
    add('ffi-cache-out-of-range ffi-cache-wrong-kind wide-ffi-cache-out-of-range resolve-cache-out-of-range',
        'Invalid full replacement FFI cache operand.')
    add('resolve-missing-annotation-push', 'Invalid full replacement FFI address metadata.')
    add('resolve-wrong-annotation', 'Invalid full replacement FFI annotation.')
    add('external-pool', 'cannot contain native pools')
    add('external-opcode external-wide-opcode unknown-opcode truncated-ffi truncated-wide-ffi truncated-resolve truncated-wide-resolve',
        'Unsupported or truncated replacement opcode.')
    add('closure-missing-pragma', 'Missing full replacement FFI pragma.')
    add('reserved-assets-carrier', 'Invalid full replacement manifest', False)
    return rows, [{'name': 'cleared-ffi-flag',
                   'path': str(work / 'admission-mutations/cleared-ffi-flag.sbr')}]


def signed_runtime_paths(suite, work):
    names = {
        'callouts': ['libfixture.dylib', 'body.snapshot', 'structural.snapshot',
                     'ordinary-assets.snapshot', 'full.snapshot', 'full-from-dill.snapshot'],
        'callbacks': ['libcallback_fixture.dylib', 'baseline.snapshot', 'compiled_controls.snapshot',
                      'async_group.snapshot', 'shared-baseline.snapshot', 'async_shared.snapshot'],
        'full': ['libreleased_full_ffi.dylib', 'native-source.snapshot', 'native-from-dill.snapshot'],
    }[suite]
    return [work / 'strict-signing' / SUITES[suite], *(work / name for name in names)]


def expected_cases(suite, work, symbols):
    """Reconstruct all argv, statuses and oracles from trusted code and inputs."""
    work = Path(work)
    harness = work / 'strict-signing' / SUITES[suite]
    cases = []
    def case(name, args, required=(), forbidden=(), death=False):
        cases.append({'id': name, 'argv': [str(harness), *map(str, args)],
                      'expected_exit': 'nonzero' if death else 0,
                      'required_markers': list(required), 'forbidden_markers': list(forbidden),
                      'timeout_seconds': 600})
    if suite == 'callouts':
        library = work / 'libfixture.dylib'
        for mode in ('body', 'structural'):
            case(mode + '-baseline', [work / (mode + '.snapshot'), '--baseline'],
                 ['No-app-FFI baseline SDK dictionaries/resolver code passed'])
        case('ordinary-assets', [work / 'ordinary-assets.snapshot', library, '--assets-baseline'],
             ['Ordinary TFA native-assets map/isolate load passed without lazy FFI retention'])
        proof = [*symbols['DRT_FfiCall'], 0, 0]
        case('structural-execution', [work / 'structural.snapshot', work / 'new_uri.bytecode', library,
                                    (work / 'new_uri.dart').as_uri(), *proof], CALL_OUT_MARKERS)
        for stage, value in [('base', 1), ('update', 2)]:
            for label, snapshot in [('source', 'full.snapshot'), ('from-dill', 'full-from-dill.snapshot')]:
                case('full-' + label + '-' + stage,
                     [work / snapshot, work / ('full-' + stage + '.sbr'), library,
                      'package:product_plugin/plugin.dart', '--full-assets', value],
                     ['Full base/update preserves native-assets map identity/content; metadata carrier absent; package plugin identities fresh'])
    elif suite == 'callbacks':
        library = work / 'libcallback_fixture.dylib'
        proof = symbols['InterpreterFfiCallbackEntry']
        def callback(name, snapshot, bytecode, uri, mode, required=(), forbidden=(), death=False):
            case(name, [work / snapshot, work / bytecode if bytecode else '-', library,
                        (work / uri).as_uri() if uri else '-', *proof, mode], required, forbidden, death)
        for interpreted in (False, True):
            label = 'runtime' if interpreted else 'control'
            for route in range(4):
                mode = f'async-{label}-{route}'
                callback(mode, 'baseline.snapshot' if interpreted else 'async_group.snapshot',
                         'async_group.bytecode' if interpreted else None, 'async_group.dart', mode,
                         [f'NESTED_ROUTE_PASS:{route}', f'EMPTY_SOURCE_PASS:{route}'])
        for interpreted in (False, True):
            label = 'runtime' if interpreted else 'control'
            required = ['PRODUCT listener/group deferred/Zone/VarArgs/pthread/copy/GC/transitions/lifecycle checks passed',
                        CALLBACK_RX, 'STATIC_FRAME_FALLBACK:0:-1911', 'STATIC_FRAME_FALLBACK:1:-1912',
                        'STATIC_FRAME_FALLBACK:2:-1913', 'STATIC_ACCESS_OWNER_AND_INITIALIZER_PASS']
            if interpreted:
                required += ['STATIC_FRAME_BYTECODE:' + name for name in
                             ['ordinaryTarget', 'ordinaryStoreTarget', 'get:ordinaryGetter', 'set:ordinarySetter']]
            callback('async-' + label, 'baseline.snapshot' if interpreted else 'async_group.snapshot',
                     'async_group.bytecode' if interpreted else None, 'async_group.dart', 'async-' + label, required)
            callback('async-shared-' + label, 'shared-baseline.snapshot' if interpreted else 'async_shared.snapshot',
                     'async_shared.bytecode' if interpreted else None, 'async_shared.dart', 'async-shared-' + label,
                     ['PRODUCT group callback flagged shared-field access passed', CALLBACK_RX])
        callback('runtime', 'baseline.snapshot', 'callbacks.bytecode', 'callbacks.dart', 'runtime',
                 ['PRODUCT interpreted static/capture scalar/pointer/VarArgs, GC, nesting, self-close, growth/reuse, rejection cleanup and owned-isolate callback checks passed', CALLBACK_RX])
        callback('aggregates', 'baseline.snapshot', 'aggregates.bytecode', 'aggregates.dart', 'aggregate',
                 ['PRODUCT callback aggregate layouts/copy/aliases/offsets/guard pages/zero fallbacks passed',
                  'PRODUCT callback Handle caller-scope/error/GC/nesting checks passed with embedding scope',
                  'PRODUCT callback Handle caller-scope/error/GC/nesting checks passed with no embedding scope',
                  'PRODUCT callback Handle+aggregate descriptor/ownership/direct-scope/cleanup checks passed', CALLBACK_RX])
        callback('missing-handle-scope', 'baseline.snapshot', 'aggregates.bytecode', 'aggregates.dart', 'missing-scope',
                 [MISSING_SCOPE_FATAL], ['FORBIDDEN_HANDLE_TARGET_WITHOUT_SCOPE'], True)
        callback('handle-result-unwind', 'baseline.snapshot', 'handle_unwind.bytecode', 'handle_unwind.dart', 'handle-result-kill',
                 ['NATIVE_HANDLE_RESULT_UNWIND_EPILOGUE', 'API_UNWIND_OBSERVED'], ['FORBIDDEN_OUTER_DART_AFTER_KILL'])
        callback('compiled-controls', 'compiled_controls.snapshot', None, None, 'control',
                 ['Compiled static/capture/Handle/aggregate/listener/group-bound callback controls passed', CALLBACK_RX])
        for mode, marker in [('kill', 'NATIVE_CALLBACK_EPILOGUE:-19001'), ('handle-kill', 'NATIVE_HANDLE_CALLBACK_EPILOGUE')]:
            callback(mode, 'baseline.snapshot', 'kill.bytecode', 'kill.dart', mode,
                     [marker, 'API_UNWIND_OBSERVED'], ['FORBIDDEN_OUTER_DART_AFTER_KILL'])
    elif suite == 'full':
        library = work / 'libreleased_full_ffi.dylib'
        snapshots = [('source', work / 'native-source.snapshot'), ('from-dill', work / 'native-from-dill.snapshot')]
        mutations, positives = mutation_specs(work)
        for item in mutations:
            boundary = 'poisoned' if item['poisoned'] else 'preflight'
            for label, snapshot in snapshots:
                case(label + '-reject-' + item['name'],
                     [snapshot, item['path'], library, '--reject', work / 'base.sbr',
                      item['diagnostic'], boundary, item['name']],
                     [f'FULL_FFI_REJECT:{item["name"]}:{boundary}:fresh-B-recovery'])
        payloads = [('base', 'base', work / 'base.sbr'), ('update', 'update', work / 'update.sbr')]
        payloads += [(p['name'], 'update', Path(p['path'])) for p in positives]
        for name, mode, payload in payloads:
            for label, snapshot in snapshots:
                required = [f'FULL_FFI_PASS:{mode}:fresh-group-0', f'FULL_FFI_PASS:{mode}:fresh-group-1']
                if mode == 'update':
                    required += ['FULL_FFI_CALLBACKS:local-static-capture-aggregate-listener-group-gc-close-pages',
                                 'FULL_FFI_CLEANUP:dead-metadata-roots-list-ports-keepalive-static-survivor']
                digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
                case(label + '-' + name, [snapshot, payload, library, mode, work / ('envelopes-' + name),
                                         digest(work / 'base.sbr'), digest(payload)], required)
    else:
        raise ValueError('Unknown prepared suite')
    return cases
