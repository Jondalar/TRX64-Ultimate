#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# U20 F-b (D25): the Vivado builds of job `work` of .github/workflows/ephemeral-b.yml, on a server made from the snapshot of
# ci/ephemeral/snapshot.sh (Docker image named in /etc/gh-ephemeral-image, Vivado cache entries in /opt/trx-cache).
#
#   ci/ephemeral/vivado.sh OUT_DIR THREADS
#
# Two builds of `ci/bitstream.sh c64u` (every designs/*/build.tcl for the XC7A50T), as the builder runs it, in the image,
# each with an empty bitstream cache of its own, so both run Vivado:
#   warm   TRX_CACHE = a copy of /opt/trx-cache: the IP and out-of-context entries (tools/vivado_lib.tcl trx_ip, trx_ooc)
#          the builder's cache held when the snapshot was made; an entry whose key moved since is built again
#   cold   TRX_CACHE empty: nothing kept anywhere (no builder, no cache)
# TRX_VIVADO_THREADS = THREADS (general.maxThreads, tools/vivado_lib.tcl).
# OUT_DIR/<warm|cold>/ holds the stage's files (bitstreams, reports, *.steps.txt, wns.txt, key) and log.txt;
# OUT_DIR/times.txt "<name> <epoch seconds>" (warm_start, warm_end, cold_start, cold_end); OUT_DIR/info.txt the image
# record and THREADS. A failed build does not stop the other; the exit code is 1 when either failed.
set -euo pipefail
out=${1:?OUT_DIR}
threads=${2:?THREADS}
[[ "$threads" =~ ^[0-9]+$ ]] || { echo "THREADS must be a number, not '$threads'" >&2; exit 2; }
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
rec=/etc/gh-ephemeral-image
[ -r "$rec" ] || { echo "no $rec: this server was not made from the F-b snapshot" >&2; exit 1; }
image=$(awk '$1 == "image_ref" { print $2 }' "$rec")
want=$(awk '$1 == "image_id" { print $2 }' "$rec")
got=$(docker image inspect --format '{{.Id}}' "$image")
[ "$got" = "$want" ] || { echo "image $image is $got, the snapshot's record says $want" >&2; exit 1; }
mkdir -p "$out"
{ cat "$rec"; echo "threads $threads"; echo "nproc $(nproc)"; } > "$out/info.txt"
w="$ROOT/.eph"
rm -rf "$w"
rc=0
for b in warm cold; do
  mkdir -p "$w/$b/cache" "$w/$b/bits" "$w/$b/handoff" "$out/$b"
  if [ "$b" = warm ] && [ -d /opt/trx-cache ]; then cp -a /opt/trx-cache/. "$w/$b/cache/"; fi
  # a build directory of the previous build must not reach this one
  rm -rf "$ROOT"/designs/*/build
  echo "${b}_start $(date -u +%s)" >> "$out/times.txt"
  echo "=== $b build: TRX_CACHE with $(find "$w/$b/cache" -mindepth 1 -maxdepth 1 | wc -l) entries, $threads threads"
  if docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/w" -w /w \
       -e TRX_VIVADO_THREADS="$threads" -e TRX_CACHE="/w/.eph/$b/cache" -e CI_BIT_CACHE="/w/.eph/$b/bits" \
       -e CI_HANDOFF="/w/.eph/$b/handoff" -e CI_RUN_ID=eph \
       "$image" ci/bitstream.sh c64u > "$out/$b/log.txt" 2>&1; then
    echo "${b}_end $(date -u +%s)" >> "$out/times.txt"
  else
    echo "${b}_end $(date -u +%s)" >> "$out/times.txt"
    echo "${b}_failed 1" >> "$out/times.txt"
    rc=1
  fi
  tail -5 "$out/$b/log.txt"
  if [ -d "$w/$b/handoff/bit-c64u" ]; then cp -a "$w/$b/handoff/bit-c64u/." "$out/$b/"; fi
done
rm -rf "$w"
cat "$out/times.txt"
exit "$rc"
