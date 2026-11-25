#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore ci/lane.sh (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 B. Changed here: our lanes (no SDK lane, no
# host lane, one flat bitstream, one core) and the core lane's steps (software/trx64u's build.sh and pack_cor.sh).
# One cell of the matrix - one step of one lane - run by its job in .github/workflows/matrix.yml. Each cell prints what it checked; a red cell stops its
# lane and the lanes that need it.
#
#   ci/lane.sh <lane> <step>
#     bits:c64_basic   key | fetch | report     (the entry c64u-<key> of ci/keys.sh: the builder builds a missing key, ci/fetch_bits.sh restores it)
#     core:trx64u      changed | build | package | deliver | emu
#   The tree lane has step 1 only (ci/lint.sh tree, its own job).
#
# The steps (MultiCore T239.5 D8): 1 lint (ci/lint.sh <lane>), 2 unit tests (the bitstream lane: the benches, on demand, job sims; the core has no unit
# tests of its own), 3 changed, 4 comparison (the reverse lint of step 1; the TOML A against the RTL waits for the SDK's rtl_check.py fix, U18 §5),
# 5 what must be in it and 7 complete and fits (both inside pack_cor.sh: app_build.py's link against the TOML A and B, the symbol check, cor_pack.py's
# --ops check, the heap check), 6 build, 8-10 none (the SDK's coreapp test units name no trx64u test), 11 package, 12 emulator (emu, its own job),
# 13 deliver (the artifact of the job).
# env: CI_HANDOFF (the hand-over directory: the dispatcher's plan.json), CI_BIT_CACHE (the restored bitstreams), SDK_FETCH (fetch_sdk.sh's source:
# --release on GitHub, the default), CI_EMU_ROMS_CACHE (a restored copy of the emulator's ROM folder).
set -euo pipefail
# shellcheck source=lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"
# ci/pkg.env, a value of the environment first (tests point MC_BUILDS_REPO at another repository)
for v in MC_BUILDS_REPO MC_SRC_REPO BUILDS_TAG_PREFIX; do
  [ -n "${!v:-}" ] || printf -v "$v" %s "$(sed -n "s/^$v=//p" "$CI_CODE/ci/pkg.env")"
done
lane="${1:?usage: ci/lane.sh <lane> <step>}" step="${2:?usage: ci/lane.sh <lane> <step>}"
die() { echo "LANE RED: $lane $step: $*" >&2; exit 1; }
say() { echo "=== $lane $step: $*"; }
differs() {   # differs <base> <pathspec>... : yes or no; a failing git diff (exit > 1) is red, not "yes"
  local rc=0; git -C "$ROOT" diff --quiet "$1" HEAD -- "${@:2}" || rc=$?
  case $rc in 0) echo no ;; 1) echo yes ;; *) die "git diff $1..HEAD failed (exit $rc)" ;; esac
}
entry() {   # the one entry of ci/keys.sh (an assignment in the caller: a failure ends the step)
  local e; e="$("$CI_CODE/ci/fetch_bits.sh" --names)"
  [[ "$e" =~ ^c64u-[0-9a-f]{16}$ ]] || die "ci/keys.sh printed '$e', not one entry c64u-<key>"
  printf '%s\n' "$e"
}
# the version the .cor carries: a v<x.y.z> tag's x.y.z, else <branch>-<sha8> (as the Gitea stage cor names it)
cor_version() {
  local ref; ref="${CI_BRANCH:-${GITHUB_REF_NAME:-}}"
  if [ "${CI_REF_TYPE:-${GITHUB_REF_TYPE:-}}" = tag ] && [[ "$ref" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then echo "${BASH_REMATCH[1]}"
  else echo "$(branch_slug)-$(git -C "$ROOT" rev-parse --short=8 HEAD)"; fi
}

case "$lane" in
bits:c64_basic)
  case "$step" in
    # 3: the content key (ci/keys.sh: the cache entry and the name of its release)
    key)
      e="$(entry)"
      say "$e, release ${BUILDS_TAG_PREFIX}bit-$e in $MC_BUILDS_REPO" ;;
    # 6, 11: the entry from the builds repository (the builder built a missing one on the dispatcher's work order; this job waited at `bitstreams`)
    fetch) "$CI_CODE/ci/fetch_bits.sh" ;;
    # 7: what the builder reported: the commit it built from, WNS per design (wns.txt), the timing verdict and the LUTs of c64_basic
    report)
      e="$(entry)"; d="${CI_BIT_CACHE:?CI_BIT_CACHE}/$e"
      [ -f "$d/DONE" ] || die "no restored entry $d (step fetch)"
      [ -s "$d/c64_basic.bit" ] || die "$e holds no c64_basic.bit"
      say "$e: built from $(cat "$d/from" 2>/dev/null || echo ?)"
      [ ! -f "$d/wns.txt" ] || sed 's/^/    WNS (ns) /' "$d/wns.txt"
      # awk, not grep | head: a report without such a line is no failure (MultiCore run 45476822869: grep's exit 1 ended the step under pipefail)
      for f in "$d"/c64_basic.timing.rpt "$d"/c64_basic.utilization.rpt; do
        [ -f "$f" ] || continue
        awk -v n="${f##*/}" '/All user specified timing constraints are met|Timing constraints are not met/ { print "    " n ": " $0 }
                              /^\| Slice LUTs/ && !l { gsub(/[ ]+/, " "); print "    " n ": " $0; l = 1 }' "$f"
      done ;;
    *) die "the bitstream lane has no step $step" ;;
  esac ;;

