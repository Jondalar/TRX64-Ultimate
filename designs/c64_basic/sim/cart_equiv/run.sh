#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Equivalence of the descriptor-driven cartridge (T33) with the per-type model
# it replaced: both cart_slots side by side on the same random bus stream, per
# type, once with descriptors that reproduce the old model (legacy/*.json) and
# once with the app's descriptors (tools/cart_descriptors), where only the
# listed intended differences may show (tb_cart_equiv.v). The old model is
# taken from git at LEGACY, the last commit that had it; iverilog, niced.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
C=$root/cores/icepi
D=$root/designs/c64_basic
out=$here/build
LEGACY=${LEGACY:-2701ce9}
mkdir -p "$out"
git -C "$root" show $LEGACY:cores/icepi/c64_cartridge.v | sed \
  -e 's/module c64_cartridge (/module c64_cartridge_legacy (/' \
  -e 's/c64_action_replay c64_action_replay_0/c64_action_replay_legacy c64_action_replay_0/' \
  -e 's/module c64_action_replay (/module c64_action_replay_legacy (/' \
  -e 's/LS163_4BIT_COUNTER/LS163_4BIT_COUNTER_legacy/g' \
  -e 's/LS273_D_FF_8BIT/LS273_D_FF_8BIT_legacy/g' \
  -e 's/LS74_D_FF/LS74_D_FF_legacy/g' > "$out/c64_cartridge_legacy.v"
git -C "$root" show $LEGACY:designs/c64_basic/cart_slot.v | sed \
  -e 's/^module cart_slot (/module cart_slot_legacy (/' \
  -e 's/    c64_cartridge model (/    c64_cartridge_legacy model (/' > "$out/cart_slot_legacy.v"
python3 "$root/tools/gen_cart_descriptors.py" --verilog "$out/desc_app.vh"
python3 "$root/tools/gen_cart_descriptors.py" --dir "$here/legacy" --verilog "$out/desc_legacy.vh"
sed -i.bak 's/function \[343:0\] cart_desc(/function [343:0] cart_desc_legacy(/; s/cart_desc = /cart_desc_legacy = /' "$out/desc_legacy.vh"
nice -n 15 iverilog -g2012 -I "$out" -o "$out/tb_cart_equiv.vvp" -s tb_cart_equiv "$here/tb_cart_equiv.v" \
  $D/cart_slot.v "$out/cart_slot_legacy.v" $C/c64_cartridge.v "$out/c64_cartridge_legacy.v" \
  $C/c64_flash_amd.v $C/c64_eeprom_93c86.v $C/c64_action_replay/c64_action_replay_pla.v
nice -n 15 vvp -n "$out/tb_cart_equiv.vvp" "$@"
