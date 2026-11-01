# Native Intel prepared FFI proof

The three PRODUCT runners retain their combined modes and also support a split
workflow. Preparation may run x64 compiler tools through Rosetta on a Mac. It
never launches a PRODUCT harness, and produces `prepared_runtime_pending` only.
Native runtime acceptance requires a separate native Intel Mac. A certificate
private key stays on the preparation/signing Mac.

Use one unique task root, with suite directories named exactly `callouts`,
`callbacks`, and `full`:

```
/private/tmp/shorebird-macos-x64-proof-<nonce>/callouts
/private/tmp/shorebird-macos-x64-proof-<nonce>/callbacks
/private/tmp/shorebird-macos-x64-proof-<nonce>/full
```

Each suite directory must be fresh, empty, and outside the original build tree.
The paths must be identical on both machines: native-assets maps, source URIs,
and admission-mutation paths contain absolute paths. The manifest rejects
relocation, symlinks, hardlinks, undeclared files, or changed bytes.

Preparation, using exact pinned x64 build artifacts and a locally available
Developer ID Application identity:

```sh
python3 tools/test_aot_product_ffi.py --prepare-only \
  --target-os=macos --target-arch=x64 --macos-deployment-target=14.0 \
  --build-dir=/PATH/TO/X64/BUILD \
  --work-dir=/private/tmp/shorebird-macos-x64-proof-NONCE/callouts \
  --macos-signing-identity=IDENTITY
```

Use the same options with `test_aot_product_ffi_callbacks.py` and the `callbacks`
suite directory. Use `test_aot_full_replacement_ffi.py` with the `full` directory
and its existing `--envelope-producer`, `--envelope-dart`, and
`--envelope-packages` arguments. The envelope producer must support explicit
`--target-os=macos --target-arch=x64`. Partial-suite flags are rejected.

Preparation signs a private harness copy **before** computing the host ABI and
compiling snapshots. The ABI remains the existing length-prefixed v1 descriptor:
`darwin`, `x86_64`, PRODUCT/full profile, signed harness hash, original snapshot
compiler hash, and original platform hash. No ABI convention changes. Fixture C
and assembly compile explicitly for x86_64 with the recorded deployment target.
The original four inputs are verified on every exit and copied to
`original-inputs/` as provenance only. The copies are never command targets.
Actual preparation host, x64 launch-context translation probe, tool machines and
hashes are recorded separately from the target identity.

A successful preparation writes `prepared-manifest.json`. Its structural schema
is [native_ffi_prepared_manifest.schema.json](native_ffi_prepared_manifest.schema.json).
The trusted consumer additionally requires exact source revisions, role bindings,
complete inventories and all suite-specific commands/oracles reconstructed from
[native_ffi_prepared_cases.py](native_ffi_prepared_cases.py). The current counts
are 8 callout cases, 19 callback cases (including all 8 isolated transitions), and
66 full cases (30 admission rejects plus 3 encrypted positives, each through
source and from-dill snapshots). Missing Handle scope requires both nonzero
termination and its specific fatal diagnostic. Generic process failure cannot pass.

Transfer the complete suite directories unchanged. The transport/extraction
wrapper must independently pin the package SHA-256 and trusted SDK revision,
reject archive traversal/links, and create only the declared task root. Manifest
self-consistency is not authentication of an untrusted replacement package.
Use a temporary draft asset or another explicitly authorized transport; publishing
and credential management are outside these runners.

On native Intel, at the same trusted SDK source revision:

```sh
python3 tools/test_aot_product_ffi.py \
  --run-prepared=/private/tmp/shorebird-macos-x64-proof-NONCE/callouts/prepared-manifest.json
```

Repeat with the callback/full runner and corresponding manifest. No build,
compiler, signing identity, producer, or partial-suite options are accepted in
this mode. The consumer rejects ARM and Rosetta, verifies every immutable hash,
strict signature, certificate/team, x64 architecture, empty entitlement set,
released entry range and ABI. Static inspection uses only system `codesign`
verification/display, `otool`, `lipo`, `nm`, and `git`; it never compiles or signs.
Only the suite's exact private signed harness may execute a test case, with a
credential-free fixed environment. Only system-library dependencies and each
Mach-O's own install name are permitted.

Results go to a separate sibling `callouts-results/`, `callbacks-results/`, or
`full-results/` directory, which must not already exist. Every case has its own
log. `native-results.json` claims `native_execution_passed` only after the full
inventory and final hash/signature/ABI checks pass. Failures leave a failed
report and verify immutable input hashes, including after child termination.
Preparation files are never result/log destinations for this phase.

These runners establish bounded SDK host evidence when actually executed.
Synthetic protocol tests, cross compilation, preparation under Rosetta, actual
Flutter release packaging, physical iOS and server verification are separate
claims. No native Intel execution is implied by a prepared manifest.
