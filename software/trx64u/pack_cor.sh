#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Pack trx64u.cor (U18) and check it. From the pinned SDK (sdk.pin): the base archive the app is linked from, its tools, and beside it MultiCore's online
# sub-app (sdk.pin online_*, .sdk/online.u2a, fetch_sdk.sh); from here: c64_basic's TOML A (gateware/c64_basic.toml), the app's TOML B (app/toml_b.toml), our
# three libraries and the sub-app TREX CRT (build.sh), our two TOMLs (base/menu.toml, base/desc.toml), the icon (base/icon.rgb565) and a c64_basic bitstream.
# Nothing of the firmware submodule (U20 A2, A3).
#  - Record 7: our libraries trx64u, ring and monitor, and the online sub-app (a resident image: it registers online.* and brings the F3 tab Online, MultiCore
#    T239.3 L-3), each checked against its pin first. Record 8, the files the core installs in its folder: our sub-app TREX CRT as apps/trex_crt.u2a
#    (T239.3 P2-5; the base finds a core's applications in <core folder>/apps/, appindex.h). No SID player (U20 D6: it is gone, its sidplayer/ files with
#    it). No Softboot (Alex, 2026-11-24: it starts a .cfw's firmware application on the running bitstream, which c64_basic under this app is not; build.sh).
#  - The app (MultiCore T239.3 D31, P3-3): record 6 is compiled against the TOML B's [sizes], then the SDK's tools/app_build.py links the app in
#    build/app/ (the driver table and the I/O addresses of the TOML A, the library table of the TOML B, the base archive, the sources that depend on
#    the core, and our UCI library's sources from uci/, --src) and the SDK's symbol check (libtab.py symcheck) runs over its objects. The TOML A is
#    in the app, not in the .cor: the .cor has one record 6, and a TOML A record (version 7) holds nothing but TOML A (api.md 6.14.1), so it cannot
#    ride with our description (U18 §5).
#  - The heap (T239.3 P4-1): word 0, the region left after the linked image and the stack reserve; cor_pack.py reports the figures, and this script
#    checks them against the image: heap = 0xE70000 - align16(E), E the linker's _end of build/app/app.elf, 0xE70000 the end of the application region
#    (__heap_limit) less the stack reserve, both as the image's heap record carries them.
#  - The operations (T239.3 D31, P2-5): base/ops_trx64u.txt is what the application exports, the linked app with the libraries and sub-apps of record 7
#    (ours, U18 R6). Two checks against it: cor_pack.py --ops checks every operation, state and binding record 6 names (always); --emu <ue2emu> runs the
#    packed .cor in ue2emu and compares what the running app registers (REST /v1/ops and /v1/state) with the list, both ways (tools/emu_ops.py --check).
#    The SDK's link step makes no operation table: app_build.py generates appsizes.h, the driver table, the library table (libtab_table.cc) and io.ld,
#    and the operations are registered at run time by the code that table names and by the libraries of record 7, so the app's list is measured by
#    running it. The Gitea stage cor runs only the first check; the GitHub matrix runs the second in its emulator job (ci/lane.sh core:trx64u emu, U20 B).
#   software/trx64u/pack_cor.sh --bit <c64_basic.bit> --bit-commit <commit8> --version <v> [--out <file>] [--no-build] [--emu <ue2emu>]
# --bit is the c64_basic.bit of a commit: the Gitea CI package <branch>-<commit8> (.../api/packages/TREX/generic/trx64-ultimate/...) or, on
# GitHub, the builder's release fw-bit-c64u-<key> that ci/fetch_bits.sh restores (U20 B); --bit-commit names that commit. --no-build packs the libraries already in build/ (the app is always linked; make redoes only what changed).
# Default --out: build/cor/trx64u.cor. Needs the SDK unpacked (fetch_sdk.sh). Where riscv32-unknown-elf-g++ is missing it runs itself in
# u64-build-env, as build.sh does (the bitstream is copied into build/cor/in/ first so that the container sees it), and the emulator check runs here after it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
pin() { sed -n "s/^$1=//p" "$HERE/sdk.pin"; }
SDK="$HERE/.sdk/trx64-sdk-$(pin sdk_version)"

