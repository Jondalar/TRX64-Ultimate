#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# T44 §7: flash_gate.vhd with Gideon's SPI block and our flash model (three
# variants on the same pins) under ghdl. Every test runs with each variant
# driving MISO, with g_wren_pass false and true, g_4byte false and true; then
# the false/true streams are compared (§7 test 7) and a negative control shows
# that model 1 catches a CS gap that is too short. Needs ghdl on PATH (OSS CAD
# Suite). One process at a time, niced.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
"$root/tools/gideon_sources.sh"

out=$here/build
mkdir -p "$out"
cd "$out"
GH="nice -n 15 ghdl"
F="--std=08 -frelaxed -fsynopsys"
$GH -a $F $root/vendor/gideon/fpga/ip/busses/vhdl_source/io_bus_pkg.vhd \
    $root/vendor/gideon/fpga/io/spi/vhdl_source/spi.vhd \
    $root/vendor/gideon/fpga/io/spi/vhdl_source/spi_peripheral_io.vhd \
    $root/designs/c64_basic/flash_gate.vhd $here/flash_model.vhd $here/tb_flash_gate.vhd
$GH -e $F tb_flash_gate

# tb_cpu: Gideon's rvlite runs tools/flashgate/mailbox.S, the lists come from
# tools/flashgate_test.py --sim (the board stages without the board)
R=$root/vendor/gideon/fpga/cpu_unit/rvlite/vhdl_source
$GH -a $F $here/../usb/tl_string_util_pkg.vhd \
    $root/vendor/gideon/fpga/ip/busses/vhdl_source/mem_bus_pkg.vhd \
    $root/vendor/gideon/fpga/ip/busses/vhdl_source/mem_bus_arbiter_pri_32.vhd \
    $R/core_pkg.vhd $R/alu_pkg.vhd $R/alu_logical.vhd $R/alu_branch.vhd $R/multiply.vhd \
    $R/dsram.vhd $R/gprf.vhd $R/csr.vhd $R/decode_comb.vhd $R/decode.vhd $R/fetch.vhd \
    $R/execute.vhd $R/writeback.vhd $R/core.vhd $R/icache.vhd $R/bus_converter.vhd \
    $root/designs/c64_basic/bootrom_pkg.vhd $R/bootrom_8k.vhd \
    $root/designs/c64_basic/cache_cfg_pkg.vhd $root/designs/c64_basic/rv_dcache.vhd \
    $root/designs/c64_basic/rv_icache.vhd $root/designs/c64_basic/rvlite_wrapper_dc.vhd \
    $here/tb_cpu.vhd
$GH -e $F tb_cpu

NRAND=${NRAND:-20000}
fails=0
run() {
    name=$1; shift
    if $GH -r $F tb_flash_gate "$@" --assert-level=failure > "$name.log" 2>&1 &&
       grep -q "PASS" "$name.log" && ! grep -q "error)" "$name.log"; then
        echo "PASS $name  $(grep -o 'random: .*' "$name.log" | head -1)"
    else
        echo "FAIL $name (see $out/$name.log)"
        grep -m 5 "error\|failure" "$name.log" || true
        fails=$((fails + 1))
    fi
}

for wp in false true; do
    for fb in false true; do
        for m in 0 1 2; do
            run "wp${wp}_4b${fb}_miso$m" -gg_wren_pass=$wp -gg_4byte=$fb -gg_miso=$m \
                -gg_nrand=$NRAND -gg_seed=$((m + 1)) -gg_log="$out/chip_wp${wp}_4b${fb}_miso$m.txt"
        done
    done
done

# §7 test 7: the chip's byte streams with g_wren_pass false and true are the
# same except 0x06 <-> 0x04 as the opcode
for fb in false true; do
    for m in 0 1 2; do
        if python3 - "$out/chip_wpfalse_4b${fb}_miso$m.txt" "$out/chip_wptrue_4b${fb}_miso$m.txt" <<'PY'
