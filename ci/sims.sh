#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Stage `sims`: the benches listed in ci/sims.txt, in trx64-sim-env.
#
#   ci/sims.sh [-t TIER] [-f] [-j JOBS] [name ...]     TIER 1 (default) or 2; 2 runs both
#   ci/sims.sh --check-deps name ...         run the benches in a copy that holds only
#                                            the files ci/sims.deps declares for them
#
# The benches that have to run go JOBS at a time (-j, CI_SIMS_JOBS from the runner's config
# envs, default 3 for the NAS's three cores; the longest first). Tier 2 takes at most
# TIER2_JOBS of them at once whatever the runner says: its benches are whole machines. A bench passes
# when its command exits 0, its output has a PASS (or PASSED) line and no FAIL,
# FAILED or TIMEOUT line, and it ends
# within its timeout. A failing bench does not stop the others: the summary
# at the end lists all of them, and the stage fails if one did.
#
# Per-bench cache (T31): a bench has a key over the content of exactly the files
# ci/sims.deps lists for it (no line there: the whole tree), its line in ci/sims.txt
# and the simulator versions. A PASS leaves a marker $CI_SIMS_OK/bench/<name>.<key>;
# a bench whose key has a marker is not run again ("cached PASS"). -f ignores the
# markers. Only a PASS writes one, and only when everything the bench reported
# reading (ci/sim_iv.sh does, through SIM_DEPS_LOG) is covered by its deps.
#
# ROMs (U20 A6): the bench ROMs are not tracked. Before a bench runs, the pins of
# ci/roms.txt it names are fetched into the tree (ci/roms.py, size and SHA-256
# checked; store ROMS_STORE, else /cache/roms). A source that cannot be reached
# makes the bench SKIPPED (not red, no marker); a wrong hash makes it FAIL.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

# This script was once part of every bench's key, so a comment here threw away all
# 40 PASS markers and the stage then ran every bench (run 1113, 2026-10-31; 40 of
# them do not fit in the stage's 120 minutes). The key carries this number instead:
# raise it by one when a change here can change a verdict - the verdict rules, the
# timeout handling, what a bench is given to read, the command it is run with. A
# comment, a message or a helper that cannot change an outcome leaves it alone.
KEY_EPOCH=1

tier=1 force="" check="" jobs="${CI_SIMS_JOBS:-3}" jobs_given=""
while [ $# -gt 0 ]; do
  case "$1" in
    -t) tier="$2"; shift 2 ;;
    -f) force=1; shift ;;
    -j) jobs="$2"; jobs_given=1; shift 2 ;;
    --check-deps) check=1; force=1; shift ;;
    *) break ;;
  esac
done
# The tier-2 benches are whole-machine Verilator and GHDL runs; too many at once
# starve each other and die in their own timeouts instead of failing on their
# content. Run 1116 (2026-10-31) started all 16 at once on the builder's
# CI_SIMS_JOBS=16 and ultsys, which had passed in 103 s four-up, timed out after
# 900 s, as did flash_gate, flash_boot, input and redip_new. So tier 2 runs
# TIER2_JOBS at a time and the runner's number is a ceiling, not an order; -j on
# the command line still wins, for a machine that can take more.
TIER2_JOBS=4
if [ "$tier" = 2 ] && [ -z "$jobs_given" ] && [ "$jobs" -gt "$TIER2_JOBS" ]; then
  echo "tier 2: $jobs at a time would starve the heavy benches, taking $TIER2_JOBS"
  jobs=$TIER2_JOBS
fi
# A bench whose timeout is this long gets the machine to itself (Alex, 2026-11-01:
# "Die Langlaeufer nur alleine und nicht parallel"). turbo measured it: 2043 s alone,
# and a timeout after 2400 s with three others beside it. The long ones run after the
# others, one at a time, longest first.
SOLO_TIMEOUT=2400
want=" $* "
[ -z "$check" ] || [ "$want" != "  " ] || { echo "--check-deps needs bench names" >&2; exit 2; }
ok_dir="${CI_SIMS_OK:-/cache/sims-ok}/bench"
logdir="${CI_SIM_LOGS:-$ROOT/.ci/sims}"
mkdir -p "$logdir"
cd "$ROOT"
cache_on=""
if mkdir -p "$ok_dir" 2>/dev/null && [ -w "$ok_dir" ]; then cache_on=1; fi
versions="trx64-sim-env:4 epoch $KEY_EPOCH $(tool_versions)"

