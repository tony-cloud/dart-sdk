# Shorebird structural interpreter patches

This opt-in profile permits additive declarations in an AOT application without
JIT compilation. It builds on the public bytecode loader and interpreter;
`DART_DYNAMIC_MODULES` remains disabled. It does not provide unrestricted hot
reload or support every source change.

## Baseline and bytecode contract

Build the AOT kernel with `--aot --no-tfa --shorebird-structural-patching`, and
pass `--shorebird-structural-patching` to `gen_snapshot`. Pass the same profile
flag to both full baseline and updated `dart2bytecode` compilation. Generate the
update with `--patch-base` referencing that baseline bytecode.

The frontend annotates the host hierarchy and callable declarations, including
private and SDK declarations, so AOT optimizations retain dynamic dispatch and
type checks needed by later classes. It skips closed-world devirtualization and
dynamic-invocation elimination. This increases retained code and snapshot size.
Ordinary `--no-tfa` does not enable this profile.

The native snapshot persists a structural capability on application libraries.
The bytecode library declaration flag `1 << 2` independently identifies the
structural profile. The compiler requires matching baseline and update markers;
the runtime requires the native marker. A runtime flag alone cannot upgrade an
old snapshot. Full release and patch must use matching SDK and build settings.
The profile also emits standalone instance-field initializer bytecode.

## Accepted and rejected changes

Within the profile, patches can add libraries, classes (including generic
classes), functions, methods, constructors, and static fields. New classes use
ordinary VM layouts. Existing function bodies can change while retaining their
signatures. A new subclass can override a host interface method; the baseline
profile preserves the AOT dispatch and type-test paths needed for this.

Instance fields can be added to existing ordinary mutable classes. Their values
use VM-owned extension storage, so old objects retain their physical layout and
old field values. Added fields may have declaration initializers, nullable
implicit defaults, or `late` initialization. Declaration initializers for
preexisting objects run lazily on first access; new constructor bodies initialize
new objects normally. This lazy behavior is observable for side-effecting
initializers and is not a replay of the original construction operation.

The compiler rejects removed declarations, changed existing signatures (including
optional parameters), changed existing fields or initializers, and changed class
inheritance, interfaces, type parameters, or declaration flags. A non-nullable
field initialized only in a constructor cannot be added to an existing class:
there is no safe automatic constructor replay for existing objects. Use a
declaration initializer or `late`, or publish a new release.

Added instance fields on an existing const, deeply immutable, or native-wrapper
class are rejected. The runtime also rejects inheritance of extension storage by
such classes. Canonical/shared immutable objects require a different migration
and canonicalization contract. New native/external functions and replacement
`dart:` libraries are rejected. These limitations mean this additive profile is
not full structural replacement parity.

## Installation and failure boundary

The existing `Dart_ReloadBytecodePatch` ABI recognizes the profile marker. Install
once in a fresh, single-isolate group before `Dart_IsolateMakeRunnable`. The Flutter
engine applies its authenticated encrypted patch in this window, before preparing
the isolate for application execution.

The loader registers all declaration identities before resolving code, permitting
forward and mutually recursive references. It decodes function bodies before
attaching them. Declaration registration itself mutates the transient group; it
is not rolled back in place. Once registration starts, any error poisons the
group. Dart invocation and making that group runnable are then rejected. The
embedder must destroy the failed group and create a fresh group from the base
snapshot. It must not continue using the partly registered group.

Extension storage uses GC-traced weak properties keyed by object identity. The
receiver can be collected even if its extension values refer back to it. Sending
mutable objects between isolates copies the extension values using the same
forwarding graph as the ordinary fields, preserving cycles and aliases without
sharing mutable sender state. Groups with added fields use the slow object-copy
path so extension storage participates in copying and GC.

This document describes the execution format, not key provisioning. The
production encrypted loader must authenticate/decrypt the envelope before
calling the bytecode API. Structural code executes as interpreter data; it does
not generate executable pages. Existing native executable files and their cache
are a separate plaintext boundary from encrypted bytecode delivery/storage.

## Acceptance checks

Build matching `dart`, `gen_snapshot`, and `dart_engine_aot_shared` with
`dart_enable_shorebird_interpreter=true` and `dart_dynamic_modules=false`.

- `tools/test_aot_structural_patch.py --build-dir <build>` exercises AOT callers
  of added declarations, generic/interface dispatch, preserved preexisting fields,
  declaration and late initializers, compacting GC, weak receiver collection,
  and a real isolate round trip with cycles, backreferences, and sender isolation.
  It checks profile/boot/repeated-install guards and failure-group teardown with
  a clean baseline in the same VM process.
- `tools/test_aot_bytecode_patch.py --build-dir <build>` covers ordinary body-only
  dispatch, rejected patch preservation, FFI callbacks, constructor retention,
  and ConstFinder compatibility.
- The workspace `scripts/verify_encrypted_aot_runtime.py --structural` runs the
  structural harness through the actual encrypted C API, including wrong-key,
  tampered-envelope and expiry rejection.

For shipping-mode coverage, additionally build `gen_snapshot_product`,
`vm_platform_product`, and `dart_engine_aot_shared_product` in the same Release
output directory. Run both structural commands above with `--product`; the
workspace encrypted command also needs `--structural`. This selects the PRODUCT
snapshot compiler, platform kernel and shared AOT runtime together and defines
`dart.vm.product=true` during kernel and bytecode compilation. Ordinary Release
shared-runtime tests are non-PRODUCT and cannot replace these checks: PRODUCT
snapshot dictionary pruning changes the runtime-resolved callable surface.

Host tests do not establish Flutter framework compatibility on iOS by themselves.
A matching Flutter engine and release snapshot must additionally be built and
exercised on a physical device with JIT disabled.
