#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stages `bit-c64u` and `bit-ue2`: Vivado builds, in vivado-env:2024.1.
#
#   ci/bitstream.sh c64u    every designs/*/build.tcl, XC7A50T (the C64 Ultimate)
#   ci/bitstream.sh ue2     designs/c64_basic for the XC7A100T (Ultimate 64 Elite II)
#
# A bitstream is built only when something that goes into it changed
# (gateware_key in lib.sh): a push that only moves the app, the docs, the
# benches or the CI gets the bitstream of the last good build with the same key
# from /cache/bit/. A bitstream with failed timing is not a good build: WNS < 0
# fails the stage and is not cached.
#
# bit-ue2 runs after `package`, only when asked for (ci/select.sh ue2: dispatch
# with ue2 set, tag, release/* branch), and publishes its own files (_100t) into
# the package versions `package` made. It cleans up the run's hand-over directory.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
release_reuse_skip bit

board="${1:?c64u or ue2}"
case "$board" in
  c64u) part=xc7a50tfgg484-1; suffix=""; designs=("$ROOT"/designs/*/build.tcl) ;;
  ue2)  part=xc7a100tfgg484-1; suffix="_100t"; designs=("$ROOT/designs/c64_basic/build.tcl")
        export TRX_PART="$part" TRX_BUILD=build_100t ;;
  *) echo "unknown board $board" >&2; exit 2 ;;
esac
out="$(stage_dir "bit-$board")"

if [ "$board" = ue2 ] && [ "$("$ROOT/ci/select.sh" ue2)" != yes ]; then
  echo "bit-ue2 skipped: not asked for (dispatch with ue2, tag or release/* branch; ci/select.sh)"
  rm -rf "${HANDOFF:?}"
  exit 0
fi

key="$(gateware_key "$part")"
cache="${CI_BIT_CACHE:-/cache/bit}/$board-$key"
echo "gateware key $key ($part)"
echo "$key" > "$out/key"
if [ -f "$cache/DONE" ]; then
  echo "=== $board: nothing that goes into the bitstream changed since $(cat "$cache/from"); using $cache"
  cp "$cache"/* "$out/"
  echo "cache $(cat "$cache/from")" > "$out/from"
else
  heavy_lock
  export HOME=/tmp/vivado-home
  export TRX_CACHE="${TRX_CACHE:-/cache}"
  mkdir -p "$HOME"
  for tcl in "${designs[@]}"; do
    d="$(dirname "$tcl")"; n="$(basename "$d")"
    b="$d/${TRX_BUILD:-build}"
    echo "=== $n ($board)"
    # Vivado opens its log before build.tcl runs: the directory has to exist
    # (without it: "CRITICAL WARNING [Common 17-183] Failed to open handle build/vivado.log")
    mkdir -p "$b"
    (cd "$d" && vivado -mode batch -nojournal -log "${TRX_BUILD:-build}/vivado.log" -source build.tcl) \
      | grep -E "^(ERROR|CRITICAL WARNING|===)" || true
    if ! test -f "$b/$n.bit"; then
      echo "no bitstream for $n; post-synthesis utilization:"
      grep -E "^\| (Slice LUTs|LUT as Memory|Slice Registers|Block RAM Tile|DSPs|Bonded IOB) " \
        "$b/utilization_synth.rpt" 2>/dev/null || true
      echo "post-synthesis utilization by module (top of the hierarchy):"
      head -60 "$b/utilization_hier_synth.rpt" 2>/dev/null || true
      tail -40 "$b/vivado.log" 2>/dev/null || true
      exit 1
    fi
    grep -m1 -E "^Slack" "$b/timing.rpt" || true
    wns="$({ grep -A2 'WNS(ns)' "$b/timing.rpt" || true; } | sed -n 3p | awk '{print $1}')"
    echo "WNS $n$suffix: ${wns:-none} ns"
    grep -E "^=== (impl pass|kept:)" "$b/vivado.log" 2>/dev/null || true
    echo "$n$suffix ${wns:-none}" >> "$out/wns.txt"
    # the reports go out whether timing passed or not
    cp "$b/timing.rpt" "$out/$n$suffix.timing.rpt"
    cp "$b/utilization.rpt" "$out/$n$suffix.utilization.rpt"
    for r in utilization_hier utilization_synth utilization_hier_synth design_analysis qor_suggestions cdc; do
      cp "$b/$r.rpt" "$out/$n$suffix.$r.rpt" 2>/dev/null || true
    done
    gzip -c "$b/vivado.log" > "$out/$n$suffix.vivado.log.gz" 2>/dev/null || true
    # where the time goes: Vivado's own timer per command, elapsed
    grep -E '^(create_project|link_design|generate_target|synth_ip|synth_design|opt_design|place_design|phys_opt_design|route_design|write_checkpoint|write_bitstream|report_[a-z_]+): Time \(s\)' "$b/vivado.log" \
      | sed -E 's/^([a-z_]+): Time \(s\): cpu = ([0-9:.]+) ; elapsed = ([0-9:.]+).*/\1 \3 (cpu \2)/' > "$out/$n$suffix.steps.txt" || true
    echo "--- Vivado steps, $n$suffix (elapsed)"; cat "$out/$n$suffix.steps.txt"
    if [ -n "$wns" ] && awk -v w="$wns" 'BEGIN { exit !(w < 0) }'; then
      echo "utilization of the failed build (the package is not made):"
      grep -E "^\| (Slice LUTs|Slice Registers) " "$b/utilization.rpt" || true
      echo "timing failed for $n$suffix (WNS $wns ns); worst paths:"
      grep -A14 "Slack (VIOLATED)" "$b/timing.rpt" \
        | grep -E "Slack \(VIOLATED\)|Source:|Destination:|Path Group:|Requirement:|Data Path Delay:|Logic Levels:" \
        | head -48 || true
      exit 1
    fi
    cp "$b/$n.bit" "$out/$n$suffix.bit"
  done
  (cd "$out" && sha256sum *.bit > SHA256SUMS)
  git -C "$ROOT" rev-parse --short=8 HEAD > "$out/from"
  cache_put "$out" "$cache"
  : > "$cache/DONE"
  echo "built $(cat "$out/from")" > "$out/from"
fi
ls -l "$out"
: > "$out/DONE"

if [ "$board" = ue2 ]; then
  pub="$ROOT/out-ue2"
  rm -rf "$pub"; mkdir -p "$pub"
  cp "$out"/*.bit "$out"/*.rpt "$out"/*.gz "$out"/*.steps.txt "$pub/" 2>/dev/null || true
  python3 - "$out" "$pub" "$key" <<'PY'
import hashlib, json, os, sys
src, pub, key = sys.argv[1:]
wns = dict(l.split() for l in open(os.path.join(src, "wns.txt"))) if os.path.exists(os.path.join(src, "wns.txt")) else {}
files = {f: hashlib.sha256(open(os.path.join(pub, f), "rb").read()).hexdigest() for f in sorted(os.listdir(pub))}
json.dump({"part": "xc7a100tfgg484-1", "gateware_key": key, "from": open(os.path.join(src, "from")).read().strip(),
           "wns_ns": wns, "files": files}, open(os.path.join(pub, "manifest_100t.json"), "w"), indent=2)
PY
  publish_files "$pub"
  rm -rf "${HANDOFF:?}"
fi
echo "=== bit-$board done in $(lap)"
