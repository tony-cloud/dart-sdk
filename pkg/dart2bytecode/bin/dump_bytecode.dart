// Copyright (c) 2024, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'dart:io';

import 'package:dart2bytecode/bytecode_profile.dart' show BytecodeProfile;
import 'package:dart2bytecode/bytecode_serialization.dart'
    show LinkReader, BufferedReader;
import 'package:dart2bytecode/declarations.dart' show Component;

final String _usage = '''
Usage: dump_bytecode [--bytecode-profile=profile.json] input.bytecode
Dumps bytecode file. A profiled (DBP3) file needs its profile.
''';

void main(List<String> arguments) async {
  const profileFlag = '--bytecode-profile=';
  final profiles = arguments.where((a) => a.startsWith(profileFlag)).toList();
  final inputs = arguments.where((a) => !a.startsWith(profileFlag)).toList();
  if (inputs.length != 1 || profiles.length > 1) {
    print(_usage);
    exit(1);
  }

  final input = inputs[0];
  final profile = profiles.isEmpty
      ? null
      : BytecodeProfile.load(profiles.single.substring(profileFlag.length));

  final List<int> bytes = File(input).readAsBytesSync();

  final linkReader = LinkReader();
  final reader = BufferedReader(linkReader, bytes, profile: profile);
  final bytecodeComponent = Component.read(reader);
  print(bytecodeComponent.toString());
}
