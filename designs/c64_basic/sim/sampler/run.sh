#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.6 benches. run.sh          the three fast ones: tb_sampler (unit), run_mix.py (mixer lanes), tb_sm_io (trx_io page 8)
#                run.sh top      the whole core (build_top.sh first; a C64 boot to READY, many minutes), enable on
#                run.sh top0     the same with C64_SAMPLER_ENABLE off
#                run.sh stress [seed lo hi cycles]  tb_stress (random DDR2 latency, retriggers; fails on a race)
#                run.sh stress [seed lo hi cycles]  tb_stress (random DDR2 latency, retriggers; fails on a race)
#                spur.py (in the default run): the step timing against the ideal time average, one voice must stay below -70 dB
#                run.sh smp N    tb_sampler with a DDR2 latency of N clk
here=$(cd "$(dirname "$0")" && pwd)
case $1 in
  top|top0)
    cd "$here/build/top/run" && nice -n 15 ../tb_sampler_top $([ "$1" = top ] && echo 1 || echo 0) $2 ;;
  stress) shift; nice -n 15 "$here/build/tb_stress" "$@" ;;
  stress) shift; nice -n 15 "$here/build/tb_stress" "$@" ;;
  smp) shift; nice -n 15 "$here/build/tb_sampler" "$@" ;;
  *)
    rc=0
    nice -n 15 "$here/build/tb_sampler" | grep -v "^rate" | tail -3 || rc=1
    "$here/run_mix.py" | tail -1 || rc=1
    "$here/spur.py" | tail -9 || rc=1
    nice -n 15 "$here/build/tb_sm_io" | tail -2 || rc=1
    exit $rc ;;
esac
