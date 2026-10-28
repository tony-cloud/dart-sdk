#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Verify TFA AOT dispatch and offline pub in an interpreter-enabled SDK.

Build create_sdk, gen_snapshot and dartaotruntime first. In contrast with the
patchable no-TFA app profile, SDK tools use TFA and must retain table dispatch
when dynamic invocation forwarders have been removed.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    sdk = build / 'dart-sdk'

    with tempfile.TemporaryDirectory(prefix='dart-aot-tool-dispatch-') as tmp:
        work = Path(tmp)
        source = work / 'main.dart'
        classes = [
            'abstract class V implements Comparable<V> { final int n; V(this.n); }\n'
        ]
        for i in range(6):
            classes.append(f'class C{i} extends V {{ C{i}(int n):super(n); '
                           'int compareTo(V other) => n + other.n; }\n')
        classes.append('@pragma("vm:never-inline")\n'
                       'int compare(Comparable<V> a, V b) => a.compareTo(b);\n')
        values = ', '.join(f'C{i}(n)' for i in range(6))
        source.write_text(''.join(classes) + "void main() {\n"
                          "  if (Uri.directory('/tmp').path != '/tmp/') {\n"
                          "    throw StateError('Incorrect SDK dispatch');\n"
                          "  }\n"
                          "  final n = DateTime.now().microsecondsSinceEpoch % 1000 + 100;\n"
                          f"  final values = <V>[{values}];\n"
                          "  if (compare(values[n % values.length], C0(7)) != n + 7) {\n"
                          "    throw StateError('Incorrect SDK interface return ABI');\n"
                          "  }\n"
                          "  print('TFA AOT SDK dispatch passed');\n"
                          "}\n")
        kernel = work / 'main.dill'
        snapshot = work / 'main.aot'
        environment = dict(os.environ, PUB_CACHE=str(work / 'pub-cache'),
                           DART_SUPPRESS_ANALYTICS='true', CI='true', BOT='true')

        def run(*command):
            subprocess.run([str(x) for x in command], cwd=work,
                           env=environment, check=True)

        run(build / 'bootstrap_gen_kernel.exe', '--aot',
            f'--platform={build / "vm_platform.dill"}',
            f'--output={kernel}', source)
        run(build / 'gen_snapshot_product', '--snapshot-kind=app-aot-elf',
            f'--elf={snapshot}', kernel)
        run(build / 'dartaotruntime_product', snapshot)

        (work / 'pubspec.yaml').write_text(
            'name: aot_tool_dispatch_probe\n'
            'environment:\n  sdk: ">=3.0.0 <4.0.0"\n')
        run(sdk / 'bin/dart', 'pub', 'get', '--offline')
        if not (work / '.dart_tool/package_config.json').is_file():
            raise AssertionError('pub did not write its package configuration')
        print('Production SDK pub isolate dispatch passed')


if __name__ == '__main__':
    main()
