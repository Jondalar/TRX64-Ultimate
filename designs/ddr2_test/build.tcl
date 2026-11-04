# SPDX-License-Identifier: GPL-3.0-or-later
# Non-project build of ddr2_test. Run through tools/vivado.sh or CI.
set part xc7a50tfgg484-1
source ../../tools/vivado_lib.tcl
file mkdir build
create_project -in_memory -part $part
set_property target_language Verilog [current_project]

# ddr2_lean (designs/c64_basic/ddr2_lean.v), our own DDR2 controller and PHY: no IP.
read_verilog top.v ddr2_tester.v ../c64_basic/cal_seq.v ../c64_basic/ddr2_lean.v ../c64_basic/jtag_dbg.v
read_xdc ../c64_basic/ddr2_pins.xdc
read_xdc pins.xdc

synth_design -top top -part $part
report_utilization -file build/utilization_synth.rpt
opt_design
place_design
route_design
write_checkpoint -force build/routed.dcp
write_bitstream -force build/ddr2_test.bit
puts "=== bitstream: build/ddr2_test.bit"
report_utilization -file build/utilization.rpt
report_timing_summary -max_paths 5 -file build/timing.rpt
puts "=== done"
