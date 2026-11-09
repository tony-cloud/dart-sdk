// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Open Shorebird `hardened-diversified` bytecode profiles (contract v3).
///
/// A profile is a release-specific permutation of the wire bytes of the
/// public opcodes (`wire_opcodes[semantic index] = wire byte`). `Trap` keeps
/// byte 0. The compiler keeps semantic opcodes everywhere (code generation,
/// jump patching, validation, disassembly) and applies the permutation only
/// when instructions are serialized, so a profiled component differs from a
/// canonical one only in its header and its opcode bytes.
///
/// The same rules are implemented by the VM build
/// (runtime/tools/bytecode_profile.py) and by the workspace reference tool
/// (scripts/diversification_profile.py).
library;

import 'dart:convert' show jsonDecode, jsonEncode, utf8;
import 'dart:io' show File, FileSystemException;
import 'dart:typed_data';

import 'package:crypto/crypto.dart' show sha256;

import 'dbc.dart';

class BytecodeProfileException implements Exception {
  final String message;
  BytecodeProfileException(this.message);

  @override
  String toString() => 'Invalid bytecode profile: $message';
}

class BytecodeProfile {
  /// Magic value of a profile-bound component ('DBP3'). The format version is
  /// followed by the [digestLength] bytes of [digest].
  static const int profiledMagicValue = 0x44425033;
  static const int digestLength = 32;
  static const String schema = 'open-shorebird.diversification-profile';
  static const int contractVersion = 3;
  static const String scheme = 'opcode-permutation-v1';
  static const int maxFixedPoints = 16;

  /// Number of public opcodes the permutation covers.
  static final int opcodeCount = Opcode.values.length;

  /// `sha256:<hex>` of the canonical profile JSON.
  final String id;

  /// The 32 SHA-256 bytes of the canonical profile JSON.
  final Uint8List digest;

  final Uint8List _toWire;
  final Uint8List _toSemantic;

  BytecodeProfile._(this.id, this.digest, this._toWire, this._toSemantic);

  /// SHA-256 of the semantic opcode names joined by '\n' (`Trap`, `Dup`, ...),
  /// which a profile must name in `opcode_table_sha256`.
  static String opcodeTableSha256() => sha256
      .convert(
        utf8.encode(
          Opcode.values.map((opcode) => opcode.name.substring(1)).join('\n'),
        ),
      )
      .toString();

  static BytecodeProfile load(String path) {
    final List<int> bytes;
    try {
      bytes = File(path).readAsBytesSync();
    } on FileSystemException catch (error) {
      throw BytecodeProfileException('cannot read $path: ${error.message}');
    }
    return BytecodeProfile.parse(bytes);
  }

  /// Parses and validates a profile document (any JSON formatting; the id is
  /// computed over its canonical form).
  factory BytecodeProfile.parse(List<int> bytes) {
    final Object? document;
    try {
      document = jsonDecode(utf8.decode(bytes));
    } on FormatException catch (error) {
      throw BytecodeProfileException('not JSON: ${error.message}');
    }
    if (document is! Map<String, Object?>) {
      throw BytecodeProfileException('not a JSON object');
    }
    _expectKeys(document, const {
      'schema',
      'contract_version',
      'bytecode',
      'native_layout',
    }, r'$');
    if (document['schema'] != schema ||
        document['contract_version'] != contractVersion) {
      throw BytecodeProfileException(
        'not a contract v3 diversification profile',
      );
    }
    if (document['native_layout'] != null) {
      throw BytecodeProfileException('native_layout must be null');
    }
    final bytecode = document['bytecode'];
    if (bytecode is! Map<String, Object?>) {
      throw BytecodeProfileException(r'$.bytecode must be an object');
    }
    _expectKeys(bytecode, const {
      'scheme',
      'format_version',
      'opcode_table_sha256',
      'wire_opcodes',
    }, r'$.bytecode');
    if (bytecode['scheme'] != scheme ||
        bytecode['format_version'] != bytecodeFormatVersion) {
      throw BytecodeProfileException('unsupported scheme or format version');
    }
    if (bytecode['opcode_table_sha256'] != opcodeTableSha256()) {
      throw BytecodeProfileException(
        'the profile was made for another opcode table',
      );
    }
    final wires = bytecode['wire_opcodes'];
    if (wires is! List ||
        wires.length != opcodeCount ||
        wires.any((wire) => wire is! int)) {
      throw BytecodeProfileException(
        'wire_opcodes must list $opcodeCount integers',
      );
    }
    final toWire = Uint8List(opcodeCount);
    final toSemantic = Uint8List(256);
    final seen = List<bool>.filled(opcodeCount, false);
    for (int i = 0; i < 256; i++) {
      toSemantic[i] = i;
    }
    int fixed = 0;
    for (int semantic = 0; semantic < opcodeCount; semantic++) {
      final wire = wires[semantic] as int;
      if (wire < 0 || wire >= opcodeCount || seen[wire]) {
        throw BytecodeProfileException(
          'wire_opcodes is not a permutation of 0..${opcodeCount - 1}',
        );
      }
      seen[wire] = true;
      toWire[semantic] = wire;
      toSemantic[wire] = semantic;
      if (semantic != 0 && semantic == wire) fixed++;
    }
    if (toWire[0] != 0) {
      throw BytecodeProfileException('Trap must keep wire byte 0');
    }
    if (fixed >= maxFixedPoints) {
      throw BytecodeProfileException('$fixed fixed points');
    }
    final digest = Uint8List.fromList(
      sha256.convert(utf8.encode(_canonicalJson(document))).bytes,
    );
    final hex = digest.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
    return BytecodeProfile._('sha256:$hex', digest, toWire, toSemantic);
  }

