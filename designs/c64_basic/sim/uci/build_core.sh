#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build c64_core (cores/icepi + c64_core.v/c64_mem.v/cart_slot.v) with
# uci_unit (through uci_flat.vhd, ghdl synth) into a Verilator model for
# tb_uci_core.cpp (T28 §7.1 on the real 6510). Needs ghdl and verilator on
# PATH (OSS CAD Suite). Run it niced: one simulation at a time on the Mac.
#   build_core.sh   -> build/tb_uci_core; run it from build/run
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../.." && pwd)
C=$root/cores/icepi
K=$C/vicii-kawari/hdl
D=$root/designs/c64_basic
out=$here/build
mkdir -p "$out/gen" "$out/run/mem"

n=$C/6502n
ghdl synth --std=08 -fsynopsys --out=verilog $n/pkg_6502_defs.vhd $n/pkg_6502_opcodes.vhd \
  $n/pkg_6502_decode.vhd $n/alu.vhd $n/bit_cpx_cpy.vhd $n/data_oper.vhd $n/implied.vhd \
  $n/shifter.vhd $n/proc_control.vhd $n/proc_interrupt.vhd $n/proc_registers.vhd $n/proc_core.vhd \
  -e proc_core > "$out/gen/proc_core.v" 2> "$out/gen/proc_core.log"
ghdl synth --std=08 -fsynopsys --out=verilog $C/c64_c1541/iecdrv_via6522.vhd \
  -e iecdrv_via6522 > "$out/gen/iecdrv_via6522.v" 2> "$out/gen/iecdrv_via6522.log"
"$root/tools/gideon_sources.sh"
G=$root/vendor/gideon/fpga
B=$G/ip/busses/vhdl_source
CI=$G/io/command_interface/vhdl_source
ghdl synth --std=08 -fsynopsys -frelaxed --out=verilog $B/io_bus_pkg.vhd $B/slot_bus_pkg.vhd \
  $B/io_bus_splitter.vhd $G/ip/synchroniser/vhdl_source/synchronizer_gzw.vhd $B/io_bus_bridge2.vhd \
  $G/ip/memory/vhdl_source/dpram.vhd $CI/command_if_pkg.vhd $CI/command_protocol.vhd \
  $CI/command_interface.vhd $D/uci_unit.vhd $here/uci_flat.vhd \
  -e uci_flat > "$out/gen/uci_flat.v" 2> "$out/gen/uci_flat.log"
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
  echo $D/c64_core.v $D/trx_dbg.v $D/c64_mem.v $D/cart_slot.v $D/slot_port.v $out/gen/proc_core.v $out/gen/iecdrv_via6522.v $out/gen/T65.v
  echo $out/gen/uci_flat.v $here/tb_uci_top.v
} > "$out/gen/files.f"

for f in $C/c64_redip_sid/reDIP-SID/gateware/*.mem $C/../trx_sid/mem/*.mem $K/*.bin $K/*.hex $C/usb_hid_host/rom/*.mem; do
  if [ -f "$f" ]; then ln -sf "$f" "$out/run/"; fi
done
for f in $C/c64_c1541/mem/*.mem $D/mem/*.mem; do ln -sf "$f" "$out/run/mem/"; done
# the stock ROMs from roms/, preloaded into c64_mem's ROM block (sim only,
# TRX_SIM_ROMS; on the board the app writes them)
python3 "$root/tools/rom2mem.py" "$out/run/mem"

verilator -DTRX_SIM_ROMS --cc --exe --build -j ${JOBS:-1} -O3 --x-assign fast --x-initial fast \
  -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-PROCASSWIRE \
  --top-module tb_uci_top -I$C -I$K -I$C/c64_redip_sid/reDIP-SID/gateware \
  --Mdir "$out/obj" -o tb_uci_core \
  $C/vicii_kawari.vh $K/common.vh -f "$out/gen/files.f" "$here/tb_uci_core.cpp" > "$out/verilator.log" 2>&1 \
  || { tail -30 "$out/verilator.log"; exit 1; }
cp "$out/obj/tb_uci_core" "$out/tb_uci_core"
echo "built $out/tb_uci_core"
