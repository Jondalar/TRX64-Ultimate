#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One iverilog bench of designs/c64_basic/sim: compile with the design's own
# directory as module library (-y: a module is found by its file name) and as
# include path (-I: hdmi_modes.vh, U16) and run.
#
#   ci/sim_iv.sh tb_scaler.v [iverilog options...]
#
# With SIM_DEPS_LOG set (ci/sims.sh does) the repository files the compiler read
# are appended to it, one path per line: ci/sims.sh checks them against ci/sims.deps.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
here="$root/designs/c64_basic"
tb="$1"; shift
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
iverilog -g2012 -y "$here" -y "$here/sim" -I "$here" -M "all=$out/deps" -o "$out/tb.vvp" "$@" "$here/sim/$tb"
if [ -n "${SIM_DEPS_LOG:-}" ]; then
  sed "s|^$root/||" "$out/deps" | sort -u >> "$SIM_DEPS_LOG"
fi
(cd "$here/sim" && vvp -n "$out/tb.vvp")
