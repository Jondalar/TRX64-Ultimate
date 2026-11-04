#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run tools/pincheck.tcl in vivado-env on the NAS (T53). Reads, writes nothing there.
#
#   tools/pincheck.sh [part A [part B]]
set -euo pipefail
NAS="${TRX_NAS:-admin@192.168.242.2}"
DOCKER=/share/CACHEDEV1_DATA/.qpkg/container-station/bin/docker
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work=/share/CACHEDEV2_DATA/trx64-build/work/pincheck
ssh "$NAS" "rm -rf '$work' && mkdir -p '$work/designs/c64_basic' '$work/constraints' '$work/tools'"
scp -q "$ROOT"/designs/c64_basic/*.xdc "$NAS:$work/designs/c64_basic/"
scp -q "$ROOT/constraints/c64u.xdc" "$ROOT/constraints/mig_ddr2.prj" "$NAS:$work/constraints/"
scp -q "$ROOT/tools/pincheck.tcl" "$NAS:$work/tools/"
ssh "$NAS" "$DOCKER run --rm --cpus=1 -v '$work:/work' -e HOME=/tmp -e PART_A=${1:-xc7a50tfgg484-1} -e PART_B=${2:-xc7a100tfgg484-1} \
  -w /work vivado-env:2024.1 vivado -mode batch -nojournal -nolog -source tools/pincheck.tcl" | grep -E '^(DIFFERS|ERROR|xc7)'