  /// Wire byte of [opcode].
  int encode(Opcode opcode) => _toWire[opcode.index];

  /// Semantic opcode of a wire byte. Files never contain VM-internal opcodes,
  /// so bytes outside the public range are refused.
  Opcode decode(int wire) {
    if (wire < 0 || wire >= opcodeCount) {
      throw FormatException('Invalid bytecode opcode byte $wire.');
    }
    return Opcode.values[_toSemantic[wire]];
  }

  /// Re-encodes canonical (semantic) instructions in this profile's wire form.
  Uint8List encodeInstructions(Uint8List canonical) {
    final out = Uint8List.fromList(canonical);
    for (int pos = 0; pos < out.length;) {
      final opcode = _canonicalOpcode(out[pos]);
      out[pos] = encode(opcode);
      pos = _next(pos, opcode, out.length);
    }
    return out;
  }

  /// Decodes wire instructions of this profile into canonical instructions.
  Uint8List decodeInstructions(List<int> wire) {
    final out = Uint8List.fromList(wire);
    for (int pos = 0; pos < out.length;) {
      final opcode = decode(out[pos]);
      out[pos] = opcode.index;
      pos = _next(pos, opcode, out.length);
    }
    return out;
  }

  static Opcode _canonicalOpcode(int byte) {
    if (byte >= Opcode.values.length) {
      throw FormatException('Invalid bytecode opcode byte $byte.');
    }
    return Opcode.values[byte];
  }

  static int _next(int pos, Opcode opcode, int length) {
    final next = pos + instructionSizeOf(opcode);
    if (next > length) {
      throw const FormatException('Truncated bytecode instruction.');
    }
    return next;
  }

  /// Size in bytes of an instruction with the given semantic opcode.
  static int instructionSizeOf(Opcode opcode) {
    final format = BytecodeFormats[opcode];
    if (format != null) return instructionSize(format.encoding, false);
    if (opcode.index > 0) {
      final compact = Opcode.values[opcode.index - kWideModifier];
      final compactFormat = BytecodeFormats[compact];
      if (compactFormat != null && hasWideVariant(compact)) {
        return instructionSize(compactFormat.encoding, true);
      }
    }
    throw FormatException(
      'Opcode ${opcode.name} cannot appear in a bytecode file.',
    );
  }

  static void _expectKeys(
    Map<String, Object?> map,
    Set<String> keys,
    String where,
  ) {
    if (map.length != keys.length || !keys.containsAll(map.keys)) {
      throw BytecodeProfileException(
        '$where must have exactly the fields ${keys.join(', ')}',
      );
    }
  }

  static String _canonicalJson(Object? value) {
    Object? normalize(Object? input) {
      if (input is Map) {
        final keys = input.keys.cast<String>().toList()..sort();
        return {for (final key in keys) key: normalize(input[key])};
      }
      if (input is List) return input.map(normalize).toList();
      return input;
    }

    return jsonEncode(normalize(value));
  }
}
