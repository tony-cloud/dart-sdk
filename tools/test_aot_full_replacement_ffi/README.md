# Full-domain FFI acceptance fixture

This fixture exercises the full-profile compiler/runtime FFI admission and
encrypted replacement path. Building or checking its source does not establish
a runtime result; acceptance requires running the native consumer below.

`../test_aot_full_replacement_ffi.py --check-fixtures` runs Python/source
consistency checks only. Normal execution requires native ARM64 macOS/Linux,
the admitted PRODUCT runtime, a separately built `product_full_ffi_test`, and
the real envelope producer used by `test_aot_full_replacement.py`. A closed
gate, missing artifact, unsupported host, compiler error or child crash fails
the run; none is converted into a successful skip.

The real run compiles **one** released `released_fixture.c` library before
creating B/N. All exports and callback consumers for U already exist there.
It produces full B, then native N from both source and imported dill, rewrites
only the mutable package's Dart files, and produces U with `--patch-base=B`.
Each N is used independently for N→B and N→U. The embedder creates two fresh
groups per invocation, checks that no resolver registrations leaked from the
first group, and never installs B before U in the same group.

The unchanged consumer in `main.dart` observes the changed constructor default
and canonical constants. The real package remains mutable. U removes one
library/class, adds a library and FFI definitions, changes `Plugin` fields and
method parameters, and changes `PluginRecord` from 16 to 24 bytes. The matching
released C exports are V1/V2, so no test calls an export with an incompatible
compound signature. N/B have no application callback signatures or references
to the update-only HFA/Union/packed/ABI-specific paths.

The harness checks fresh library/class identities, retired library/type/closure
rejection, resolver transfer and active postinstall registration, zero-initialized
then populated native cache slots, release-owned asset-map identity, absent
metadata carrier, resolver/asset/process/dynamic-lookup routes and error retry
without native invocation. U executes scalar/bool/pointer/ABI-specific/Handle,
leaf borrowed memory, HFA/indirect/nested/packed/Union and compound VarArgs calls.
It creates Pointer.fromFunction, static and capturing isolateLocal, aggregate
isolateLocal, queued scalar/compound listener and capturing group-bound
callbacks; it exercises exception fallback, GC, self-close, concurrent foreign
threads, post-group owner re-entry, and a pool spanning multiple signed alias
pages. Metadata must point at the released static interpreter callback entry.
Cleanup has an independent embedder oracle: immediately after a separately
invoked self-close, its metadata must be dead and registration counts unchanged.
After the native workers join and queued deliveries complete, every closeable
address must have dead metadata, Dart owner lists/fields must be empty, and live
persistent roots, isolate callback-list length, code-cache length, ports and
keepalive state must return to their checkpoint. That checkpoint explicitly
includes the one noncloseable Pointer.fromFunction descriptor/root/list entry;
its metadata must remain live until shutdown. No zero-baseline assumption or
unconditional Dart cleanup return value serves as the cleanup oracle.
The original detailed raw-library callback and call-out suites remain required.

## Build integration

`runtime/bin/BUILD.gn` builds `product_full_ffi_test` under the same native-ARM64
interpreter/macOS/Linux condition as `product_ffi_test`, using its PRODUCT
engine dependency and conditional Perfetto dependency. The macOS signer chooses
entitlements by executable basename, so the target has its own
`runtime/tools/entitlements/product_full_ffi_test.plist`, identical to the
existing `product_ffi_test.plist`. It contains only the isolated host-test
library-validation entitlement; no JIT, unsigned-executable-memory or debugger
entitlement is enabled. The driver verifies the signed binary before execution.

After admission/build integration:

```sh
python3 tools/test_aot_full_replacement_ffi.py \
  --build-dir /ABS/BUILD \
  --work-dir /ABS/PERSISTENT/ACCEPTANCE \
  --envelope-dart /ABS/CLI/DART \
  --envelope-packages /ABS/CLI/.dart_tool/package_config.json \
  --envelope-producer /ABS/REAL/full_replacement_envelope_producer.dart
```

The producer contract is exactly the existing full-replacement test contract:
`producer BASE_SBR PAYLOAD_SBR OUTPUT_DIRECTORY`; output `valid.json`,
`tampered.json`, and `expired.json`. It must use the real CLI encryption and
license-envelope code, with the existing public test key/context
(`encrypted-aot-host-*`, key bytes `0x42`). For B the two SBR arguments are
both B. The host checks wrong-key, tampered and expired rejection before valid
publication, then compares the decrypted SBR with the compiler output. It never
constructs substitute JSON or swaps in a test parser. The installer context
uses the existing fixture's `target_os=ios`; execution is still a macOS/Linux
host test, not an iOS runtime test. The driver requires this real producer and
does not provide a plaintext-install option.

## Generated admission rejection mode

The driver invokes the reusable
`pkg/dart2bytecode/test/full_replacement_admission_mutations.dart` generator
against its real U. Its `cases.json` contract is `format_version: 1`, nonempty
`cases` with `name`, absolute `path`, a specific `diagnostic` substring, and
boolean `poisoned`. Optional `positive_cases` contain `name` and absolute `path`.
The driver validates this contract before starting any native rejection child.

For each generated rejection and each source/from-dill native N, the harness
receives a separate mode:

```text
product_full_ffi_test N CASE.sbr RELEASED_NATIVE --reject B.sbr DIAGNOSTIC poisoned|preflight CASE_NAME
```

