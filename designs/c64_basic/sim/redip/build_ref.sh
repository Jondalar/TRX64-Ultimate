#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T25.4: the reference = the reDIP SID exactly as it was before the T25.4 changes
# (the pinned base commit), verilated as it is, with the bench tb_ref.cpp.
# Needs git history down to BASE (not a shallow clone) and verilator on PATH.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
BASE=c534e15
# build_ref.sh [tag base offset]: with a tag, the 6581 curve of the reference is another one (T25.5 presets):
# SID 1 = base/offset, SID 2 = base-5 / offset-5 (the second chip, as the original is 245/-780 and 240/-785)
tag=$1
b=$here/build/ref$tag
rm -rf "$b/src"; mkdir -p "$b/src"
cd "$root"
for f in $(git ls-tree -r --name-only $BASE cores/icepi/c64_redip_sid | grep -v LICENSE); do
  git show $BASE:$f > "$b/src/$(basename $f)"
done
cd "$b/src"
if [ -n "$tag" ]; then
  sed -i.bak -e "s/9'd245;/9'd$2;/" -e "s/-11'sd780;/11'($3);/" -e "s/9'd240;/9'd$(($2-5));/" -e "s/-11'sd785;/11'($(($3-5)));/" sid_api.sv
  grep -n "fc_base\|fc_offset" sid_api.sv | head -4
fi
nice -n 15 verilator --cc --exe --build -j 1 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-lint --public-flat-rw \
  --top-module sid_api -I. \
  -CFLAGS "-O2 -std=c++17 -DREF_DEBUG" --Mdir "$b/obj" -o "$b/tb_ref" \
  sid_pkg.sv muladd.sv sid_dac.sv sid_pot.sv sid_waveform.sv sid_envelope.sv sid_voice.sv sid_filter.sv sid_control.sv sid_api.sv \
  "$here/tb_ref.cpp"
