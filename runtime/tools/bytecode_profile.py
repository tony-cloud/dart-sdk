#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.
"""Validate a Dart bytecode diversification profile for the VM build.

The GN arg `dart_bytecode_profile` holds base64 of the canonical JSON of an
Open Shorebird `diversification-profile` (contract v3). `gn gen` runs

    bytecode_profile.py gn --profile <base64> --constants vm/constants_kbc.h

and receives a GN scope with the values the VM compiles in:

    count  = number of public opcodes the map covers
    wire   = "w0,w1,..."  wire byte of each public opcode, in enum order
    digest = "0x..,..."   the 32 SHA-256 bytes of the canonical profile
    id     = "sha256:<hex>"

Any invalid profile fails `gn gen`, so a VM is never built with a map the
compiler would not produce. The same rules are implemented by
dart2bytecode (pkg/dart2bytecode/lib/bytecode_profile.dart) and by the
workspace reference tool (scripts/diversification_profile.py).
"""

import argparse
import base64
import hashlib
import json
import re
import sys

MAX_ENCODED = 8192
MAX_FIXED_POINTS = 16


class ProfileError(Exception):
    pass


def opcode_names(constants_h):
    with open(constants_h, encoding='utf-8') as f:
        text = f.read()
    match = re.search(r'#define PUBLIC_KERNEL_BYTECODES_LIST\(V\)(.*?)\n\n',
                      text, re.S)
    if match is None:
        raise ProfileError('PUBLIC_KERNEL_BYTECODES_LIST not found')
    return re.findall(r'V\((\w+),', match.group(1))


def _no_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProfileError('duplicate key %s' % key)
        result[key] = value
    return result


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=False).encode('utf-8')


def _is_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def parse_profile(encoded, names):
    if len(encoded) > MAX_ENCODED:
        raise ProfileError('profile is too large')
    try:
        raw = base64.b64decode(encoded, validate=True)
    except ValueError:
        raise ProfileError('profile is not base64')
    if base64.b64encode(raw).decode('ascii') != encoded:
        raise ProfileError('profile base64 is not canonical')
    try:
        profile = json.loads(raw.decode('utf-8'),
                             object_pairs_hook=_no_duplicates)
    except (ValueError, UnicodeDecodeError) as error:
        raise ProfileError('profile is not JSON: %s' % error)
    if _canonical(profile) != raw:
        raise ProfileError('profile is not canonical JSON')
    if not isinstance(profile, dict) or set(profile) != {
            'schema', 'contract_version', 'bytecode', 'native_layout'
    }:
        raise ProfileError('unexpected profile fields')
    if (profile['schema'] != 'open-shorebird.diversification-profile' or
            not _is_int(profile['contract_version']) or
            profile['contract_version'] != 3):
        raise ProfileError('not a contract v3 diversification profile')
    if profile['native_layout'] is not None:
        raise ProfileError('native_layout must be null in contract v3')
    bytecode = profile['bytecode']
    if not isinstance(bytecode, dict) or set(bytecode) != {
            'scheme', 'format_version', 'opcode_table_sha256', 'wire_opcodes'
    }:
        raise ProfileError('unexpected bytecode subprofile fields')
    if (bytecode['scheme'] != 'opcode-permutation-v1' or
            not _is_int(bytecode['format_version']) or
            bytecode['format_version'] != 3):
        raise ProfileError('unsupported bytecode scheme or format version')
    table = hashlib.sha256('\n'.join(names).encode('ascii')).hexdigest()
    if bytecode['opcode_table_sha256'] != table:
        raise ProfileError('profile was made for another opcode table')
    wires = bytecode['wire_opcodes']
    if (not isinstance(wires, list) or len(wires) != len(names) or
            not all(_is_int(w) for w in wires) or
            sorted(wires) != list(range(len(names)))):
        raise ProfileError('wire_opcodes is not a permutation of the table')
    if wires[0] != 0:
        raise ProfileError('Trap must keep wire byte 0')
    fixed = sum(1 for i, w in enumerate(wires) if i and i == w)
    if fixed >= MAX_FIXED_POINTS:
        raise ProfileError('too many fixed points (%d)' % fixed)
    return wires, hashlib.sha256(raw).digest()


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['gn'])
    parser.add_argument('--profile', required=True)
    parser.add_argument('--constants', required=True)
    args = parser.parse_args(argv)
    try:
        wires, digest = parse_profile(args.profile,
                                      opcode_names(args.constants))
    except ProfileError as error:
        sys.stderr.write('dart_bytecode_profile: %s\n' % error)
        return 1
    print('count = %d' % len(wires))
    print('wire = "%s"' % ','.join(str(w) for w in wires))
    print('digest = "%s"' % ','.join('0x%02x' % b for b in digest))
    print('id = "sha256:%s"' % digest.hex())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
