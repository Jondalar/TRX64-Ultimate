#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Ethernet TX bench (tb_eth_tx.vhd) in GHDL: the MAC (ethernet_interface,
# eth_flat.vhd), top.v's RMII arbiter, rmii_transceiver; every frame on the
# wire checked. Needs ghdl on PATH (or GHDL=...).
#   run.sh [orig] [-gname=value ...]
# "orig": Gideon's eth_transmit.vhd as upstream has it, made from vendor/gideon's
# copy by reversing our patch 0003 (the patched copy otherwise). Generics: see
# tb_eth_tx.vhd.
set -e
here=$(cd "$(dirname "$0")" && pwd)
design=$(cd "$here/../.." && pwd)
root=$(cd "$design/../.." && pwd)
G=${GHDL:-ghdl}
opts="--std=08 -fsynopsys -frelaxed -Wno-hide -Wno-shared -Wno-library"
"$root/tools/gideon_sources.sh"
variant=patched
if [ "$1" = orig ]; then variant=orig; shift; fi
out=$here/build/$variant
mkdir -p "$out"
# the VHDL list of build.tcl (ghdl -m takes what tb_eth_tx needs)
python3 - "$design/build.tcl" "$root/vendor/gideon/fpga" "$design" > "$out/files.txt" <<'PY'
import sys
tcl, g, design = sys.argv[1:]
body = open(tcl).read().split("\nset g ", 1)[1].split("read_vhdl -library work [list", 1)[1].split("]", 1)[0]
for tok in body.replace("\\", " ").split():
    p = tok.replace("$g", g)
    print(p if p.startswith("/") else design + "/" + p)
PY
files=$(cat "$out/files.txt")
if [ $variant = orig ]; then
  # git apply works on a path below the root it runs in (as tools/vendor_gideon.sh applies the patches)
  e=fpga/io/rmii/vhdl_source/eth_transmit.vhd
  rm -rf "$out/upstream"
  mkdir -p "$out/upstream/$(dirname "$e")"
  cp "$root/vendor/gideon/$e" "$out/upstream/$e"
  git -C "$root" apply -R -p1 --directory="${out#"$root"/}/upstream" "$root/tools/patches/gideon/0003-eth-transmit-store-and-forward.patch"
  files=$(echo "$files" | sed "s|^.*/io/rmii/vhdl_source/eth_transmit.vhd$|$out/upstream/$e|")
fi
cd "$out"
rm -f work-obj08.cf
$G -a $opts --work=unisim "$here/../usb/unisim_sim.vhd"
$G -i $opts $files "$here/eth_flat.vhd" "$here/tb_eth_tx.vhd"
nice -n 15 $G -m $opts tb_eth_tx > make.log 2>&1 || { tail -20 make.log; exit 1; }
nice -n 15 $G -r $opts tb_eth_tx "$@" > run.log 2>&1 || true
grep -E "came out|no app frame|stream frame|frames .* sent|PASS|FAIL" run.log | sed 's/^.*(report [a-z]*)://' | tail -25
grep -q PASS run.log
