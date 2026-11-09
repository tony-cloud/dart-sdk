// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Open Shorebird hardened-diversified bytecode profiles (contract v3).

import 'dart:convert';
import 'dart:io';
import 'dart:math' show Random;
import 'dart:typed_data';

import 'package:crypto/crypto.dart' show Hmac, sha256;
import 'package:dart2bytecode/bytecode_generator.dart' show generateBytecode;
import 'package:dart2bytecode/bytecode_profile.dart';
import 'package:dart2bytecode/bytecode_serialization.dart';
import 'package:dart2bytecode/dart2bytecode.dart'
    show runCompilerWithCommandLineArguments;
import 'package:dart2bytecode/dbc.dart';
import 'package:dart2bytecode/declarations.dart' as dbc;
import 'package:dart2bytecode/disassembler.dart';
import 'package:dart2bytecode/options.dart';
import 'package:front_end/src/api_unstable/vm.dart'
    show computePlatformBinariesLocation;
import 'package:kernel/class_hierarchy.dart';
import 'package:kernel/core_types.dart';
import 'package:kernel/target/targets.dart';
import 'package:test/test.dart';
import 'package:vm/modular/target/vm.dart';

import 'bytecode_generator_test.dart' as support;

// The published DEVELOPMENT-ONLY seed of the workspace reference tool
// (scripts/diversification_profile.py) and the id it derives. Pinning both
// here keeps the Dart and Python derivations identical.
const developmentSeedLabel =
    'open-shorebird DEVELOPMENT-ONLY diversification seed v1';
const developmentProfileId =
    'sha256:832856d0bed27833434e3f26b65d03cedc313f9850c2577ca7565c4dc644f46d';

