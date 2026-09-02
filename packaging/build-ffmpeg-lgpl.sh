#!/bin/sh
set -eu

# Builds an LGPL-2.1 FFmpeg for 8Kloud Switcher.
#
# Why: Ubuntu's libavcodec is configured --enable-gpl, so as shipped it is
# GPL-2.0-or-later. A binary linking it must then be conveyed under the GPL,
# which defeats this project's MPL-2.0 licensing and does not combine with the
# proprietary CUDA/NVENC runtime and the DeckLink SDK we also link. Nothing here
# needs FFmpeg's GPL-only parts -- no libx264/libx265, no postproc, none of the
# GPL filters -- so an LGPL build is a drop-in replacement.
#
# Unlike a minimal encoder-only build, a switcher ingests whatever media the
# operator points it at, so this keeps FFmpeg's full native codec set (all of it
# LGPL) rather than an allow-list. What it does NOT do is pass --enable-gpl or
# --enable-nonfree, and --disable-autodetect stops the configure script from
# quietly linking a GPL system library it happened to find.
#
# External libraries, all LGPL-compatible:
#   nv-codec-headers  MIT       -- NVENC/NVDEC interface, no NVIDIA code;
#                                  libavcodec dlopens the encoder from the driver
#   libsrt            MPL-2.0
#   dav1d             BSD-2-Clause -- software AV1 decode: operator AV1 media on
#                                  hosts without NVDEC, and the AV1 program-output
#                                  round-trip test. FFmpeg's native av1 decoder
#                                  is a hwaccel-only front end with no CPU path
#   gnutls            LGPL-2.1+  -- https:// ingest
#   zlib              Zlib       -- REQUIRED by the PNG and EXR decoders, so the
#                                  still-image inputs need it; --disable-autodetect
#                                  would otherwise leave it off
#   bzip2, liblzma    permissive -- compressed TIFF and Matroska tracks
#
# Build deps:
#   sudo apt install git build-essential nasm pkg-config libsrt-gnutls-dev libdav1d-dev
#
# Usage:
#   packaging/build-ffmpeg-lgpl.sh [prefix]      # default: build/ffmpeg-lgpl
#
# Then configure the project against it:
#   cmake -B build -G Ninja -DKLOUD_FFMPEG_PREFIX=build/ffmpeg-lgpl

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
prefix=${1:-$root/build/ffmpeg-lgpl}
case $prefix in /*) ;; *) prefix=$root/$prefix ;; esac

# Fail on a missing build dep here rather than 200 lines into ./configure.
missing=
for t in git make cc pkg-config nasm; do
  command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
pkg-config --exists srt || missing="$missing libsrt-gnutls-dev"
pkg-config --exists dav1d || missing="$missing libdav1d-dev"
if [ -n "$missing" ]; then
  echo "ERROR: missing build dependencies:$missing" >&2
  echo "  sudo apt install git build-essential nasm pkg-config libsrt-gnutls-dev libdav1d-dev" >&2
  exit 1
fi

work=$root/build/ffmpeg-src
ffmpeg_tag=${FFMPEG_TAG:-n8.0.3}
headers_tag=${NV_CODEC_HEADERS_TAG:-n13.0.19.0}

mkdir -p "$work"

if [ ! -d "$work/nv-codec-headers" ]; then
  # GitHub mirror: git.videolan.org is unreachable from some networks.
  git clone --depth 1 --branch "$headers_tag" \
    https://github.com/FFmpeg/nv-codec-headers.git \
    "$work/nv-codec-headers"
fi
make -C "$work/nv-codec-headers" PREFIX="$prefix" install

if [ ! -d "$work/ffmpeg" ]; then
  git clone --depth 1 --branch "$ffmpeg_tag" \
    https://git.ffmpeg.org/ffmpeg.git "$work/ffmpeg"
fi

cd "$work/ffmpeg"
make distclean >/dev/null 2>&1 || true

# FFmpeg does not yet ship AV1 carriage for MPEG-TS. Keep the local source
# idempotently patched so rerunning this build script is safe.
av1_ts_patch=$root/packaging/patches/ffmpeg/0001-av1-mpegts-draft.patch
if git apply --reverse --check "$av1_ts_patch" >/dev/null 2>&1; then
  : # already applied
elif git apply --check "$av1_ts_patch"; then
  git apply "$av1_ts_patch"
else
  echo "ERROR: AV1 MPEG-TS patch does not apply to FFmpeg $ffmpeg_tag" >&2
  exit 1
fi

# The switcher links libavcodec, libavformat, libavutil, libavfilter,
# libswresample and libswscale. avdevice is the one library it never touches.
#
# Enabled deliberately:
#   ffnvcodec/nvenc/nvdec  hevc_nvenc program output, NVDEC clip decode
#   libsrt                 srt:// ingest and program output
#   libdav1d               AV1 decode without a GPU (native av1 is hwaccel-only)
#   filters                atempo (clip speed), aformat, abuffer/abuffersink
#   muxers                 matroska (recording), mpegts (SRT)
#   demuxers/decoders      the full native set: operator media and the
#                          PNG/JPEG/WebP/BMP/TIFF/TGA/EXR still inputs
PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
./configure \
  --prefix="$prefix" \
  --enable-shared \
  --disable-static \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-autodetect \
  --enable-network \
  --enable-ffnvcodec \
  --enable-nvenc \
  --enable-nvdec \
  --enable-cuvid \
  --enable-libsrt \
  --enable-libdav1d \
  --enable-gnutls \
  --enable-zlib \
  --enable-bzlib \
  --enable-lzma \
  --enable-protocol=file,pipe,srt,tcp,udp,rtp,http,https,crypto,data,concat \
  --enable-pic \
  --enable-rpath

make -j"$(nproc)"
make install

# The whole exercise is pointless if the result is not actually LGPL, so assert
# it here rather than discovering it at package time.
cat > "$work/check_license.c" <<'EOF'
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <stdio.h>
int main(void) {
    printf("libavcodec  %s\n", avcodec_license());
    printf("libavformat %s\n", avformat_license());
    printf("libavfilter %s\n", avfilter_license());
    return 0;
}
EOF
cc "$work/check_license.c" -o "$work/check_license" \
  $(PKG_CONFIG_PATH="$prefix/lib/pkgconfig" pkg-config --cflags --libs \
    libavcodec libavformat libavfilter)
reported=$(LD_LIBRARY_PATH="$prefix/lib" "$work/check_license")
echo "$reported"
if printf '%s\n' "$reported" | grep -qv 'LGPL'; then
  echo "ERROR: expected an LGPL build throughout, got:" >&2
  printf '%s\n' "$reported" >&2
  exit 1
fi

echo
echo "built: $prefix"
echo "configure the project against it with:"
echo "  cmake -B build -G Ninja -DKLOUD_FFMPEG_PREFIX=$prefix"