# deps_of <bench>: the specs of its line in ci/sims.deps, macros replaced, on one line.
# Exit 3: no line for it; 1: the file is wrong (a macro that is not defined).
deps_of() {
  awk -v want="$1" '
    function resolve(tok,   n, i, parts, out) {
      if (tok !~ /^@/) return tok
      if (!(tok in text)) { bad = 1; return "" }
      n = split(text[tok], parts, /[ \t]+/); out = ""
      for (i = 1; i <= n; i++) if (parts[i] != "") out = out " " resolve(parts[i])
      return out
    }
    /^[ \t]*(#|$)/ { next }
    /^[ \t]/ { if (cur != "") text[cur] = text[cur] " " $0; next }
    {
      key = $1
      if (key ~ /^@/) { i = index($0, "="); } else { sub(/:$/, "", key); i = index($0, ":"); }
      text[key] = substr($0, i + 1); cur = key
    }
    END {
      if (!(want in text)) exit 3
      n = split(text[want], parts, /[ \t]+/); out = ""
      for (i = 1; i <= n; i++) if (parts[i] != "") out = out " " resolve(parts[i])
      if (bad) exit 1
      print out
    }' "$ROOT/ci/sims.deps" 2>/dev/null
}

# bench_specs <bench>: the pathspecs its key is made of, one per line, and whether they
# are its own (declared) or the whole tree's.
declare_specs() {
  local s rc
  s="$(deps_of "$1")"; rc=$?
  if [ $rc -eq 0 ] && [ -n "$s" ]; then
    printf 'declared\n'; printf '%s\n' $s
  else
    [ $rc -eq 3 ] || echo "ci/sims.deps is wrong for $1: whole tree" >&2
    printf 'tree\n'; printf '%s\n' designs cores vendor tools ci
  fi
}

# bench_key <bench> <command>: key and kind (declared | tree) of the bench.
bench_key() {
  local name="$1" cmd="$2" kind k specs=()
  { read -r kind; while IFS= read -r k; do specs+=("$k"); done; } < <(declare_specs "$name")
  k="$(files_key "${specs[@]}")" || {
    echo "key of $name: a declared path is missing, whole tree instead" >&2
    kind=tree
    k="$(files_key designs cores vendor tools ci)"
  }
  printf '%s %s\n' "$(printf '%s\n%s\n%s\n%s\n' "$k" "$name" "$cmd" "$versions" | sha256sum | cut -c1-16)" "$kind"
}

# verdict_of <log> <rc> <tmo>
verdict_of() {
  if [ "$2" -eq 124 ]; then echo "TIMEOUT after ${3}s"
  elif [ "$2" -ne 0 ]; then echo "FAIL (exit $2)"
  elif grep -qE 'FAIL|TIMEOUT' "$1"; then echo "FAIL (output)"
  elif ! grep -qE '(^|[^A-Za-z])PASS(ED)?([^A-Za-z]|$)' "$1"; then echo "FAIL (no PASS line)"
  else echo PASS
  fi
}

# sandbox <bench>: a directory with the files the bench declares and nothing else.
sandbox() {
  local dir s kind specs=() k
  dir="$(mktemp -d)"
  { read -r kind; while IFS= read -r k; do specs+=("$k"); done; } < <(declare_specs "$1")
  [ "$kind" = declared ] || { echo "$1 has no declaration to check" >&2; return 1; }
  git -C "$ROOT" ls-files -s -z -- "${specs[@]}" | python3 -c '
import os, shutil, sys
root, dst = sys.argv[1:3]
for ent in sys.stdin.buffer.read().split(b"\0"):
    if not ent: continue
    meta, path = ent.split(b"\t", 1)
    path = path.decode()
    src, to = os.path.join(root, path), os.path.join(dst, path)
    os.makedirs(os.path.dirname(to), exist_ok=True)
    shutil.copy2(src, to, follow_symlinks=False)
' "$ROOT" "$dir" || return 1
  printf '%s\n' "$dir"
}

# plan: what to run, with the keys
names=() tmos=() cmds=() tiers=() keys=() kinds=() cached=()
n_run=0
while IFS='|' read -r t name tmo cmd; do
  case "$t" in ''|'#'*) continue ;; esac
  if [ -z "$check" ]; then [ "$t" -le "$tier" ] || continue; fi
  [ "$want" = "  " ] || [[ "$want" == *" $name "* ]] || continue
  read -r key kind < <(bench_key "$name" "$cmd")
  names+=("$name"); tmos+=("$tmo"); cmds+=("$cmd"); tiers+=("$t"); keys+=("$key"); kinds+=("$kind")
  if [ -z "$force" ] && [ -n "$cache_on" ] && [ -f "$ok_dir/$name.$key" ]; then
    cached+=(1)
  else
    cached+=(""); n_run=$((n_run + 1))
  fi
