// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/bytecode_generator.dart' show generateBytecode;
import 'package:dart2bytecode/options.dart';
import 'package:dart2bytecode/dart2bytecode.dart';
import 'package:dart2bytecode/declarations.dart' as dbc;
import 'package:dart2bytecode/constant_pool.dart';
import 'package:dart2bytecode/dbc.dart';
import 'package:dart2bytecode/disassembler.dart';
import 'package:dart2bytecode/full_replacement.dart';
import 'package:dart2bytecode/patch.dart';
import 'package:front_end/src/api_unstable/vm.dart'
    show computePlatformBinariesLocation;
import 'package:kernel/binary/ast_from_binary.dart'
    show BinaryBuilderWithMetadata;
import 'package:kernel/ast.dart' as ast;
import 'package:kernel/core_types.dart';
import 'package:kernel/class_hierarchy.dart';
import 'package:kernel/target/targets.dart';
import 'package:vm/modular/target/vm.dart';
import 'package:test/test.dart';
import 'package:vm/bytecode/full_replacement.dart';
import 'package:vm/kernel_front_end.dart' as kernel;
import 'package:vm/metadata/procedure_attributes.dart';
import 'package:vm/metadata/shorebird_replacement.dart';

import 'bytecode_generator_test.dart' as support;

const abi = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

