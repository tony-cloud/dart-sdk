#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Run actual native ARM64 PRODUCT interpreter call-outs in a new raw library.

The native baseline has no application FFI. Ordinary bytecode is loaded at a new
URI with Dart_LoadLibraryFromBytecode; this does not establish FFI support for
full-domain or structural replacement and does not change their admission
policies. Callbacks, downloaded native code, and device support are unproven.
The native fixture is compiled and locally signed before starting the process.
"""

import argparse
import contextlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from test_interpreter_ffi_calls import NATIVE, DART, interpreter_entry_range

FORGED_CARRIER = r'''
import 'dart:io';
import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_from_binary.dart' show BinaryBuilderWithMetadata;
import 'package:kernel/binary/ast_to_binary.dart' show BinaryPrinter;
import 'package:vm/metadata/procedure_attributes.dart';
import 'package:vm/metadata/shorebird_replacement.dart';

Future<void> main(List<String> args) async {
  final component = Component()
    ..addMetadataRepository(ProcedureAttributesMetadataRepository())
    ..addMetadataRepository(ShorebirdReplacementMetadataRepository());
  BinaryBuilderWithMetadata(File(args[0]).readAsBytesSync(),
      disableLazyReading: true).readComponent(component);
  final carrier = component.libraries.singleWhere(
      (library) => library.importUri.toString() == 'vm:ffi:native-assets');
  carrier.addField(Field.mutable(Name('forgedNativeAssetDeclaration'),
      isStatic: true, initializer: IntLiteral(1), fileUri: carrier.fileUri));
  final sink = File(args[1]).openWrite();
  BinaryPrinter(sink).writeComponentFile(component);
  await sink.close();
}
'''

NATIVE_EXTRA = r'''
#include <sys/mman.h>
#include <unistd.h>
int64_t ProductAdd(int64_t a, int64_t b) { return a + b; }
int64_t ProductProcessOnly(int64_t value) { return value + 200; }
int64_t ProductAssetOnly(int64_t value) { return value + 500; }
intptr_t ProductIntPtr(intptr_t value) { return value + 1; }
static intptr_t product_abi_calls;
int16_t ProductAbiInt(int16_t value) { ++product_abi_calls; return value + 7; }
intptr_t ProductAbiCalls(void) { return product_abi_calls; }
void* ProductAllocate(intptr_t size) { return calloc(1, (size_t)size); }
void* ProductGuardPage(void) {
  void* result = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_NONE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
  if (result == MAP_FAILED) abort();
  return result;
}
void ProductFreeGuard(void* value) { munmap(value, sysconf(_SC_PAGESIZE)); }
int ProductTrampolineCaller(uint64_t start, uint64_t end, uint64_t image_base) {
  void* caller = __builtin_return_address(0);
  Dl_info info;
  if (dladdr(caller, &info) == 0) return 0;
  uintptr_t address = (uintptr_t)caller - (uintptr_t)info.dli_fbase + image_base;
  return start <= address && address < end;
}
typedef union { int64_t bits; double number; } ProductWord;
ProductWord ProductChangeUnion(ProductWord value) { value.bits += 17; return value; }
typedef struct { Small inner; ProductWord word; } ProductNested;
ProductNested ProductChangeNested(ProductNested value) {
  value.inner.a += 3; value.inner.b *= 2; value.word.bits += 5; return value;
}
typedef struct __attribute__((packed)) { uint8_t flag; double number; } ProductPacked;
ProductPacked ProductChangePacked(ProductPacked value) {
  value.flag += 2; value.number *= 3; return value;
}
typedef struct { int16_t values[3]; int16_t tag; } ProductInline;
ProductInline ProductChangeInline(ProductInline value) {
  value.values[0] += 10; value.values[1] += 20;
  value.values[2] += 30; value.tag += 40; return value;
}
'''

DART_EXTRA = r'''
// With no library at this asset ID, @Native resolves in the process, independently
// of the resolver registered on this loaded library.
@Native<Int64 Function(Int64)>(symbol: 'ProductProcessOnly', assetId: 'product-process-only')
external int productProcess(int value);
@Native<Int64 Function(Int64, Int64)>(symbol: 'ProductResolverAdd')
external int productResolvedAdd(int a, int b);
@Native<Int64 Function()>(symbol: 'ProductResolverMissing')
external int productMissing();
@Native<Int64 Function(Int64)>(symbol: 'ProductAssetOnly', assetId: 'product-release-asset')
external int productNativeAsset(int value);
@Native<Int64 Function()>(symbol: 'ProductMissingAssetSymbol', assetId: 'product-release-asset')
external int productMissingAssetSymbol();
@pragma('vm:entry-point')
int runNativeAsset() => productNativeAsset(9);
@pragma('vm:entry-point')
int runNativeAssetErrors() {
  var caught = 0;
  for (var i = 0; i < 2; ++i) {
    try { productMissingAssetSymbol(); }
    catch (error) {
      if (!error.toString().contains('ProductMissingAssetSymbol')) rethrow;
      ++caught;
    }
  }
  return caught;
}
@pragma('vm:entry-point')
int runResolverCache() => productResolvedAdd(7, 9) + productResolvedAdd(20, 22);
@pragma('vm:entry-point')
int runResolverAddressOf() {
  final function = Native.addressOf<NativeFunction<Int64 Function(Int64, Int64)>>(
      productResolvedAdd).asFunction<int Function(int, int)>();
  return function(30, 43);
}
@pragma('vm:entry-point')
int runResolverErrors() {
  var caught = 0;
  for (var i = 0; i < 2; ++i) {
    try { productMissing(); }
    catch (error) {
      if (!error.toString().contains('ProductResolverMissing')) rethrow;
      ++caught;
    }
  }
  return caught;
}
@AbiSpecificIntegerMapping({Abi.macosArm64: Int16(), Abi.linuxArm64: Int16()})
final class ProductAbiInt extends AbiSpecificInteger { const ProductAbiInt(); }
@AbiSpecificIntegerMapping({Abi.fuchsiaArm64: Int8()})
final class ProductMissingInt extends AbiSpecificInteger { const ProductMissingInt(); }
final class ProductAbiFields extends Struct {
  @ProductAbiInt() external int value;
  @Array(3) external Array<ProductAbiInt> items;
}
final class ProductWord extends Union {
  @Int64() external int bits;
  @Double() external double number;
}
final class ProductNested extends Struct {
  external Small inner;
  external ProductWord word;
}
@Packed(1)
final class ProductPacked extends Struct {
  @Uint8() external int flag;
  @Double() external double number;
}
final class ProductInline extends Struct {
  @Array(3) external Array<Int16> values;
  @Int16() external int tag;
}
void requireMissingMapping(String stage, void Function() action) {
  print('PRODUCT missing mapping begin: $stage');
  var caught = false;
  try { action(); }
  catch (error) {
    if (error is! ArgumentError) rethrow;
    final message = error.toString();
    if (!message.contains('ProductMissingInt') || !message.contains('missing mapping')) rethrow;
    caught = true;
  }
  if (!caught) throw StateError('missing ABI mapping was silently accepted');
  print('PRODUCT missing mapping caught ArgumentError: $stage');
}
void productAbiIntegers(DynamicLibrary lib) {
  final pointerInteger = lib.lookupFunction<IntPtr Function(IntPtr), int Function(int)>('ProductIntPtr');
  equal(0x100000000, pointerInteger(0xffffffff));
  final abiInteger = lib.lookupFunction<ProductAbiInt Function(ProductAbiInt), int Function(int)>('ProductAbiInt');
  final abiCalls = lib.lookupFunction<IntPtr Function(), int Function()>('ProductAbiCalls');
  equal(-293, abiInteger(-300));
  final callsBeforeUnsupported = abiCalls();
  final allocate = lib.lookupFunction<Pointer<Void> Function(IntPtr), Pointer<Void> Function(int)>('ProductAllocate');
  final free = lib.lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('FreeMemory');
  final memory = allocate(64);
  final guard = lib.lookupFunction<Pointer<Void> Function(), Pointer<Void> Function()>('ProductGuardPage')();
  final freeGuard = lib.lookupFunction<Void Function(Pointer<Void>), void Function(Pointer<Void>)>('ProductFreeGuard');
  try {
    final values = memory.cast<ProductAbiInt>();
    // Exercise the nonindexed load/store and indexed load/store native entries.
    values.value = -123;
    equal(-123, values.value);
    values[2] = -456;
    equal(-456, values[2]);
    values.value = 30000;
    equal(30000, values[0]);
    values[0] = -30000;
    equal(-30000, values.value);
    // The same four entries also accept Dart TypedData backing a compound.
    final fields = Struct.create<ProductAbiFields>();
    fields.value = -321;
    equal(-321, fields.value);
    fields.items[2] = -654;
    equal(-654, fields.items[2]);
    equal(-321, fields.value);
    print('PRODUCT ABI-specific positive args/results and four memory variants passed');
    final sentinels = memory.cast<Uint8>().asTypedList(64).toList().join(',');
    final missingPointers = [memory.cast<ProductMissingInt>(), guard.cast<ProductMissingInt>()];
    final labels = ['sentinel memory', 'PROT_NONE guard'];
    for (var i = 0; i < missingPointers.length; ++i) {
      final missing = missingPointers[i];
      final label = labels[i];
      requireMissingMapping('$label load', () { missing.value; });
      requireMissingMapping('$label store', () { missing.value = 1; });
      requireMissingMapping('$label indexed load', () { missing[1]; });
      requireMissingMapping('$label indexed store', () { missing[1] = 2; });
      equal(sentinels, memory.cast<Uint8>().asTypedList(64).join(','));
    }
    requireMissingMapping('function signature', () {
      final function = lib.lookupFunction<ProductMissingInt Function(ProductMissingInt), int Function(int)>('ProductAbiInt');
      function(1);
    });
    equal(callsBeforeUnsupported, abiCalls());
    print('PRODUCT unsupported signatures left native target count unchanged; invalid memory probes preserved sentinels/guard pages');
  } finally { freeGuard(guard); free(memory); }
  print('PRODUCT ABI-specific args/results and four memory variants/missing mappings passed');
}
void productAggregateLayouts(DynamicLibrary lib) {
  for (final leaf in [false, true]) {
    final union = leaf
      ? lib.lookupFunction<ProductWord Function(ProductWord), ProductWord Function(ProductWord)>('ProductChangeUnion', isLeaf: true)
      : lib.lookupFunction<ProductWord Function(ProductWord), ProductWord Function(ProductWord)>('ProductChangeUnion');
    final word = Union.create<ProductWord>()..bits = 100;
    equal(117, union(word).bits); equal(100, word.bits);
    final nested = leaf
      ? lib.lookupFunction<ProductNested Function(ProductNested), ProductNested Function(ProductNested)>('ProductChangeNested', isLeaf: true)
      : lib.lookupFunction<ProductNested Function(ProductNested), ProductNested Function(ProductNested)>('ProductChangeNested');
    final compound = Struct.create<ProductNested>();
    compound.inner.a = 20; compound.inner.b = 3.5; compound.word.bits = 80;
    final nestedResult = nested(compound);
    equal(23, nestedResult.inner.a); equal(7.0, nestedResult.inner.b);
    equal(85, nestedResult.word.bits); equal(20, compound.inner.a);
    final packed = leaf
      ? lib.lookupFunction<ProductPacked Function(ProductPacked), ProductPacked Function(ProductPacked)>('ProductChangePacked', isLeaf: true)
      : lib.lookupFunction<ProductPacked Function(ProductPacked), ProductPacked Function(ProductPacked)>('ProductChangePacked');
    final packedInput = Struct.create<ProductPacked>()..flag = 7..number = 2.5;
    equal(9, sizeOf<ProductPacked>());
    final packedResult = packed(packedInput);
    equal(9, packedResult.flag); equal(7.5, packedResult.number);
    final inline = leaf
      ? lib.lookupFunction<ProductInline Function(ProductInline), ProductInline Function(ProductInline)>('ProductChangeInline', isLeaf: true)
      : lib.lookupFunction<ProductInline Function(ProductInline), ProductInline Function(ProductInline)>('ProductChangeInline');
    final inlineInput = Struct.create<ProductInline>()..tag = 4;
    inlineInput.values[0] = 1; inlineInput.values[1] = 2; inlineInput.values[2] = 3;
    final inlineResult = inline(inlineInput);
    equal(11, inlineResult.values[0]); equal(22, inlineResult.values[1]);
    equal(33, inlineResult.values[2]); equal(44, inlineResult.tag);
  }
  print('PRODUCT Union/nested/packed/inline-array leaf and nonleaf calls passed');
}
@pragma('vm:entry-point')
void main(List<String> args) {
  runBaseFixture(args);
  final lib = DynamicLibrary.open(args[0]);
  final caller = lib.lookupFunction<Int32 Function(Uint64, Uint64, Uint64), int Function(int, int, int)>('ProductTrampolineCaller');
  equal(1, caller(int.parse(args[5]), int.parse(args[6]), int.parse(args[3])));
  equal(207, productProcess(7));
  productAbiIntegers(lib);
  productAggregateLayouts(lib);
  print('PRODUCT native DRT_FfiCall frame and signed static trampoline caller verified');
}
'''


def symbol_range(binary, name):
    symbols = []
    start = None
    for line in subprocess.check_output(['nm', '-n', str(binary)], text=True).splitlines():
        fields = line.split()
        if len(fields) != 3 or fields[1] not in ('t', 'T'):
            continue
        address = int(fields[0], 16)
        symbols.append(address)
        if fields[2] in (name, '_' + name):
            start = address
    if start is None:
        raise AssertionError(f'{binary} must retain local {name} symbol')
    return start, min(address for address in symbols if address > start)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--baseline-only', action='store_true')
    parser.add_argument('--body-profile', action='store_true',
                        help='Also diagnose raw loading against body-only SDK exports')
    parser.add_argument('--full-assets-only', action='store_true',
                        help='Run release-map publication checks without raw call-outs')
    args = parser.parse_args()
    if platform.machine().lower() not in ('arm64', 'aarch64') or sys.platform not in ('darwin', 'linux'):
        parser.error('This first PRODUCT call-out test requires native ARM64 macOS/Linux')
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()
    dart = build / 'dart'
    platform_file = build / 'vm_platform_product.dill'
    gen_snapshot = build / 'gen_snapshot_product'
    test = build / 'product_ffi_test'
    compiler = shutil.which('clang')
    if compiler is None:
        parser.error('clang is required to build the signed local native fixture')
    for artifact in (dart, platform_file, gen_snapshot, test):
        if not artifact.is_file():
            parser.error(f'Missing required artifact: {artifact}')

    def run(*command, log=None, expect_error=None):
        print('+', *command, flush=True)
        result = subprocess.run([str(part) for part in command], cwd=root,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=300)
        if log is not None:
            log.write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        if expect_error is None:
            result.check_returncode()
        elif result.returncode == 0 or expect_error not in result.stdout:
            raise AssertionError(f'Native backend did not reject forged carrier: {result.stdout}')
        return result.stdout

    def snapshot(kernel, output, profile):
        if sys.platform == 'darwin':
            run(gen_snapshot, '--snapshot-kind=app-aot-macho-dylib', *profile,
                f'--macho={output}', kernel)
            run('codesign', '--force', '--sign', '-', output)
        else:
            run(gen_snapshot, '--snapshot-kind=app-aot-elf', *profile,
                f'--elf={output}', kernel)

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else tempfile.TemporaryDirectory(prefix='product-ffi-'))
    with workspace as directory:
        work = Path(directory)
        baseline = work / 'baseline.dart'
        baseline.write_text('void main() {}\n')
        library = work / ('libfixture.dylib' if sys.platform == 'darwin' else 'libfixture.so')
        native_assets = work / 'released-native-assets.json'
        native_assets.write_text(json.dumps({
            'format-version': [1, 0, 0],
            'native-assets': {
                ('macos_arm64' if sys.platform == 'darwin' else 'linux_arm64'): {
                    'product-release-asset': ['absolute', str(library)],
                },
            },
        }))
        for mode, profile in [('body', []), ('structural', ['--shorebird-structural-patching'])]:
            kernel = work / f'{mode}.dill'
            native_snapshot = work / f'{mode}.snapshot'
            run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot', '--no-tfa',
                '-Ddart.vm.product=true', f'--platform={platform_file}', *profile,
                f'--native-assets={native_assets}',
                f'--output={kernel}', baseline)
            snapshot(kernel, native_snapshot, profile)
            run(test, native_snapshot, '--baseline', log=work / f'{mode}-baseline.log')
        ordinary_kernel = work / 'ordinary-assets.dill'
        ordinary_snapshot = work / 'ordinary-assets.snapshot'
        run(dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
            '-Ddart.vm.product=true', f'--platform={platform_file}',
            f'--native-assets={native_assets}', f'--output={ordinary_kernel}', baseline)
        snapshot(ordinary_kernel, ordinary_snapshot, [])
        run(test, ordinary_snapshot, library, '--assets-baseline',
            log=work / 'ordinary-assets.log')
        # Bypass the Dart producer's carrier validator deliberately: alter a
        # valid generated kernel and submit it directly to the native backend.
        # Preserve the genuine library profile metadata, so the observed error
        # must be the C++ metadata-only carrier guard.
        forge = work / 'forge_carrier.dart'
        forge.write_text(FORGED_CARRIER)
        forged_kernel = work / 'forged-carrier.dill'
        run(dart, f'--packages={root / ".dart_tool/package_config.json"}', forge,
            work / 'structural.dill', forged_kernel)
        forged_output = work / 'forged-carrier.snapshot'
        forged_output.unlink(missing_ok=True)
        output_option = '--macho' if sys.platform == 'darwin' else '--elf'
        snapshot_kind = 'app-aot-macho-dylib' if sys.platform == 'darwin' else 'app-aot-elf'
        run(gen_snapshot, f'--snapshot-kind={snapshot_kind}',
            '--shorebird-structural-patching', f'{output_option}={forged_output}',
            forged_kernel,
            expect_error='Native assets metadata library cannot define fields.',
            log=work / 'forged-carrier-rejected.log')
        if forged_output.exists():
            raise AssertionError('Rejected native metadata carrier produced an output artifact')
        print('Native backend rejected executable declaration in metadata-only carrier; no output produced')
        if args.baseline_only:
            print('PRODUCT no-app-FFI baseline inspection passed')
            return
        native = work / 'fixture.c'
        source = work / 'new_uri.dart'
        bytecode = work / 'new_uri.bytecode'
        parameters = ', '.join(f'int64_t i{i}, double d{i}' for i in range(12))
        total = ' + '.join(f'i{i} + d{i}' for i in range(12))
        native.write_text(NATIVE + NATIVE_EXTRA + f'\ndouble Overflow({parameters}) {{ return {total}; }}\n')
        native_types = ', '.join(['Int64, Double'] * 12)
        dart_types = ', '.join(['int, double'] * 12)
        values = ', '.join(f'{i+1}, {i+1}.5' for i in range(12))
        signature = f'Double Function({native_types}), double Function({dart_types})'
        overflow = (f"final overflow = leaf ? lib.lookupFunction<{signature}>('Overflow', isLeaf: true) "
                    f": lib.lookupFunction<{signature}>('Overflow');\n"
                    f'equal(162.0, overflow({values}));')
        dart_fixture = DART.replace('// OVERFLOW_CALL', overflow).replace(
            'void main(List<String> args)', 'void runBaseFixture(List<String> args)')
        source.write_text(dart_fixture + DART_EXTRA)
        run(compiler, '-shared', '-fPIC', '-O0', '-fno-omit-frame-pointer',
            '-I', root / 'runtime/include', native, '-o', library,
            *(['-ldl'] if sys.platform == 'linux' else []))
        if sys.platform == 'darwin':
            run('codesign', '--force', '--sign', '-', library)
        if not args.full_assets_only:
            run(dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                '-Ddart.vm.product=true', f'--platform={platform_file}', '-o', bytecode, source)
            drt_start, drt_end, image_base = interpreter_entry_range(test)
            trampoline_start, trampoline_end = symbol_range(test, 'FfiCallTrampoline')
        required = [
            'Handle arguments/results and 21 nonleaf native GC calls passed',
            'leaf TypedData.address/view/element and Dart/native Struct.address passed',
            'PRODUCT ABI-specific args/results and four memory variants/missing mappings passed',
            'PRODUCT Union/nested/packed/inline-array leaf and nonleaf calls passed',
            'PRODUCT native DRT_FfiCall frame and signed static trampoline caller verified',
            'PRODUCT actual interpreted call-out/resolver/cache/error checks passed',
            'Released baseline native-assets positive/cache/error route passed',
        ]
        # Structural snapshots preserve the SDK call surface needed by a new
        # library. A body-only baseline may legitimately lack unrelated SDK
        # entrypoints (the diagnostic fixture exposed core::identical).
        modes = (() if args.full_assets_only else
                 ('body', 'structural') if args.body_profile else ('structural',))
        for mode in modes:
            output = run(test, work / f'{mode}.snapshot', bytecode, library, source.as_uri(),
                         drt_start, drt_end, image_base, trampoline_start, trampoline_end,
                         log=work / f'{mode}-execution.log')
            for marker in required:
                if marker not in output:
                    raise AssertionError(f'{mode} did not complete: {marker}')

        # The full-domain fixture has no FFI imports/declarations. Only the
        # released native kernel receives the asset map. Verify that both its
        # source and --from-dill forms retain that same map while real package
        # Dart code remains in the fresh mutable domain.
        plugin_dir = work / 'plugin'
        (plugin_dir / 'lib').mkdir(parents=True, exist_ok=True)
        plugin = plugin_dir / 'lib/plugin.dart'
        plugin.write_text('class Plugin { int get value => 1; }\n')
        packages = work / 'full.packages.json'
        packages.write_text(json.dumps({'configVersion': 2, 'packages': [{
            'name': 'product_plugin', 'rootUri': plugin_dir.as_uri() + '/',
            'packageUri': 'lib/', 'languageVersion': '3.3',
        }]}))
        full_source = work / 'full.dart'
        full_source.write_text("import 'package:product_plugin/plugin.dart';\n"
                               "@pragma('vm:entry-point') int probe() => Plugin().value;\n"
                               'void main() {}\n')
        profile = ['--shorebird-full-replacement', '--shorebird-host-abi=' + 'a' * 64]
        full_compiler = [dart, root / 'pkg/dart2bytecode/bin/dart2bytecode.dart',
                         f'--packages={packages}', '-Ddart.vm.product=true',
                         f'--platform={platform_file}', *profile]
        full_base = work / 'full-base.sbr'
        run(*full_compiler, '-o', full_base, full_source)
        kernel = work / 'full.dill'
        kernel_args = [dart, root / 'pkg/vm/bin/gen_kernel.dart', '--aot', '--no-tfa',
                       f'--packages={packages}', '-Ddart.vm.product=true',
                       f'--platform={platform_file}', *profile,
                       f'--shorebird-replacement-base={full_base}']
        run(*kernel_args, f'--native-assets={native_assets}', f'--output={kernel}', full_source)
        full_snapshot = work / 'full.snapshot'
        snapshot(kernel, full_snapshot, ['--shorebird-full-replacement'])
        from_dill = work / 'full-from-dill.dill'
        run(*kernel_args, f'--from-dill={kernel}', f'--output={from_dill}', full_source)
        from_dill_snapshot = work / 'full-from-dill.snapshot'
        snapshot(from_dill, from_dill_snapshot, ['--shorebird-full-replacement'])
        plugin_uri = 'package:product_plugin/plugin.dart'
        for label, native_snapshot in [('source', full_snapshot), ('from-dill', from_dill_snapshot)]:
            run(test, native_snapshot, full_base, library, plugin_uri, '--full-assets', 1,
                log=work / f'full-{label}-base.log')
        plugin.write_text('class Plugin { int added = 40; int get value => 2; }\n')
        full_update = work / 'full-update.sbr'
        run(*full_compiler, f'--patch-base={full_base}', '-o', full_update, full_source)
        for label, native_snapshot in [('source', full_snapshot), ('from-dill', from_dill_snapshot)]:
            run(test, native_snapshot, full_update, library, plugin_uri, '--full-assets', 2,
                log=work / f'full-{label}-update.log')
        if args.full_assets_only:
            print('PRODUCT release-map publication checks passed; raw call-outs skipped')
        else:
            print('PRODUCT native ARM64 raw-library interpreter FFI call-out proof passed; '
                  'full admission/callbacks/devices remain unproven')


if __name__ == '__main__':
    main()
