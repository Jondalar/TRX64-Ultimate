#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The two test cartridges of tb_dbgsess (T12 §14.8), built with sim/cart's carts.py: an EasyFlash
and a normal 16K cartridge, both with the cart bench's boot code and an empty test table (the
runner at $0800 ends at once and loops there). Every ROM byte is the pattern P(bank, half, off).
Usage: gen_crts.py DIR   -> DIR/ef.crt, DIR/n16.crt"""
import os
import sys

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, "..", "cart"))
import carts  # noqa: E402

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
carts.work = out                    # carts.asm() writes boot.bin and runner.bin there
ops = carts.Ops()
chips = []
for b in range(64):
    chips.append((b, 0x8000, carts.chip(b, 0)))
    chips.append((b, 0xA000, carts.with_boot(carts.chip(0, 1), ops, 0xE000, vectors=True) if b == 0 else carts.chip(b, 1)))
open(os.path.join(out, "ef.crt"), "wb").write(carts.crt("EASYFLASH", 32, 1, 0, chips))
c = carts.with_boot(carts.chip(0, 0), ops, 0x8000)
open(os.path.join(out, "n16.crt"), "wb").write(carts.crt("NORMAL 16K", 0, 0, 0, [(0, 0x8000, c), (0, 0xA000, carts.chip(0, 1))]))
