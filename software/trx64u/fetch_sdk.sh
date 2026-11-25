#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Unpack the pinned TRX64 SDK (sdk.pin) into software/trx64u/.sdk/ and put the pinned online sub-app beside it (U18).
#   software/trx64u/fetch_sdk.sh <path to the tarball sdk.pin names> [<path to online.u2a>]
#   software/trx64u/fetch_sdk.sh --download      (our registry's copies, sdk.pin's sdk_package and online_package; the server is GITEA_URL,
#                                                  which CI sets to its own server, else the url: of the tea login)
#   software/trx64u/fetch_sdk.sh --release       (the release sdk.pin's sdk_release of the builds repository, U20 D11: tools/builds.sh fetch checks
#                                                  each asset against the SHA-256 GitHub lists and the release's SHA256SUMS; GitHub's runners)
# Each file is checked against its sha256 in sdk.pin first; a different file is refused. The tarball unpacks to trx64-sdk-<sdk_version>/
# whatever its own name (some dev builds carry their commit in it, 0.3.0 and the 0.4.x do not); its VERSION must match the pin (build.sh --check).
# online.u2a (sdk.pin online_*: MultiCore's online sub-app of the same t239.3 commit, not part of the tarball) goes to .sdk/online.u2a, where
# pack_cor.sh takes it for record 7 and checks it again; with a local tarball and no second argument only the SDK is unpacked.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
sha() { s="$(shasum -a 256 "$1" 2>/dev/null || sha256sum "$1")"; echo "${s%% *}"; }
VER="$(pin sdk_version)"
SUM="$(pin sdk_tarball_sha256)"
DIR="trx64-sdk-$VER"
TAR="${1:?usage: fetch_sdk.sh <path to $(pin sdk_tarball)> [<path to $(pin online_u2a)>] | --download | --release}"
ONL="${2:-}"
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
    ONL="$HERE/.sdk/download-$(pin online_u2a)"
    curl -sfL -o "$ONL" "$REG/$(pin online_package)/$(pin online_u2a)" || {
        echo "fetch_sdk.sh: cannot download $(pin online_package)/$(pin online_u2a)" >&2; exit 1; }
elif [ "$TAR" = --release ]; then
    mkdir -p "$HERE/.sdk/release"
    REL="$(pin sdk_release)"
    [ -n "$REL" ] || { echo "fetch_sdk.sh: sdk.pin names no sdk_release" >&2; exit 1; }
    for fs in "$(pin sdk_tarball) $(pin sdk_tarball_sha256)" "$(pin online_u2a) $(pin online_u2a_sha256)"; do
        f="${fs% *}"
        # a file the job's cache restored with the pinned hash is not fetched again (the checks below run on it as on a download)
        [ -f "$HERE/.sdk/release/$f" ] && [ "$(sha "$HERE/.sdk/release/$f")" = "${fs#* }" ] && continue
        bash "$HERE/../../tools/builds.sh" fetch "$REL" "$f" "$HERE/.sdk/release/$f" || {
            echo "fetch_sdk.sh: cannot fetch $f of the release $REL" >&2; exit 1; }
    done
    TAR="$HERE/.sdk/release/$(pin sdk_tarball)"
    ONL="$HERE/.sdk/release/$(pin online_u2a)"
fi
GOT="$(sha "$TAR")"
if [ "$GOT" != "$SUM" ]; then
    echo "fetch_sdk.sh: $TAR has sha256 $GOT, sdk.pin wants $SUM" >&2
    exit 1
fi
if [ -n "$ONL" ]; then
    GOT="$(sha "$ONL")"
    if [ "$GOT" != "$(pin online_u2a_sha256)" ]; then
        echo "fetch_sdk.sh: $ONL has sha256 $GOT, sdk.pin wants $(pin online_u2a_sha256)" >&2
        exit 1
    fi
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
if [ -n "$ONL" ]; then
    if ! [ "$ONL" -ef "$HERE/.sdk/$(pin online_u2a)" ]; then   # the same file by another path (a relative one) is neither removed nor copied
        rm -f "$HERE/.sdk/$(pin online_u2a)"                 # an earlier copy can be read only (MultiCore's .ci/sdk/ files are)
        cp "$ONL" "$HERE/.sdk/$(pin online_u2a)"
    fi
    echo "online sub-app $(pin online_package) ($(pin online_source_commit)) in $HERE/.sdk/$(pin online_u2a)"
fi
