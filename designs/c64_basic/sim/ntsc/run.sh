#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T40 benches (Icarus, e.g. from the OSS CAD Suite, on PATH): mode_ctl with
# mode_req + cdce_mode (U16) against an MMCM DRP model; udp_stream in both frame
# families (vic_scaler: sim/scaler/run.sh, every HDMI row in both families). The KERNAL in each mode and the live
# switch: sim/lorenz (VMODE=n tb_lorenz boot, tb_lorenz modes). Output under build/.
set -e
here=$(cd "$(dirname "$0")" && pwd)
D=$here/../..
out=$here/build
mkdir -p "$out"
iverilog -g2012 -I"$D" -o "$out/mode_ctl.vvp" "$here/tb_mode_ctl.v" "$D/mode_ctl.v" "$D/mode_req.v" "$D/cdce_mode.v"
nice -n 15 vvp -n "$out/mode_ctl.vvp" | grep -v finish
for v in "" -DV60; do
  iverilog -g2012 $v -o "$out/udp.vvp" "$here/tb_udp_stream.v" "$D/udp_stream.v"
  nice -n 15 vvp -n "$out/udp.vvp" | grep -v finish
done
