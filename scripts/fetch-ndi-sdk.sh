#!/usr/bin/env bash
# Fetch and unpack the proprietary NDI SDK for OpenShowQt's optional helper.
#
# The SDK is NOT committed and NOT a git submodule. You must obtain the NDI SDK
# from <https://ndi.video/> and accept the NDI SDK Licence Agreement yourself.
# This script only unpacks a copy you already downloaded. It refuses to accept
# the licence on your behalf unless you pass --accept-eula explicitly.
#
# Usage:
#   scripts/fetch-ndi-sdk.sh [ARCHIVE_OR_SDK_DIR] [--force] [--accept-eula]
#
# ARCHIVE_OR_SDK_DIR may be:
#   - the NDI Linux .tar.gz download (it contains Install_NDI_SDK_*.sh), or
#   - an already-extracted "NDI SDK for Linux" directory, or
#   - omitted, in which case NDI_SDK_ARCHIVE is used.
#
# The SDK is unpacked to third_party/ndi-sdk (gitignored). Existing content is
# kept unless --force is passed. At the end the script prints the
# OPENSHOW_NDI_SDK_DIR value to use when building tools/ndi-helper.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${ROOT}/third_party/ndi-sdk"
FORCE=0
ACCEPT=0
INPUT="${NDI_SDK_ARCHIVE:-}"

usage() {
    sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
    case "${arg}" in
        --force) FORCE=1 ;;
        --accept-eula) ACCEPT=1 ;;
        -h|--help) usage; exit 0 ;;
        -*)
            echo "fetch-ndi-sdk: unknown option '${arg}'" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -n "${INPUT}" ]]; then
                echo "fetch-ndi-sdk: more than one input given" >&2
                exit 2
            fi
            INPUT="${arg}"
            ;;
    esac
done

if [[ -z "${INPUT}" ]]; then
    cat >&2 <<'EOF'
fetch-ndi-sdk: no NDI SDK archive or directory given.

Download the NDI SDK for Linux from <https://ndi.video/>, accept the NDI SDK
Licence Agreement there, then run:

  scripts/fetch-ndi-sdk.sh /path/to/Install_NDI_SDK_v6_Linux.tar.gz --accept-eula

You may also set NDI_SDK_ARCHIVE to the download path. The SDK is proprietary;
it is not redistributed by this repository.
EOF
    exit 2
fi

if [[ ! -e "${INPUT}" ]]; then
    echo "fetch-ndi-sdk: '${INPUT}' does not exist" >&2
    exit 2
fi

if [[ -e "${TARGET}" && "${FORCE}" -ne 1 ]]; then
    echo "fetch-ndi-sdk: '${TARGET}' already exists; pass --force to replace it" >&2
    exit 2
fi

if [[ "${ACCEPT}" -ne 1 ]]; then
    if [[ -t 0 ]]; then
        echo "The NDI SDK is proprietary. Read the NDI SDK Licence Agreement at"
        echo "<https://ndi.video/> before continuing."
        printf "Type 'yes' to confirm you have read and accept the NDI SDK licence: "
        read -r reply
        if [[ "${reply}" != "yes" ]]; then
            echo "fetch-ndi-sdk: licence not accepted; nothing changed" >&2
            exit 2
        fi
    else
        echo "fetch-ndi-sdk: refusing to accept the NDI SDK licence on your behalf" >&2
        echo "                in a non-interactive shell; pass --accept-eula after" >&2
        echo "                reading the licence at <https://ndi.video/>" >&2
        exit 2
    fi
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/openshow-ndi-sdk.XXXXXX")"
cleanup() {
    rm -rf "${WORK}"
}
trap cleanup EXIT

sdk_dir=""

if [[ -d "${INPUT}" ]]; then
    sdk_dir="${INPUT}"
else
    case "${INPUT}" in
        *.tar.gz|*.tgz)
            mkdir -p "${WORK}/archive"
            tar -xzf "${INPUT}" -C "${WORK}/archive"
            ;;
        *)
            echo "fetch-ndi-sdk: expected a .tar.gz/.tgz or an extracted SDK directory" >&2
            exit 2
            ;;
    esac

    installer="$(find "${WORK}/archive" -maxdepth 2 -type f -name 'Install_NDI_SDK_*.sh' | head -n 1)"
    if [[ -z "${installer}" ]]; then
        # Some downloads are the installer script itself, not a tarball.
        if [[ -f "${INPUT}" && "${INPUT}" == *.sh ]]; then
            installer="${INPUT}"
        else
            echo "fetch-ndi-sdk: no Install_NDI_SDK_*.sh found in the archive" >&2
            exit 2
        fi
    fi

    mkdir -p "${WORK}/install"
    # The installer prints the EULA and asks for "y". We already required an
    # explicit acceptance above; feed the answer and keep the pager quiet.
    ( cd "${WORK}/install" && PAGER=cat sh "${installer}" <<< "y" >/dev/null 2>&1 ) || {
        echo "fetch-ndi-sdk: the NDI installer failed" >&2
        exit 3
    }
    sdk_dir="$(find "${WORK}/install" -maxdepth 2 -type d -name 'NDI SDK for Linux' | head -n 1)"
    if [[ -z "${sdk_dir}" ]]; then
        sdk_dir="$(find "${WORK}/install" -maxdepth 3 -type f -name 'Processing.NDI.Lib.h' -printf '%h\n' 2>/dev/null | head -n 1)"
        if [[ -n "${sdk_dir}" ]]; then
            sdk_dir="$(cd "${sdk_dir}/../.." && pwd)"
        fi
    fi
    if [[ -z "${sdk_dir}" ]]; then
        echo "fetch-ndi-sdk: the installer ran but no SDK directory was found" >&2
        exit 3
    fi
fi

if [[ ! -f "${sdk_dir}/include/Processing.NDI.Lib.h" ]]; then
    echo "fetch-ndi-sdk: '${sdk_dir}' does not look like an NDI SDK" >&2
    echo "               (missing include/Processing.NDI.Lib.h)" >&2
    exit 3
fi

rm -rf "${TARGET}"
mkdir -p "$(dirname "${TARGET}")"
cp -a "${sdk_dir}" "${TARGET}"

cat <<EOF
NDI SDK installed at: ${TARGET}

Build the optional helper with:

  cmake -S tools/ndi-helper -B build-ndi-helper \\
    -DOPENSHOW_NDI_SDK_DIR="${TARGET}"
  cmake --build build-ndi-helper
  build-ndi-helper/ndi-helper --probe

The SDK and its runtime are gitignored. Do not commit them.
EOF
