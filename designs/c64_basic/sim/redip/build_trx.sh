#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: trx_sid + sid_multi (the equivalence build) with tb_trx.cpp.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
g=$root/cores/icepi/c64_redip_sid
w=$g/reDIP-SID/gateware
b=$here/build/trx
mkdir -p "$b/run"
cp "$w"/*.mem "$b/run/"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-lint -Wno-TIMESCALEMOD --public-flat-rw \
  --top-module trx_sid -DSID_MULTI -DSID_MULTI_EQUIV -I"$w" -GMEMDIR='"."' \
  -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj" -o "$b/tb_trx" \
  "$w/sid_pkg.sv" "$g/muladd.sv" "$w/sid_dac.sv" "$w/sid_waveform.sv" \
  "$w/sid_envelope.sv" "$w/sid_voice.sv" "$w/sid_filter.sv" "$w/sid_control.sv" "$w/sid_core.sv" "$g/sid_multi.sv" \
  "$root/cores/trx_sid/rtl/trx_sid.v" "$here/tb_trx.cpp" > "$b/build.log" 2>&1 || { grep -E "^%Error" -A4 "$b/build.log" | head -60; exit 1; }
