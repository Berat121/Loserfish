#!/bin/bash
# Builds Loserfish variants for speed comparison (Linux/macOS, needs g++ and make).
#   tests/build_variants.sh [out_dir]          default out_dir: bin
#   ARCHS="x86-64 x86-64-bmi2" tests/build_variants.sh
# Then compare:  python3 tests/bench.py bin/lf-* --threads 1
# A variant the CPU cannot run (illegal instruction) is reported and removed.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/bin}
ARCHS=${ARCHS:-"x86-64 x86-64-sse41-popcnt x86-64-avx2 x86-64-bmi2"}
PGO_ARCH=${PGO_ARCH:-}
mkdir -p "$OUT"
cd "$ROOT/src" || exit 1

runs() { printf 'uci\nquit\n' | timeout 10 "$1" > /dev/null 2>&1; }

for a in $ARCHS; do
  make -s clean > /dev/null 2>&1
  if make -j"$(nproc 2>/dev/null || echo 2)" build ARCH="$a" COMP=gcc > "/tmp/lf_build_$a.log" 2>&1; then
    cp loserfish "$OUT/lf-$a"
    if runs "$OUT/lf-$a"; then echo "built   lf-$a"; BEST=$a; else echo "skipped lf-$a (CPU cannot run it)"; rm -f "$OUT/lf-$a"; fi
  else
    echo "FAILED  lf-$a (see /tmp/lf_build_$a.log)"
  fi
done

PGO_ARCH=${PGO_ARCH:-${BEST:-x86-64}}
make -s clean > /dev/null 2>&1
if make -j"$(nproc 2>/dev/null || echo 2)" profile-build ARCH="$PGO_ARCH" COMP=gcc > /tmp/lf_build_pgo.log 2>&1; then
  cp loserfish "$OUT/lf-$PGO_ARCH-pgo"; echo "built   lf-$PGO_ARCH-pgo"
else
  echo "FAILED  profile-build (see /tmp/lf_build_pgo.log)"
fi
make -s clean > /dev/null 2>&1
ls -la "$OUT"
