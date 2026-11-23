#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Unpack the pinned TRX64 SDK (sdk.pin) into software/trx64u/.sdk/ (U18).
#   software/trx64u/fetch_sdk.sh <path to the tarball sdk.pin names>
# The tarball is checked against sdk_tarball_sha256 first; a different file is refused. It unpacks to
# trx64-sdk-<sdk_version>/ whatever the tarball's own name (a dev build's name carries its commit); its VERSION must
# match the pin.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
VER="$(pin sdk_version)"
SUM="$(pin sdk_tarball_sha256)"
DIR="trx64-sdk-$VER"
TAR="${1:?usage: fetch_sdk.sh <path to $(pin sdk_tarball)>}"
GOT="$(shasum -a 256 "$TAR" 2>/dev/null || sha256sum "$TAR")"
GOT="${GOT%% *}"
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
