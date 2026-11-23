#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Pack trx64u.cor (U18) and check it: MultiCore's app core image and cor_pack.py from the pinned SDK, our three libraries
# (build.sh), our two TOMLs (base/menu.toml, base/desc.toml), the icon (base/icon.rgb565), the SID player's files
# (record 8, built from the submodule's 6502 sources) and a c64_basic bitstream.
#   software/trx64u/pack_cor.sh --bit <c64_basic.bit> --bit-commit <commit8> --version <v> [--out <file>] [--no-build]
# --bit is the c64_basic.bit of our CI package <branch>-<commit8> (.../api/packages/TREX/generic/trx64-ultimate/...);
# --bit-commit names that commit. --no-build packs the libraries already in build/. Default --out: build/cor/trx64u.cor.
# Needs the SDK unpacked (fetch_sdk.sh). Where riscv32-unknown-elf-g++ is missing it runs itself in u64-build-env, as
# build.sh does; the bitstream is copied into build/cor/in/ first so that the container sees it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
APP="$ROOT/software/ultimate-app"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
SDK="$HERE/.sdk/trx64-sdk-$(pin sdk_version)"

BIT= BITC= VER= OUT="$HERE/build/cor/trx64u.cor" BUILD=yes
while [ $# -gt 0 ]; do
    case "$1" in
        --bit) BIT="$2"; shift 2 ;;
        --bit-commit) BITC="$2"; shift 2 ;;
        --version) VER="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --no-build) BUILD=no; shift ;;
        *) echo "pack_cor.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
if [ -z "$BIT" ] || [ -z "$BITC" ] || [ -z "$VER" ]; then
    echo "usage: pack_cor.sh --bit <c64_basic.bit> --bit-commit <commit8> --version <v> [--out <file>] [--no-build]" >&2
    exit 2
fi
[ -f "$BIT" ] || { echo "pack_cor.sh: no bitstream $BIT" >&2; exit 1; }
sh "$HERE/build.sh" --check

if ! command -v riscv32-unknown-elf-g++ >/dev/null 2>&1; then
    mkdir -p "$HERE/build/cor/in" "$(dirname "$OUT")"
    cp "$BIT" "$HERE/build/cor/in/c64_basic.bit"
    OUTD="$(cd "$(dirname "$OUT")" && pwd)"
    case "$OUTD" in "$ROOT"/*) ;; *) echo "pack_cor.sh: --out must be inside $ROOT for the container" >&2; exit 2 ;; esac
    NB=; [ "$BUILD" = no ] && NB=--no-build
    exec container run --rm --name "trx64u-pack-$$" --rosetta --arch amd64 -v "$ROOT:/trx" -w /trx u64-build-env \
        sh /trx/software/trx64u/pack_cor.sh --bit /trx/software/trx64u/build/cor/in/c64_basic.bit --bit-commit "$BITC" \
        --version "$VER" --out "/trx/${OUTD#"$ROOT"/}/$(basename "$OUT")" $NB
fi

[ "$BUILD" = no ] || sh "$HERE/build.sh"

# record 8: the SID player, assembled by the submodule's own 64tass (software/6502/sidcrt, target/ is not in git)
make -s -C "$APP/tools/64tass" >/dev/null
make -s -C "$APP/software/6502/sidcrt" >/dev/null
F="$HERE/build/cor/files"
rm -rf "$F"
mkdir -p "$F/sidplayer"
for f in sidcrt muscrt musplayer; do
    cp "$APP/software/6502/sidcrt/target/$f.bin" "$F/sidplayer/"
done

mkdir -p "$(dirname "$OUT")"
python3 -I "$SDK/tools/cor_pack.py" pack-large -o "$OUT" --id trx64u --name TRX64U --version "$VER" \
    --bit "$BIT" --app "$SDK/image/coreapp.bin" --icon "$HERE/base/icon.rgb565" \
    --desc "$HERE/base/menu.toml" --desc "$HERE/base/desc.toml" \
    --lib "$HERE/build/trx64u/trx64u.u2a" --lib "$HERE/build/ring/ring.u2a" --lib "$HERE/build/monitor/monitor.u2a" \
    --files "$F" --bit-commit "$BITC" --app-commit "$(pin source_commit)"
python3 -I "$SDK/tools/cor_pack.py" check "$OUT" > /dev/null
SUM="$(sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT")"
echo "trx64u.cor: $OUT, $(wc -c < "$OUT" | tr -d ' ') bytes, sha256 ${SUM%% *}, checked"
