#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: the in-tree reDIP SID (sid_api, NS = 2, all the generalised modules)
# with the same bench as the reference (tb_ref.cpp), for the bit-exact compare.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
g=$root/cores/icepi/c64_redip_sid
w=$g/reDIP-SID/gateware
b=$here/build/api
mkdir -p "$b/run"
cp "$w"/*.mem "$b/run/"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-lint -Wno-TIMESCALEMOD --public-flat-rw \
  --top-module sid_api -I"$w" \
  -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj" -o "$b/tb_api" \
  "$w/sid_pkg.sv" "$g/muladd.sv" "$w/sid_dac.sv" "$w/sid_pot.sv" "$w/sid_waveform.sv" \
  "$w/sid_envelope.sv" "$w/sid_voice.sv" "$w/sid_filter.sv" "$w/sid_control.sv" "$w/sid_core.sv" "$g/sid_api.sv" \
  "$here/tb_ref.cpp" > "$b/build.log" 2>&1 || { grep -E "^%Error" -A4 "$b/build.log" | head -60; exit 1; }