done < "$ROOT/ci/sims.txt"
[ ${#names[@]} -gt 0 ] || { echo "no bench selected"; exit 1; }

[ "$n_run" -eq 0 ] || heavy_lock

# roms_gate <bench> <dir>: the pinned upstream ROMs the bench reads (ci/roms.txt, U20 D14) are made to exist under <dir>.
# Sets rom_verdict: empty = go on; "SKIPPED (no ROM: <path>)" = a source could not be reached (not red, not cached);
# "FAIL (ROM: <path>)" = size or SHA-256 wrong. MultiCore's form (T239.1 D12, S16).
roms_gate() {
  local out rc
  rom_verdict=""
  [ -n "$(python3 -I "$ROOT/ci/roms.py" need "$1")" ] || return 0
  out="$(python3 -I "$ROOT/ci/roms.py" fetch --root "$2" --bench "$1" --strict 2>&1)"; rc=$?
  printf '%s\n' "$out" >> "$logdir/$1.out"
  case $rc in
    0) ;;
    3) rom_verdict="SKIPPED (no ROM: $(printf '%s\n' "$out" | sed -n 's/^roms: MISSING \([^ ]*\) .*/\1/p' | head -1))" ;;
    *) rom_verdict="FAIL (ROM: $(printf '%s\n' "$out" | sed -n 's/^roms: BAD \([^ ]*\) .*/\1/p' | head -1))" ;;
  esac
}

# run_one <bench>: in a worker. What the stage prints for the bench goes to
# $logdir/<bench>.out, the verdict to <bench>.result (verdict|seconds|kind|note).
run_one() {
  local name="$1" tmo cmd key kind verdict note="" rc start secs sb specs k missing log
  # shellcheck disable=SC1090
  source "$logdir/$name.job"
  start=$(date +%s)
  log="$logdir/$name.log"
  rm -f "$logdir/$name.deps" "$logdir/$name.out"
  echo "--- start $name"
  if [ -n "$check" ]; then
    if ! sb="$(sandbox "$name")"; then echo "no sandbox for $name" > "$logdir/$name.out"; echo "no sandbox||$kind|" > "$logdir/$name.result"; return 0; fi
    roms_gate "$name" "$sb"
    if [ -n "$rom_verdict" ]; then rm -rf "$sb"; printf '%s|0|%s|\n' "$rom_verdict" "$kind" > "$logdir/$name.result"; printf -- '--- done %s: %s\n' "$name" "$rom_verdict"; return 0; fi
    (cd "$sb" && timeout "$tmo" bash -c "set -o pipefail; $cmd") > "$log" 2>&1 < /dev/null
    rc=$?
    rm -rf "$sb"
  elif roms_gate "$name" "$ROOT"; [ -n "$rom_verdict" ]; then
    printf '%s|0|%s|\n' "$rom_verdict" "$kind" > "$logdir/$name.result"; printf -- '--- done %s: %s\n' "$name" "$rom_verdict"; return 0
  else
    SIM_DEPS_LOG="$logdir/$name.deps" timeout "$tmo" bash -c "set -o pipefail; $cmd" > "$log" 2>&1 < /dev/null
    rc=$?
  fi
  secs=$(( $(date +%s) - start ))
  verdict="$(verdict_of "$log" "$rc" "$tmo")"
  if [ "$verdict" = PASS ] && [ -z "$check" ] && [ "$kind" = declared ] && [ -s "$logdir/$name.deps" ]; then
    # the files the bench says it read, against what its deps cover
    specs=()
    { read -r _; while IFS= read -r k; do specs+=("$k"); done; } < <(declare_specs "$name")
    git -C "$ROOT" ls-files -- "${specs[@]}" | sort -u > "$logdir/$name.covered"
    missing="$(sort -u "$logdir/$name.deps" | grep -vxFf "$logdir/$name.covered" || true)"
    if [ -n "$missing" ]; then
      { echo "DEPS INCOMPLETE: $name read, and ci/sims.deps does not list:"; printf '  %s\n' $missing; } >> "$logdir/$name.out"
      note=" (deps incomplete, not cached)"
      kind=incomplete
    fi
  fi
  if [ "$verdict" != PASS ]; then
    tail -25 "$log" >> "$logdir/$name.out"
  elif [ -z "$check" ] && [ -n "$cache_on" ] && [ "$kind" != incomplete ]; then
    printf '%s %s\n' "$(git -C "$ROOT" rev-parse --short=8 HEAD)" "$(date -u +%FT%TZ)" > "$ok_dir/$name.$key"
    # shellcheck disable=SC2012
    ls -1t "$ok_dir/$name".* | tail -n +4 | xargs rm -f
  fi
  printf '%s|%s|%s|%s\n' "$verdict$note" "$secs" "$kind" "$note" > "$logdir/$name.result"
  printf -- '--- done %s: %s, %ds\n' "$name" "$verdict$note" "$secs"
}