This mode deliberately feeds the valid-inner-digest SBR directly to the public
loader. It must report the case's specific diagnostic; a generic error or a
crash fails the driver. It checks no host resolver/native export/callback
execution, unchanged active root/plugin/removed-library/library-table and
native-assets-map identities, unchanged callback ownership counts, and the
expected preflight or staged-poison boundary. Failed/inactive N must remain
unrunnable. Poisoned groups must reject resolver changes and retry installation.
The failed group is always destroyed; a new group then installs and executes
valid B, including released resolver/asset calls, to establish recovery.

These direct-loader mutation tests establish no authentication claim. Base,
update and the generator's cleared-FFI-flag positive case each still run through
the separate mandatory real encrypted-envelope producer and positive harness.
The ordinary inert `fullFfiBad*` functions supply evaluated annotations solely
for mutation. `Native<int>` and the ordinary app `FakeNative` are nested pragma
options, rather than direct annotations that CFE would treat as FFI declarations.
Their Dart bodies are never invoked.

## Remaining acceptance gates

These are **not** established by this bounded fixture:

- Actual execution until compiler/loader FFI admission is implemented and this
  new target is built/run. Python checks are not Dart/C compilation evidence.
- The broad existing full-replacement profile/domain/poison/staging-failure,
  raw-component, runnable/multi-isolate/repeat-install and recovery matrix.
- Execution of the generated mutation matrix: paired/native metadata, library
  flags, normal/wide/truncated FFI/address-resolution opcodes and cache operands,
  old-native/ExternalCall paths, closure pragmas and the reserved-carrier
  manifest exclusion. Its producer has run; native rejection/poison/recovery
  assertions remain pending. Missing ABI mappings and native-field layout
  rejection still require the existing focused suites. Narrow source-level
  plain external, old-native and native-wrapper rejection controls also remain.
- Every callback ABI, Handle callbacks, listener Zone errors, nested cross-group
  restoration and comprehensive instruction/RX/RW audit. This fixture adds real
  full-install representatives; the detailed callback suite remains necessary.
- Server signature verification and host/release-context tampering beyond the
  existing installer's encryption/license checks. The public wrapper parser
  test does not establish upstream signature verification.
- CLI release archive differ enforcement, native-plugin archive equality,
  updater persistence/retry behavior and release signing. The fixture's native
  file hash only proves this host test reused its one native binary.
- Physical signed iOS N→B/N→U no-JIT execution, encryption/signing integration,
  no debugger/unsigned-executable entitlements, and Flutter/plugin/device E2E.

No process or build resources are started by the source-only check. The real
driver bounds subprocesses with timeouts; native worker threads are joined
before callbacks close, and every isolate group and the fixture library are
shut down by the embedder.

## Static Dart validation performed (2026-11-01)

Used the existing
`xcodebuild/ReleaseARM64/dart-sdk/bin/dart`; no pub get, network access,
gen_snapshot, C++ build, SDK rebuild, or new target wiring was performed.

```text
dart format tools/test_aot_full_replacement_ffi
Formatted 8 files (0 changed) in 0.03 seconds.

dart analyze --fatal-infos TEMP/base
Analyzing base...
No issues found!

dart analyze --fatal-infos TEMP/update
Analyzing update...
No issues found!
```

Each temporary package contained the unchanged `main.dart`, the respective
base/update files copied to `lib/`, a synthetic `pubspec.yaml` named
`full_ffi_plugin`, and a local `.dart_tool/package_config.json` with language
version 3.8. Both analysis commands exited zero. Temporary directories were
removed afterward. Intentional rejection fixtures were formatted but excluded
from positive analysis. Initial analysis found missing explicit native type
arguments on `Native.addressOf` and the ABI-specific integer's required const
constructor; both were corrected before the clean runs. These are formatter
and analyzer results only, not full-admission, AOT-build, or runtime evidence.

The existing Dart binary was subsequently authorized to run the current
bytecode producer source and mutation generator only. With an isolated package
configuration, the matching existing `vm_platform_product.dill`, PRODUCT define,
full profile and a test-only `a`-repeated 64-character host ABI:

```text
dart pkg/dart2bytecode/bin/dart2bytecode.dart [profile/package/platform options] -o B.sbr main.dart
base-producer: exit 0
dart pkg/dart2bytecode/bin/dart2bytecode.dart [same options] --patch-base=B.sbr -o U.sbr main.dart
update-producer-fixed: exit 0
dart --packages=.dart_tool/package_config.json pkg/dart2bytecode/test/full_replacement_admission_mutations.dart U.sbr OUTDIR
Generated 30 native loader rejection cases.
mutation-generator: exit 0
```

The first U producer attempt rejected a direct `@Native<int>` annotation on an
ordinary function with `Native functions and fields must be marked external.`
(exit 254). Nesting that constant, and `FakeNative`, inside the expected pragma
option preserved inert metadata and made U compile successfully. Final isolated
B/U fatal-info analysis remained clean. The generated JSON contained 30 rejects
and one positive and passed the driver contract parser. Synthetic wrong-version,
generic-diagnostic, nonboolean-poison, relative-path and duplicate-name contracts
were each rejected. No gen_kernel, gen_snapshot, native target or SDK build was
run during these checks; no native N or runtime acceptance was established.
