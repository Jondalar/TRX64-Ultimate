#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: NS = 2 equivalence: sid_api (in tree) against the reference (the
# unmodified reDIP), same op lists, both models, both variant settings.
# Run build_ref.sh and build_api.sh first. Niced, one job.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
b=$here/build
mkdir -p "$b/logs" "$b/ops" "$b/out"
[ -f "$b/logs/tones.log" ] || python3 "$root/cores/trx_sid/model/gen_logs.py" "$b/logs"
MAX=${MAX:-1500000}
fail=0
run_pair() {
  name=$1; l0=$2; l1=$3
  python3 "$here/mklog.py" "$b/ops/$name.txt" "$b/logs/$l0.log" "$b/logs/$l1.log" --max $MAX
  for m in 6581 8580; do
    for v in 0 1; do
      (cd "$b/ref/src" && nice -n 15 ../tb_ref "$b/ops/$name.txt" $m $v 0 "$b/out/ref_$name.$m.$v")
      (cd "$b/api/run" && nice -n 15 ../tb_api "$b/ops/$name.txt" $m $v 0 "$b/out/api_$name.$m.$v")
      if cmp -s "$b/out/ref_$name.$m.$v" "$b/out/api_$name.$m.$v"; then
        echo "PASS $name $m variants=$v $(wc -l < "$b/out/ref_$name.$m.$v") cycles"
      else
        echo "FAIL $name $m variants=$v first diff:"; diff "$b/out/ref_$name.$m.$v" "$b/out/api_$name.$m.$v" | head -4
        fail=$((fail+1))
      fi
      rm -f "$b/out/ref_$name.$m.$v" "$b/out/api_$name.$m.$v"
    done
  done
}
run_pair tones_adsr tones adsr
run_pair filter_testbit filter testbit
run_pair ring_digi ringsync digi
run_pair float_fuzz0 floating fuzz0
run_pair fuzz12 fuzz1 fuzz2
run_pair fuzz3_tones fuzz3 tones
# mono (sid_mono = 1: both SIDs at $D400, every op reaches both, reads return SID 1)
run_mono() {
  name=$1; l0=$2
  python3 "$here/mklog.py" "$b/ops/$name.txt" "$b/logs/$l0.log" --max $MAX
  for m in 6581 8580; do
    (cd "$b/ref/src" && nice -n 15 ../tb_ref "$b/ops/$name.txt" $m 1 0 "$b/out/ref_$name.$m" 1)
    (cd "$b/api/run" && nice -n 15 ../tb_api "$b/ops/$name.txt" $m 1 0 "$b/out/api_$name.$m" 1)
    if cmp -s "$b/out/ref_$name.$m" "$b/out/api_$name.$m"; then
      echo "PASS $name $m mono $(wc -l < "$b/out/ref_$name.$m") cycles"
    else
      echo "FAIL $name $m mono first diff:"; diff "$b/out/ref_$name.$m" "$b/out/api_$name.$m" | head -4
      fail=$((fail+1))
    fi
    rm -f "$b/out/ref_$name.$m" "$b/out/api_$name.$m"
  done
}
run_mono mono_fuzz fuzz0
run_mono mono_fuzz2 fuzz2
run_mono mono_tones tones
[ $fail = 0 ] && echo "equiv2: PASS" || { echo "equiv2: $fail FAIL"; exit 1; }