import sys
a = open(sys.argv[1]).read().splitlines()
b = open(sys.argv[2]).read().splitlines()
ok = len(a) == len(b) and len(a) > 0
swaps = 0
for x, y in zip(a, b):
    if x == y:
        continue
    if x.startswith("04 ") and y.startswith("06 ") and x[3:] == y[3:]:
        swaps += 1
        continue
    ok = False
    print("differs:", x, "|", y)
    break
print(f"{len(a)} transfers, {swaps} WRDI/WREN swaps")
sys.exit(0 if ok and swaps > 0 else 1)
PY
        then echo "PASS equivalence 4b$fb miso$m"; else echo "FAIL equivalence 4b$fb miso$m"; fails=$((fails + 1)); fi
    done
done

# negative control: model 1 misses CS-high pulses under 90 ns. With the gate's
# 100 ns gap it sees every one; with a 1-clock gap the framing test must fail.
run "tdesel90_gap10" -gg_tdesel=90 -gg_only=5
if $GH -r $F tb_flash_gate -gg_tdesel=90 -gg_cs_gap=1 -gg_only=5 --assert-level=failure \
       > gap1.log 2>&1 && ! grep -q "error)" gap1.log; then
    echo "FAIL control: a 1-clock CS gap was not caught"
    fails=$((fails + 1))
else
    echo "PASS control: a 1-clock CS gap is caught ($(grep -c 'error)' gap1.log) errors)"
fi

# the board script's stages on rvlite + gate + model
T="python3 $root/tools/flashgate_test.py"
if $T stage0 --sim "$out" --out "$out/fe8000.bin" -gg_variant=2 > script0.log 2>&1 &&
   grep -q "JEDEC ID ef 40 18" script0.log && grep -q "all 0xFF" script0.log; then
    echo "PASS script stage 0 on rvlite"
else
    echo "FAIL script stage 0 (see $out/script0.log)"; fails=$((fails + 1))
fi
for v in 0 1 2; do
    if $T stage1 --sim "$out" --stage0 "$out/fe8000.bin" --control -gg_variant=$v > script1_$v.log 2>&1 &&
       grep -q "STAGE 1 PASS" script1_$v.log; then
        echo "PASS script stage 1 on rvlite, model $v ($(grep -c '^  ok' script1_$v.log) checks)"
    else
        echo "FAIL script stage 1, model $v (see $out/script1_$v.log)"; fails=$((fails + 1))
    fi
done
# a build with g_wren_pass true: the script must stop at its gate check
if ! $T stage1 --sim "$out" -gg_wren_pass=true > script1_wp.log 2>&1 &&
   grep -q "STOPPED by a guard in 'step 0" script1_wp.log; then
    echo "PASS script refuses a g_wren_pass build"
else
    echo "FAIL script ran on a g_wren_pass build (see $out/script1_wp.log)"; fails=$((fails + 1))
fi

# stage 2 (release build, g_wren_pass true): scratch erase/program/erase and
# the WEL = 0 block, plus the --wel1-probe (gate as the only barrier) in sim
for v in 0 1 2; do
    if $T stage2 --sim "$out" --out "$out/fe8000_s2_$v.bin" --wel1-probe -gg_wren_pass=true \
           -gg_variant=$v > script2_$v.log 2>&1 && grep -q "STAGE 2 PASS" script2_$v.log; then
        echo "PASS script stage 2 on rvlite, model $v ($(grep -c '^  ok' script2_$v.log) checks)"
    else
        echo "FAIL script stage 2, model $v (see $out/script2_$v.log)"; fails=$((fails + 1))
    fi
done
# stage 2 on a g_wren_pass false build: the script must stop at its gate check
if ! $T stage2 --sim "$out" --out "$out/fe8000_s2_wp0.bin" > script2_wp0.log 2>&1 &&
   grep -q "STOPPED by a guard in 'stage 2 step 0" script2_wp0.log; then
    echo "PASS script stage 2 refuses a g_wren_pass false build"
else
    echo "FAIL script stage 2 ran on a g_wren_pass false build (see $out/script2_wp0.log)"; fails=$((fails + 1))
fi

[ $fails -eq 0 ] && echo "ALL PASS" || { echo "$fails FAILED"; exit 1; }
