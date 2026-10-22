#!/bin/bash
# Builds Loserfish variants for speed comparison (Linux/macOS, needs g++ and make).
#   tests/build_variants.sh [out_dir]          default out_dir: bin
#   ARCHS="x86-64 x86-64-avx2" tests/build_variants.sh   (default list depends on the CPU)
# Then compare:  python3 tests/bench.py bin/lf-* --threads 1
# A variant the CPU cannot run (illegal instruction) is reported and removed.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/bin}
case "$(uname -m)" in
  x86_64|amd64) DEFAULT_ARCHS="x86-64 x86-64-sse41-popcnt x86-64-avx2 x86-64-bmi2" ;;
  arm64|aarch64) if [ "$(uname -s)" = Darwin ]; then DEFAULT_ARCHS="apple-silicon"; else DEFAULT_ARCHS="armv8 armv8-dotprod"; fi ;;
  *) DEFAULT_ARCHS="general-64" ;;
esac
ARCHS=${ARCHS:-$DEFAULT_ARCHS}
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

PGO_ARCH=${PGO_ARCH:-${BEST:-${DEFAULT_ARCHS%% *}}}
make -s clean > /dev/null 2>&1
if make -j"$(nproc 2>/dev/null || echo 2)" profile-build ARCH="$PGO_ARCH" COMP=gcc > /tmp/lf_build_pgo.log 2>&1; then
  cp loserfish "$OUT/lf-$PGO_ARCH-pgo"; echo "built   lf-$PGO_ARCH-pgo"
else
  echo "FAILED  profile-build (see /tmp/lf_build_pgo.log)"
fi
make -s clean > /dev/null 2>&1
ls -la "$OUT"
