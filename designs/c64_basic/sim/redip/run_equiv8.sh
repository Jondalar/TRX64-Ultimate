#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: NS = 8 equivalence. Eight register logs, one per SID, merged to one op
# list; each pair of SIDs (2p, 2p+1) is run through the unmodified reDIP as its
# SID 1 and 2 (tb_ref, pair p, the model of that pair's UltiSID), the eight SIDs
# of sid_multi are compared with those streams, per cycle. UltiSID 1 = SIDs 0-3,
# UltiSID 2 = SIDs 4-7, one model each.
# Run build_ref.sh and build_multi.sh 8 first. Niced, one job.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
b=$here/build
mkdir -p "$b/logs" "$b/ops" "$b/out"
[ -f "$b/logs/tones.log" ] || python3 "$root/cores/trx_sid/model/gen_logs.py" "$b/logs"
MAX=${MAX:-1000000}
REFTAG=${REFTAG:-}
fail=0
run8() {
  name=$1; m0=$2; m1=$3; shift 3
  l=""; for x in "$@"; do l="$l $b/logs/$x.log"; done
  python3 "$here/mklog.py" "$b/ops/$name.txt" $l --max $MAX
  for p in 0 1 2 3; do
    if [ $p -lt 2 ]; then m=$m0; else m=$m1; fi
    (cd "$b/ref$REFTAG/src" && nice -n 15 ../tb_ref "$b/ops/$name.txt" $m 1 $p "$b/out/ref8_$name.$p")
  done
  for v in ${VARIANTS:-multi8 multi8fc2}; do
    [ -x "$b/$v/tb_multi" ] || continue
    if (cd "$b/$v/run" && nice -n 15 ../tb_multi "$b/ops/$name.txt" $m0 $m1 "$b/out/ref8_$name"); then
      echo "PASS $name (UltiSID 1 $m0, UltiSID 2 $m1) [$v]"
    else
      echo "FAIL $name (UltiSID 1 $m0, UltiSID 2 $m1) [$v]"; fail=$((fail+1))
    fi
  done
  rm -f "$b"/out/ref8_$name.*
}
run8 a 6581 8580 tones adsr filter ringsync digi fuzz0 testbit fuzz1
run8 b 8580 6581 fuzz2 fuzz3 tones adsr filter ringsync digi floating
run8 c 6581 6581 adsr tones ringsync filter fuzz1 digi fuzz2 fuzz3
run8 d 8580 8580 digi fuzz0 floating testbit tones fuzz3 adsr filter
# T25.5: the hardware filter control at other settings = the original reDIP with other 6581 curve constants
# (build_ref.sh <tag> <base> <offset>: SID 1 = base/offset, SID 2 = base-5/offset-5), the presets of the menu,
# and Auto must not look at RES / DIGI / the explicit values (FC2_SCRAMBLE)
if [ -z "$PRESET_RUN" ] && [ -x "$b/multi8fc2/tb_multi" ]; then
  PMAX=${PMAX:-300000}
  FC2_SCRAMBLE=1 PRESET_RUN=1 VARIANTS=multi8fc2 MAX=$PMAX sh "$0" > "$b/preset.log" 2>&1 || fail=$((fail+1))
  grep -v "^PASS:" "$b/preset.log" | sed 's/^/scramble: /'
  for p in "_ext 200 760" "_gal 280 -405" "_avg 250 0" "_str 260 400"; do
    set -- $p
    sh "$here/build_ref.sh" $1 $2 $3 > /dev/null 2>&1
    REFTAG=$1 FC_BASE=$2 FC_OFF=$3 PRESET_RUN=1 VARIANTS=multi8fc2 MAX=$PMAX sh "$0" > "$b/preset.log" 2>&1 || fail=$((fail+1))
    grep -v "^PASS:" "$b/preset.log" | sed "s/^/base $2 offset $3: /"
  done
fi
[ $fail = 0 ] && echo "equiv8: PASS" || { echo "equiv8: $fail FAIL"; exit 1; }