run_names=()
for i in "${!names[@]}"; do
  [ -z "${cached[$i]}" ] || continue
  name="${names[$i]}"
  printf 'tmo=%q cmd=%q key=%q kind=%q\n' "${tmos[$i]}" "${cmds[$i]}" "${keys[$i]}" "${kinds[$i]}" > "$logdir/$name.job"
  run_names+=("${tmos[$i]} $name")
done
if [ ${#run_names[@]} -gt 0 ]; then
  export ROOT logdir check cache_on ok_dir
  export -f run_one verdict_of declare_specs deps_of sandbox roms_gate
  solo=() para=()
  for entry in "${run_names[@]}"; do
    if [ "${entry%% *}" -ge "$SOLO_TIMEOUT" ]; then solo+=("$entry"); else para+=("$entry"); fi
  done
  echo "=== ${#run_names[@]} of ${#names[@]} benches to run: ${#para[@]} at $jobs at a time, ${#solo[@]} alone"
  if [ ${#para[@]} -gt 0 ]; then
    printf '%s\n' "${para[@]}" | sort -rn | cut -d' ' -f2 | xargs -P "$jobs" -I{} bash -c 'run_one "$1"' _ {}
  fi
  if [ ${#solo[@]} -gt 0 ]; then
    echo "=== the long benches, one at a time (timeout >= ${SOLO_TIMEOUT}s)"
    printf '%s\n' "${solo[@]}" | sort -rn | cut -d' ' -f2 | xargs -P 1 -I{} bash -c 'run_one "$1"' _ {}
  fi
fi

failed=()
summary=()
for i in "${!names[@]}"; do
  name="${names[$i]}"
  if [ -n "${cached[$i]}" ]; then
    echo "=== $name (tier ${tiers[$i]}): cached PASS ${keys[$i]} ($(cat "$ok_dir/$name.${keys[$i]}"))"
    summary+=("$(printf '%-16s %-22s %4ds  %s' "$name" "cached PASS" 0 "${keys[$i]} ${kinds[$i]}")")
    continue
  fi
  echo "=== $name (tier ${tiers[$i]}) key ${keys[$i]} ${kinds[$i]}"
  [ ! -f "$logdir/$name.out" ] || cat "$logdir/$name.out"
  if [ ! -f "$logdir/$name.result" ]; then
    failed+=("$name"); summary+=("$(printf '%-16s %-22s' "$name" "no result")"); continue
  fi
  IFS='|' read -r verdict secs _ < "$logdir/$name.result"
  note=""
  case "$verdict" in
    PASS*) ;;
    SKIPPED*) note="  (no ROM source reached: not red, not cached)" ;;
    *) failed+=("$name") ;;
  esac
  summary+=("$(printf '%-16s %-22s %4ss%s' "$name" "$verdict" "$secs" "$note")")
done

echo
echo "=== summary (tier $tier)"
printf '%s\n' "${summary[@]}"
if [ ${#failed[@]} -gt 0 ]; then echo "FAILED: ${failed[*]}"; exit 1; fi
echo "=== sims done in $(lap)"
