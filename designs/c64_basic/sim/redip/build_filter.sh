#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# U7: sid_filter alone (NS = 1, LEGACY_FC = 2) for the filter precision measurement of
# tb_filter.cpp. The module needs sid_pkg, sid_dac (the 6581 cutoff DAC) and our muladd;
# dac_6581_cutoff.mem is read at elaboration, so the .mem files go next to the binary.
#
#   build_filter.sh [FILTER_FRAC]    0 = reDIP's own arithmetic (default), 6 = the build's
#
# The binary goes to build/filter<F>/, so both can exist side by side.
set -e
F=${1:-0}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
g=$root/cores/icepi/c64_redip_sid
w=$g/reDIP-SID/gateware
b=$here/build/filter$F
mkdir -p "$b/run"
cp "$w"/*.mem "$b/run/"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-lint \
  -Wno-TIMESCALEMOD -Wno-MULTIDRIVEN --public-flat-rw \
  --top-module sid_filter -GNS=1 -GLEGACY_FC=2 -GFILTER_FRAC=$F -I"$w" \
  -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj" -o "$b/tb_filter" \
  "$w/sid_pkg.sv" "$g/muladd.sv" "$w/sid_dac.sv" "$w/sid_filter.sv" \
  "$here/tb_filter.cpp" > "$b/build.log" 2>&1 || { grep -E "^%Error" -A4 "$b/build.log" | head -60; exit 1; }
echo "built $b/tb_filter (FILTER_FRAC = $F)"
