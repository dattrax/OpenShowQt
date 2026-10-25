#!/bin/sh
#
# Task 6.2c: build the Skia submodule (third_party/skia) with the arguments
# OpenShowQt has validated, so a fresh clone needs no manual Skia recipe.
#
#   scripts/build-skia.sh [--check] [--no-sync] [SKIA_ROOT] [SKIA_BUILD_DIR]
#
#   --check     Only verify the tree is at the pinned revision and has the
#               libraries OpenShowQt links; print what is missing and exit 1.
#               No toolchain is required.
#   --no-sync   Skip tools/git-sync-deps (assume third_party/externals is
#               already populated).
#
# SKIA_ROOT defaults to the submodule, SKIA_BUILD_DIR to ${SKIA_ROOT}/out/Release.
# A prebuilt tree is used as it stands: this script never rebuilds a tree that
# already has the required libraries.
#
# Network access is needed the first time, for the DEPS-pinned git repositories
# (bundled libpng/FreeType/zlib/Expat/libjpeg-turbo) and for GN when neither
# bin/gn nor a system gn is available.

set -eu

# The revision the application and its fixtures are validated against; keep in
# step with OPENSHOW_SKIA_REVISION in cmake/OpenShowSkia.cmake.
SKIA_REVISION=a94df1cdabb03d3ead2486413bc2999b500aa4f8

# Skia's static archives the worker links against. libskunicode_icu needs
# system ICU headers to build, so a tree without it cannot serve OpenShowQt
# (task 6.2b).
LIBRARIES="libskia.a libskparagraph.a libskshaper.a libskunicode_core.a libskunicode_icu.a"

check_only=false
sync=true
root=''
build_dir=''

while [ $# -gt 0 ]; do
  case "$1" in
    --check)   check_only=true ;;
    --no-sync) sync=false ;;
    -h|--help)
      sed -n '2,20p' "$0"
      exit 0
      ;;
    *)
      if [ -z "$root" ]; then root=$1
      elif [ -z "$build_dir" ]; then build_dir=$1
      else echo "build-skia: unexpected argument '$1'" >&2; exit 2
      fi
      ;;
  esac
  shift
done

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
[ -n "$root" ] || root="$script_dir/../third_party/skia"
if ! root=$(CDPATH='' cd -- "$root" 2>/dev/null && pwd); then
  cat >&2 <<EOF
build-skia: no Skia checkout at the given path (is the submodule initialized?)
  git submodule update --init --recursive third_party/skia
EOF
  exit 1
fi
[ -n "$build_dir" ] || build_dir="$root/out/Release"

missing=''
for library in $LIBRARIES; do
  [ -f "$build_dir/$library" ] || missing="$missing $library"
done

if [ -z "$missing" ]; then
  if $check_only; then
    echo "Skia at $root is usable: all libraries present in $build_dir"
    exit 0
  fi
  echo "Skia at $root is already built ($build_dir); nothing to do."
  echo "Re-run 'ninja -C $build_dir' after changing Skia itself."
  exit 0
fi

if $check_only; then
  echo "Skia build incomplete: missing$missing" >&2
  echo "in $build_dir" >&2
  exit 1
fi

if [ ! -f "$root/DEPS" ]; then
  cat >&2 <<EOF
build-skia: no Skia checkout at $root

Fetch the submodule first:
  git submodule update --init --recursive third_party/skia
EOF
  exit 1
fi

# Fail early and loudly on a mis-checked-out submodule rather than compiling
# the wrong revision and only checking afterwards.
if git -C "$root" rev-parse --verify HEAD >/dev/null 2>&1; then
  actual=$(git -C "$root" rev-parse HEAD)
  if [ "$actual" != "$SKIA_REVISION" ]; then
    cat >&2 <<EOF
build-skia: Skia at $root is at
  $actual
but OpenShowQt is pinned to
  $SKIA_REVISION
Check the submodule out at the pinned revision:
  git -C $root checkout $SKIA_REVISION
EOF
    exit 1
  fi
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "build-skia: python3 is required to sync Skia's DEPS" >&2
  exit 1
fi

if $sync; then
  echo "==> Syncing Skia's pinned dependencies (network)"
  # Run from the checkout root: git-sync-deps' emsdk hook resolves
  # third_party/externals/emsdk relative to the current directory.
  if ! ( cd "$root" && ./tools/git-sync-deps ); then
    cat >&2 <<EOF
build-skia: tools/git-sync-deps failed. The DEPS repositories and the
emsdk post-sync hook all download from the network, so a transient
failure is usually fixed by simply re-running this script. The emsdk
toolchain itself is only needed for WebAssembly targets.
EOF
    exit 1
  fi
fi

# GN comes from the checkout, the environment or the system. Skia's bin/gn is
# a thin CIPD wrapper, so bin/fetch-gn fills it in when it is absent.
gn=${GN:-}
if [ -z "$gn" ]; then
  if [ -x "$root/bin/gn" ]; then
    gn="$root/bin/gn"
  elif command -v gn >/dev/null 2>&1; then
    gn=gn
  elif [ -f "$root/bin/fetch-gn" ]; then
    echo "==> Fetching GN into $root/bin"
    ( cd "$root" && python3 bin/fetch-gn )
    gn="$root/bin/gn"
  fi
fi
if [ -z "$gn" ] || ! command -v "$gn" >/dev/null 2>&1; then
  echo "build-skia: no gn found; install gn or pass GN=/path/to/gn" >&2
  exit 1
fi

if ! command -v ninja >/dev/null 2>&1; then
  echo "build-skia: ninja-build is required (dnf install ninja-build)" >&2
  exit 1
fi

# These match the validated build: Vulkan output, no GL, and everything but
# ICU taken from Skia's bundled copies. System ICU is what lets
# libskunicode_icu agree with the distro's versioned symbols (task 6.2b).
args='skia_use_vulkan=true skia_use_gl=false is_official_build=true skia_use_libwebp_decode=false skia_use_libwebp_encode=false skia_use_system_libpng=false skia_use_system_freetype2=false skia_use_system_libjpeg_turbo=false skia_use_system_zlib=false skia_use_system_expat=false skia_use_system_icu=true'

echo "==> gn gen $build_dir"
mkdir -p "$build_dir"
# gn locates the source root by walking up from the current directory.
( cd "$root" && "$gn" gen "$build_dir" --args="$args" )

echo "==> ninja -C $build_dir"
ninja -C "$build_dir" skia \
  modules/skparagraph:skparagraph modules/skshaper:skshaper \
  modules/skunicode:skunicode_core modules/skunicode:skunicode_icu

missing=''
for library in $LIBRARIES; do
  [ -f "$build_dir/$library" ] || missing="$missing $library"
done
if [ -n "$missing" ]; then
  echo "build-skia: finished without$missing in $build_dir" >&2
  exit 1
fi

echo "==> Skia $SKIA_REVISION ready in $build_dir"
