// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/dart2bytecode.dart';
import 'package:dart2bytecode/declarations.dart' as dbc;
import 'package:dart2bytecode/full_replacement.dart';
import 'package:dart2bytecode/patch.dart';
import 'package:front_end/src/api_unstable/vm.dart'
    show computePlatformBinariesLocation;
import 'package:kernel/binary/ast_from_binary.dart'
    show BinaryBuilderWithMetadata;
import 'package:kernel/ast.dart' as ast;
import 'package:test/test.dart';
import 'package:vm/bytecode/full_replacement.dart';
import 'package:vm/kernel_front_end.dart' as kernel;
import 'package:vm/metadata/procedure_attributes.dart';
import 'package:vm/metadata/shorebird_replacement.dart';

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
  }) async {
    source.writeAsStringSync(contents);
    final output = File('${directory.path}/$name.sbr');
    expect(
      await runCompilerWithCommandLineArguments([
        '--platform=$platform',
        '--shorebird-full-replacement',
        '--shorebird-host-abi=$host',
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
    'profile rejects ffi, external/native definitions and invalid mode binding',
    () async {
      for (final contents in [
        "import 'dart:ffi'; void main() {}",
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
