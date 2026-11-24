#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build tb_dbg_top (sim/drive's bench, c64_core, drive A through
# drive_flat.vhd, iec_bus.v, mig_arb with a MIG model, plus the debug
# hardware: trx_dbg.v in c64_core, drv_dbg.v) with tb_dbg.cpp into a
# Verilator model for the T21 tests (§9.1). Needs ghdl and verilator on PATH,
# e.g. from the OSS CAD Suite. Run it niced: one simulation at a time.
#
#   build.sh [--trace]   -> build/tb_dbg[_trace], run directory build/run
set -e
here=$(cd "$(dirname "$0")" && pwd)
drv=$here/../drive
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

n=$C/6502n
ghdl synth --std=08 -fsynopsys --out=verilog $n/pkg_6502_defs.vhd $n/pkg_6502_opcodes.vhd \
  $n/pkg_6502_decode.vhd $n/alu.vhd $n/bit_cpx_cpy.vhd $n/data_oper.vhd $n/implied.vhd \
  $n/shifter.vhd $n/proc_control.vhd $n/proc_interrupt.vhd $n/proc_registers.vhd $n/proc_core.vhd \
  -e proc_core > "$out/gen/proc_core.v" 2> "$out/gen/proc_core.log"
ghdl synth --std=08 -fsynopsys --out=verilog $C/c64_c1541/iecdrv_via6522.vhd \
  -e iecdrv_via6522 > "$out/gen/iecdrv_via6522.v" 2> "$out/gen/iecdrv_via6522.log"
G=$root/vendor/gideon/fpga
"$root/tools/gideon_sources.sh"
B=$G/ip/busses/vhdl_source; N=$G/6502n/vhdl_source; L=$G/1541/vhdl_source
ghdl synth --std=08 -fsynopsys --out=verilog $B/io_bus_pkg.vhd $B/mem_bus_pkg.vhd $B/endianness_pkg.vhd \
  $B/io_bus_splitter.vhd $B/mem_bus_arbiter_pri.vhd $B/mem_bus_arbiter_pri_32.vhd $B/mem_to_mem32.vhd \
  $G/ip/clock/vhdl_source/fractional_div.vhd \
  $N/pkg_6502_defs.vhd $N/pkg_6502_opcodes.vhd $N/pkg_6502_decode.vhd $N/alu.vhd $N/bit_cpx_cpy.vhd \
  $N/implied.vhd $N/shifter.vhd $N/data_oper.vhd $N/proc_control.vhd $N/proc_interrupt.vhd \
  $N/proc_registers.vhd $N/proc_core.vhd $N/cpu6502.vhd \
  $G/ip/memory/vhdl_source/pseudo_dpram_8x32.vhd $D/pseudo_dpram_8x32_x7.vhd \
  $G/io/sigma_delta_dac/vhdl_source/noise_generator.vhd \
  $L/c1541_pkg.vhd $L/via6522.vhd $L/cpu_part_1541.vhd $L/c1541_timing.vhd $L/floppy_param_mem.vhd \
  $L/floppy_mem.vhd $L/floppy_stream.vhd $L/floppy.vhd $L/floppy_sound.vhd $L/drive_registers.vhd \
  $L/c1541_drive.vhd $D/drive_mem.vhd $D/drive_unit.vhd $drv/drive_flat.vhd \
  -e drive_flat > "$out/gen/drive_flat.v" 2> "$out/gen/drive_flat.log"
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
  echo $D/c64_core.v $D/trx_dbg.v $D/c64_mem.v $D/cart_slot.v $D/slot_port.v $out/gen/proc_core.v $out/gen/iecdrv_via6522.v $out/gen/T65.v
  echo $out/gen/drive_flat.v $D/iec_bus.v $D/ddr2_port.v $D/sim/mig_ui_model.v $D/drv_dbg.v $here/tb_dbg_top.v
} > "$out/gen/files.f"

# init files the sources read by bare name, relative to the run directory
for f in $C/c64_redip_sid/reDIP-SID/gateware/*.mem $C/../trx_sid/mem/*.mem $K/*.bin $K/*.hex $C/usb_hid_host/rom/*.mem; do
  if [ -f "$f" ]; then ln -sf "$f" "$out/run/"; fi
done
for f in $C/c64_c1541/mem/*.mem $D/mem/*.mem; do ln -sf "$f" "$out/run/mem/"; done

# the C64 ROMs preloaded into c64_mem's ROM block (mem/c64_rom.mem, with
# TRX_SIM_ROMS; on the board the app writes them)
python3 "$root/tools/rom2mem.py" "$out/run/mem"

verilator -DTRX_SIM_ROMS --cc --exe --build -j ${JOBS:-1} -O3 --x-assign fast --x-initial fast --savable \
  ${trace:+--trace -CFLAGS -DTRACE} \
  -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-PROCASSWIRE \
  --top-module tb_dbg_top -I$C -I$K -I$C/c64_redip_sid/reDIP-SID/gateware \
  --Mdir "$out/obj$suffix" -o tb_dbg \
  $C/vicii_kawari.vh $K/common.vh -f "$out/gen/files.f" "$here/tb_dbg.cpp" > "$out/verilator.log" 2>&1 \
  || { tail -30 "$out/verilator.log"; exit 1; }
rm -f "$out/tb_dbg$suffix"
cp "$out/obj$suffix/tb_dbg" "$out/tb_dbg$suffix"
echo "built $out/tb_dbg$suffix"