BIT= BITC= VER= OUT="$HERE/build/cor/trx64u.cor" BUILD=yes EMU=
while [ $# -gt 0 ]; do
    case "$1" in
        --bit) BIT="$2"; shift 2 ;;
        --bit-commit) BITC="$2"; shift 2 ;;
        --version) VER="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --no-build) BUILD=no; shift ;;
        --emu) EMU="$2"; shift 2 ;;
        *) echo "pack_cor.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
if [ -z "$BIT" ] || [ -z "$BITC" ] || [ -z "$VER" ]; then
    echo "usage: pack_cor.sh --bit <c64_basic.bit> --bit-commit <commit8> --version <v> [--out <file>] [--no-build] [--emu <ue2emu>]" >&2
    exit 2
fi
[ -f "$BIT" ] || { echo "pack_cor.sh: no bitstream $BIT" >&2; exit 1; }
[ -z "$EMU" ] || [ -x "$EMU" ] || { echo "pack_cor.sh: no emulator $EMU" >&2; exit 1; }
sh "$HERE/build.sh" --check
# the second check of the operations: what the running app registers against the list (tools/emu_ops.py starts and stops ue2emu itself)
emu_check() {
    python3 -I "$HERE/tools/emu_ops.py" --ue2emu "$EMU" --cor "$1" --app "$HERE/build/app/app.elf" --check "$HERE/base/ops_trx64u.txt"
}

