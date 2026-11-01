// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:convert';

import 'package:crypto/crypto.dart';
import 'package:kernel/ast.dart';

import 'metadata.dart';

/// Accept only an explicit OS/ABI pair, preserving the requested target.
bool shorebirdNativeAssetsTargetMatchesOS(String? targetOS, String target) =>
    switch (targetOS) {
      'ios' => target == 'ios_arm64',
      'macos' => target == 'macos_arm64' || target == 'macos_x64',
      _ => false,
    };

/// Describes the effective native-assets map carried by the compiled Kernel.
///
/// This deliberately consumes constants after native-assets validation/pruning,
/// rather than rereading the source YAML. The canonical digest is SHA-256 of
/// UTF-8 JSON for the target's asset-ID object, sorted by ID, with path arrays
/// and their strings unchanged. A carrier without this target has an empty map.
Map<String, Object> shorebirdNativeAssetsDescriptor(
  Component component, {
  Library? nativeAssetsLibrary,
  required String target,
  required String? targetOS,
}) {
  if (!shorebirdNativeAssetsTargetMatchesOS(targetOS, target)) {
    throw const FormatException(
      'Unsupported or mismatched Shorebird native-assets target OS.',
    );
  }
  final carriers = [
    for (final library in component.libraries)
      if (isNativeAssetsMetadataLibrary(library)) library,
    if (nativeAssetsLibrary != null) nativeAssetsLibrary,
  ];
  if (carriers.length > 1) {
    throw const FormatException('Duplicate native-assets metadata carriers.');
  }
  final descriptor = <String, Object>{
    'schema': 1,
    'target': target,
    'carrier_present': carriers.isNotEmpty,
  };
  if (carriers.isEmpty) return descriptor;
  final carrier = carriers.single;
  if (!isNativeAssetsMetadataLibrary(carrier)) {
    throw const FormatException('Invalid native-assets metadata carrier URI.');
  }

  MapConstant? mapping;
  var entryPointSeen = false;
  for (final annotation in carrier.annotations) {
    if (annotation is! ConstantExpression ||
        annotation.constant is! InstanceConstant) {
      throw const FormatException('Malformed native-assets annotation.');
    }
    final pragma = annotation.constant as InstanceConstant;
    final klass = pragma.classNode;
    if (klass.name != 'pragma' ||
        klass.enclosingLibrary.importUri.toString() != 'dart:core' ||
        pragma.typeArguments.isNotEmpty ||
        pragma.fieldValues.length != 2) {
      throw const FormatException('Malformed native-assets pragma.');
    }
    final fields = <String, Constant>{};
    for (final entry in pragma.fieldValues.entries) {
      final field = entry.key.asField;
      if (field.enclosingClass != klass ||
          fields.containsKey(field.name.text)) {
        throw const FormatException('Ambiguous native-assets pragma fields.');
      }
      fields[field.name.text] = entry.value;
    }
    final name = fields['name'];
    final options = fields['options'];
    if (name is! StringConstant || options == null) {
      throw const FormatException('Malformed native-assets pragma fields.');
    }
    if (name.value == 'vm:entry-point' &&
        !entryPointSeen &&
        options is NullConstant) {
      entryPointSeen = true;
    } else if (name.value == 'vm:ffi:native-assets' &&
        mapping == null &&
        options is MapConstant) {
      mapping = options;
    } else {
      throw const FormatException('Duplicate or invalid native-assets pragma.');
    }
  }
  if (mapping == null) {
    throw const FormatException('Missing native-assets mapping pragma.');
  }

  final targets = _stringMap(mapping);
  var effective = <String, List<String>>{};
  for (final entry in targets.entries) {
    if (entry.value is! MapConstant) {
      throw const FormatException('Malformed native-assets target map.');
    }
    final assets = <String, List<String>>{};
    for (final asset in _stringMap(entry.value as MapConstant).entries) {
      final path = asset.value;
      if (path is! ListConstant ||
          path.entries.isEmpty ||
          path.entries.any((value) => value is! StringConstant)) {
        throw const FormatException('Malformed native-assets path.');
      }
      final strings = [
        for (final value in path.entries) (value as StringConstant).value,
      ];
      final withPath = const ['absolute', 'relative', 'system'];
      if ((!withPath.contains(strings.first) &&
              !const ['executable', 'process'].contains(strings.first)) ||
          strings.length != (withPath.contains(strings.first) ? 2 : 1)) {
        throw const FormatException('Malformed native-assets loader path.');
      }
      assets[asset.key] = strings;
    }
    if (entry.key == target) effective = assets;
  }
  final ids = effective.keys.toList()..sort();
  final canonical = {for (final id in ids) id: effective[id]!};
  descriptor['mapping_sha256'] = sha256
      .convert(utf8.encode(jsonEncode(canonical)))
      .toString();
  return descriptor;
}

Map<String, Constant> _stringMap(MapConstant constant) {
  final result = <String, Constant>{};
  for (final entry in constant.entries) {
    final key = entry.key;
    if (key is! StringConstant || result.containsKey(key.value)) {
      throw const FormatException('Malformed or duplicate native-assets key.');
    }
    result[key.value] = entry.value;
  }
  return result;
}
