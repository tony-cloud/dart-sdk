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
private application declarations and the public SDK API (see "Retention rule for
`dart:` libraries"), so AOT optimizations retain dynamic dispatch and type
checks needed by later classes. It skips closed-world devirtualization and
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

## Call path to patchable functions

Calls from AOT code to a patchable function (static calls, constructors,
factories, tear-off forwarders and `super` calls) load the target `Function`
from the object pool into `FUNCTION_REG`, keep the arguments descriptor in
`ARGS_DESC_REG`, and call through `Function::entry_point`. Installing a bytecode
patch only swaps that entry point to the `InterpretCall` stub (which needs the
`Function` and the arguments descriptor), so no executable AOT code is rewritten
and no runtime call is made per call. The checked entry point is always used:
the flow graph builder already clears unchecked-entry use for patchable targets.
`tools/test_aot_patchable_direct_calls.py` checks the generated code (no
`CallStaticFunction` stub references) and the AOT/patched results.

## Closed world over the base class hierarchy

The profile compiles app classes as open: a patch can add subclasses. Treating
them as open for every call and type check is expensive (about 60% of the gap to
a TFA build in a Flutter-like workload: slow type tests and lost devirtualization
and field-accessor inlining), so `gen_snapshot --shorebird-structural-patching`
instead optimizes over the **base class hierarchy**: the classes of the snapshot,
i.e. class ids below `L = ClassTable::NumCids()` at snapshot time (persisted in
`ObjectStore::shorebird_base_num_cids`). This is sound because

- patches cannot change the superclass, interfaces or type parameters of an
  existing class, nor its fields or the signature of its functions (function
  bodies change through `Function::entry_point`); and
- every class a patch adds gets a class id `>= L`.

So, for class ids `< L`, the subtype relation and the set of subclasses are exactly
those of the snapshot. Code compiled over the base hierarchy:

- **Type testing stubs** (`as`, argument checks), and the inline fast path in front
  of their call at assignability checks, use class id ranges for open
  classes too. A stub only treats a range hit as success; every other class id
  (including the ids of patch-added classes) takes the slow type test, which sees
  the patch's classes. `is` tests against an open class with a single range are
  lowered to `cid in range ? true : (cid < L ? false : generic test)`.
- **Calls** whose receiver static type is a class with a closed set of concrete
  base subclasses are devirtualized over those subclasses (a direct call, an
  inlined implicit field accessor, or a polymorphic call with per-class targets).
  Receivers of patch-added classes miss the guard below and take the generic
  switchable call, which resolves the patch's overrides. Calls to patched function
  bodies still go through `Function::entry_point`.

### The per-class override guard

One hole remains: a patch may add an instance member (method, getter, setter,
operator, or a field and its implicit accessors) to an **existing** class that
overrides an inherited member. That is accepted (it is not a rejection and not a
global switch): the runtime keeps a byte array indexed by class id
(`IsolateGroup::shorebird_overridden_cids`, reachable from
`Thread::shorebird_overridden_cids_`; zero-initialized memory of which only the
touched pages are resident). The byte of every base class id is 1 and that of
patch-added class ids stays 0. Each devirtualized site first tests
`flag[cid(receiver)] != 0` (one load and compare) and takes the generic path
otherwise. When a structural patch is installed and an added member of an
existing class `C` overrides an inherited one (resolved in `C`'s superclass
chain, including mixins), the byte is cleared for `C` and for every base subclass
of `C` that does not itself declare that member (subclasses that do keep their
fast path). Setting `DART_TRACE_SHOREBIRD_STRUCTURAL_PATCHING=1` or
`--trace_shorebird_structural_patching` prints the classes cleared per member.

User-visible behavior: **patches that add overrides to existing classes are
allowed**, are dispatched correctly, and those classes (and the subclasses
inheriting the override) fall back to generic dispatch at call sites compiled in
the base snapshot (a small slowdown there). Unrelated classes keep the fast
path. `dart2bytecode --patch-base ... --structural-patch-report=<file>` writes
the instance members the patch adds to existing classes as JSON so tools can
warn that devirtualized calls on those classes fall back.

Limits: a polymorphic call over more than `--max_exhaustive_polymorphic_checks`
classes with different targets stays generic. Existing optimizations that depend
on the exact class of a receiver (allocation sites) use the same guard, and the
`==`-to-identity rewrite is not applied to patchable classes. Range analysis,
`has_dynamically_extendable_classes()` and the monomorphic checked entries are
unchanged. `tools/test_aot_structural_closed_world.py` covers added subclasses,
added implementors through `is`/`as`, patched devirtualized bodies, added
overrides (method, getter, operator, field getter, added field) on existing
classes and on a superclass with base subclasses, and that only the expected
classes are cleared.

## Retention rule for `dart:` libraries

Patches cannot add code to `dart:` libraries and can never name their private
declarations, and the language forbids extending, implementing or mixing in a
private, `final` or `sealed` class from another library. Application libraries
are annotated completely (every class, member and private declaration, because
a patch can add code to an existing library). `dart:` libraries are not: every
`callable` or `dynamically-callable` annotation is an AOT root (an entry
point), so annotating private `dart:` declarations only bloated the snapshot
with dynamic-invocation forwarders, tear-offs, method extractors, `Function`
objects, strings and code source maps (`dart:typed_data` dominated).
`annotateShorebirdStructuralComponent` therefore applies this rule to `dart:`:

* Public classes are annotated as before (callable; extendable and overridable
  unless `final`, `sealed` or private). Their public members are callable and
  dynamically callable (and overridable). Their private members are not
  annotated.
* Private classes get no class annotation (they are neither extendable nor
  instantiated roots): they are retained exactly when reachable code allocates
  them, for example `List()` returning a `_GrowableList` or `Float32List(n)`
  returning a `_Float32List`.
* An instance member of a private class is dynamically callable (only; that is
  not a root by itself) if its name and getter/setter kind is declared by a
  public supertype, including `Object`. This is the "public selector" rule: a
  patch can call exactly those on an object whose runtime class is private,
  statically through the public interface (`list.add(1)`), dynamically
  (`(x as dynamic).add(1)`, which the interpreter only allows for
  dynamically-callable functions; others raise `NoSuchMethodError`) or by
  tear-off (`list.add`). With `--shorebird-structural-patching` or
  `--shorebird-full-replacement`, `gen_snapshot` does not root such members;
  once the class is allocated it keeps the member, its dynamic invocation
  forwarder (where one is needed) and, via normal selector handling, any
  tear-off support. Public-named members that no public supertype declares are
  not annotated: only an unjustified dynamic call could reach them.
* Private top-level and static members and private constructors are not
  annotated, with exceptions that a patch can reach indirectly and that stay
  retained: (a) declarations the patch compilers reference by name
  (`_StringBase._interpolate`, `_SuspendState` for `async`, `LateError`,
  `_Cell`, `_Future`, `_Enum`, `_GrowableList._literal`, FFI helpers, ...; the
  list `_bytecodeSdkDependencies` is checked by a test against every private
  `dart:` `LibraryIndex` lookup in `core_types.dart`, `pkg/vm/lib/**` and
  `pkg/dart2bytecode/lib/**`); (b) targets of redirecting factories (the CFE
  compiles `MapEntry(k, v)` as a call of `MapEntry._`), with their class; (c)
  classes with a const constructor and their instance fields, since a patch
  can create constants (`Endian.little`, `Abi.macosArm64` with its private
  `_OS`, enum values) and the bytecode refers to classes and fields by name.

A patch that needs a removed private declaration (`_GrowableList`,
`_StringBase`, a private member such as `_growToNextCapacity`, or
`dart:_internal`) still cannot be expressed: the bytecode compiler rejects it.
`tools/test_aot_structural_public_api.py` records this together with patches
that use public `dart:` APIs (collections, strings, typed data, dynamic calls,
tear-offs, Future/Stream, language features) that the base program never
reaches, and compares their results with the host VM. Patch bytecode that
references a declaration the snapshot dropped is rejected at install time with
the missing name in the message.

The unsound upper bound from not annotating public selectors of private
classes at all (host `widgets.dart` snapshot 14.3 to 7.6 MB) is therefore not
reachable; the rule above measures 14.3 to 10.2 MB (see the commit message).
Limiting `tornOff` in `table_selector_assigner.dart` to public methods was
considered and not done: without TFA nothing proves that no `dart:` code
tears off a private instance method through dynamic dispatch.

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
- `tools/test_aot_structural_closed_world.py --build-dir <build>` checks the
  closed-world devirtualization, base-world type checks and the override guard
  (see "Closed world over the base class hierarchy").
- `tools/test_aot_structural_smi_cast.py --build-dir <build>` checks that a
  failing `as` cast of a Smi (or any non-subtype) after a cast that a
  patch-added class passed throws `TypeError`. The slow type test caches that
  positive answer in the call site's SubtypeTestCache and the 1/2-input cache
  lookup stubs used to load the class id of a Smi as a heap object (SIGSEGV).
  The same crash reproduces in stock AOT (upstream Dart, with and without TFA),
  which the test also covers.
- `tools/test_aot_patchable_direct_calls.py --build-dir <build>` checks that AOT
  callers of patchable static, constructor, tear-off and `super` targets call
  through `Function::entry_point` (no `CallStaticFunction` stub) and observe the
  AOT bodies before, and the interpreted replacements after, a patch.
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