core:trx64u)
  T="$ROOT/software/trx64u"; out="$HANDOFF/core-trx64u"; d="$HANDOFF/deliver-trx64u"
  mkdir -p "$out"
  case "$step" in
    # 3: what this push changed for the core against the dispatcher's base: the gateware (the pathspecs of gateware_key in ci/lib.sh; the key hashes
    # blob ids, so "yes" here means a new key), its TOML A, the app (software/trx64u)
    changed)
      [ -f "$HANDOFF/plan.json" ] || die "no plan.json from the dispatcher"
      base="$(python3 -I -c 'import json,sys; print((json.load(open(sys.argv[1])).get("base") or {}).get("commit") or "")' "$HANDOFF/plan.json")"
      gw=unknown ta=unknown ap=unknown
      if [ -n "$base" ] && git -C "$ROOT" cat-file -e "$base^{commit}" 2>/dev/null; then
        gw="$(differs "$base" "${GATEWARE_SPECS[@]}")"
        ta="$(differs "$base" software/trx64u/gateware/c64_basic.toml)"
        ap="$(differs "$base" software/trx64u)"
      fi
      printf '%s\n' "gateware=$gw" "toml_a=$ta" "app=$ap" > "$out/changed"
      say "the gateware: $gw, its TOML A: $ta, the app: $ap (base ${base:-none})" ;;
    # 6: the SDK from its pinned release (sdk.pin sdk_release, D11), then our libraries and the two sub-apps, trex_crt and online (build.sh)
    build)
      sh "$T/fetch_sdk.sh" "${SDK_FETCH:---release}"
      sh "$T/build.sh" ;;
    # 11 (with 5 and 7): the app linked from the SDK with the TOML A and B, the symbol check, the .cor with this commit's bitstream, its ops and heap checks
    package)
      e="$(entry)"; bit="${CI_BIT_CACHE:?CI_BIT_CACHE}/$e/c64_basic.bit"
      [ -s "$bit" ] || die "no $e/c64_basic.bit among the restored bitstreams (the lane bits:c64_basic)"
      ver="$(cor_version)"
      say "$e/c64_basic.bit, version $ver"
      sh "$T/pack_cor.sh" --no-build --bit "$bit" --bit-commit "$(git -C "$ROOT" rev-parse --short=8 HEAD)" --version "$ver" --out "$out/cor/trx64u.cor" ;;
    # 13: the .cor, the two sub-apps it carries (U20 D22: the fw- pre-release ships them beside it, checked to be the bytes in the .cor's records 7 and 8),
    # the app's ELF (the emulator's symbols) and deliver.json (what ci/prerelease.sh puts into manifest.json)
    deliver)
      [ -s "$out/cor/trx64u.cor" ] || die "no trx64u.cor (step package)"
      e="$(entry)"
      rm -rf "$d"; mkdir -p "$d"
      cp "$out/cor/trx64u.cor" "$d/trx64u.cor"
      cp "$T/build/trex_crt/trex_crt.u2a" "$T/build/online/online.u2a" "$d/"
      python3 -I - "$T/tools" "$d" <<'PY' || die "the sub-apps are not the ones trx64u.cor carries"