void main() {
  late Directory directory;
  late File source;
  final platform = computePlatformBinariesLocation()
      .resolve('vm_platform.dill')
      .toFilePath();
  setUp(() {
    directory = Directory.systemTemp.createTempSync('full-replacement-test-');
    source = File('${directory.path}/main.dart');
  });
  tearDown(() => directory.deleteSync(recursive: true));

  Future<File> compile(
    String contents,
    String name, {
    File? base,
    String host = abi,
    List<String> extraArguments = const [],
  }) async {
    source.writeAsStringSync(contents);
    final output = File('${directory.path}/$name.sbr');
    expect(
      await runCompilerWithCommandLineArguments([
        '--platform=$platform',
        '--shorebird-full-replacement',
        '--shorebird-host-abi=$host',
        ...extraArguments,
        if (base != null) '--patch-base=${base.path}',
        '-o',
        output.path,
        source.path,
      ]),
      0,
    );
    return output;
  }

  test(
    'complete replacement retains const layout, signatures, statics and removals',
    () async {
      File(
        '${directory.path}/removed.dart',
      ).writeAsStringSync('class Removed {}');
      final baseline = await compile('''
import 'removed.dart';
class Widget { final int value; const Widget(this.value); }
int oldStatic = 1;
int unchanged() => 42;
int changed() => 1;
void main() { print(const Widget(1)); print(Removed()); }
''', 'base');
      final updated = await compile(
        '''
class Widget { final String value; final int added; const Widget(this.value, this.added); }
String newStatic = 'new';
int unchanged() => 42;
int changed([int value = 2]) => value;
void main() { print(const Widget('new', 3)); print(changed()); }
''',
        'patch',
        base: baseline,
      );
      final base = decodeShorebirdReplacement(baseline.readAsBytesSync());
      final patch = decodeShorebirdReplacement(updated.readAsBytesSync());
      expect(patch.manifest.removedUris, [
        File('${directory.path}/removed.dart').uri.toString(),
      ]);
      expect(patch.manifest.baselineDigest, base.manifest.currentDigest);
      final component = dbc.Component.read(
        BufferedReader(LinkReader(), patch.component),
      );
      expect(
        component.libraries.single.flags,
        dbc.LibraryDeclaration.shorebirdFullReplacementFlag,
      );
      final top = component.libraries.single.classes.first;
      expect(
        top.members.functions.map((f) => f.name.toString()),
        containsAll(["'unchanged'", "'changed'"]),
      );
      expect(top.members.fields.single.name.toString(), "'newStatic'");
      expect(
        component.libraries.single.classes.last.members.fields,
        hasLength(2),
      );
      expect(component.dynModuleEntryPoint, isNotNull);
      expect(
        () => createBytecodePatch(base.component, patch.component),
        throwsFormatException,
      );
      expect(
        () => createBytecodePatch(
          base.component,
          patch.component,
          shorebirdStructuralPatching: true,
        ),
        throwsFormatException,
      );
      final rebuilt = createShorebirdFullReplacement(
        patch.component,
        hostAbi: parseShorebirdHostAbi(abi),
        rootUri: source.uri.toString(),
        baseline: baseline.readAsBytesSync(),
      );
      expect(rebuilt, updated.readAsBytesSync());
      final m = base.manifest;
      final incomplete = ShorebirdReplacementManifest(
        hostAbi: m.hostAbi,
        baselineDigest: m.baselineDigest,
        currentDigest: m.currentDigest,
        rootUri: m.rootUri,
        baselineUris: [m.rootUri],
        currentUris: [m.rootUri],
        removedUris: [],
      );
      expect(
        () => createShorebirdFullReplacement(
          patch.component,
          hostAbi: m.hostAbi,
          rootUri: m.rootUri,
          baseline: encodeShorebirdReplacement(incomplete, base.component),
        ),
        throwsFormatException,
      );
      final otherRoot = m.baselineUris.firstWhere((uri) => uri != m.rootUri);
      final wrongRoot = ShorebirdReplacementManifest(
        hostAbi: m.hostAbi,
        baselineDigest: m.baselineDigest,
        currentDigest: m.currentDigest,
        rootUri: otherRoot,
        baselineUris: m.baselineUris,
        currentUris: m.currentUris,
        removedUris: [],
      );
      expect(
        () => createShorebirdFullReplacement(
          patch.component,
          hostAbi: m.hostAbi,
          rootUri: m.rootUri,
          baseline: encodeShorebirdReplacement(wrongRoot, base.component),
        ),
        throwsFormatException,
      );
    },
  );

  test(
    'extra source roots match the native domain and retain registrants',
    () async {
      final generated = File(
        '${directory.path}/generated_plugin_registrant.dart',
      );
      final packageRoot = Directory('${directory.path}/registrant')
        ..createSync();
      final packageLibrary = File(
        '${packageRoot.path}/lib/src/dart_plugin_registrant.dart',
      )..createSync(recursive: true);
      packageLibrary.writeAsStringSync('''
@pragma('vm:entry-point')
class DartPluginRegistrant {
  @pragma('vm:entry-point')
  static void ensureInitialized() {}
}
void main() {}
''');
      generated.writeAsStringSync('''
@pragma('vm:entry-point')
class GeneratedPluginRegistrant {
  @pragma('vm:entry-point')
  static void register() {}
}
''');
      final packages = File('${directory.path}/package_config.json');
      packages.writeAsStringSync(
        jsonEncode({
          'configVersion': 2,
          'packages': [
            {
              'name': 'registrant',
              'rootUri': packageRoot.uri.toString(),
              'packageUri': 'lib/',
              'languageVersion': '3.12',
            },
          ],
        }),
      );
      final extraArguments = [
        '--packages=${packages.path}',
        '--source=${generated.uri}',
        '--source=package:registrant/src/dart_plugin_registrant.dart',
        '-Dflutter.dart_plugin_registrant=${generated.uri}',
      ];
      const program = '''
const registrant = String.fromEnvironment('flutter.dart_plugin_registrant');
void main() { print(registrant); }
''';
      final baseline = await compile(
        program,
        'extra_base',
        extraArguments: extraArguments,
      );
      final base = decodeShorebirdReplacement(baseline.readAsBytesSync());
      final expected = canonicalReplacementUris([
        source.uri.toString(),
        generated.uri.toString(),
        'package:registrant/src/dart_plugin_registrant.dart',
      ]);
      expect(base.manifest.rootUri, source.uri.toString());
      expect(base.manifest.baselineUris, expected);
      expect(base.manifest.currentUris, expected);
      void checkRegistrant(ShorebirdReplacementBundle bundle) {
        final component = dbc.Component.read(
          BufferedReader(LinkReader(), bundle.component),
        );
        final root = component.libraries.singleWhere(
          (library) =>
              library.importUri!.stringConstantValue == source.uri.toString(),
        );
        expect(
          root.classes.first.members.fields
              .singleWhere((field) => field.name.toString() == "'registrant'")
              .value!
              .stringConstantValue,
          generated.uri.toString(),
        );
        final library = component.libraries.singleWhere(
          (library) =>
              library.importUri!.stringConstantValue ==
              generated.uri.toString(),
        );
        final registrant = library.classes.singleWhere(
          (cls) => cls.name.toString() == "'GeneratedPluginRegistrant'",
        );
        expect(
          registrant.members.functions.map((member) => member.name.toString()),
          contains("'register'"),
        );
        expect(component.dynModuleEntryPoint.toString(), '${source.uri}::main');
      }

      checkRegistrant(base);
      final native = File('${directory.path}/extra_native.dill');
      final nativeArguments = [
        '--platform=$platform',
        '--aot',
        '--no-tfa',
        '--shorebird-full-replacement',
        '--shorebird-host-abi=$abi',
        '--shorebird-replacement-base=${baseline.path}',
        ...extraArguments,
        '-o',
        native.path,
        source.path,
      ];
      expect(
        await kernel.runCompiler(
          kernel.createCompilerArgParser().parse(nativeArguments),
          '',
        ),
        0,
      );
      final component = ast.Component();
      BinaryBuilderWithMetadata(
        native.readAsBytesSync(),
        disableLazyReading: true,
      ).readComponent(component);
      expect(component.mainMethod!.enclosingLibrary.importUri, source.uri);
      expect(
        canonicalReplacementUris(
          component.libraries
              .where((library) => library.importUri.scheme != 'dart')
              .map((library) => library.importUri.toString()),
        ),
        expected,
      );
      await expectLater(
        kernel.runCompiler(
          kernel.createCompilerArgParser().parse(
            nativeArguments
                .where((arg) => !arg.startsWith('--source='))
                .toList(),
          ),
          '',
        ),
        throwsFormatException,
      );
      generated.writeAsStringSync(
        generated.readAsStringSync().replaceFirst(
          'register()',
          'register([int added = 1])',
        ),
      );
      final update = await compile(
        program,
        'extra_update',
        base: baseline,
        extraArguments: extraArguments,
      );
      final patch = decodeShorebirdReplacement(update.readAsBytesSync());
      expect(patch.manifest.rootUri, base.manifest.rootUri);
      expect(patch.manifest.baselineUris, expected);
      expect(patch.manifest.currentUris, expected);
      expect(patch.manifest.removedUris, isEmpty);
      checkRegistrant(patch);
      final missing = File('${directory.path}/missing.sbr');
      expect(
        await runCompilerWithCommandLineArguments([
          '--platform=$platform',
          '--shorebird-full-replacement',
          '--shorebird-host-abi=$abi',
          '--packages=${packages.path}',
          '--source=${directory.uri.resolve('missing.dart')}',
          '-o',
          missing.path,
          source.path,
        ]),
        isNot(0),
      );
      expect(missing.existsSync(), isFalse);
    },
  );

  test(
    'native baseline binds actual root and domain and stamps canonical descriptor',
    () async {
      final base = await compile('void main() { print(1); }', 'base');
      final output = File('${directory.path}/native.dill');
      final args = [
        '--platform=$platform',
        '--aot',
        '--no-tfa',
        '--shorebird-full-replacement',
        '--shorebird-host-abi=$abi',
        '--shorebird-replacement-base=${base.path}',
        '-o',
        output.path,
        source.path,
      ];
      expect(
        await kernel.runCompiler(
          kernel.createCompilerArgParser().parse(args),
          '',
        ),
        0,
      );
      final metadata = ShorebirdReplacementMetadataRepository();
      final attributes = ProcedureAttributesMetadataRepository();
      final component = ast.Component()
        ..addMetadataRepository(metadata)
        ..addMetadataRepository(attributes);
      BinaryBuilderWithMetadata(
        output.readAsBytesSync(),
        disableLazyReading: true,
      ).readComponent(component);
      expect(
        metadata.mapping.keys.single,
        component.mainMethod!.enclosingLibrary,
      );
      expect(
        metadata.mapping.values.single,
        decodeShorebirdReplacement(
          base.readAsBytesSync(),
        ).manifest.baselineProjection().encode(),
      );
      expect(
        attributes
            .mapping[component.mainMethod!.enclosingLibrary]!
            .isShorebirdFullReplacement,
        isTrue,
      );
      expect(
        attributes
            .mapping[component.mainMethod!.enclosingLibrary]!
            .isWithoutTypeFlowAnalysis,
        isTrue,
      );
      File('${directory.path}/extra.dart').writeAsStringSync('int extra = 0;');
      source.writeAsStringSync(
        "import 'extra.dart'; void main() { print(extra); }",
      );
      await expectLater(
        kernel.runCompiler(kernel.createCompilerArgParser().parse(args), ''),
        throwsFormatException,
      );
      await expectLater(
        kernel.runCompiler(
          kernel.createCompilerArgParser().parse(
            args
                .where((a) => !a.startsWith('--shorebird-replacement-base'))
                .toList(),
          ),
          '',
        ),
        throwsArgumentError,
      );
    },
  );

  test(
    'full profile admits lowered FFI calls, closures and address resolution',
    () async {
      final output = await compile('''
import 'dart:ffi';
@Native<Int32 Function(Int32)>(symbol: 'full_ffi_scalar')
external int scalar(int value);
final class Record extends Struct {
  @Int32()
  external int value;
}
int invoke(Pointer<NativeFunction<Int32 Function(Int32)>> pointer) =>
    pointer.asFunction<int Function(int)>()(42);
void main() {
  print(scalar(7));
  print(Native.addressOf<NativeFunction<Int32 Function(Int32)>>(scalar));
  print(sizeOf<Record>());
}
''', 'ffi');
      final wrapped = decodeShorebirdReplacement(output.readAsBytesSync());
      final component = dbc.Component.read(
        BufferedReader(LinkReader(), wrapped.component),
      );
      expect(
        component.libraries.single.flags,
        dbc.LibraryDeclaration.shorebirdFullReplacementFlag |
            dbc.LibraryDeclaration.usesDartFfiFlag,
      );
      final functions = component.libraries.single.classes.expand(
        (cls) => cls.members.functions,
      );
      expect(
        functions.where(
          (function) =>
              (function.flags & dbc.FunctionDeclaration.isNativeFlag) != 0,
        ),
        isNotEmpty,
      );
      expect(component.codes.expand((code) => code.closures), isNotEmpty);
      expect(
        createShorebirdFullReplacement(
          wrapped.component,
          hostAbi: parseShorebirdHostAbi(abi),
          rootUri: source.uri.toString(),
        ),
        output.readAsBytesSync(),
      );
      // The native-baseline frontend uses the same post-lowering validator.
      expect(
        await kernel.runCompiler(
          kernel.createCompilerArgParser().parse([
            '--platform=$platform',
            '--aot',
            '--no-tfa',
            '--shorebird-full-replacement',
            '--shorebird-host-abi=$abi',
            '--shorebird-replacement-base=${output.path}',
            '-o',
            '${directory.path}/ffi.dill',
            source.path,
          ]),
          '',
        ),
        0,
      );
      expect(
        await kernel.runCompiler(
          kernel.createCompilerArgParser().parse([
            '--platform=$platform',
            '--aot',
            '--no-tfa',
            '--shorebird-full-replacement',
            '--shorebird-host-abi=$abi',
            '--shorebird-replacement-base=${output.path}',
            '--from-dill=${directory.path}/ffi.dill',
            '-o',
            '${directory.path}/ffi-from-dill.dill',
            source.path,
          ]),
          '',
        ),
        0,
      );
    },
  );

  test(
    'direct bytecode API validates lowered FFI metadata using loaded SDK identity',
    () async {
      source.writeAsStringSync('''
import 'dart:ffi';
@Native<Int32 Function(Int32)>(symbol: 'direct_scalar')
external int scalar(int value);
void main() { print(scalar(1)); }
''');
      final target = VmTarget(TargetFlags());
      final result = await support.compileTestCaseToKernelProgram(
        source.uri,
        target: target,
      );
      final coreTypes = result.coreTypes ?? CoreTypes(result.component);
      final hierarchy =
          result.classHierarchy ?? ClassHierarchy(result.component, coreTypes);
      Uint8List generate() {
        final sink = support.ByteSink();
        generateBytecode(
          result.component,
          sink,
          options: BytecodeOptions(shorebirdFullReplacement: true),
          libraries: result.component.libraries
              .where((lib) => !result.loadedLibraries.contains(lib))
              .toList(),
          coreTypes: coreTypes,
          hierarchy: hierarchy,
          target: target,
          extraLoadedLibraries: result.loadedLibraries,
        );
        return sink.builder.takeBytes();
      }

      expect(
        createShorebirdFullReplacement(
          generate(),
          hostAbi: parseShorebirdHostAbi(abi),
          rootUri: source.uri.toString(),
        ),
        isNotEmpty,
      );
      final native = result.component.mainMethod!.enclosingLibrary.procedures
          .singleWhere((procedure) => procedure.isExternal);
      native.annotations.clear();
      expect(generate, throwsFormatException);
    },
  );

  test(
    'bundle validates full flags, FFI declarations, owners and cache operands',
    () async {
      final output = await compile('''
import 'dart:ffi';
@Native<Int32 Function(Int32)>(symbol: 'scalar')
external int scalar(int value);
@pragma('vm:ffi:native', 42)
int wrongAnnotation() => 0;
@pragma('vm:ffi:native', Native<int>(symbol: 'scalar', assetId: 'asset'))
int wrongSignature() => 0;
final class FakeNative<T> {
  final String symbol = 'scalar';
  final String assetId = 'asset';
  final bool isLeaf = false;
  const FakeNative();
}
@pragma('vm:ffi:native', FakeNative<Int32 Function(Int32)>())
int wrongIdentity() => 0;
int invoke(Pointer<NativeFunction<Int32 Function(Int32)>> pointer) =>
    pointer.asFunction<int Function(int)>()(7);
void main() { print(scalar(1)); }
''', 'mutations');
      final original = decodeShorebirdReplacement(
        output.readAsBytesSync(),
      ).component;
      dbc.Component read() =>
          dbc.Component.read(BufferedReader(LinkReader(), original));
      Uint8List write(dbc.Component component) {
        final writer = BufferedWriter(
          component.stringTable,
          component.objectTable,
          LinkWriter(),
        );
        component.write(writer);
        return writer.getContents();
      }

      void rejects(void Function(dbc.Component) mutate) {
        final component = read();
        mutate(component);
        expect(
          () => createShorebirdFullReplacement(
            write(component),
            hostAbi: parseShorebirdHostAbi(abi),
            rootUri: source.uri.toString(),
          ),
          throwsFormatException,
        );
      }

      List<dbc.FunctionDeclaration> functions(dbc.Component c) =>
          c.libraries.single.classes.first.members.functions;
      dbc.FunctionDeclaration scalar(dbc.Component c) =>
          functions(c).singleWhere((f) => f.name.toString() == "'scalar'");
      void replaceScalar(
        dbc.Component c, {
        int? flags,
        bool oldName = false,
        dbc.AnnotationsDeclaration? annotations,
        bool omitAnnotations = false,
      }) {
        final f = scalar(c);
        functions(c)[functions(c).indexOf(f)] = dbc.FunctionDeclaration(
          flags ?? f.flags,
          f.name,
          f.script,
          f.position,
          f.endPosition,
          f.typeParameters,
          f.numRequiredParameters,
          f.parameters,
          f.parameterFlags,
          f.returnType,
          oldName ? c.libraries.single.importUri : null,
          f.code,
          omitAnnotations ? null : annotations ?? f.annotations,
        );
      }

      for (final flags in [0, 2, 8 | 1, 8 | 4, 8 | 16]) {
        rejects((c) {
          final lib = c.libraries.single;
          c.libraries[0] = dbc.LibraryDeclaration(
            lib.importUri,
            flags,
            lib.name,
            lib.script,
            lib.classes,
          );
        });
      }
      for (final flag in [
        dbc.FunctionDeclaration.isNativeFlag,
        dbc.FunctionDeclaration.isExternalFlag,
      ]) {
        rejects((c) => replaceScalar(c, flags: scalar(c).flags & ~flag));
      }
      rejects((c) => replaceScalar(c, oldName: true));
      rejects(
        (c) => replaceScalar(
          c,
          flags:
              scalar(c).flags &
              ~(dbc.FunctionDeclaration.hasAnnotationsFlag |
                  dbc.FunctionDeclaration.hasPragmaFlag),
          omitAnnotations: true,
        ),
      );
      for (final name in [
        'wrongAnnotation',
        'wrongSignature',
        'wrongIdentity',
      ]) {
        rejects(
          (c) => replaceScalar(
            c,
            annotations: functions(
              c,
            ).singleWhere((f) => f.name.toString() == "'$name'").annotations,
          ),
        );
      }
      rejects((c) {
        final code = scalar(c).code!;
        final call = BytecodeDisassembler()
            .decode(code.bytecodes)
            .singleWhere((i) => i.opcode == Opcode.kFfiCall);
        code.bytecodes[call.pc + 1] = 255;
      });
      rejects((c) {
        final code = scalar(c).code!;
        final call = BytecodeDisassembler()
            .decode(code.bytecodes)
            .singleWhere((i) => i.opcode == Opcode.kFfiCall);
        code.bytecodes[call.pc] = Opcode.kFfiCall_Wide.index;
      });
      rejects((c) {
        final code = scalar(c).code!;
        final call = BytecodeDisassembler()
            .decode(code.bytecodes)
            .singleWhere((i) => i.opcode == Opcode.kFfiCall);
        code.bytecodes[call.pc] = Opcode.kExternalCall.index;
      });
      rejects((c) {
        final code = scalar(c).code!;
        final index = code.constantPool.entries.indexWhere(
          (e) => e is ConstantNativeFunction,
        );
        code.constantPool.entries[index] = ConstantObjectRef(null);
      });
      rejects((c) {
        final closure = c.codes.expand((code) => code.closures).first;
        closure.flags &= ~dbc.ClosureDeclaration.hasPragmaFlag;
      });
      rejects((c) {
        final code = scalar(c).code!;
        // The two-slot ExternalCall pool entry is rejected independently of opcodes.
        code.constantPool.addExternalCall();
      });
    },
  );

  test(
    'profile rejects external/native definitions and invalid mode binding',
    () async {
      for (final contents in [
        "external int native(); void main() {}",
        "import 'dart:mirrors'; void main() {}",
      ]) {
        source.writeAsStringSync(contents);
        expect(
          await runCompilerWithCommandLineArguments([
            '--platform=$platform',
            '--shorebird-full-replacement',
            '--shorebird-host-abi=$abi',
            '-o',
            '${directory.path}/invalid.sbr',
            source.path,
          ]),
          isNot(0),
        );
        expect(File('${directory.path}/invalid.sbr').existsSync(), isFalse);
      }
      source.writeAsStringSync('void main() {}');
      for (final flags in [
        ['--shorebird-full-replacement'],
        [
          '--shorebird-full-replacement',
          '--shorebird-host-abi=${abi.toUpperCase()}',
        ],
        [
          '--shorebird-full-replacement',
          '--shorebird-host-abi=$abi',
          '--shorebird-structural-patching',
        ],
      ]) {
        expect(
          await runCompilerWithCommandLineArguments([
            '--platform=$platform',
            ...flags,
            source.path,
          ]),
          isNot(0),
        );
      }
      final base = await compile('void main() {}', 'base');
      expect(
        () => createShorebirdFullReplacement(
          decodeShorebirdReplacement(base.readAsBytesSync()).component,
          hostAbi: Uint8List(32),
          rootUri: source.uri.toString(),
          baseline: base.readAsBytesSync(),
        ),
        throwsFormatException,
      );
    },
  );

  test(
    'manifest parser enforces bounds, utf8, canonical sets, root and digests',
    () {
      final digest = Uint8List(32);
      ShorebirdReplacementManifest manifest({
        List<String> uris = const ['file:a'],
        String root = 'file:a',
      }) => ShorebirdReplacementManifest(
        hostAbi: digest,
        baselineDigest: digest,
        currentDigest: digest,
        rootUri: root,
        baselineUris: uris,
        currentUris: uris,
        removedUris: const [],
      );
      final valid = manifest().encode();
      expect(ShorebirdReplacementManifest.decode(valid).encode(), valid);
      for (final uris in [
        ['file:b', 'file:a'],
        ['file:a', 'file:a'],
        [''],
        ['file:a\u0000'],
        ['dart:core'],
        ['vm:ffi:native-assets'],
        ['file:${'x' * shorebirdReplacementMaxUri}'],
      ]) {
        expect(() => manifest(uris: uris).encode(), throwsFormatException);
      }
      expect(
        () => manifest(root: 'file:missing').encode(),
        throwsFormatException,
      );
      for (var length = 0; length < valid.length; ++length) {
        expect(
          () => ShorebirdReplacementManifest.decode(valid.sublist(0, length)),
          throwsFormatException,
        );
      }
      expect(
        () => ShorebirdReplacementManifest.decode([...valid, 0]),
        throwsFormatException,
      );
      final malformed = Uint8List.fromList(valid);
      malformed[104] = 0xff;
      expect(
        () => ShorebirdReplacementManifest.decode(malformed),
        throwsFormatException,
      );

      final tooMany = Uint8List.fromList(valid);
      ByteData.sublistView(tooMany).setUint32(
        104 + utf8.encode('file:a').length,
        shorebirdReplacementMaxUris + 1,
        Endian.little,
      );
      expect(
        () => ShorebirdReplacementManifest.decode(tooMany),
        throwsFormatException,
      );
      expect(
        manifest(
          uris: ['vm:ffi:native-assets-extra'],
          root: 'vm:ffi:native-assets-extra',
        ).encode(),
        isNotEmpty,
      );
      final unicode = canonicalReplacementUris([
        'file:\u{10000}',
        'file:\u{e000}',
      ]);
      expect(unicode, ['file:\u{e000}', 'file:\u{10000}']);
      expect(manifest(uris: unicode, root: unicode.first).encode(), isNotEmpty);
      final payload = utf8.encode('DBC3');
      final m = ShorebirdReplacementManifest(
        hostAbi: digest,
        baselineDigest: replacementDigest(payload),
        currentDigest: replacementDigest(payload),
        rootUri: 'file:a',
        baselineUris: ['file:a'],
        currentUris: ['file:a'],
        removedUris: [],
      );
      final wrapper = encodeShorebirdReplacement(m, payload);
      expect(decodeShorebirdReplacement(wrapper).component, payload);
      for (final offset in [0, 8, 12, 16, 20, wrapper.length - 1]) {
        final bad = Uint8List.fromList(wrapper);
        bad[offset] ^= 1;
        expect(() => decodeShorebirdReplacement(bad), throwsFormatException);
      }
      expect(
        () => decodeShorebirdReplacement([...wrapper, 0]),
        throwsFormatException,
      );
    },
  );
}
