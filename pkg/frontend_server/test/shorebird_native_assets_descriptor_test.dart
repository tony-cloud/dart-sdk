// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io';

import 'package:front_end/src/api_unstable/vm.dart' show IncrementalSerializer;
import 'package:frontend_server/frontend_server.dart';
import 'package:kernel/ast.dart';
import 'package:kernel/kernel.dart' show loadComponentFromBinary;
import 'package:test/test.dart';
import 'package:vm/bytecode/full_replacement.dart';
import 'package:vm/kernel_front_end.dart';
import 'package:vm/native_assets/json_to_kernel_constant.dart';
import 'package:vm/native_assets/shorebird_descriptor.dart';
import 'package:vm/native_assets/synthesizer.dart';

import 'frontend_server_test.dart' show computePlatformBinariesLocation;

Library carrier(Map<String, Object> targets) =>
    NativeAssetsSynthesizer.synthesizeLibrary({'native-assets': targets});

Map<String, Object> describe(Component component, {Library? extra}) =>
    shorebirdNativeAssetsDescriptor(
      component,
      nativeAssetsLibrary: extra,
      target: 'ios_arm64',
    );

void replaceMapping(Library library, Constant constant) {
  final annotation = library.annotations.last as ConstantExpression;
  final pragma = annotation.constant as InstanceConstant;
  annotation.constant = InstanceConstant(pragma.classReference, [], {
    for (final entry in pragma.fieldValues.entries)
      entry.key: entry.key.asField.name.text == 'options'
          ? constant
          : entry.value,
  });
}

class _AddCarrier implements ProgramTransformer {
  @override
  void transform(Component component) {
    final library = carrier({'ios_arm64': <String, Object>{}});
    component.libraries.add(library);
    library.parent = component;
  }
}

class _FailDillWrite extends FrontendCompiler {
  new() : super(StringBuffer());

  @override
  Future<void> writeDillFile(
    KernelCompilationResults results,
    String filename, {
    bool filterExternal = false,
    IncrementalSerializer? incrementalSerializer,
  }) async {
    throw const FileSystemException('simulated output failure');
  }
}

class _RemoveDescriptorDirectory implements ProgramTransformer {
  final Directory directory;
  new(this.directory);

  @override
  void transform(Component component) {
    directory.deleteSync(recursive: true);
  }
}

