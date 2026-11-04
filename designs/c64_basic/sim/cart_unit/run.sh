#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Cartridge unit bench (tb_cart_unit.v) under iverilog: cart_slot with the
# descriptor-driven model (the app's descriptors, tools/cart_descriptors), cart_fill, trx_io. One process, niced.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
C=$root/cores/icepi
D=$root/designs/c64_basic
out=$here/build
mkdir -p "$out"
python3 "$root/tools/gen_cart_descriptors.py" --verilog "$out/desc_app.vh"
nice -n 15 iverilog -g2012 -I "$out" -o "$out/tb_cart_unit.vvp" -s tb_cart_unit "$here/tb_cart_unit.v" \
  $D/cart_slot.v $D/cart_fill.v $D/trx_io.v \
  $C/c64_cartridge.v $C/c64_flash_amd.v $C/c64_eeprom_93c86.v $C/c64_action_replay/c64_action_replay_pla.v
nice -n 15 vvp -n "$out/tb_cart_unit.vvp"
