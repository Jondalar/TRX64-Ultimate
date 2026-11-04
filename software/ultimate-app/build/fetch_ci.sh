#!/bin/bash
# Fetch what the NAS CI built for a branch: update.u64, update.ue2, update.cfw
# and the .u2a apps, and optionally put them on a device's USB stick.
#
#   build/fetch_ci.sh [branch|version] [--to HOST[:/USB1]]
#
# branch     defaults to the current one; its <branch>-latest version is fetched
# version    a full package version such as trex_main-b7efb36c fetches that build
# --to       also uploads the files over FTP into <volume>/<version>/ on HOST
# WITH_ZIP=1 also fetches the release bundle 1541ultimate-<version>.zip
#
# The CI publishes them as the generic package 1541ultimate-fw in the Gitea
# registry (.gitea/workflows/build.yml). The U64-II files and the apps only
# appear once the job's emulator suites have passed.

set -eu

GITEA="${GITEA_URL:-http://gitea.fritz.box}"
OWNER="${GITEA_OWNER:-TREX}"
TEA_CONFIG="${TEA_CONFIG:-$HOME/Library/Application Support/tea/config.yml}"
TOKEN="${GITEA_TOKEN:-$(awk '/token:/{print $2; exit}' "$TEA_CONFIG" 2>/dev/null || true)}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

what=""
target=""
while [ $# -gt 0 ]; do
    case "$1" in
        --to) target="$2"; shift 2 ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) what="$1"; shift ;;
    esac
done
[ -n "$what" ] || what="$(git -C "$ROOT" rev-parse --abbrev-ref HEAD)"
version="$(printf '%s' "$what" | tr '/' '_')"
case "$version" in
    *-latest|*-[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
    *) version="$version-latest" ;;
esac
[ -n "$TOKEN" ] || { echo "no token: set GITEA_TOKEN or log in with tea" >&2; exit 1; }
auth=(-H "Authorization: token $TOKEN")

files="$(curl -sf "${auth[@]}" "$GITEA/api/v1/packages/$OWNER/generic/1541ultimate-fw/$version/files" \
    | python3 -c 'import json,sys; print("\n".join(f["name"] for f in json.load(sys.stdin)))')" || {
    echo "no CI outputs for $version in $GITEA" >&2
    exit 1
}
out="$ROOT/ci-out/$version"
rm -rf "$out"
mkdir -p "$out"
for f in $files; do
    case "$f" in *.zip) [ -n "${WITH_ZIP:-}" ] || continue ;; esac
    curl -sf "${auth[@]}" -o "$out/$f" "$GITEA/api/packages/$OWNER/generic/1541ultimate-fw/$version/$f"
done
(cd "$out" && for sums in SHA256SUMS.*; do shasum -a 256 -c --quiet "$sums"; done)
echo "$version -> $out"
ls -l "$out"

if [ -n "$target" ]; then
    host="${target%%:*}"
    volume="/USB1"
    [ "$host" != "$target" ] && volume="${target#*:}"
    remote="$volume/$version"
    for f in "$out"/*; do
        case "$f" in *SHA256SUMS*) continue ;; esac
        curl -s --ftp-create-dirs -T "$f" "ftp://$host$remote/"
    done
    echo "uploaded to ftp://$host$remote/"
fi
