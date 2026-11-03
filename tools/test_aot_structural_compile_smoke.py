#!/usr/bin/env python3
# Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""Compile smoke tests for the structural AOT profile (closed-world guards).

gen_snapshot must not crash on programs that make the closed-world
devirtualization (see docs/shorebird-structural-patching.md) emit its guard
diamond in unusual shapes:

* `shapes`: a compact program with effect-only and exact-class uses of
  devirtualized calls, where both arms of a guard collapse into a redundant
  branch. The guard's branch, gotos and block entries must carry an
  environment like every other branch in the graph, because
  EliminateRedundantBranches copies the branch's deopt target into the goto it
  creates. Without it gen_snapshot crashed (Environment::DeepCopyTo on a null
  environment) on a real Flutter application.
* `large`: pkg/vm/bin/gen_kernel.dart, a ~35 MB kernel program, as a
  framework-sized program.
* `--app-dill <file>`: optionally an additional real application kernel (for
  example a Flutter application built with the same SDK hash).

Each program is compiled with --no-tfa --shorebird-structural-patching to an
ELF snapshot with the matching gen_snapshot. Use --product for PRODUCT.
"""

import argparse
import contextlib
from pathlib import Path
import subprocess
import sys
import tempfile

SHAPES = r'''
abstract class Shape {
  final int w;
  Shape(this.w);
  int area();
  int get sides => 3;
  void touch() {}
  void note(List<int> log) {}
}
class Sq extends Shape {
  Sq(int w) : super(w);
  int area() => w * w;
  void note(List<int> log) { log.add(1); }
}
class Circle extends Shape {
  Circle(int w) : super(w);
  int area() => w * 3;
  int get sides => 0;
}
class Tri extends Shape {
  Tri(int w) : super(w);
  int area() => w;
}
// Effect-only uses of devirtualized calls: the result is unused, so the guard
// diamond has no phi and its arms can collapse into a redundant branch.
void unusedField(Shape s) { s.w; }
void unusedGetter(Shape s) { s.sides; }
void unusedPure(Shape s) { s.area(); }
void emptyBody(Shape s) { s.touch(); }
void unusedInLoop(List<Shape> l) { for (final s in l) { s.w; s.touch(); s.sides; } }
bool unusedIs(Object o) { o is Shape; return true; }
void unusedBranch(Shape s, bool b) { if (b) { s.w; } else { s.touch(); } }
// Exact class known from the allocation: the guard tests a constant class id.
void exactUnused() { final s = Sq(1); s.touch(); s.w; s.sides; }
void exactUnusedChain() { final c = Circle(2); c.touch(); c.touch(); c.w; }
class Holder {
  bool a = false;
  bool b = true;
  final Shape inner = Tri(1);
  Holder() { a = true; b = false; inner.touch(); inner.w; }
}
int main() {
  exactUnused(); exactUnusedChain(); Holder();
  final l = <Shape>[Sq(1), Circle(2), Tri(3)];
  for (final s in l) { unusedField(s); unusedGetter(s); unusedPure(s); emptyBody(s); unusedBranch(s, s.w > 1); }
  unusedInLoop(l);
  unusedIs(l);
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--work-dir', type=Path)
    parser.add_argument('--product', action='store_true')
    parser.add_argument('--app-dill', type=Path, action='append', default=[])
    parser.add_argument('--skip-large', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build_dir.resolve()

    def run(*command):
        result = subprocess.run([str(value) for value in command], cwd=root,
                                capture_output=True, text=True, timeout=1800)
        if result.returncode != 0 or 'CRASH' in result.stderr + result.stdout:
            print(result.stdout[-3000:], result.stderr[-3000:], sep='\n')
            raise AssertionError(f'Failed ({result.returncode}): {command[:3]}')

    if args.work_dir:
        args.work_dir.mkdir(parents=True, exist_ok=True)
    workspace = (contextlib.nullcontext(str(args.work_dir.resolve()))
                 if args.work_dir else
                 tempfile.TemporaryDirectory(prefix='dart-aot-structural-compile-'))
    with workspace as tmp:
        work = Path(tmp)
        dart = build / 'dart'
        platform = build / ('vm_platform_product.dill' if args.product
                            else 'vm_platform.dill')
        gen_snapshot = build / ('gen_snapshot_product' if args.product
                                else 'gen_snapshot')
        for artifact in (dart, platform, gen_snapshot):
            if not artifact.is_file():
                parser.error(f'Required artifact is missing: {artifact}')
        defines = ['-Ddart.vm.product=true'] if args.product else []
        packages = f'--packages={root / ".dart_tool/package_config.json"}'
        print(f'Runtime mode: {"PRODUCT" if args.product else "non-PRODUCT"}',
              flush=True)
        (work / 'shapes.dart').write_text(SHAPES)
        programs = {'shapes': work / 'shapes.dart'}
        if not args.skip_large:
            programs['large'] = root / 'pkg/vm/bin/gen_kernel.dart'
        for name, source in programs.items():
            kernel = work / f'{name}.dill'
            run(dart, packages, root / 'pkg/vm/bin/gen_kernel.dart', '--aot',
                '--no-tfa', *defines, '--shorebird-structural-patching',
                f'--platform={platform}', f'--output={kernel}', source)
            run(gen_snapshot, '--shorebird-structural-patching',
                '--snapshot-kind=app-aot-elf', f'--elf={work / name}.so', kernel)
            print(f'{name}: structural gen_snapshot ok', flush=True)
        for kernel in args.app_dill:
            run(gen_snapshot, '--deterministic', '--shorebird-structural-patching',
                '--snapshot-kind=app-aot-elf', f'--elf={work / "app"}.so', kernel)
            print(f'{kernel.name}: structural gen_snapshot ok', flush=True)


if __name__ == '__main__':
    main()
