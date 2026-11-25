#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the debugger-stop bench THROUGH ultimate_sys (tb_ultsys.cpp, T12 §14.8): tb_dbgs_top
# (sim/dbgsess) with the app's DMA window made by ultimate_sys.vhd itself (ghdl synth to Verilog, the
# RISC-V replaced by rvlite_bfm.v), in a Verilator model. Needs ghdl
# (the 6502n and 1541 VIA are VHDL: ghdl synth turns them into Verilog), verilator, 64tass and
# python3 on PATH, e.g. the OSS CAD Suite. Run it niced: one simulation at a time on the Mac.
# JOBS = verilator's -j (default 1).
#   build.sh [--trace]   -> build/tb_ultsys[_trace], run directory build/run
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

n=$C/6502n
ghdl synth --std=08 -fsynopsys --out=verilog $n/pkg_6502_defs.vhd $n/pkg_6502_opcodes.vhd \
  $n/pkg_6502_decode.vhd $n/alu.vhd $n/bit_cpx_cpy.vhd $n/data_oper.vhd $n/implied.vhd \
  $n/shifter.vhd $n/proc_control.vhd $n/proc_interrupt.vhd $n/proc_registers.vhd $n/proc_core.vhd \
  -e proc_core > "$out/gen/proc_core.v" 2> "$out/gen/proc_core.log"
ghdl synth --std=08 -fsynopsys --out=verilog $C/c64_c1541/iecdrv_via6522.vhd \
  -e iecdrv_via6522 > "$out/gen/iecdrv_via6522.v" 2> "$out/gen/iecdrv_via6522.log"
# T65 is not used (CPU_MODEL 1) and has a signal called "break": an empty shell
cat > "$out/gen/T65.v" <<'EOT'
module T65(input [1:0] Mode, input Res_n, Enable, Clk, Rdy, Abort_n, IRQ_n, NMI_n, SO_n,
           output R_W_n, output [15:0] A, input [7:0] DI, output [7:0] DO, output NMI_ack);
endmodule
EOT


# ultimate_sys.vhd and Gideon's VHDL it needs -> Verilog (ghdl synth). The file list is the one the
# synthesis has (build.tcl, as sim/input takes it); two entities do not go through ghdl and are
# stand-ins: the RISC-V (replaced again, in Verilog, by rvlite_bfm.v) and async_fifo_ft (Ethernet/USB
# only); the USB host and the drive are stand-ins too, for simulation speed (they are not on the
# window's path). Gideon's sources are our copy in vendor/gideon.
"$root/tools/gideon_sources.sh"
mkdir -p "$out/vh"
python3 - "$D/build.tcl" "$root/vendor/gideon/fpga" "$D" "$here" > "$out/vh/files.txt" <<'PY'
import sys
tcl, g, design, here = sys.argv[1:]
body = open(tcl).read().split("\nset g ", 1)[1].split("read_vhdl -library work [list", 1)[1].split("]", 1)[0]
for tok in body.replace("\\", " ").split():
    p = tok.replace("$g", g)
    p = p if p.startswith("/") else design + "/" + p
    if p.endswith("/rvlite_wrapper_dc.vhd"): p = here + "/rvlite_stub.vhd"
    if p.endswith("/async_fifo_ft.vhd"): p = here + "/async_fifo_stub.vhd"
    if p.endswith("/usb_host_nano.vhd"): p = here + "/usb_stub.vhd"
    if p.endswith("/drive_unit.vhd"): p = here + "/drive_stub.vhd"
    print(p)
PY
vopts="--std=08 -fsynopsys -frelaxed -Wno-hide -Wno-shared -Wno-library"
(cd "$out/vh" && rm -f *.cf &&
  ghdl -a $vopts --work=unisim "$here/../usb/unisim_sim.vhd" &&
  ghdl -i $vopts "$here/../usb/tl_string_util_pkg.vhd" $(cat files.txt) > /dev/null &&
  ghdl -m $vopts ultimate_sys > /dev/null &&
  ghdl --synth $vopts --out=verilog ultimate_sys > "$out/gen/ultimate_sys.v" 2> "$out/gen/ultimate_sys.log") ||
  { tail -20 "$out/gen/ultimate_sys.log"; exit 1; }
python3 "$here/gen.py" "$out/gen/ultimate_sys.v" "$out/gen" > /dev/null
python3 - "$here/tb_us_top.v" "$out/gen/tb_us_inst.vh" > "$out/gen/tb_us_top.v" <<'PY'
import sys
print(open(sys.argv[1]).read().replace("/*ULT_INST*/", open(sys.argv[2]).read()))
PY

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
  echo $D/c64_core.v $D/trx_dbg.v $D/c64_mem.v $D/cart_slot.v $D/slot_port.v $D/cart_fill.v $out/gen/proc_core.v $out/gen/iecdrv_via6522.v $out/gen/T65.v
  echo $D/dma_win.v $D/trx_sys.v $D/ddr2_port.v $D/sim/dbgsess/tb_dbgs_top.v $out/gen/tb_us_top.v $out/gen/ultimate_sys_nostub.v $out/gen/rvlite_bfm.v
} > "$out/gen/files.f"

# init files the sources read by bare name, relative to the run directory
for f in $C/c64_redip_sid/reDIP-SID/gateware/*.mem $C/../trx_sid/mem/*.mem $K/*.bin $K/*.hex $C/usb_hid_host/rom/*.mem; do
  if [ -f "$f" ]; then ln -sf "$f" "$out/run/"; fi
done
for f in $C/c64_c1541/mem/*.mem $D/mem/*.mem; do ln -sf "$f" "$out/run/mem/"; done
# the stock ROMs from roms/, preloaded into c64_mem's ROM block (sim only, TRX_SIM_ROMS)
python3 "$root/tools/rom2mem.py" "$out/run/mem"

verilator -DTRX_SIM_ROMS --cc --exe --build -j ${JOBS:-1} -O3 --x-assign fast --x-initial fast --savable \
  ${trace:+--trace -CFLAGS -DTRACE} -CFLAGS -I$out/gen \
  -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTH -Wno-PROCASSWIRE -Wno-PINMISSING -Wno-MULTIDRIVEN \
  --top-module tb_us_top -I$C -I$K -I$C/c64_redip_sid/reDIP-SID/gateware \
  --Mdir "$out/obj$suffix" -o tb_ultsys \
  $C/vicii_kawari.vh $K/common.vh -f "$out/gen/files.f" "$here/tb_ultsys.cpp" > "$out/verilator.log" 2>&1 \
  || { tail -30 "$out/verilator.log"; exit 1; }
rm -f "$out/tb_ultsys$suffix"
cp "$out/obj$suffix/tb_ultsys" "$out/tb_ultsys$suffix"
echo "built $out/tb_ultsys$suffix"
