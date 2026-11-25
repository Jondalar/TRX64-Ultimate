#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Unpack the pinned TRX64 SDK (sdk.pin) into software/trx64u/.sdk/ (U18).
#   software/trx64u/fetch_sdk.sh <path to the tarball sdk.pin names>
#   software/trx64u/fetch_sdk.sh --download      (our registry's copy, sdk.pin's sdk_package; the server is GITEA_URL,
#                                                  which CI sets to its own server, else the url: of the tea login)
#   software/trx64u/fetch_sdk.sh --release       (the release sdk.pin's sdk_release of the builds repository, U20 D11: tools/builds.sh fetch checks
#                                                  the asset against the SHA-256 GitHub lists and the release's SHA256SUMS; GitHub's runners)
# The tarball is checked against its sha256 in sdk.pin first; a different file is refused. It unpacks to trx64-sdk-<sdk_version>/
# whatever its own name (some dev builds carry their commit in it, 0.3.0 and the 0.4.x do not); its VERSION must match the pin (build.sh --check).
# Only the tarball: the online sub-app is built here from apps/online (build.sh, U20 D22), no longer taken from the SDK's release.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
sha() { s="$(shasum -a 256 "$1" 2>/dev/null || sha256sum "$1")"; echo "${s%% *}"; }
VER="$(pin sdk_version)"
SUM="$(pin sdk_tarball_sha256)"
DIR="trx64-sdk-$VER"
TAR="${1:?usage: fetch_sdk.sh <path to $(pin sdk_tarball)> | --download | --release}"
[ $# -le 1 ] || { echo "usage: fetch_sdk.sh <path to $(pin sdk_tarball)> | --download | --release (online.u2a is built by build.sh, U20 D22)" >&2; exit 2; }
if [ "$TAR" = --download ]; then
    mkdir -p "$HERE/.sdk"
    # the Gitea server: GITEA_URL, else the url: of the tea login (tea's config on macOS, then on Linux); the public tree names no host (U20 A8)
    SRV="${GITEA_URL:-}"
    for c in "$HOME/Library/Application Support/tea/config.yml" "$HOME/.config/tea/config.yml"; do
        [ -n "$SRV" ] || [ ! -f "$c" ] || SRV="$(awk '/^ *url:/ { print $2; exit }' "$c")"
    done
    [ -n "$SRV" ] || { echo "fetch_sdk.sh --download: no Gitea server: set GITEA_URL (or log in with tea)" >&2; exit 1; }
    REG="${SRV%/}/api/packages/TREX/generic"
    TAR="$HERE/.sdk/$(pin sdk_tarball)"
    curl -sfL -o "$TAR" "$REG/$(pin sdk_package)/$(pin sdk_tarball)" || {
        echo "fetch_sdk.sh: cannot download $(pin sdk_package)/$(pin sdk_tarball)" >&2; exit 1; }
elif [ "$TAR" = --release ]; then
    mkdir -p "$HERE/.sdk/release"
    REL="$(pin sdk_release)"
    [ -n "$REL" ] || { echo "fetch_sdk.sh: sdk.pin names no sdk_release" >&2; exit 1; }
    TAR="$HERE/.sdk/release/$(pin sdk_tarball)"
    # a tarball the job's cache restored with the pinned hash is not fetched again (the checks below run on it as on a download)
    if ! { [ -f "$TAR" ] && [ "$(sha "$TAR")" = "$SUM" ]; }; then
        bash "$HERE/../../tools/builds.sh" fetch "$REL" "$(pin sdk_tarball)" "$TAR" || {
            echo "fetch_sdk.sh: cannot fetch $(pin sdk_tarball) of the release $REL" >&2; exit 1; }
    fi
fi
GOT="$(sha "$TAR")"
if [ "$GOT" != "$SUM" ]; then
    echo "fetch_sdk.sh: $TAR has sha256 $GOT, sdk.pin wants $SUM" >&2
    exit 1
fi
TOP="$(tar -tzf "$TAR" | sed -n '1s,/.*,,p')"
if [ "$TOP" != "$DIR" ]; then
    echo "fetch_sdk.sh: $TAR unpacks to $TOP/, sdk.pin's sdk_version wants $DIR/" >&2
    exit 1
fi
rm -rf "$HERE/.sdk/$DIR"
mkdir -p "$HERE/.sdk"
tar -xzf "$TAR" -C "$HERE/.sdk"
sh "$HERE/build.sh" --check
echo "SDK $VER ($(pin source_commit)) in $HERE/.sdk/$DIR"
