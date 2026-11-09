#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.1 M2/M3: build the Verilator bench of trx_sid_engine against the
# reference model. Needs verilator on PATH (OSS CAD Suite). run_m1.sh first
# (logs, curves). Niced, one job: another simulation may be running.
#   build.sh   -> build/tb_sid (engine), build/tb_sid_bus (trx_sid with the bus decode),
#                 build/tb_audio (T25.3 mixer and decimator, analyse_audio.py)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
M=$root/cores/trx_sid/model
b=$here/build
mkdir -p "$b"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw \
  --top-module trx_sid_engine -GMEMDIR="\"$root/cores/trx_sid/mem\"" \
  -CFLAGS "-O2 -std=c++17 -I$M" --Mdir "$b/obj_sid" -o "$b/tb_sid" \
  "$root/cores/trx_sid/rtl/trx_sid_engine.v" "$here/tb_sid.cpp" "$M/sidref.cpp"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
  --top-module trx_sid -GMEMDIR="\"$root/cores/trx_sid/mem\"" \
  -CFLAGS "-O2 -std=c++17 -I$M" --Mdir "$b/obj_bus" -o "$b/tb_sid_bus" \
  "$root/cores/trx_sid/rtl/trx_sid.v" "$root/cores/trx_sid/rtl/trx_sid_engine.v" "$here/tb_sid_bus.cpp" "$M/sidref.cpp"
# U12 D5: the whole chain to a .wav, for listening on a host
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw \
  --top-module c64_trx_sid -GMEMDIR="\"$root/cores/trx_sid/mem\"" \
  -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_wav" -o "$b/tb_wav" \
  "$root/cores/trx_sid/rtl/c64_trx_sid.v" "$root/cores/trx_sid/rtl/trx_sid.v" \
  "$root/cores/trx_sid/rtl/trx_sid_engine.v" "$root/cores/trx_sid/rtl/trx_audio.v" \
  "$root/cores/icepi/cdc/cdc_handshake.v" "$here/tb_wav.cpp"

# T25.3 decimator bench
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw \
  --top-module trx_audio -GMEMDIR="\"$root/cores/trx_sid/mem\"" \
  -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_audio" -o "$b/tb_audio" \
  "$root/cores/trx_sid/rtl/trx_audio.v" "$root/cores/icepi/cdc/cdc_handshake.v" "$here/tb_audio.cpp"
