// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';
import 'dart:typed_data';

import 'package:dart2bytecode/bytecode_generator.dart';
import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/dart2bytecode.dart';
import 'package:dart2bytecode/declarations.dart' as dbc;
import 'package:dart2bytecode/options.dart';
import 'package:front_end/src/api_unstable/vm.dart'
    show CompilerOptions, computePlatformBinariesLocation;
import 'package:kernel/ast.dart';
import 'package:kernel/binary/ast_from_binary.dart'
    show BinaryBuilderWithMetadata;
import 'package:kernel/binary/ast_to_binary.dart' show BinaryPrinter;
import 'package:test/test.dart';
import 'package:vm/bytecode/full_replacement.dart';
import 'package:vm/kernel_front_end.dart' as kernel;
import 'package:vm/metadata/procedure_attributes.dart';
import 'package:vm/metadata/shorebird_replacement.dart';
import 'package:vm/native_assets/metadata.dart';
import 'package:vm/native_assets/synthesizer.dart';
import 'package:vm/transformations/type_flow/table_selector_assigner.dart';

final dummyUri = Uri.parse('dummy');

const abi = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

class _BytesSink implements Sink<List<int>> {
  final bytes = BytesBuilder();
  @override
  void add(List<int> data) => bytes.add(data);
  @override
  void close() {}
}

Library carrier() => NativeAssetsSynthesizer.synthesizeLibrary({
  'native-assets': <String, Object>{},
});

