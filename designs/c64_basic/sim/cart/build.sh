#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build tb_cart_top (c64_core with its cartridge, cart_fill and mig_arb) into
# a Verilator model for tb_cart.cpp. Needs ghdl (the 6502n and 1541 VIA are
# VHDL: ghdl synth turns them into Verilog) and verilator on PATH, e.g. from
# the OSS CAD Suite. JOBS = verilator's -j (default 1).
#   [GEN_FROM=dir] build.sh [--trace]   -> build/tb_cart[_trace], run directory build/run
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
C=$root/cores/icepi
K=$C/vicii-kawari/hdl
D=$root/designs/c64_basic
out=$here/build
trace= suffix=
for a in "$@"; do
  case $a in
    --trace) trace=1; suffix=_trace ;;
    *) echo "unknown option $a"; exit 1 ;;
  esac
done
mkdir -p "$out/gen" "$out/run/mem"
# the app's descriptor table, as the app has it (software/io/c64/trx_cart_desc.h)
python3 "$root/tools/gen_cart_descriptors.py" --c-header "$out/gen/trx_cart_desc.h"

n=$C/6502n
# The two VHDL cores (6502n, the 1541's VIA) become Verilog with ghdl. Without ghdl: GEN_FROM=<a gen/
# directory of an earlier build> supplies proc_core.v and iecdrv_via6522.v (they only change with the VHDL).
if command -v ghdl >/dev/null 2>&1; then
  ghdl synth --std=08 -fsynopsys --out=verilog $n/pkg_6502_defs.vhd $n/pkg_6502_opcodes.vhd \
    $n/pkg_6502_decode.vhd $n/alu.vhd $n/bit_cpx_cpy.vhd $n/data_oper.vhd $n/implied.vhd \
    $n/shifter.vhd $n/proc_control.vhd $n/proc_interrupt.vhd $n/proc_registers.vhd $n/proc_core.vhd \
    -e proc_core > "$out/gen/proc_core.v" 2> "$out/gen/proc_core.log"
  ghdl synth --std=08 -fsynopsys --out=verilog $C/c64_c1541/iecdrv_via6522.vhd \
    -e iecdrv_via6522 > "$out/gen/iecdrv_via6522.v" 2> "$out/gen/iecdrv_via6522.log"
else
  [ -n "$GEN_FROM" ] && [ -f "$GEN_FROM/proc_core.v" ] || { echo "no ghdl and no GEN_FROM=<gen dir with proc_core.v>"; exit 1; }
  cp "$GEN_FROM/proc_core.v" "$GEN_FROM/iecdrv_via6522.v" "$out/gen/"
fi
# T65 is not used (CPU_MODEL 1) and has a signal called "break": an empty shell
cat > "$out/gen/T65.v" <<'EOT'
module T65(input [1:0] Mode, input Res_n, Enable, Clk, Rdy, Abort_n, IRQ_n, NMI_n, SO_n,
           output R_W_n, output [15:0] A, input [7:0] DI, output [7:0] DO, output NMI_ack);
endmodule
EOT

{
  echo $C/c64_redip_cia/reDIP-CIA/gateware/CIA/cia_pkg.sv
  ls $C/c64_redip_cia/reDIP-CIA/gateware/CIA/*.sv | grep -v cia_pkg
  echo $C/c64_redip_cia/cia_core.sv
  echo $C/c64_redip_cia/reDIP-CIA/gateware/VIA/via_pkg.sv
  ls $C/c64_redip_cia/reDIP-CIA/gateware/VIA/*.sv | grep -v via_pkg
  echo $C/c64_redip_cia/via_core.sv
  echo $C/c64_redip_sid/reDIP-SID/gateware/sid_pkg.sv
  ls $C/c64_redip_sid/reDIP-SID/gateware/*.sv | grep -v sid_pkg
  echo $C/c64_redip_sid/sid_api.sv $C/c64_redip_sid/muladd.sv
  ls $C/hdmi/*.sv $C/*.v $C/cdc/*.v $C/c64_c1541/*.v $C/usb_hid_host/rtl/*.v
  echo $C/hdmi/hdmi.v $C/c64_action_replay/c64_action_replay_pla.v $C/c64_redip_sid/dc_blocker.v
  ls $C/../trx_sid/rtl/*.v
  for f in videoram serration lumareg equalization divide colorreg sprites registers_scaled raster \
           pixel_sequencer matrix lightpen hires_dvi_sync_scaled hires_pixel_sequencer hires_matrix \
           hires_addressgen cycles comp_sync bus_access border addressgen_spartan vicii; do
    echo $K/$f.v
  done
  echo $D/c64_core.v $D/trx_dbg.v $D/c64_mem.v $D/cart_slot.v $D/slot_port.v $D/cart_fill.v $D/dma_win.v $D/ddr2_port.v $here/tb_cart_top.v $out/gen/proc_core.v $out/gen/iecdrv_via6522.v $out/gen/T65.v
} > "$out/gen/files.f"

# init files the sources read by bare name, relative to the run directory
for f in $C/c64_redip_sid/reDIP-SID/gateware/*.mem $C/../trx_sid/mem/*.mem $K/*.bin $K/*.hex $C/usb_hid_host/rom/*.mem; do
  if [ -f "$f" ]; then ln -sf "$f" "$out/run/"; fi
done
for f in $C/c64_c1541/mem/*.mem $D/mem/*.mem; do ln -sf "$f" "$out/run/mem/"; done
# the stock ROMs from roms/, preloaded into c64_mem's ROM block (sim only,
# TRX_SIM_ROMS; on the board the app writes them)
python3 "$root/tools/rom2mem.py" "$out/run/mem"

verilator -DTRX_SIM_ROMS --cc --exe --build -j ${JOBS:-1} -O3 --x-assign fast --x-initial fast \
  ${trace:+--trace -CFLAGS -DTRACE} -CFLAGS -I$out/gen \
  -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-PROCASSWIRE \
  --top-module tb_cart_top -I$C -I$K -I$C/c64_redip_sid/reDIP-SID/gateware \
  --Mdir "$out/obj$suffix" -o tb_cart \
  $C/vicii_kawari.vh $K/common.vh -f "$out/gen/files.f" "$here/tb_cart.cpp" > "$out/verilator.log" 2>&1 \
  || { tail -30 "$out/verilator.log"; exit 1; }
rm -f "$out/tb_cart$suffix"
cp "$out/obj$suffix/tb_cart" "$out/tb_cart$suffix"
echo "built $out/tb_cart$suffix"
