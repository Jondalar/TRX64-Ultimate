#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.5: filter bypass = reSID's enable_filter(false). In reSID the disabled filter takes nothing into the
# filter (sum = 0) and mixes all four inputs (v1, v2, v3, EXT IN) directly; the filter mode bits and voice
# 3 off have no effect (set_sum_mix: mix = 0x0f). That is exactly what the unmodified reDIP computes when
# FILT (the routing bits $17 b3:0) = 0 and the mode bits of $18 (LP, BP, HP, 3 OFF) = 0, master volume
# kept. So: the DUT with the bypass on, fed the original register log, must equal the original reDIP fed the
# same log with $17 & $F0 and $18 & $0F (RES and VOL kept), bit exact, samples and read-backs.
# Run build_ref.sh and build_multi.sh 8 [2] first. Niced, one job.
set -e
here=$(cd "$(dirname "$0")" && pwd)
b=$here/build
mkdir -p "$b/logs" "$b/ops" "$b/out"
root=$(cd "$here/../../../.." && pwd)
[ -f "$b/logs/tones.log" ] || python3 "$root/cores/trx_sid/model/gen_logs.py" "$b/logs"
MAX=${MAX:-300000}
fail=0
run8() {
  name=$1; m0=$2; m1=$3; shift 3
  l=""; for x in "$@"; do l="$l $b/logs/$x.log"; done
  python3 "$here/mklog.py" "$b/ops/$name.txt" $l --max $MAX
  python3 - "$b/ops/$name.txt" "$b/ops/${name}_byp.txt" <<'PY'
import sys
out = []
for line in open(sys.argv[1]):
    t = line.split()
    if len(t) == 5 and t[2] == 'W':
        r = int(t[3], 16); v = int(t[4], 16)
        if r == 0x17: v &= 0xF0
        elif r == 0x18: v &= 0x0F
        line = "%s %s W %02x %02x\n" % (t[0], t[1], r, v)
    out.append(line)
open(sys.argv[2], 'w').write("".join(out))
PY
  for p in 0 1 2 3; do
    if [ $p -lt 2 ]; then m=$m0; else m=$m1; fi
    (cd "$b/ref/src" && nice -n 15 ../tb_ref "$b/ops/${name}_byp.txt" $m 1 $p "$b/out/byp_$name.$p")
  done
  for v in ${VARIANTS:-multi8 multi8fc2}; do
    [ -x "$b/$v/tb_multi" ] || continue
    if (cd "$b/$v/run" && BYPASS=1 nice -n 15 ../tb_multi "$b/ops/$name.txt" $m0 $m1 "$b/out/byp_$name"); then
      echo "PASS bypass $name (UltiSID 1 $m0, UltiSID 2 $m1) [$v]"
    else
      echo "FAIL bypass $name (UltiSID 1 $m0, UltiSID 2 $m1) [$v]"; fail=$((fail+1))
    fi
  done
  rm -f "$b"/out/byp_$name.*
}
run8 a 6581 8580 tones adsr filter ringsync digi fuzz0 testbit fuzz1
run8 b 8580 6581 fuzz2 fuzz3 tones adsr filter ringsync digi floating
run8 c 6581 6581 adsr tones ringsync filter fuzz1 digi fuzz2 fuzz3
run8 d 8580 8580 digi fuzz0 floating testbit tones fuzz3 adsr filter
[ $fail = 0 ] && echo "bypass: PASS" || { echo "bypass: $fail FAIL"; exit 1; }
