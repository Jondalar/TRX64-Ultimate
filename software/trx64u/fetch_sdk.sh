#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Unpack the pinned TRX64 SDK (sdk.pin) into software/trx64u/.sdk/ and put the pinned online sub-app beside it (U18).
#   software/trx64u/fetch_sdk.sh <path to the tarball sdk.pin names> [<path to online.u2a>]
#   software/trx64u/fetch_sdk.sh --download      (our registry's copies, sdk.pin's sdk_package and online_package; GITEA_URL, default
#                                                  http://gitea.example, CI passes its server)
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
TAR="${1:?usage: fetch_sdk.sh <path to $(pin sdk_tarball)> [<path to $(pin online_u2a)>] | --download}"
ONL="${2:-}"
if [ "$TAR" = --download ]; then
    mkdir -p "$HERE/.sdk"
    REG="${GITEA_URL:-http://gitea.example}/api/packages/TREX/generic"
    TAR="$HERE/.sdk/$(pin sdk_tarball)"
    curl -sfL -o "$TAR" "$REG/$(pin sdk_package)/$(pin sdk_tarball)" || {
        echo "fetch_sdk.sh: cannot download $(pin sdk_package)/$(pin sdk_tarball)" >&2; exit 1; }
    ONL="$HERE/.sdk/download-$(pin online_u2a)"
    curl -sfL -o "$ONL" "$REG/$(pin online_package)/$(pin online_u2a)" || {
        echo "fetch_sdk.sh: cannot download $(pin online_package)/$(pin online_u2a)" >&2; exit 1; }
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
