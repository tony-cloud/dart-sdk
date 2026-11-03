#!/bin/bash
# Usage: gen.sh [BUILD_DIR] [OUT_DIR]
# Builds base (K=3, Mach-O dylib, signed: the "release") and patch (K=5, ELF,
# loaded with non-executable text) from mixed_template.dart.
set -euo pipefail
cd "$(dirname "$0")"
SDK=../dart-sdk
B=${1:-$SDK/xcodebuild/ReleaseARM64}
OUT=${2:-.}
mkdir -p "$OUT"
DART=$SDK/tools/sdks/dart-sdk/bin/dart
DEFS=()
case "$B" in *Product*) DEFS=(-Ddart.vm.product=true) ;; esac
for v in base:3 patch:5; do
  name=${v%%:*}; k=${v##*:}
  sed "s/__K__/$k/g" mixed_template.dart > $OUT/$name.dart
  $DART --packages=$SDK/.dart_tool/package_config.json $SDK/pkg/vm/bin/gen_kernel.dart \
    --platform=$B/vm_platform.dill --aot ${DEFS[@]+"${DEFS[@]}"} -o $OUT/$name.dill $OUT/$name.dart
  $B/gen_snapshot --deterministic --snapshot_kind=app-aot-elf --elf=$OUT/$name.aot $OUT/$name.dill
  $B/gen_snapshot --deterministic --snapshot_kind=app-aot-macho-dylib --macho=$OUT/$name.dylib $OUT/$name.dill
  codesign -f -s - $OUT/$name.dylib 2>/dev/null
done
