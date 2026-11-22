#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build trx64u's libraries against the pinned TRX64 SDK (U18; sdk.pin, fetch_sdk.sh first).
#   software/trx64u/build.sh [trx64u] [ring] [monitor]      (default: all three)
# Each library is built in build/<name>/ from copies of its sources (lib.mk writes its objects next to the sources,
# and the monitor's engine is trxmon's own folder in the application submodule, D4). In the build image
# (riscv32-unknown-elf-g++ on PATH) it runs make directly; elsewhere it runs itself in the u64-build-env container.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
VER="$(sed -n 's/^sdk_version=//p' "$HERE/sdk.pin")"
SDK="$HERE/.sdk/trx64-sdk-$VER"
if [ ! -f "$SDK/VERSION" ]; then
    echo "build.sh: no SDK $VER in $HERE/.sdk: run fetch_sdk.sh <tarball> first" >&2
    exit 1
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
        TRX_INC="-I$HERE/include -I$HERE/contract" \
        RV_PREFIX=riscv32-unknown-elf- || rc=1
done
exit $rc