void main() {
  late Directory directory;
  late File source;
  final platform = computePlatformBinariesLocation().resolve(
    'vm_platform.dill',
  );
  setUp(() {
    directory = Directory.systemTemp.createTempSync('native-assets-profile-');
    File(
      '${directory.path}/plugin.dart',
    ).writeAsStringSync('class Plugin { int value() => 42; }');
    source = File('${directory.path}/main.dart')
      ..writeAsStringSync(
        "import 'plugin.dart'; void main() { print(Plugin().value()); }",
      );
  });
  tearDown(() => directory.deleteSync(recursive: true));

  Future<File> writeKernel(String name, Component component) async {
    final file = File('${directory.path}/$name.dill');
    final sink = file.openWrite();
    BinaryPrinter(sink).writeComponentFile(component);
    await sink.close();
    return file;
  }

  Future<kernel.KernelCompilationResults> compileSource() =>
      kernel.compileToKernel(
        kernel.KernelCompilationArguments(
          source: source.uri,
          options: CompilerOptions()
            ..sdkSummary = platform
            ..fileSystem = kernel.createFrontEndFileSystem(null, null)
            ..target = kernel.createFrontEndTarget('vm', supportMirrors: false),
          requireMain: true,
          includePlatform: false,
        ),
      );

  test('only the metadata-only reserved URI bypasses profile markers', () {
    for (final profile in [(false, false), (true, false), (false, true)]) {
      final metadata = carrier();
      final plugin = Library(
        Uri.parse('package:plugin/plugin.dart'),
        fileUri: dummyUri,
      );
      final near = Library(
        Uri.parse('vm:ffi:native-assets-extra'),
        fileUri: dummyUri,
      );
      final component = Component(libraries: [metadata, plugin, near]);
      expect(isNativeAssetsMetadataLibrary(metadata), isTrue);
      expect(isNativeAssetsMetadataLibrary(near), isFalse);
      annotateWithoutTypeFlow(
        component,
        shorebirdStructuralPatching: profile.$1,
        shorebirdFullReplacement: profile.$2,
      );
      final attributes =
          component.metadata[ProcedureAttributesMetadataRepository
                  .repositoryTag]
              as ProcedureAttributesMetadataRepository;
      expect(attributes.mapping.containsKey(metadata), isFalse);
      for (final library in [plugin, near]) {
        expect(attributes.mapping[library]!.isWithoutTypeFlowAnalysis, isTrue);
        expect(
          attributes.mapping[library]!.isShorebirdStructuralPatching,
          profile.$1,
        );
        expect(
          attributes.mapping[library]!.isShorebirdFullReplacement,
          profile.$2,
        );
      }
    }
  });

  test(
    'reserved URI cannot conceal executable definitions or dependencies',
    () {
      final function = carrier()
        ..addProcedure(
          Procedure(
            Name('hidden'),
            ProcedureKind.Method,
            FunctionNode(ReturnStatement(IntLiteral(1))),
            isStatic: true,
            fileUri: dummyUri,
          ),
        );
      final field = carrier()
        ..addField(Field.mutable(Name('hidden'), fileUri: dummyUri));
      final cls = carrier()..addClass(Class(name: 'Hidden', fileUri: dummyUri));
      final dependency = carrier()
        ..addDependency(
          LibraryDependency.import(
            Library(Uri.parse('package:plugin/plugin.dart'), fileUri: dummyUri),
          ),
        );
      for (final library in [function, field, cls, dependency]) {
        expect(
          () => isNativeAssetsMetadataLibrary(library),
          throwsFormatException,
        );
      }
    },
  );

  test(
    'direct bytecode API omits carrier from complete replacement payload',
    () async {
      final result = await compileSource();
      final component = result.component!;
      final metadata = carrier();
      component.libraries.add(metadata);
      metadata.parent = component;
      final sink = _BytesSink();
      generateBytecode(
        component,
        sink,
        libraries: component.libraries
            .where((l) => l.importUri.scheme != 'dart')
            .toList(),
        options: BytecodeOptions(shorebirdFullReplacement: true),
        coreTypes: result.coreTypes!,
        hierarchy: result.classHierarchy!,
        target: kernel.createFrontEndTarget('vm', supportMirrors: false)!,
        extraLoadedLibraries: result.loadedLibraries,
      );
      final decoded = dbc.Component.read(
        BufferedReader(LinkReader(), sink.bytes.takeBytes()),
      );
      final uris = decoded.libraries
          .map((l) => l.importUri!.stringConstantValue)
          .toList();
      expect(uris, contains(source.uri.toString()));
      expect(uris, contains(directory.uri.resolve('plugin.dart').toString()));
      expect(uris, isNot(contains('vm:ffi:native-assets')));
      metadata.addField(Field.mutable(Name('hidden'), fileUri: dummyUri));
      final invalidSink = _BytesSink();
      expect(
        () => generateBytecode(
          component,
          invalidSink,
          libraries: component.libraries
              .where((l) => l.importUri.scheme != 'dart')
              .toList(),
          options: BytecodeOptions(shorebirdFullReplacement: true),
          coreTypes: result.coreTypes!,
          hierarchy: result.classHierarchy!,
          target: kernel.createFrontEndTarget('vm', supportMirrors: false)!,
          extraLoadedLibraries: result.loadedLibraries,
        ),
        throwsFormatException,
      );
      expect(invalidSink.bytes.isEmpty, isTrue);
    },
  );

  test(
    'imported carrier is omitted and forged carrier fails before output',
    () async {
      final imported = await writeKernel(
        'assets',
        Component(libraries: [carrier()]),
      );
      final output = File('${directory.path}/base.sbr');
      Future<int> compile(File input) => runCompilerWithCommandLineArguments([
        '--platform=${platform.toFilePath()}',
        '--shorebird-full-replacement',
        '--shorebird-host-abi=$abi',
        '--import-dill=${input.path}',
        '-o',
        output.path,
        source.path,
      ]);
      expect(await compile(imported), 0);
      final bundle = decodeShorebirdReplacement(output.readAsBytesSync());
      final decoded = dbc.Component.read(
        BufferedReader(LinkReader(), bundle.component),
      );
      final uris = decoded.libraries
          .map((l) => l.importUri!.stringConstantValue)
          .toList();
      expect(
        bundle.manifest.currentUris,
        contains(directory.uri.resolve('plugin.dart').toString()),
      );
      expect(
        bundle.manifest.currentUris,
        isNot(contains('vm:ffi:native-assets')),
      );
      expect(uris, unorderedEquals(bundle.manifest.currentUris));
      output.deleteSync();
      final forged = carrier()
        ..addField(Field.mutable(Name('hidden'), fileUri: dummyUri));
      final invalid = await writeKernel(
        'forged',
        Component(libraries: [forged]),
      );
      expect(await compile(invalid), kernel.compileTimeErrorExitCode);
      expect(output.existsSync(), isFalse);
    },
  );

  test(
    'from-dill native full baseline excludes carrier and preserves plugin markers',
    () async {
      final base = File('${directory.path}/base.sbr');
      expect(
        await runCompilerWithCommandLineArguments([
          '--platform=${platform.toFilePath()}',
          '--shorebird-full-replacement',
          '--shorebird-host-abi=$abi',
          '-o',
          base.path,
          source.path,
        ]),
        0,
      );
      final result = await compileSource();
      final component = result.component!;
      final metadata = carrier();
      component.libraries.add(metadata);
      metadata.parent = component;
      final input = await writeKernel('input', component);
      final output = File('${directory.path}/native.dill');
      expect(
        await kernel.runCompiler(
          kernel.createCompilerArgParser().parse([
            '--platform=${platform.toFilePath()}',
            '--aot',
            '--no-tfa',
            '--shorebird-full-replacement',
            '--shorebird-host-abi=$abi',
            '--shorebird-replacement-base=${base.path}',
            '--from-dill=${input.path}',
            '-o',
            output.path,
            source.path,
          ]),
          '',
        ),
        0,
      );
      final attributes = ProcedureAttributesMetadataRepository();
      final replacement = ShorebirdReplacementMetadataRepository();
      final loaded = Component()
        ..addMetadataRepository(attributes)
        ..addMetadataRepository(replacement);
      BinaryBuilderWithMetadata(
        output.readAsBytesSync(),
        disableLazyReading: true,
      ).readComponent(loaded);
      final loadedCarrier = loaded.libraries.singleWhere(
        isNativeAssetsMetadataLibrary,
      );
      expect(attributes.mapping.containsKey(loadedCarrier), isFalse);
      final plugin = loaded.libraries.singleWhere(
        (l) => l.importUri.path.endsWith('plugin.dart'),
      );
      expect(attributes.mapping[plugin]!.isShorebirdFullReplacement, isTrue);
      final manifest = ShorebirdReplacementManifest.decode(
        replacement.mapping.values.single,
      );
      expect(manifest.baselineUris, contains(plugin.importUri.toString()));
      expect(manifest.baselineUris, isNot(contains('vm:ffi:native-assets')));
      loadedCarrier.addField(Field.mutable(Name('hidden'), fileUri: dummyUri));
      final forged = await writeKernel('forged-native', loaded);
      final failedOutput = File('${directory.path}/rejected-native.dill');
      await expectLater(
        kernel.runCompiler(
          kernel.createCompilerArgParser().parse([
            '--platform=${platform.toFilePath()}',
            '--aot',
            '--no-tfa',
            '--shorebird-full-replacement',
            '--shorebird-host-abi=$abi',
            '--shorebird-replacement-base=${base.path}',
            '--from-dill=${forged.path}',
            '-o',
            failedOutput.path,
            source.path,
          ]),
          '',
        ),
        throwsFormatException,
      );
      expect(failedOutput.existsSync(), isFalse);
    },
  );
}
