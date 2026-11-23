#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build trx64u's libraries against the pinned TRX64 SDK (U18; sdk.pin, fetch_sdk.sh first).
#   software/trx64u/build.sh [trx64u] [ring] [monitor]      (default: all three)
#   software/trx64u/build.sh --check                         (only check the unpacked SDK against sdk.pin)
# The SDK is .sdk/trx64-sdk-<sdk_version>/; its VERSION must name the pinned commit, base API and hashes (a dev build
# and the release of the same version unpack to the same folder).
# Each library is built in build/<name>/ from copies of its sources (lib.mk writes its objects next to the sources,
# and the monitor's engine is trxmon's own folder in the application submodule, D4). In the build image
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
# VERSION's keys against the pin's: sdk, base_api, abi_major, api_headers_sha256, commit, coreapp_sha256.
for kv in "sdk sdk_version" "base_api base_api" "abi_major abi_major" "api_headers_sha256 api_headers_sha256" \
          "commit source_commit" "coreapp_sha256 coreapp_sha256"; do
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
APP="$ROOT/software/ultimate-app"
TRXMON="$APP/apps/trxmon/src"
LIBS="${*:-trx64u ring monitor}"
rc=0
for lib in $LIBS; do
    case "$lib" in
        trx64u) src=base ;;
        ring|monitor) src="$lib" ;;
        *) echo "build.sh: no library $lib" >&2; exit 1 ;;
    esac
    B="$HERE/build/$lib"
    mkdir -p "$B"
    find "$HERE/$src" -maxdepth 1 -type f -exec cp -p {} "$B/" \;
    if [ "$lib" = monitor ]; then
        mkdir -p "$B/trxmon"
        for f in "$TRXMON"/*.c "$TRXMON"/*.h "$APP/software/io/c64/trx_lens.c" "$APP/software/io/c64/trx_lens.h"; do
            case "$(basename "$f")" in main.c|libc_min.c) continue ;; esac
            cp -p "$f" "$B/trxmon/"
        done
    fi
    echo "== $lib"
    make -C "$B" SDK="$SDK" APPSDK="$APP/software/appsdk/include" APPLOADER="$APP/software/app_loader" \
        RV_PREFIX=riscv32-unknown-elf- || rc=1
done
exit $rc