void main() {
  group('compiled native-assets descriptor', () {
    test('absent differs from present empty and missing target', () {
      expect(describe(Component()), {
        'schema': 1,
        'target': 'ios_arm64',
        'carrier_present': false,
      });
      final empty = describe(Component(), extra: carrier({'ios_arm64': {}}));
      expect(empty['carrier_present'], true);
      expect(
        empty['mapping_sha256'],
        '44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a',
      );
      expect(describe(Component(), extra: carrier({})), empty);
    });

    test('sorts IDs; preserves exact paths and ignores other target maps', () {
      Map<String, Object> mapping(Map<String, Object> assets) => describe(
        Component(
          libraries: [
            carrier({'ios_arm64': assets}),
          ],
        ),
      );
      final expected = mapping({
        'b': ['relative', 'a/../lib.dylib'],
        'a': ['process'],
      });
      expect(
        mapping({
          'a': ['process'],
          'b': ['relative', 'a/../lib.dylib'],
        }),
        expected,
      );
      for (final changed in [
        {
          'A': ['process'],
          'b': ['relative', 'a/../lib.dylib'],
        },
        {
          'a': ['executable'],
          'b': ['relative', 'a/../lib.dylib'],
        },
        {
          'a': ['process'],
          'b': ['relative', 'lib.dylib'],
        },
      ]) {
        expect(mapping(changed), isNot(expected));
      }
      expect(
        describe(
          Component(),
          extra: carrier({
            'macos_arm64': {
              'ignored': ['process'],
            },
            'ios_arm64': {
              'a': ['process'],
              'b': ['relative', 'a/../lib.dylib'],
            },
          }),
        ),
        expected,
      );
    });

    test('rejects duplicate carriers even if identical object', () {
      final library = carrier({});
      expect(
        () => describe(Component(libraries: [library]), extra: library),
        throwsFormatException,
      );
      expect(
        () => describe(Component(libraries: [carrier({}), carrier({})])),
        throwsFormatException,
      );
    });

    test(
      'rejects executable carrier and absent/duplicate/malformed pragmas',
      () {
        final declaration = carrier({})
          ..fields.add(
            Field.mutable(Name('unsafe'), fileUri: Uri.parse('test:')),
          );
        final absent = carrier({})..annotations.removeLast();
        final duplicate = carrier({});
        duplicate.annotations.add(duplicate.annotations.last);
        final malformed = carrier({})..annotations.add(StringLiteral('bad'));
        for (final library in [declaration, absent, duplicate, malformed]) {
          expect(
            () => describe(Component(), extra: library),
            throwsFormatException,
          );
        }
      },
    );

    test('rejects duplicate target/asset keys and invalid compiled paths', () {
      final empty = jsonToKernelConstant({});
      ConstantMapEntry entry(String key, Constant value) =>
          ConstantMapEntry(StringConstant(key), value);
      MapConstant map(List<ConstantMapEntry> entries) =>
          MapConstant(DynamicType(), DynamicType(), entries);
      for (final constant in [
        map([entry('ios_arm64', empty), entry('ios_arm64', empty)]),
        map([
          entry(
            'ios_arm64',
            map([
              entry('a', jsonToKernelConstant(['process'])),
              entry('a', jsonToKernelConstant(['executable'])),
            ]),
          ),
        ]),
        jsonToKernelConstant({
          'ios_arm64': {
            'a': ['unknown'],
          },
        }),
        jsonToKernelConstant({
          'ios_arm64': {
            'a': ['relative'],
          },
        }),
        jsonToKernelConstant({
          'ios_arm64': {
            'a': [1],
          },
        }),
        jsonToKernelConstant({'ios_arm64': []}),
      ]) {
        final library = carrier({});
        replaceMapping(library, constant);
        expect(
          () => describe(Component(), extra: library),
          throwsFormatException,
        );
      }
    });
  });

  group('actual full replacement frontend compile', () {
    late Directory directory;
    late File source, yaml, output, descriptor, depfile, base;
    late StringBuffer protocol;
    final sdkRoot = computePlatformBinariesLocation();
    final hostAbi = '00' * 32;

    setUp(() async {
      directory = await Directory.systemTemp.createTemp('native-descriptor-');
      source = File('${directory.path}/main.dart')
        ..writeAsStringSync('void main() {}');
      yaml = File('${directory.path}/native assets.yaml');
      output = File('${directory.path}/app.dill');
      descriptor = File('${directory.path}/native-assets.json');
      depfile = File('${directory.path}/app.d');
      base = File('${directory.path}/base.bytecode');
      final digest = replacementDigest([1]);
      base.writeAsBytesSync(
        encodeShorebirdReplacement(
          ShorebirdReplacementManifest(
            hostAbi: List.filled(32, 0),
            baselineDigest: digest,
            currentDigest: digest,
            rootUri: source.uri.toString(),
            baselineUris: [source.uri.toString()],
            currentUris: [source.uri.toString()],
            removedUris: [],
          ),
          [1],
        ),
      );
      protocol = StringBuffer();
    });
    tearDown(() => directory.deleteSync(recursive: true));

    Future<bool> compile({
      File? input,
      FrontendCompiler? compiler,
      bool full = true,
      bool sidecar = true,
      List<String> flags = const [],
    }) => (compiler ?? FrontendCompiler(protocol)).compile(
      source.path,
      argParser.parse([
        '--sdk-root=${sdkRoot.toFilePath()}',
        '--platform=${sdkRoot.resolve('vm_platform.dill').toFilePath()}',
        '--aot',
        '--no-tfa',
        '--target-os=ios',
        if (full) ...[
          '--shorebird-full-replacement',
          '--shorebird-host-abi=$hostAbi',
          '--shorebird-replacement-base=${base.path}',
        ],
        if (sidecar) ...[
          '--shorebird-native-assets-descriptor=${descriptor.path}',
          '--shorebird-native-assets-target=ios_arm64',
        ],
        '--output-dill=${output.path}',
        '--depfile=${depfile.path}',
        if (input != null) '--native-assets=${input.path}',
        ...flags,
      ]),
    );

    Map<String, Object?> readDescriptor() =>
        jsonDecode(descriptor.readAsStringSync()) as Map<String, Object?>;

    test('absent and empty describe the actual serialized carriers', () async {
      expect(await compile(), true);
      expect(readDescriptor()['carrier_present'], false);
      expect(
        descriptor.readAsStringSync(),
        '{"schema":1,"target":"ios_arm64","carrier_present":false}\n',
      );
      yaml.writeAsStringSync('format-version: [1,0,0]\nnative-assets: {}\n');
      expect(await compile(input: yaml), true);
      final compiled = loadComponentFromBinary(output.path);
      expect(readDescriptor(), describe(compiled));
      expect(readDescriptor()['carrier_present'], true);
    });

    test(
      'component-only carrier and validator-pruned map are described',
      () async {
        expect(
          await compile(
            compiler: FrontendCompiler(protocol, transformer: _AddCarrier()),
          ),
          true,
        );
        final empty = readDescriptor();
        expect(empty, describe(loadComponentFromBinary(output.path)));
        expect(empty['carrier_present'], true);
        yaml.writeAsStringSync('''
format-version: [1,0,0]
native-assets:
  future_unknown_target: ignored by native assets validator
  ios_arm64: {}
''');
        expect(await compile(input: yaml), true);
        expect(readDescriptor(), empty);
        expect(
          readDescriptor(),
          describe(loadComponentFromBinary(output.path)),
        );
      },
    );

    test(
      'relocation/comment/order stable, same-path mutation changes digest',
      () async {
        yaml.writeAsStringSync('''
format-version: [1,0,0]
native-assets:
  ios_arm64:
    z: [relative, 'a/../lib.dylib']
    a: [process]
''');
        expect(await compile(input: yaml), true);
        final original = readDescriptor();
        expect(
          original['mapping_sha256'],
          '7c51a0ca7652edc17bef189038ed4a5e71facff0013efd1af43d4da26f84dbc3',
        );
        expect(original, describe(loadComponentFromBinary(output.path)));
        expect(protocol.toString(), contains('+${yaml.uri}'));
        expect(depfile.readAsStringSync(), contains('native\\ assets.yaml'));
        final relocated = File('${directory.path}/relocated.yaml')
          ..writeAsStringSync('''
# Source filename, YAML comments, and target/asset key order are not provenance.
native-assets:
  macos_arm64:
    ignored: [executable]
  ios_arm64:
    a: [process]
    z: [relative, 'a/../lib.dylib']
format-version: [1,0,0]
''');
        expect(await compile(input: relocated), true);
        expect(readDescriptor(), original);
        yaml.writeAsStringSync(
          yaml.readAsStringSync().replaceAll('process', 'executable'),
        );
        expect(await compile(input: yaml), true);
        expect(
          readDescriptor()['mapping_sha256'],
          '6b62b2594b15e060459eca6b9e13a43af7c26924015a9933873a15734083a916',
        );
      },
    );

    test('failed source or YAML compile clears previous descriptor', () async {
      descriptor.writeAsStringSync('stale');
      source.writeAsStringSync('void main( {');
      expect(await compile(), false);
      expect(descriptor.existsSync(), false);
      source.writeAsStringSync('void main() {}');
      yaml.writeAsStringSync('not: valid');
      descriptor.writeAsStringSync('stale');
      expect(await compile(input: yaml), false);
      expect(descriptor.existsSync(), false);
    });

    test(
      'duplicate compiled carrier and output write failure leave no descriptor',
      () async {
        yaml.writeAsStringSync('format-version: [1,0,0]\nnative-assets: {}\n');
        descriptor.writeAsStringSync('stale');
        await expectLater(
          compile(
            input: yaml,
            compiler: FrontendCompiler(protocol, transformer: _AddCarrier()),
          ),
          throwsFormatException,
        );
        expect(descriptor.existsSync(), false);
        descriptor.writeAsStringSync('stale');
        await expectLater(
          compile(compiler: _FailDillWrite()),
          throwsA(isA<FileSystemException>()),
        );
        expect(descriptor.existsSync(), false);
      },
    );

    test(
      'paired flags require full mode; generic compilation stays supported',
      () async {
        descriptor.writeAsStringSync('stale');
        expect(await compile(full: false), false);
        expect(descriptor.existsSync(), false);
        expect(await compile(full: false, sidecar: false), true);
        expect(descriptor.existsSync(), false);
        for (final flags in [
          ['--shorebird-native-assets-descriptor=${descriptor.path}'],
          ['--shorebird-native-assets-target=ios_arm64'],
          [
            '--shorebird-native-assets-descriptor=${descriptor.path}',
            '--shorebird-native-assets-target=macos_arm64',
          ],
        ]) {
          expect(await compile(sidecar: false, flags: flags), false);
        }
      },
    );

    test(
      'descriptor write failure reports failure after clearing stale output',
      () async {
        final descriptorDirectory = Directory('${directory.path}/descriptor')
          ..createSync();
        descriptor = File('${descriptorDirectory.path}/native-assets.json')
          ..writeAsStringSync('stale evidence');
        final compiler = FrontendCompiler(
          protocol,
          transformer: _RemoveDescriptorDirectory(descriptorDirectory),
        );
        expect(await compile(compiler: compiler), false);
        expect(output.existsSync(), true);
        expect(descriptor.existsSync(), false);
        expect(
          compiler.errors.single,
          contains('Failed to write Shorebird native-assets descriptor'),
        );
        final lines = const LineSplitter().convert(protocol.toString());
        final boundary = lines.first.substring('result '.length);
        expect(lines, isNot(contains('$boundary ${output.path} 0')));
        expect(lines.last, '$boundary ${output.path} 1');
        final diagnostic = lines.indexWhere(
          (line) => line.startsWith('Error:'),
        );
        final dependencyBoundary = lines.indexOf(boundary);
        expect(diagnostic, greaterThan(0));
        expect(diagnostic, lessThan(dependencyBoundary));
        expect(lines[dependencyBoundary + 1], startsWith('+'));
      },
    );

    test(
      'descriptor full mode cannot select filtered dill serialization',
      () async {
        descriptor.writeAsStringSync('stale evidence');
        expect(await compile(flags: ['--import-dill=${base.path}']), false);
        expect(descriptor.existsSync(), false);
        descriptor.writeAsStringSync('stale evidence');
        await expectLater(
          compile(flags: ['--minimal-kernel']),
          throwsArgumentError,
        );
        expect(descriptor.existsSync(), false);
      },
    );
  });
}
