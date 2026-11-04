#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: sid_multi (NS SIDs, equivalence build: LEGACY_FC = 1, VARIANT_B = 1) with tb_multi.cpp.
#   build_multi.sh [NS]    (default 8; NS = 4 is two reDIP pairs, 2 the plain pair)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
g=$root/cores/icepi/c64_redip_sid
w=$g/reDIP-SID/gateware
NS=${1:-8}
FC=${2:-1}      # LEGACY_FC: 1 the equivalence build, 2 the hardware filter control of T25.5 at its reset values
sfx=; [ "$FC" = 2 ] && sfx=fc2
b=$here/build/multi$NS$sfx
mkdir -p "$b/run"
cp "$w"/*.mem "$b/run/"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-lint -Wno-TIMESCALEMOD --public-flat-rw \
  --top-module sid_multi -I"$w" -GNS=$NS -GLEGACY_FC=$FC -GVARIANT_B=1 -GPULSE_FIX=0 -GMEMDIR='"."' \
  -CFLAGS "-O2 -std=c++17 -DNSIDS=$NS" --Mdir "$b/obj" -o "$b/tb_multi" \
  "$w/sid_pkg.sv" "$g/muladd.sv" "$w/sid_dac.sv" "$w/sid_waveform.sv" \
  "$w/sid_envelope.sv" "$w/sid_voice.sv" "$w/sid_filter.sv" "$w/sid_control.sv" "$w/sid_core.sv" "$g/sid_multi.sv" \
  "$here/tb_multi.cpp" > "$b/build.log" 2>&1 || { grep -E "^%Error" -A4 "$b/build.log" | head -60; exit 1; }
