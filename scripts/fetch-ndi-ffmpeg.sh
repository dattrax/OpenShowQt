#!/usr/bin/env bash
# Build the minimal FFmpeg 7.1 shared libraries that NDI|HX decoding needs.
#
# NDI 6.3.2 decodes HX inside the runtime with libavcodec.so.61 and
# libavutil.so.59. Fedora and RPM Fusion ship FFmpeg 9 (libavcodec.so.63),
# whose ABI the runtime cannot use, and no package provides the 7.1 sonames.
# This script downloads FFmpeg 7.1.1 and builds only the H.264/HEVC decoders
# into third_party/ffmpeg-7.1 (gitignored). Full-bandwidth NDI needs none of
# this; it is only for NDI|HX sources such as phone cameras and many PTZ
# cameras.
#
# Usage:
#   scripts/fetch-ndi-ffmpeg.sh [--force] [--jobs N] [TARBALL]
#
# TARBALL may be a local ffmpeg-7.1.x.tar.xz; otherwise the release is
# downloaded from <https://ffmpeg.org/releases/>. The build uses the system
# toolchain and installs nothing into the system. The result is LGPL-2.1+
# (no GPL-only components are enabled).
#
# After building, reconfigure tools/ndi-helper (it picks up
# third_party/ffmpeg-7.1 automatically) and rebuild. The helper then stages
# the four libraries beside libndi.so.6 and preloads them.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${ROOT}/third_party/ffmpeg-7.1"
VERSION="7.1.1"
URL="https://ffmpeg.org/releases/ffmpeg-${VERSION}.tar.xz"
FORCE=0
JOBS="$(nproc 2>/dev/null || echo 4)"
TARBALL="${FFMPEG_TARBALL:-}"

usage() {
    sed -n '2,27p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "${1}" in
        --force) FORCE=1; shift ;;
        --jobs)
            shift
            JOBS="${1:-}"
            if ! [[ "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
                echo "fetch-ndi-ffmpeg: --jobs needs a positive number" >&2
                exit 2
            fi
            shift
            ;;
        -h|--help) usage; exit 0 ;;
        -*)
            echo "fetch-ndi-ffmpeg: unknown option '${1}'" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -n "${TARBALL}" ]]; then
                echo "fetch-ndi-ffmpeg: more than one tarball given" >&2
                exit 2
            fi
            TARBALL="${1}"
            shift
            ;;
    esac
done

if [[ -e "${TARGET}" && "${FORCE}" -ne 1 ]]; then
    echo "fetch-ndi-ffmpeg: '${TARGET}' already exists; pass --force to replace it" >&2
    exit 2
fi

for tool in cc c++ make tar; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "fetch-ndi-ffmpeg: '${tool}' is required" >&2
        exit 2
    fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/openshow-ffmpeg.XXXXXX")"
cleanup() {
    rm -rf "${WORK}"
}
trap cleanup EXIT

if [[ -z "${TARBALL}" ]]; then
    TARBALL="${WORK}/ffmpeg-${VERSION}.tar.xz"
    echo "Downloading FFmpeg ${VERSION} from ${URL}"
    if ! curl -L -f --retry 3 -o "${TARBALL}" "${URL}"; then
        echo "fetch-ndi-ffmpeg: download failed; pass a local tarball instead" >&2
        exit 3
    fi
fi
if [[ ! -f "${TARBALL}" ]]; then
    echo "fetch-ndi-ffmpeg: '${TARBALL}' does not exist" >&2
    exit 2
fi

mkdir -p "${WORK}/src"
tar -xf "${TARBALL}" -C "${WORK}/src"
SOURCE="$(find "${WORK}/src" -maxdepth 1 -type d -name 'ffmpeg-7.1*' | head -n 1)"
if [[ -z "${SOURCE}" || ! -x "${SOURCE}/configure" ]]; then
    echo "fetch-ndi-ffmpeg: the tarball does not look like FFmpeg 7.1" >&2
    exit 3
fi

ASM_FLAGS=()
if ! command -v nasm >/dev/null 2>&1 && ! command -v yasm >/dev/null 2>&1; then
    echo "nasm/yasm not found: building without x86 assembly."
    echo "NDI|HX decoding will be significantly slower (the C fallback used about"
    echo "56% of one core at 1080p60 here; the SIMD build used about 37%)."
    echo "Install nasm (e.g. sudo dnf install nasm) and rerun with --force."
    ASM_FLAGS=(--disable-x86asm)
fi

(
    cd "${SOURCE}"
    ./configure \
        --prefix="${TARGET}" \
        --enable-shared --disable-static \
        --disable-programs --disable-doc \
        --disable-everything \
        --enable-decoder=h264,hevc \
        --enable-parser=h264,hevc \
        --disable-network --disable-autodetect \
        --disable-avdevice --disable-avfilter --disable-avformat \
        --disable-postproc --enable-swscale --enable-swresample \
        "${ASM_FLAGS[@]}"
    make -j"${JOBS}"
    make install
)

cat <<EOF
FFmpeg ${VERSION} installed at: ${TARGET}

Rebuild the helper so it stages these files beside libndi.so.6:

  cmake -S tools/ndi-helper -B build-ndi-helper \\
    -DOPENSHOW_NDI_SDK_DIR="\${PWD}/third_party/ndi-sdk"
  cmake --build build-ndi-helper
EOF
