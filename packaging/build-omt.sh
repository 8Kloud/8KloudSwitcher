#!/usr/bin/env bash
# Builds the OMT runtime this project links -- libomt-c (the native C
# implementation of the libomt API, with the pre-built decoder pool) and libvmx
# (the VMX codec, from the 8Kloud fork carrying the VMX_Create memset trim,
# openmediatransport/libvmx PR #13) -- and lays them out as the SDK CMake
# expects:
#
#   third_party/omt/include/libomt.h
#   third_party/omt/lib/libomt.so
#   third_party/omt/lib/libvmx.so     (dlopened by libomt.so from its own dir)
#
# Usage: packaging/build-omt.sh [DEST]      (default: third_party/omt)
# Needs: git, cmake, ninja or make, clang++ (libvmx's own build script uses it).
#
# Overrides: LIBOMT_C_REPO / LIBOMT_C_REF, LIBVMX_REPO / LIBVMX_REF, e.g.
#   LIBVMX_REPO=https://github.com/openmediatransport/libvmx LIBVMX_REF=master
# once the memset trim has merged upstream.
#
# The stock NativeAOT libomt (openmediatransport/libomt, .NET 8) still works in
# the same layout, minus the decoder pool; docs/omt.md describes that route.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT/third_party/omt}"
SRC="$ROOT/build/omt-src"
LIBOMT_C_REPO="${LIBOMT_C_REPO:-https://github.com/8Kloud/libomt-c.git}"
LIBOMT_C_REF="${LIBOMT_C_REF:-master}"
LIBVMX_REPO="${LIBVMX_REPO:-https://github.com/8Kloud/libvmx.git}"
LIBVMX_REF="${LIBVMX_REF:-create-cost}"

for tool in git cmake clang++; do
    command -v "$tool" >/dev/null || { echo "build-omt: $tool is required" >&2; exit 1; }
done

fetch() {  # fetch REPO REF DIR
    if [ -d "$3/.git" ]; then
        git -C "$3" fetch -q origin "$2"
        git -C "$3" checkout -q FETCH_HEAD
    else
        git clone -q --depth 1 --branch "$2" "$1" "$3"
    fi
    echo "  $(basename "$3"): $(git -C "$3" rev-parse --short HEAD) ($2)"
}

mkdir -p "$SRC" "$DEST/include" "$DEST/lib"
echo "== sources"
fetch "$LIBVMX_REPO" "$LIBVMX_REF" "$SRC/libvmx"
fetch "$LIBOMT_C_REPO" "$LIBOMT_C_REF" "$SRC/libomt-c"

echo "== libvmx"
(cd "$SRC/libvmx/build" && sh buildlinuxx64.sh)

echo "== libomt-c"
cmake -S "$SRC/libomt-c" -B "$SRC/libomt-c/build" -DCMAKE_BUILD_TYPE=Release \
      -DOMT_BUILD_TESTS=OFF -DOMT_BUILD_EXAMPLES=OFF >/dev/null
cmake --build "$SRC/libomt-c/build" --target omt >/dev/null

cp "$SRC/libomt-c/include/libomt.h" "$DEST/include/libomt.h"
cp "$SRC/libomt-c/build/libomt.so" "$DEST/lib/libomt.so"
cp "$SRC/libvmx/build/libvmx.so" "$DEST/lib/libvmx.so"
echo "== installed to $DEST"
ls -l "$DEST/lib" | awk 'NR>1 {print "  " $5 "  " $9}'
echo "Re-run cmake; it should report 'OMT decoder prewarm: available (libomt-c)'."