/// The derivation `open-shorebird.diversification.v1` (contract section 11.2).
List<int> deriveWireOpcodes(List<int> seed, int count) {
  final subkey = Hmac(sha256, seed)
      .convert(
        utf8.encode(
          'open-shorebird.diversification.v1/bytecode-opcode-permutation',
        ),
      )
      .bytes;
  final buffer = <int>[];
  var counter = 0;
  int uint32() {
    if (buffer.length < 4) {
      final block = ByteData(4)..setUint32(0, counter++);
      buffer.addAll(
        Hmac(sha256, subkey).convert(block.buffer.asUint8List()).bytes,
      );
    }
    final value =
        (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    buffer.removeRange(0, 4);
    return value;
  }

  int below(int bound) {
    const range = 1 << 32;
    final limit = range - (range % bound);
    while (true) {
      final value = uint32();
      if (value < limit) return value % bound;
    }
  }

  final wires = [for (var i = 1; i < count; i++) i];
  for (var i = wires.length - 1; i > 0; i--) {
    final j = below(i + 1);
    final swap = wires[i];
    wires[i] = wires[j];
    wires[j] = swap;
  }
  return [0, ...wires];
}

Map<String, Object?> profileDocument(List<int> wires) => {
  'schema': 'open-shorebird.diversification-profile',
  'contract_version': 3,
  'bytecode': {
    'scheme': 'opcode-permutation-v1',
    'format_version': 3,
    'opcode_table_sha256': BytecodeProfile.opcodeTableSha256(),
    'wire_opcodes': wires,
  },
  'native_layout': null,
};

List<int> encodeDocument(Object document) =>
    utf8.encode(const JsonEncoder.withIndent('  ').convert(document));

BytecodeProfile profileForSeed(List<int> seed) => BytecodeProfile.parse(
  encodeDocument(
    profileDocument(deriveWireOpcodes(seed, BytecodeProfile.opcodeCount)),
  ),
);

List<int> seedNumber(int n) =>
    sha256.convert(utf8.encode('dart2bytecode profile test seed $n')).bytes;

final developmentSeed = sha256.convert(utf8.encode(developmentSeedLabel)).bytes;

/// One instruction of every encodable semantic opcode and width, with
/// pseudo-random operand bytes.
Uint8List everyInstruction(Random random) {
  final out = BytesBuilder();
  for (final opcode in Opcode.values) {
    int size;
    try {
      size = BytecodeProfile.instructionSizeOf(opcode);
    } on FormatException {
      continue; // Reserved opcodes never appear in files.
    }
    out.addByte(opcode.index);
    for (var i = 1; i < size; i++) {
      out.addByte(random.nextInt(256));
    }
  }
  return out.takeBytes();
}

void main() {
  group('derivation and profile rules', () {
    test('the DEVELOPMENT-ONLY seed derives the reference profile id', () {
      expect(profileForSeed(developmentSeed).id, developmentProfileId);
    });

    test('the opcode table hash matches the VM opcode list', () {
      final constants = File(
        Platform.script
            .resolve('../../../runtime/vm/constants_kbc.h')
            .toFilePath(),
      ).readAsStringSync();
      final list = RegExp(
        r'#define PUBLIC_KERNEL_BYTECODES_LIST\(V\)(.*?)\n\n',
        dotAll: true,
      ).firstMatch(constants)!.group(1)!;
      final names = RegExp(
        r'V\((\w+),',
      ).allMatches(list).map((m) => m.group(1)!).toList();
      expect(
        names,
        Opcode.values.map((opcode) => opcode.name.substring(1)).toList(),
      );
      expect(
        BytecodeProfile.opcodeTableSha256(),
        sha256.convert(utf8.encode(names.join('\n'))).toString(),
      );
    });

    test(
      '16 seeds: bijection, Trap at 0, deterministic, mutually distinct',
      () {
        final ids = <String>{};
        for (var n = 0; n < 16; n++) {
          final profile = profileForSeed(seedNumber(n));
          expect(profileForSeed(seedNumber(n)).id, profile.id);
          ids.add(profile.id);
          expect(profile.encode(Opcode.kTrap), 0);
          final wires = <int>{};
          var moved = 0;
          for (final opcode in Opcode.values) {
            final wire = profile.encode(opcode);
            expect(wire, lessThan(BytecodeProfile.opcodeCount));
            expect(profile.decode(wire), opcode);
            wires.add(wire);
            if (wire != opcode.index) moved++;
          }
          expect(wires.length, BytecodeProfile.opcodeCount);
          expect(moved, greaterThan(BytecodeProfile.opcodeCount - 17));
        }
        expect(ids.length, 16);
      },
    );

    test('the id is independent of JSON formatting and key order', () {
      final wires = deriveWireOpcodes(
        developmentSeed,
        BytecodeProfile.opcodeCount,
      );
      final document = profileDocument(wires);
      final reordered = {
        for (final key in document.keys.toList().reversed) key: document[key],
      };
      expect(
        BytecodeProfile.parse(utf8.encode(jsonEncode(reordered))).id,
        developmentProfileId,
      );
    });

    test('invalid profiles are refused', () {
      final wires = deriveWireOpcodes(
        developmentSeed,
        BytecodeProfile.opcodeCount,
      );
      void refuse(Object? document) => expect(
        () => BytecodeProfile.parse(utf8.encode(jsonEncode(document))),
        throwsA(isA<BytecodeProfileException>()),
      );
      Map<String, Object?> edit(void Function(Map<String, Object?> d) f) {
        final document = jsonDecode(jsonEncode(profileDocument(wires)));
        f(document as Map<String, Object?>);
        return document;
      }

      Map<String, Object?> bytecode(Map<String, Object?> d) =>
          d['bytecode'] as Map<String, Object?>;
      refuse([]);
      refuse(edit((d) => d['schema'] = 'other'));
      refuse(edit((d) => d['contract_version'] = 2));
      refuse(edit((d) => d['native_layout'] = {}));
      refuse(edit((d) => d.remove('native_layout')));
      refuse(edit((d) => d['extra'] = 1));
      refuse(edit((d) => bytecode(d)['scheme'] = 'other'));
      refuse(edit((d) => bytecode(d)['format_version'] = 4));
      refuse(edit((d) => bytecode(d)['opcode_table_sha256'] = '0' * 64));
      refuse(edit((d) => bytecode(d)['extra'] = 1));
      refuse(edit((d) => bytecode(d)['wire_opcodes'] = wires.sublist(1)));
      refuse(edit((d) => bytecode(d)['wire_opcodes'] = [...wires, 176]));
      refuse(
        edit((d) => bytecode(d)['wire_opcodes'] = [...wires]..[5] = wires[6]),
      );
      refuse(edit((d) => bytecode(d)['wire_opcodes'] = [...wires]..[5] = 300));
      // Trap moved off byte 0.
      final trapMoved = [...wires];
      final zeroAt = 7;
      trapMoved[0] = trapMoved[zeroAt];
      trapMoved[zeroAt] = 0;
      refuse(edit((d) => bytecode(d)['wire_opcodes'] = trapMoved));
      // Too many fixed points (a hand-made, weak map).
      final weak = [for (var i = 0; i < wires.length; i++) i];
      final swap = weak[20];
      weak[20] = weak[21];
      weak[21] = swap;
      refuse(edit((d) => bytecode(d)['wire_opcodes'] = weak));
      expect(
        () => BytecodeProfile.load('/nonexistent/profile.json'),
        throwsA(isA<BytecodeProfileException>()),
      );
    });
  });

  group('instruction transcoding', () {
    test('every opcode and width round-trips under 16 seeds', () {
      final random = Random(1);
      for (var n = 0; n < 16; n++) {
        final profile = profileForSeed(seedNumber(n));
        final canonical = everyInstruction(random);
        final wire = profile.encodeInstructions(canonical);
        expect(wire.length, canonical.length);
        expect(wire, isNot(canonical));
        expect(profile.decodeInstructions(wire), canonical);
        // Operands are untouched: only opcode bytes differ, and the decoded
        // stream disassembles to the same instructions.
        final original = BytecodeDisassembler().decode(canonical);
        final decoded = BytecodeDisassembler().decode(
          profile.decodeInstructions(wire),
        );
        expect(decoded.length, original.length);
        for (var i = 0; i < original.length; i++) {
          expect(decoded[i].opcode, original[i].opcode);
          expect(decoded[i].isWide, original[i].isWide);
          expect(decoded[i].operands, original[i].operands);
        }
      }
    });

    test('malformed wire instructions are refused', () {
      final profile = profileForSeed(developmentSeed);
      // A VM-internal or out-of-range byte never appears in a file.
      expect(
        () => profile.decodeInstructions([BytecodeProfile.opcodeCount]),
        throwsFormatException,
      );
      expect(() => profile.decodeInstructions([255]), throwsFormatException);
      // A truncated instruction.
      final entry = profile.encode(Opcode.kEntry_Wide);
      expect(
        () => profile.decodeInstructions([entry, 0, 0]),
        throwsFormatException,
      );
      // A reserved opcode has no encoding size.
      expect(
        () => profile.decodeInstructions([profile.encode(Opcode.kUnused04)]),
        throwsFormatException,
      );
    });
  });

  group('components', () {
    late Directory directory;
    late File source;

    setUp(() {
      directory = Directory.systemTemp.createTempSync('bytecode-profile-test-');
      source = File('${directory.path}/main.dart');
    });
    tearDown(() => directory.deleteSync(recursive: true));

    Future<Uint8List> compile(String contents, BytecodeProfile? profile) async {
      source.writeAsStringSync(contents);
      final target = VmTarget(TargetFlags());
      final result = await support.compileTestCaseToKernelProgram(
        source.uri,
        target: target,
      );
      final coreTypes = result.coreTypes ?? CoreTypes(result.component);
      final sink = support.ByteSink();
      generateBytecode(
        result.component,
        sink,
        options: BytecodeOptions(bytecodeProfile: profile),
        libraries: result.component.libraries
            .where((library) => !result.loadedLibraries.contains(library))
            .toList(),
        extraLoadedLibraries: result.loadedLibraries,
        coreTypes: coreTypes,
        hierarchy:
            result.classHierarchy ??
            ClassHierarchy(result.component, coreTypes),
        target: target,
      );
      return sink.builder.takeBytes();
    }

    const program = '''
int add(int a, int b) => a + b;
class Counter {
  int value = 0;
  int bump([int by = 1]) => value += by;
}
Future<int> later(int x) async => x * 2;
Iterable<int> range(int n) sync* { for (var i = 0; i < n; i++) yield i; }
T first<T>(List<T> items) => items.isEmpty ? throw StateError('empty') : items[0];
void main() {
  final counter = Counter();
  for (final i in range(10)) {
    try { counter.bump(add(i, 1)); } catch (e) { print(e); }
  }
  print(first([counter.value]));
  later(3).then(print);
  final f = (int x) => x + counter.value;
  print(f(2));
}
''';

    dbc.Component read(List<int> bytes, [BytecodeProfile? profile]) =>
        dbc.Component.read(
          BufferedReader(LinkReader(), bytes, profile: profile),
        );

    test('a profiled component differs from the canonical one only in its '
        'header and opcode bytes', () async {
      final profile = profileForSeed(developmentSeed);
      final canonical = await compile(program, null);
      final profiled = await compile(program, profile);
      expect(profiled.length, canonical.length + BytecodeProfile.digestLength);
      // The decoded components are identical: same text, same instructions.
      expect(read(profiled, profile).toString(), read(canonical).toString());
      // And the opcode bytes really are re-encoded.
      final codes = read(canonical).codes;
      final profiledCodes = read(profiled, profile).codes;
      expect(profiledCodes.length, codes.length);
      for (var i = 0; i < codes.length; i++) {
        expect(profiledCodes[i].bytecodes, codes[i].bytecodes);
      }
      final rawCanonical = canonical.sublist(dbc.Component.headerSize);
      final rawProfiled = profiled.sublist(
        dbc.Component.headerSizeFor(profile),
      );
      expect(rawProfiled, isNot(rawCanonical));
      var differing = 0;
      for (var i = 0; i < rawCanonical.length; i++) {
        if (rawCanonical[i] != rawProfiled[i]) differing++;
      }
      // Only opcode bytes change (far fewer than all instruction bytes).
      expect(differing, greaterThan(0));
    });

    test('the DBP3 header binds the profile digest', () async {
      final profile = profileForSeed(developmentSeed);
      final profiled = await compile(program, profile);
      final data = ByteData.sublistView(profiled);
      expect(
        data.getUint32(0, Endian.little),
        BytecodeProfile.profiledMagicValue,
      );
      expect(data.getUint32(4, Endian.little), bytecodeFormatVersion);
      expect(profiled.sublist(8, 40), profile.digest);
      final canonical = await compile(program, null);
      expect(
        ByteData.sublistView(canonical).getUint32(0, Endian.little),
        dbc.Component.magicValue,
      );
    });

    test(
      'same seed and inputs reproduce identical bytes; another seed differs',
      () async {
        final a = await compile(program, profileForSeed(seedNumber(1)));
        final b = await compile(program, profileForSeed(seedNumber(1)));
        final c = await compile(program, profileForSeed(seedNumber(2)));
        expect(a, b);
        expect(a, isNot(c));
      },
    );

    test('cross-profile reads are refused', () async {
      final profile = profileForSeed(seedNumber(3));
      final other = profileForSeed(seedNumber(4));
      final profiled = await compile(program, profile);
      final canonical = await compile(program, null);
      expect(() => read(profiled), throwsFormatException);
      expect(() => read(canonical, profile), throwsFormatException);
      expect(() => read(profiled, other), throwsFormatException);
      expect(read(profiled, profile).libraries, isNotEmpty);
    });

    test(
      '--bytecode-profile: baseline and patch stay in one profile',
      () async {
        final platform = computePlatformBinariesLocation()
            .resolve('vm_platform.dill')
            .toFilePath();
        final profileFile = File('${directory.path}/profile.json')
          ..writeAsBytesSync(
            encodeDocument(
              profileDocument(
                deriveWireOpcodes(developmentSeed, BytecodeProfile.opcodeCount),
              ),
            ),
          );
        final profile = BytecodeProfile.load(profileFile.path);
        Future<int> run(List<String> extra, String output) =>
            runCompilerWithCommandLineArguments([
              '--platform=$platform',
              ...extra,
              '-o',
              output,
              source.path,
            ]);

        source.writeAsStringSync('int value() => 1;\nvoid main() {}\n');
        final baseline = '${directory.path}/baseline.bytecode';
        final canonicalBaseline = '${directory.path}/canonical.bytecode';
        expect(
          await run(['--bytecode-profile=${profileFile.path}'], baseline),
          0,
        );
        expect(await run([], canonicalBaseline), 0);
        expect(
          read(File(baseline).readAsBytesSync(), profile).libraries,
          isNotEmpty,
        );

        source.writeAsStringSync('int value() => 2;\nvoid main() {}\n');
        final patch = '${directory.path}/patch.bytecode';
        expect(
          await run([
            '--bytecode-profile=${profileFile.path}',
            '--patch-base=$baseline',
          ], patch),
          0,
        );
        final delta = File(patch).readAsBytesSync();
        expect(
          ByteData.sublistView(delta).getUint32(0, Endian.little),
          BytecodeProfile.profiledMagicValue,
        );
        expect(read(delta, profile).codes, isNotEmpty);

        // A canonical baseline under a profile, a profiled baseline without
        // one, and an invalid profile all fail without writing output.
        final refused = '${directory.path}/refused.bytecode';
        expect(
          await run([
            '--bytecode-profile=${profileFile.path}',
            '--patch-base=$canonicalBaseline',
          ], refused),
          isNot(0),
        );
        expect(File(refused).existsSync(), isFalse);
        expect(await run(['--patch-base=$baseline'], refused), isNot(0));
        expect(File(refused).existsSync(), isFalse);
        final broken = File('${directory.path}/broken.json')
          ..writeAsStringSync('{"schema": "x"}');
        expect(
          await run(['--bytecode-profile=${broken.path}'], refused),
          isNot(0),
        );
        expect(File(refused).existsSync(), isFalse);
      },
    );
  });
}
