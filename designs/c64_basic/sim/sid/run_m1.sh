#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.1 M1: the SID reference model against libresidfp, and its golden output.
#  - fetches libresidfp (GPL-2+, pinned) into build/libresidfp
#  - regenerates the combined-waveform tables and checks them against the
#    committed cores/trx_sid/mem/sid_wf_*.mem
#  - generates the register-op logs (gen_logs.py) and the app's curves
#  - compare_resid: digital part per cycle, both models, every log
#  - sidref_run: golden CRCs -> build/golden.txt
# Run niced: one CPU-heavy job at a time on the Mac.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
M=$root/cores/trx_sid/model
T=$root/cores/trx_sid/mem
b=$here/build
mkdir -p "$b/obj"
R=$("$root/tools/fetch_libresidfp.sh" "$b/libresidfp")
CXX="nice -n 15 c++ -O2 -std=c++17"
$CXX -I"$R" "$M/gen_tables.cpp" "$R/WaveformCalculator.cpp" -o "$b/gen_tables"
mkdir -p "$b/mem"
"$b/gen_tables" "$b/mem" 2>/dev/null
cmp "$b/mem/sid_wf_dir.mem" "$T/sid_wf_dir.mem" && cmp "$b/mem/sid_wf_data.mem" "$T/sid_wf_data.mem"
python3 "$M/gen_logs.py" "$b/logs"
python3 "$M/gen_curves.py" "$root/vendor/gideon/software/io/audio/sid_coeff.c" "$b/curves"
for f in "$R"/*.cpp "$R/resample/SincResampler.cpp"; do
  o=$b/obj/$(basename "$f" .cpp).o
  [ -f "$o" ] || $CXX -I"$R" -c "$f" -o "$o"
done
$CXX -I"$R" -I"$M" "$M/compare_resid.cpp" "$M/sidref.cpp" "$b"/obj/*.o -o "$b/compare_resid"
$CXX -I"$M" "$M/sidref_run.cpp" "$M/sidref.cpp" -o "$b/sidref_run"
fail=0
for m in 6581 8580; do
  for l in "$b"/logs/*.log; do
    r=$(nice -n 15 "$b/compare_resid" "$T" $m "$l" 3 | tail -1); echo "$r"
    case $r in *" 0 mismatches"*) ;; *) fail=$((fail+1)) ;; esac
  done
done
: > "$b/golden.txt"
for l in "$b"/logs/*.log; do
  nice -n 15 "$b/sidref_run" "$T" "$l" 6581 0 2 "$b/curves/curve2.mem" >> "$b/golden.txt"
  nice -n 15 "$b/sidref_run" "$T" "$l" 8580 1 3 "$b/curves/curve0.mem" >> "$b/golden.txt"
done
cat "$b/golden.txt"
echo "compare_resid: $fail log(s) with mismatches (known: 8580 OSC3 one cycle after a waveform change into P+T)"