if ! command -v riscv32-unknown-elf-g++ >/dev/null 2>&1; then
    mkdir -p "$HERE/build/cor/in" "$(dirname "$OUT")"
    cp "$BIT" "$HERE/build/cor/in/c64_basic.bit"
    OUTD="$(cd "$(dirname "$OUT")" && pwd)"
    case "$OUTD" in "$ROOT"/*) ;; *) echo "pack_cor.sh: --out must be inside $ROOT for the container" >&2; exit 2 ;; esac
    NB=; [ "$BUILD" = no ] && NB=--no-build
    container run --rm --name "trx64u-pack-$$" --rosetta --arch amd64 -v "$ROOT:/trx" -w /trx u64-build-env \
        sh /trx/software/trx64u/pack_cor.sh --bit /trx/software/trx64u/build/cor/in/c64_basic.bit --bit-commit "$BITC" \
        --version "$VER" --out "/trx/${OUTD#"$ROOT"/}/$(basename "$OUT")" $NB
    [ -z "$EMU" ] || emu_check "$OUTD/$(basename "$OUT")"
    exit 0
fi

[ "$BUILD" = no ] || sh "$HERE/build.sh"

# the app: record 6 against the [sizes] of the TOML B (app_build.py checks it; cor_pack.py compiles the same files the same way below; descrec.py's
# command line takes one TOML), the link, the symbol check.
TB="$HERE/app/toml_b.toml"
A="$HERE/build/app"
[ "$(cat "$A/.sdk" 2>/dev/null)" = "$(pin sdk_tarball_sha256)" ] || rm -rf "$A"   # objects of another SDK would stay (build.sh says why)
mkdir -p "$A"
pin sdk_tarball_sha256 > "$A/.sdk"
python3 -I - "$SDK/tools" "$TB" "$A/desc.bin" "$HERE/base/menu.toml" "$HERE/base/desc.toml" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import descrec
try:
    descrec.apply_sizes(descrec.toml_loads(open(sys.argv[2]).read()).get("sizes", {}))
    blob = descrec.compile_files([(f, open(f).read()) for f in sys.argv[4:]])
except descrec.DescError as e:
    sys.exit("pack_cor.sh: record 6: %s" % e)
open(sys.argv[3], "wb").write(blob)
PY
python3 -I "$SDK/tools/app_build.py" build --sdk "$SDK" --toml-a "$HERE/gateware/c64_basic.toml" --toml-b "$TB" --out "$A" --record "$A/desc.bin" \
    --src "$HERE/uci"
# An object whose source left the link (a library the TOML B no longer names, or names with other sources, as uci since U18's owner split) stays in
# obj/, and the symbol check reads every object there: one the link did not load (no LOAD line in app.map) goes, with its dependency file.
for o in "$A"/obj/*.o; do
    if ! grep -qx "LOAD obj/$(basename "$o")" "$A/app.map"; then
        echo "pack_cor.sh: obj/$(basename "$o") is not in the link: removed"
        rm -f "$o" "${o%.o}.d"
    fi
done
NM=riscv32-unknown-elf-nm python3 -I "$SDK/tools/libtab.py" symcheck "$TB" "$A/obj" "$SDK/base/src"
echo "symcheck: no object of the base uses a symbol of a library of $(basename "$TB")"

# record 8: the sub-app (build.sh), installed into <core folder>/apps/, where the base's application index finds it
F="$HERE/build/cor/files"
rm -rf "$F"
mkdir -p "$F/apps"
for f in trex_crt; do
    cp "$HERE/build/$f/$f.u2a" "$F/apps/"
done
# record 7's online sub-app: the pinned file (fetch_sdk.sh put it there), checked again
ONL="$HERE/.sdk/$(pin online_u2a)"
[ -f "$ONL" ] || { echo "pack_cor.sh: no $ONL: run fetch_sdk.sh <tarball> <$(pin online_u2a)> or fetch_sdk.sh --download" >&2; exit 1; }
SUM="$(sha256sum "$ONL" 2>/dev/null || shasum -a 256 "$ONL")"
[ "${SUM%% *}" = "$(pin online_u2a_sha256)" ] || { echo "pack_cor.sh: $ONL has sha256 ${SUM%% *}, sdk.pin wants $(pin online_u2a_sha256)" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
python3 -I "$SDK/tools/cor_pack.py" pack-large -o "$OUT" --id trx64u --name TRX64U --version "$VER" \
    --bit "$BIT" --app "$A/app.bin" --heap 0 --sizes "$TB" --icon "$HERE/base/icon.rgb565" \
    --desc "$HERE/base/menu.toml" --desc "$HERE/base/desc.toml" \
    --lib "$HERE/build/trx64u/trx64u.u2a" --lib "$HERE/build/ring/ring.u2a" --lib "$HERE/build/monitor/monitor.u2a" --lib "$ONL" \
    --files "$F" --ops "$HERE/base/ops_trx64u.txt" --bit-commit "$BITC" --app-commit "$(pin source_commit)" > "$A/pack.log" 2>&1 \
    || { cat "$A/pack.log" >&2; exit 1; }
cat "$A/pack.log"
python3 -I "$SDK/tools/cor_pack.py" check "$OUT" > /dev/null

# P4-1: the heap the packer reported is what is left behind the image: the region end and the reserve from the image's heap record, E from the ELF
E="$(riscv32-unknown-elf-nm "$A/app.elf" | sed -n 's/^\([0-9a-f]*\) [A-Za-z] _end$/\1/p')"
python3 -I - "$A/app.bin" "$E" "$A/pack.log" <<'PY'
import re, struct, sys
img, end, log = open(sys.argv[1], "rb").read(), int(sys.argv[2], 16), open(sys.argv[3]).read()
o = img.index(b"TRXHEAP:") + 8
_, image_end, region_end, reserve = struct.unpack_from("<4I", img, o)
m = re.search(r"; heap 0x([0-9a-f]+) = \d+ bytes \(word 0: the region left\)", log)
if image_end != end or not m:
    sys.exit("pack_cor.sh: heap: the image's record says image end 0x%x, the ELF's _end is 0x%x%s" % (image_end, end, "" if m else "; cor_pack.py reported no heap of word 0"))
heap, top, aligned = int(m.group(1), 16), region_end - reserve, (end + 15) & ~15
if heap != top - aligned:
    sys.exit("pack_cor.sh: heap 0x%x reported, 0x%x - align16(0x%x) = 0x%x" % (heap, top, end, top - aligned))
print("heap = 0x%X - align16(E) = 0x%X - 0x%X = 0x%X (%d bytes); E = _end = 0x%X" % (top, top, aligned, heap, heap, end))
PY
SUM="$(sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT")"
echo "trx64u.cor: $OUT, $(wc -c < "$OUT" | tr -d ' ') bytes, sha256 ${SUM%% *}, checked"
[ -z "$EMU" ] || emu_check "$OUT"
