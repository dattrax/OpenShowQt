#!/usr/bin/env bash
# Prepare the PINNED font corpus the TS.5 raster/performance fixtures render
# through (tests/fixtures/fonts/manifest.json).
#
# This is a developer step, run by hand. Nothing here runs inside ctest and
# the test suite never downloads: font_fixture_tests resolves the same cache
# directory, re-checks every sha256 in the manifest itself, and reports a
# visible skip when the corpus has not been prepared.
#
#   tools/setup-font-fixtures.sh prepare [--cache DIR] (--download | --from DIR) [--force]
#   tools/setup-font-fixtures.sh verify  [--cache DIR]
#   tools/setup-font-fixtures.sh status  [--cache DIR]
#
# --download   fetch from the pinned google/fonts revision (needs network)
# --from DIR   copy from a directory you prepared elsewhere, offline. Both the
#              flat layout (Inter[opsz,wght].ttf, Inter-OFL.txt) and the
#              upstream layout (ofl/inter/Inter[opsz,wght].ttf, ofl/inter/OFL.txt)
#              are found.
# --cache DIR  where the corpus lives. Default, in order: $OPENSHOW_FONT_FIXTURE_DIR,
#              then ${XDG_CACHE_HOME:-$HOME/.cache}/openshow/font-fixtures.
#
# The fonts are pinned because a raster fixture that measures "whatever
# fontconfig found today" measures nothing: family resolution is asserted
# before pixels, and a face that silently changed would change every number
# below it. Each family ships under the SIL Open Font License 1.1; the licence
# text is fetched and hash-checked with the font. See
# tests/fixtures/fonts/README.md for why the corpus is cached rather than
# committed, and docs/fixtures/fonts.md for what the fixtures prove.

set -euo pipefail

REPO_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
MANIFEST="$REPO_ROOT/tests/fixtures/fonts/manifest.json"

usage() {
    sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

command="${1:-}"
case "$command" in
    prepare | verify | status) shift ;;
    -h | --help | "") usage; exit 0 ;;
    *) printf 'setup-font-fixtures: unknown command "%s"\n\n' "$command" >&2; usage >&2; exit 2 ;;
esac

CACHE_DIR="${OPENSHOW_FONT_FIXTURE_DIR:-}"
SOURCE_DIR=""
DOWNLOAD=0
FORCE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cache) CACHE_DIR="${2:-}"; shift 2 ;;
        --from) SOURCE_DIR="${2:-}"; shift 2 ;;
        --download) DOWNLOAD=1; shift ;;
        --force) FORCE=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) printf 'setup-font-fixtures: unknown option "%s"\n' "$1" >&2; exit 2 ;;
    esac
done

if [[ -z "$CACHE_DIR" ]]; then
    CACHE_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/openshow/font-fixtures"
fi
if [[ ! -f "$MANIFEST" ]]; then
    printf 'setup-font-fixtures: no manifest at %s\n' "$MANIFEST" >&2
    exit 1
fi
if ! command -v sha256sum >/dev/null 2>&1; then
    printf 'setup-font-fixtures: sha256sum is required\n' >&2
    exit 1
fi

# One manifest row per line, tab separated, in the fixed column order the
# loop below reads. Parsing here rather than with grep keeps the manifest
# free of quoting rules a shell would have to guess at.
rows() {
    python3 - "$MANIFEST" <<'PY'
import json, sys
manifest = json.load(open(sys.argv[1]))
up = manifest["upstream"]
template = up["urlTemplate"]
for entry in manifest["fonts"]:
    print("\t".join([
        entry["role"], entry["family"], entry["file"], entry["sha256"],
        str(entry["bytes"]), entry["upstreamPath"], entry["licenceFile"],
        entry["licenceSha256"], str(entry["licenceBytes"]),
        entry["licenceUpstreamPath"],
        template.format(revision=up["revision"], path=entry["upstreamPath"]),
        template.format(revision=up["revision"], path=entry["licenceUpstreamPath"]),
    ]))
PY
}

# The bytes we have, or empty when we have nothing readable.
hash_of() { [[ -f "$1" ]] && sha256sum "$1" | cut -d' ' -f1 || true; }

# A source file may be laid out flat or like the upstream tree.
find_source() {
    local dir="$1"
    shift
    for candidate in "$@"; do
        if [[ -f "$dir/$candidate" ]]; then
            printf '%s' "$dir/$candidate"
            return 0
        fi
    done
    return 1
}

fetch() { # fetch URL DEST
    local url="$1" dest="$2" part="$dest.part"
    command -v curl >/dev/null 2>&1 || {
        printf 'setup-font-fixtures: --download needs curl (or use --from DIR)\n' >&2
        return 1
    }
    # --globoff because these font names carry [opsz,wght] brackets, which
    # curl would otherwise read as a range glob.
    if ! curl --fail --globoff --silent --show-error --location --proto '=https' --tlsv1.2 \
        --max-time 300 --output "$part" "$url"; then
        rm -f "$part"
        return 1
    fi
    mv -f "$part" "$dest"
}

