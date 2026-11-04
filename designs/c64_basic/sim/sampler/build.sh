#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.6: Verilator bench of trx_sampler (rates, loops, 8/16 bit, volume/pan, IRQ, registers, app handshake) and of the
# mixer with the sampler inputs (trx_audio, bit exact with the sampler silent). Niced, one job.
#   build.sh    -> build/tb_sampler, build/tb_mix
#   run.sh      runs them (run.sh stress [seed lat_lo lat_hi cycles]: tb_stress, DDR2 latency and retrigger races)
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
b=$here/build
mkdir -p "$b"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PROCASSINIT --public-flat-rw \
  --top-module trx_sampler -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_smp" -o "$b/tb_sampler" \
  "$root/cores/trx_sid/rtl/trx_sampler.v" "$here/tb_sampler.cpp" > "$b/smp.log" 2>&1 || { tail -30 "$b/smp.log"; exit 1; }
echo "built $b/tb_sampler"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw \
  --top-module trx_audio -GMEMDIR="\"$root/cores/trx_sid/mem\"" -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_mix" -o "$b/tb_mix" \
  "$root/cores/trx_sid/rtl/trx_audio.v" "$root/cores/icepi/cdc/cdc_handshake.v" "$here/tb_mix.cpp" > "$b/mix.log" 2>&1 || { tail -30 "$b/mix.log"; exit 1; }
echo "built $b/tb_mix"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PROCASSINIT --public-flat-rw \
  --top-module sm_io_top -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_io" -o "$b/tb_sm_io" \
  "$here/sm_io_top.v" "$root/designs/c64_basic/trx_io.v" "$root/cores/trx_sid/rtl/trx_sampler.v" "$here/tb_sm_io.cpp" > "$b/io.log" 2>&1 || { tail -30 "$b/io.log"; exit 1; }
echo "built $b/tb_sm_io"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PROCASSINIT --public-flat-rw \
  --top-module trx_sampler -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_stress" -o "$b/tb_stress" \
  "$root/cores/trx_sid/rtl/trx_sampler.v" "$here/tb_stress.cpp" > "$b/stress.log" 2>&1 || { tail -30 "$b/stress.log"; exit 1; }
echo "built $b/tb_stress"
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PROCASSINIT --public-flat-rw \
  --top-module trx_sampler -CFLAGS "-O2 -std=c++17" --Mdir "$b/obj_spur" -o "$b/tb_spur" \
  "${SAMPLER_V:-$root/cores/trx_sid/rtl/trx_sampler.v}" "$here/tb_spur.cpp" > "$b/spur.log" 2>&1 || { tail -30 "$b/spur.log"; exit 1; }
echo "built $b/tb_spur"
