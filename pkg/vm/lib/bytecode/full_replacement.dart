// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// The native-independent wire format for complete-domain replacement.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart';
import 'package:kernel/ast.dart';

const int shorebirdReplacementMaxComponent = 128 * 1024 * 1024;
const int shorebirdReplacementMaxManifest = 4 * 1024 * 1024;
const int shorebirdReplacementMaxUri = 16 * 1024;
const int shorebirdReplacementMaxUris = 65536;
const _magic = <int>[83, 66, 82, 80, 76, 67, 69, 0];

Uint8List replacementDigest(List<int> bytes) =>
    Uint8List.fromList(sha256.convert(bytes).bytes);

Uint8List parseShorebirdHostAbi(String value) {
  if (!RegExp(r'^[0-9a-f]{64}$').hasMatch(value)) {
    throw const FormatException(
      '--shorebird-host-abi requires 64 lowercase hex digits.',
    );
  }
  return Uint8List.fromList([
    for (var i = 0; i < value.length; i += 2)
      int.parse(value.substring(i, i + 2), radix: 16),
  ]);
}

bool _equal(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  for (var i = 0; i < a.length; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

int _compare(String a, String b) {
  final aa = utf8.encode(a), bb = utf8.encode(b);
  for (var i = 0; i < aa.length && i < bb.length; ++i) {
    if (aa[i] != bb[i]) return aa[i].compareTo(bb[i]);
  }
  return aa.length.compareTo(bb.length);
}

List<String> canonicalReplacementUris(Iterable<String> uris) =>
    uris.toList()..sort(_compare);

class ShorebirdReplacementManifest {
  final Uint8List hostAbi;
  final Uint8List baselineDigest;
  final Uint8List currentDigest;
  final String rootUri;
  final List<String> baselineUris;
  final List<String> currentUris;
  final List<String> removedUris;

  ShorebirdReplacementManifest({
    required List<int> hostAbi,
    required List<int> baselineDigest,
    required List<int> currentDigest,
    required this.rootUri,
    required List<String> baselineUris,
    required List<String> currentUris,
    required List<String> removedUris,
  }) : hostAbi = Uint8List.fromList(hostAbi),
       baselineDigest = Uint8List.fromList(baselineDigest),
       currentDigest = Uint8List.fromList(currentDigest),
       baselineUris = List.unmodifiable(baselineUris),
       currentUris = List.unmodifiable(currentUris),
       removedUris = List.unmodifiable(removedUris);

  ShorebirdReplacementManifest baselineProjection() =>
      ShorebirdReplacementManifest(
        hostAbi: hostAbi,
        baselineDigest: baselineDigest,
        currentDigest: baselineDigest,
        rootUri: rootUri,
        baselineUris: baselineUris,
        currentUris: baselineUris,
        removedUris: const [],
      );

  void _validate() {
    if (hostAbi.length != 32 ||
        baselineDigest.length != 32 ||
        currentDigest.length != 32) {
      throw const FormatException('Replacement digests must contain 32 bytes.');
    }
    var encodedLength = 116;
    void uri(String value) {
      final encoded = utf8.encode(value);
      encodedLength += encoded.length;
      if (encodedLength > shorebirdReplacementMaxManifest) {
        throw const FormatException("Replacement manifest exceeds limit.");
      }
      if (value.isEmpty ||
          value.contains('\u0000') ||
          encoded.length > shorebirdReplacementMaxUri ||
          utf8.decode(encoded) != value) {
        throw const FormatException('Invalid replacement URI.');
      }
    }

    void set(List<String> values) {
      if (values.length > shorebirdReplacementMaxUris) {
        throw const FormatException('Too many replacement URIs.');
      }
      encodedLength += 4 * values.length;
      for (var i = 0; i < values.length; ++i) {
        uri(values[i]);
        if (values[i].startsWith('dart:') ||
            (i > 0 && _compare(values[i - 1], values[i]) >= 0)) {
          throw const FormatException(
            'Replacement URI sets must be canonical mutable-library sets.',
          );
        }
      }
    }

    uri(rootUri);
    set(baselineUris);
    set(currentUris);
    set(removedUris);
    if (!baselineUris.contains(rootUri) || !currentUris.contains(rootUri)) {
      throw const FormatException(
        'Replacement root is missing from the mutable domain.',
      );
    }
    final currentSet = currentUris.toSet();
    final removed = baselineUris
        .where((uri) => !currentSet.contains(uri))
        .toList();
    if (removed.length != removedUris.length ||
        List.generate(
          removed.length,
          (i) => i,
        ).any((i) => removed[i] != removedUris[i])) {
      throw const FormatException(
        'Replacement removed URI set does not equal B minus U.',
      );
    }
  }

  Uint8List encode() {
    _validate();
    final writer = _Writer()..u32(1);
    writer.bytes.add(hostAbi);
    writer.bytes.add(baselineDigest);
    writer.bytes.add(currentDigest);
    writer.uri(rootUri);
    for (final values in [baselineUris, currentUris, removedUris]) {
      writer.u32(values.length);
      for (final value in values) {
        writer.uri(value);
      }
    }
    final result = writer.bytes.takeBytes();
    if (result.length > shorebirdReplacementMaxManifest) {
      throw const FormatException('Replacement manifest exceeds limit.');
    }
    return result;
  }

  static ShorebirdReplacementManifest decode(List<int> bytes) {
    if (bytes.length > shorebirdReplacementMaxManifest) {
      throw const FormatException('Replacement manifest exceeds limit.');
    }
    final reader = _Reader(bytes);
    if (reader.u32() != 1) {
      throw const FormatException('Unsupported replacement policy.');
    }
    final host = reader.take(32),
        base = reader.take(32),
        current = reader.take(32);
    final root = reader.uri();
    List<String> set() {
      final count = reader.u32();
      if (count > shorebirdReplacementMaxUris) {
        throw const FormatException('Too many replacement URIs.');
      }
      return List.generate(count, (_) => reader.uri());
    }

    final result = ShorebirdReplacementManifest(
      hostAbi: host,
      baselineDigest: base,
      currentDigest: current,
      rootUri: root,
      baselineUris: set(),
      currentUris: set(),
      removedUris: set(),
    );
    if (reader.remaining != 0) {
      throw const FormatException('Trailing replacement manifest bytes.');
    }
    result._validate();
    return result;
  }
}

class ShorebirdReplacementBundle {
  final ShorebirdReplacementManifest manifest;
  final Uint8List component;
  ShorebirdReplacementBundle(this.manifest, this.component);
}

Uint8List encodeShorebirdReplacement(
  ShorebirdReplacementManifest manifest,
  List<int> component,
) {
  if (component.isEmpty ||
      component.length > shorebirdReplacementMaxComponent) {
    throw const FormatException(
      'Replacement component exceeds limit or is empty.',
    );
  }
  if (!_equal(replacementDigest(component), manifest.currentDigest)) {
    throw const FormatException('Replacement component digest mismatch.');
  }
  final metadata = manifest.encode();
  final writer = _Writer();
  writer.bytes.add(_magic);
  writer.u32(1);
  writer.u32(metadata.length);
  writer.u32(component.length);
  writer.u32(0);
  writer.bytes.add(metadata);
  writer.bytes.add(component);
  return writer.bytes.takeBytes();
}

ShorebirdReplacementBundle decodeShorebirdReplacement(List<int> bytes) {
  if (bytes.length >
      24 + shorebirdReplacementMaxManifest + shorebirdReplacementMaxComponent) {
    throw const FormatException('Replacement wrapper exceeds limit.');
  }
  final reader = _Reader(bytes);
  if (!_equal(reader.take(8), _magic) || reader.u32() != 1) {
    throw const FormatException('Invalid replacement wrapper or version.');
  }
  final manifestLength = reader.u32(),
      componentLength = reader.u32(),
      high = reader.u32();
  if (high != 0 ||
      componentLength == 0 ||
      componentLength > shorebirdReplacementMaxComponent ||
      manifestLength > shorebirdReplacementMaxManifest ||
      manifestLength + componentLength != reader.remaining) {
    throw const FormatException('Invalid replacement wrapper lengths.');
  }
  final manifest = ShorebirdReplacementManifest.decode(
    reader.take(manifestLength),
  );
  final component = reader.take(componentLength);
  if (!_equal(replacementDigest(component), manifest.currentDigest)) {
    throw const FormatException('Replacement component digest mismatch.');
  }
  return ShorebirdReplacementBundle(manifest, component);
}

class _Writer {
  final bytes = BytesBuilder(copy: false);
  void u32(int value) {
    final b = ByteData(4)..setUint32(0, value, Endian.little);
    bytes.add(b.buffer.asUint8List());
  }

  void uri(String value) {
    final b = utf8.encode(value);
    u32(b.length);
    bytes.add(b);
  }
}

class _Reader {
  final Uint8List bytes;
  int offset = 0;
  _Reader(List<int> bytes) : bytes = Uint8List.fromList(bytes);
  int get remaining => bytes.length - offset;
  Uint8List take(int count) {
    if (count < 0 || count > remaining) {
      throw const FormatException('Truncated replacement data.');
    }
    final result = Uint8List.sublistView(bytes, offset, offset + count);
    offset += count;
    return result;
  }

  int u32() => ByteData.sublistView(take(4)).getUint32(0, Endian.little);
  String uri() {
    final length = u32();
    if (length > shorebirdReplacementMaxUri) {
      throw const FormatException('Replacement URI exceeds limit.');
    }
    return utf8.decode(take(length), allowMalformed: false);
  }
}

/// Validates mutable definitions before lowering can hide native declarations.
void validateShorebirdReplacementLibraries(Iterable<Library> libraries) {
  for (final library in libraries) {
    if (library.importUri.scheme == 'dart') continue;
    for (final dependency in library.dependencies) {
      if (const {
        'dart:ffi',
        'dart:mirrors',
        'dart:nativewrappers',
      }.contains(dependency.targetLibrary.importUri.toString())) {
        throw FormatException(
          'Unsupported replacement v1 import: ${dependency.targetLibrary.importUri}.',
        );
      }
    }
    void member(Member member) {
      if (member.isExternal) {
        throw FormatException(
          'External/native replacement definition: ${library.importUri}::${member.name}.',
        );
      }
    }

    for (final declaration in library.members) {
      member(declaration);
    }
    for (final cls in library.classes) {
      for (
        Class? ancestor = cls;
        ancestor != null;
        ancestor = ancestor.superclass
      ) {
        if (ancestor.enclosingLibrary.importUri.toString() ==
            'dart:nativewrappers') {
          throw FormatException(
            'Native wrapper replacement class: ${cls.name}.',
          );
        }
      }
      for (final declaration in cls.members) {
        member(declaration);
      }
    }
  }
}
