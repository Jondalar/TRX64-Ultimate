#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T12 §14.8 through ultimate_sys: a debugger stop (no C64_STOP) opens the D page for the app's window.
# Builds the bench (ghdl synth of ultimate_sys, Verilator), boots the machine, runs the tests.
# One process at a time, niced; ghdl and verilator on PATH (OSS CAD Suite).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
nice -n 15 sh "$here/build.sh"
cd "$here/build/run"
# the boot (cold start to READY, ~90 s) comes from the CI's boot-state cache when the model binary and what
# the boot reads are unchanged (ci/bootstate.sh); without /cache it boots every time
nice -n 15 "$root/ci/bootstate.sh" -b ../tb_ultsys -k cores -k roms -k designs/c64_basic/mem -k tools/rom2mem.py \
    -s ../tb.state ultsys -- ../tb_ultsys boot ../tb.state
nice -n 15 ../tb_ultsys run ../tb.state
