#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Unpack the pinned TRX64 SDK (sdk.pin) into software/trx64u/.sdk/ (U18).
#   software/trx64u/fetch_sdk.sh <path to trx64-sdk-<version>.tar.gz>
# The tarball is checked against sdk_tarball_sha256 first; a different file is refused.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
VER="$(pin sdk_version)"
SUM="$(pin sdk_tarball_sha256)"
TAR="${1:?usage: fetch_sdk.sh <path to $(pin sdk_tarball)>}"
GOT="$(shasum -a 256 "$TAR" 2>/dev/null || sha256sum "$TAR")"
GOT="${GOT%% *}"
if [ "$GOT" != "$SUM" ]; then
    echo "fetch_sdk.sh: $TAR has sha256 $GOT, sdk.pin wants $SUM" >&2
    exit 1
fi
rm -rf "$HERE/.sdk/trx64-sdk-$VER"
mkdir -p "$HERE/.sdk"
tar -xzf "$TAR" -C "$HERE/.sdk"
test -f "$HERE/.sdk/trx64-sdk-$VER/VERSION"
echo "SDK $VER in $HERE/.sdk/trx64-sdk-$VER"
