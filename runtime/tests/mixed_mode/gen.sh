#!/bin/bash
# Builds base (K=3, Mach-O dylib, signed: the "release") and patch (K=5, ELF,
# loaded with non-executable text) from mixed_template.dart.
set -euo pipefail
cd "$(dirname "$0")"
SDK=../dart-sdk
B=$SDK/xcodebuild/ReleaseARM64
DART=$SDK/tools/sdks/dart-sdk/bin/dart
for v in base:3 patch:5; do
  name=${v%%:*}; k=${v##*:}
  sed "s/__K__/$k/g" mixed_template.dart > $name.dart
  $DART --packages=$SDK/.dart_tool/package_config.json $SDK/pkg/vm/bin/gen_kernel.dart \
    --platform=$B/vm_platform.dill --aot -o $name.dill $name.dart
  $B/gen_snapshot --deterministic --snapshot_kind=app-aot-elf --elf=$name.aot $name.dill
  $B/gen_snapshot --deterministic --snapshot_kind=app-aot-macho-dylib --macho=$name.dylib $name.dill
  codesign -f -s - $name.dylib 2>/dev/null
done
ls -la base.aot base.dylib patch.aot patch.dylib
