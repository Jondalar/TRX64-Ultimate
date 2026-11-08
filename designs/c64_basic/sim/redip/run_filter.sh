#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# U7: run the filter precision measurement (tb_filter.cpp). Builds if needed.
#   ./run_filter.sh [FILTER_FRAC of the RTL] [passes per point] [--csv]
# The bench compares the RTL against the model at the same setting (that check must stay exact)
# and both against the same filter in double precision.
set -e
here=$(cd "$(dirname "$0")" && pwd)
F=${1:-0}
shift 2>/dev/null || true
b=$here/build/filter$F
[ -x "$b/tb_filter" ] || "$here/build_filter.sh" "$F"
cd "$b/run"                       # dac_6581_cutoff.mem is read from the working directory
exec nice -n 15 "$b/tb_filter" --rtlf "$F" "$@"
