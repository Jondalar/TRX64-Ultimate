#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: trx_sid + sid_multi at the bus (app decode, split, mirroring, read-back) against the
# unmodified reDIP. Run build_ref.sh and build_trx.sh first. Niced, one job.
#   run_trx.sh [cycles]
set -e
here=$(cd "$(dirname "$0")" && pwd)
b=$here/build
mkdir -p "$b/out"
N=${1:-400000}
fail=0
one() {
  seed=$1; m0=$2; m1=$3
  "$b/trx/tb_trx" gen $seed $N "$b/out/trx_acc.txt" "$b/out/trx_ops.txt"
  for p in 0 1 2 3; do
    if [ $p -lt 2 ]; then m=$m0; else m=$m1; fi
    (cd "$b/ref/src" && nice -n 15 ../tb_ref "$b/out/trx_ops.txt" $m 1 $p "$b/out/trx_ref.$p")
  done
  if (cd "$b/trx/run" && nice -n 15 ../tb_trx run "$b/out/trx_acc.txt" $m0 $m1 "$b/out/trx_ref"); then :; else fail=$((fail+1)); fi
  rm -f "$b"/out/trx_ref.*
}
one 1 6581 8580
one 2 8580 6581
one 3 6581 6581
one 4 8580 8580
[ $fail = 0 ] && echo "trx: PASS" || { echo "trx: $fail FAIL"; exit 1; }