place() { # place WANT_SHA BYTES DEST (fills DEST from --from or --download)
    local sha="$1" bytes="$2" dest="$3" upstream="$4" url="$5" src
    if [[ -f "$dest" && "$FORCE" -ne 1 ]]; then
        if [[ "$(hash_of "$dest")" == "$sha" ]]; then
            printf '  ok      %s (already in the cache)\n' "$(basename "$dest")"
            return 0
        fi
        printf '  stale   %s has the wrong bytes; refetching\n' "$(basename "$dest")"
    fi
    if [[ -n "$SOURCE_DIR" ]]; then
        # `OFL.txt` is the same NAME in every family directory, so the
        # upstream path is what distinguishes one licence from another.
        if ! src=$(find_source "$SOURCE_DIR" "$(basename "$dest")" "$upstream"); then
            printf '  missing %s is not under --from %s\n' "$(basename "$dest")" "$SOURCE_DIR" >&2
            return 1
        fi
        install -m 0644 "$src" "$dest"
    elif [[ "$DOWNLOAD" -eq 1 ]]; then
        printf '  fetch   %s\n' "$url"
        fetch "$url" "$dest"
    else
        printf '  missing %s is not in the cache.\n' "$(basename "$dest")" >&2
        printf '          Re-run with --download (needs network) or --from DIR.\n' >&2
        return 1
    fi
    if [[ ! -f "$dest" ]]; then
        printf '  BAD     nothing was written for %s\n' "$(basename "$dest")" >&2
        return 1
    fi
    if [[ "$(hash_of "$dest")" != "$sha" ]]; then
        printf '  BAD     %s does not match the pinned sha256\n' "$(basename "$dest")" >&2
        rm -f "$dest"
        return 1
    fi
    if [[ "$(stat -c%s "$dest" 2>/dev/null || stat -f%z "$dest")" != "$bytes" ]]; then
        printf '  BAD     %s is not the pinned size\n' "$(basename "$dest")" >&2
        rm -f "$dest"
        return 1
    fi
    printf '  placed  %s\n' "$(basename "$dest")"
}

status_line() { # status_line SHA DEST
    local sha="$1" dest="$2" have
    have=$(hash_of "$dest")
    if [[ -z "$have" ]]; then
        printf '  absent  %s\n' "$(basename "$dest")"
    elif [[ "$have" == "$sha" ]]; then
        printf '  ok      %s\n' "$(basename "$dest")"
    else
        printf '  BAD     %s (sha256 %s, pinned %s)\n' "$(basename "$dest")" "${have:0:12}" "${sha:0:12}"
    fi
}

revision=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["upstream"]["revision"])' "$MANIFEST")
printf 'corpus: %s\n' "$CACHE_DIR"
printf 'upstream: google/fonts %s\n' "$revision"

exit_code=0
count=0
while IFS=$'\t' read -r role family file sha bytes upstream licence licenceSha licenceBytes licenceUpstream fontUrl licenceUrl; do
    printf '%-10s %s\n' "$role" "$family"
    case "$command" in
        prepare)
            mkdir -p "$CACHE_DIR"
            place "$sha" "$bytes" "$CACHE_DIR/$file" "$upstream" "$fontUrl" || exit_code=1
            place "$licenceSha" "$licenceBytes" "$CACHE_DIR/$licence" "$licenceUpstream" "$licenceUrl" || exit_code=1
            ;;
        verify | status) status_line "$sha" "$CACHE_DIR/$file" ;;
    esac
    count=$((count + 1))
done < <(rows)

if [[ "$command" == "prepare" ]]; then
    if [[ $exit_code -ne 0 ]]; then
        printf '\nNOT READY - fix the lines above and run prepare again.\n'
        exit "$exit_code"
    fi
    manifest_sha=$(sha256sum "$MANIFEST" | cut -d' ' -f1)
    python3 - "$CACHE_DIR/corpus.json" "$MANIFEST" "$manifest_sha" "$revision" <<'PY'
import json, sys
out, manifest, manifest_sha, revision = sys.argv[1:5]
entry = json.load(open(manifest))
entry["prepared"] = {"manifestSha256": manifest_sha, "cacheFormat": 1}
json.dump(entry, open(out, "w"), indent=2, sort_keys=True)
open(out, "a").write("\n")
PY
    printf '\nREADY - %s families verified in %s\n' "$count" "$CACHE_DIR"
    printf 'ctest will now run font_fixture_tests against these bytes.\n'
else
    echo
    printf 'verify: %s entries checked in %s\n' "$count" "$CACHE_DIR"
    if [[ "$command" == "verify" ]]; then
        while IFS=$'\t' read -r role family file sha bytes upstream licence licenceSha licenceBytes licenceUpstream fontUrl licenceUrl; do
            [[ -f "$CACHE_DIR/$file" ]] || exit_code=1
            [[ -f "$CACHE_DIR/$licence" ]] || exit_code=1
            [[ -f "$CACHE_DIR/$file" && "$(hash_of "$CACHE_DIR/$file")" == "$sha" ]] || exit_code=1
            [[ -f "$CACHE_DIR/$licence" && "$(hash_of "$CACHE_DIR/$licence")" == "$licenceSha" ]] || exit_code=1
        done < <(rows)
    fi
    exit "$exit_code"
fi
