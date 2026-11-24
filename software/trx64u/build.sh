#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build trx64u's libraries and sub-apps against the pinned TRX64 SDK (U18; sdk.pin, fetch_sdk.sh first).
#   software/trx64u/build.sh [trx64u] [ring] [monitor] [trex_crt]                 (default: all four)
#   software/trx64u/build.sh --check                                               (only check the unpacked SDK against sdk.pin)
# The SDK is .sdk/trx64-sdk-<sdk_version>/; its VERSION must name the pinned commit, base API and hashes (a dev build
# and the release of the same version unpack to the same folder).
# Each library is built in build/<name>/ from copies of its sources (lib.mk writes its objects next to the sources); the monitor also gets
# trxmon/, the monitor's engine (U20 A2: moved from the firmware fork, this tree's only copy). The monitor and the ring library read the
# firmware's application ABI in fwabi/ (FWABI). The sub-app TREX CRT (apps/trex_crt, MultiCore T239.3
# P2-5) is built the same way with the SDK's mk/app.mk, its folder copied whole (its C sources are in src/). Softboot is not built here (Alex,
# 2026-11-24): it starts the firmware application of a .cfw on the running bitstream, and c64_basic under this app is not the bitstream that
# application needs; it stays in the firmware build. The driver table of c64_basic's
# TOML A (gateware/c64_basic.toml) is made there by the SDK's descrec.py (api.md 6.17, SDK 0.4.0 and later). In the build image
# (riscv32-unknown-elf-g++ on PATH) it runs make directly; elsewhere it runs itself in the u64-build-env container.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
VER="$(pin sdk_version)"
SDK="$HERE/.sdk/trx64-sdk-$VER"
if [ ! -f "$SDK/VERSION" ]; then
    echo "build.sh: no SDK $VER in $HERE/.sdk: run fetch_sdk.sh <tarball> first" >&2
    exit 1
fi
# VERSION's keys against the pin's: sdk, base_api, abi_major, api_headers_sha256, commit, base_archive_sha256.
for kv in "sdk sdk_version" "base_api base_api" "abi_major abi_major" "api_headers_sha256 api_headers_sha256" \
          "commit source_commit" "base_archive_sha256 base_archive_sha256"; do
    k="${kv% *}"; p="${kv#* }"
    have="$(sed -n "s/^$k //p" "$SDK/VERSION")"
    if [ "$have" != "$(pin "$p")" ]; then
        echo "build.sh: $SDK/VERSION has $k '$have', sdk.pin's $p is '$(pin "$p")': run fetch_sdk.sh <tarball>" >&2
        exit 1
    fi
done
if [ "${1:-}" = --check ]; then
    exit 0
fi
if ! command -v riscv32-unknown-elf-g++ >/dev/null 2>&1; then
    exec container run --rm --name "trx64u-build-$$" --rosetta --arch amd64 -v "$ROOT:/trx" -w /trx u64-build-env \
        sh /trx/software/trx64u/build.sh "$@"
fi
LIBS="${*:-trx64u ring monitor trex_crt}"
rc=0
for lib in $LIBS; do
    case "$lib" in
        trx64u) src=base ;;
        ring|monitor) src="$lib" ;;
        trex_crt) src="apps/$lib" ;;
        *) echo "build.sh: no library or sub-app $lib" >&2; exit 1 ;;
    esac
    B="$HERE/build/$lib"
    # Objects of another SDK would stay: make takes them as up to date, as their .d files name that SDK's headers, which stay in .sdk/ (U18 T-3,
    # 2026-11-24: the first builds after a pin change linked objects of 0.4.1). A build folder made with another SDK tarball starts clean.
    [ "$(cat "$B/.sdk" 2>/dev/null)" = "$(pin sdk_tarball_sha256)" ] || rm -rf "$B"
    # lib.mk's .u2a depends on the .elf and the menu only, not on the Makefile's budget, needs, version or exports: a library is linked and packed again
    # each time; app.mk tracks no header either (no .d files), so a sub-app is built from a clean copy each time
    case "$src" in apps/*) rm -rf "$B" ;; *) rm -f "$B/$lib.elf" "$B/$lib.u2a" ;; esac
    mkdir -p "$B"
    pin sdk_tarball_sha256 > "$B/.sdk"
    case "$src" in
        apps/*) cp -Rp "$HERE/$src/." "$B/" ;;
        *) find "$HERE/$src" -maxdepth 1 -type f -exec cp -p {} "$B/" \; ;;
    esac
    if [ "$lib" = monitor ]; then
        mkdir -p "$B/trxmon"
        cp -p "$HERE"/trxmon/*.c "$HERE"/trxmon/*.h "$B/trxmon/"
    fi
    echo "== $lib"
    make -C "$B" SDK="$SDK" FWABI="$HERE/fwabi" GATEWARE="$HERE/gateware" RV_PREFIX=riscv32-unknown-elf- || rc=1
done
exit $rc