import os, sys
sys.path.insert(0, sys.argv[1])
import emu_ops
try:
    _, recs, _ = emu_ops.read_cor(os.path.join(sys.argv[2], "trx64u.cor"))
    emu_ops.check_subapps(recs, {n: open(os.path.join(sys.argv[2], n), "rb").read() for n in emu_ops.SUBAPPS})
except emu_ops.Failed as e:
    sys.exit("deliver: %s" % e)
print("    " + ", ".join(sorted(emu_ops.SUBAPPS)) + ": the bytes trx64u.cor carries")
PY
      cp "$T/build/app/app.elf" "$d/trx64u.elf"
      pin() { sed -n "s/^$1=//p" "$T/sdk.pin"; }
      python3 -I - "$d/deliver.json" "$(git -C "$ROOT" rev-parse HEAD)" "$(cor_version)" "$e" "${BUILDS_TAG_PREFIX}bit-$e" \
        "$(sha256sum "${CI_BIT_CACHE:?}/$e/c64_basic.bit" | cut -d' ' -f1)" "$(pin sdk_version)" "$(pin source_commit)" "$(pin sdk_release)" "$(pin sdk_tarball_sha256)" <<'PY'
import json, sys
out, commit, ver, entry, tag, bitsha, sdkv, sdkc, sdkr, sdks = sys.argv[1:]
json.dump({"commit": commit, "version": ver, "bitstream": {"entry": entry, "release": tag, "c64_basic.bit": bitsha},
           "sdk": {"version": sdkv, "commit": sdkc, "release": sdkr, "tarball_sha256": sdks}}, open(out, "w"), indent=2, sort_keys=True)
PY
      (cd "$d" && sha256sum trx64u.cor trex_crt.u2a online.u2a trx64u.elf deliver.json | tee SHA256SUMS) ;;
    # 12: the .cor in ue2emu with its own app (software/trx64u/tools/emu_ops.py: READY., the ping of the monitor library, the operations and states the
    # running app registers against base/ops_trx64u.txt, both ways)
    emu)
      [ -s "$d/trx64u.cor" ] && [ -s "$d/trx64u.elf" ] || die "no trx64u.cor and trx64u.elf (artifact deliver-trx64u)"
      (cd "$d" && sha256sum -c --quiet SHA256SUMS) || die "the artifact deliver-trx64u does not match its SHA256SUMS"
      emu="$(command -v ue2emu || true)"; [ -n "$emu" ] || die "no ue2emu on PATH (the emulator image)"
      roms="$HANDOFF/emu-roms"
      if [ -n "${CI_EMU_ROMS_CACHE:-}" ] && [ -d "$CI_EMU_ROMS_CACHE" ]; then "$CI_CODE/ci/emu_roms.sh" "$roms" --from "$CI_EMU_ROMS_CACHE"
      else
        "$CI_CODE/ci/emu_roms.sh" "$roms"
        [ -z "${CI_EMU_ROMS_CACHE:-}" ] || cp -R "$roms" "$CI_EMU_ROMS_CACHE"      # the job's cache saves it under the commit's key
      fi
      rm -rf "$HANDOFF/emu"; mkdir -p "$HANDOFF/emu"
      python3 -I "$T/tools/emu_ops.py" --ue2emu "$emu" --roms "$roms" --cor "$d/trx64u.cor" --app "$d/trx64u.elf" \
        --check "$T/base/ops_trx64u.txt" --out "$HANDOFF/emu/ops.txt" ;;     # no --keep: its work directory holds the ROMs, which no artifact may carry
    *) die "the core lane has no step $step" ;;
  esac ;;

tree) die "the tree lane has step 1 only (ci/lint.sh tree)" ;;
*) die "unknown lane (bits:c64_basic, core:trx64u)" ;;
esac
say "green"
