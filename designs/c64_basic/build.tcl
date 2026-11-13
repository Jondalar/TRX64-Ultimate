# SPDX-License-Identifier: GPL-3.0-or-later
# Non-project build of c64_basic. Run through tools/vivado.sh or CI.
# TRX_PART picks the part (T53: xc7a100tfgg484-1 for the Ultimate 64 Elite II);
# TRX_BUILD the output directory, so builds for two parts do not overwrite each other.
set part xc7a50tfgg484-1
if {[info exists ::env(TRX_PART)] && $::env(TRX_PART) ne ""} { set part $::env(TRX_PART) }
set bdir build
if {[info exists ::env(TRX_BUILD)] && $::env(TRX_BUILD) ne ""} { set bdir $::env(TRX_BUILD) }
# capability bits 29:28: the FPGA type the app and our boot ROM read (itu.h
# CAPAB_FPGA_TYPE): 2 = XC7A50T, 3 = XC7A100T (flash layout, update, app address)
switch -glob $part {
  xc7a50t*  { set fpga_type 2 }
  xc7a100t* { set fpga_type 3 }
  default   { error "no FPGA type for part $part" }
}
set core ../../cores/icepi
set kawari $core/vicii-kawari/hdl
source ../../tools/vivado_lib.tcl
file mkdir $bdir
set ::trx_build $bdir

# The icepi sources read their init files by bare name ($readmemh "x.mem"),
# relative to where synthesis runs: put copies here.
foreach f [concat [glob $core/c64_redip_sid/reDIP-SID/gateware/*.mem] [glob $kawari/*.bin] \
                  [glob $core/usb_hid_host/rom/*.mem]] {
  file copy -force $f .
}
file copy -force {*}[glob $core/c64_c1541/mem/*.mem] mem/
# the T25.1 SID engine's tables (cores/trx_sid, read as ./sid_*.mem)
set trxsid ../../cores/trx_sid
file copy -force {*}[glob $trxsid/mem/*.mem] .

# --- C64 core, out of context and cached (c64_core.v wraps icepi's c64_top) ---
set core_read {
  # list follows icepi's c64_top.py
  read_verilog $core/vicii_kawari.vh $kawari/common.vh
  set_property is_global_include true [get_files [list $core/vicii_kawari.vh $kawari/common.vh]]
  read_vhdl -library work [list \
    $core/6502n/pkg_6502_defs.vhd $core/6502n/pkg_6502_opcodes.vhd $core/6502n/pkg_6502_decode.vhd \
    $core/6502n/alu.vhd $core/6502n/bit_cpx_cpy.vhd $core/6502n/data_oper.vhd $core/6502n/implied.vhd \
    $core/6502n/shifter.vhd $core/6502n/proc_control.vhd $core/6502n/proc_interrupt.vhd \
    $core/6502n/proc_registers.vhd $core/6502n/proc_core.vhd \
    $core/T65/T65_Pack.vhd $core/T65/T65_ALU.vhd $core/T65/T65_MCode.vhd $core/T65/T65.vhd \
    $core/c64_c1541/iecdrv_via6522.vhd ]
  read_verilog -sv [list \
    $core/c64_redip_cia/reDIP-CIA/gateware/CIA/cia_pkg.sv \
    {*}[lsort [glob $core/c64_redip_cia/reDIP-CIA/gateware/CIA/*.sv]] \
    $core/c64_redip_cia/cia_core.sv \
    $core/c64_redip_cia/reDIP-CIA/gateware/VIA/via_pkg.sv \
    {*}[lsort [glob $core/c64_redip_cia/reDIP-CIA/gateware/VIA/*.sv]] \
    $core/c64_redip_cia/via_core.sv \
    $core/c64_redip_sid/reDIP-SID/gateware/sid_pkg.sv \
    {*}[lsort [glob $core/c64_redip_sid/reDIP-SID/gateware/*.sv]] \
    $core/c64_redip_sid/sid_api.sv $core/c64_redip_sid/sid_multi.sv $core/c64_redip_sid/muladd.sv \
    {*}[glob $core/hdmi/*.sv] ]
  # icepi's .v files use SystemVerilog here and there (i++ in for loops);
  # yosys takes them as SV, so read them the same way.
  read_verilog -sv [list \
    {*}[glob $core/*.v] {*}[glob $core/cdc/*.v] $core/hdmi/hdmi.v \
    {*}[glob $core/c64_c1541/*.v] $core/c64_action_replay/c64_action_replay_pla.v \
    $core/c64_redip_sid/dc_blocker.v \
    {*}[glob $trxsid/rtl/*.v] \
    {*}[glob $core/usb_hid_host/rtl/*.v] \
    $kawari/videoram.v $kawari/serration.v $kawari/lumareg.v $kawari/equalization.v $kawari/divide.v \
    $kawari/colorreg.v $kawari/sprites.v $kawari/registers_scaled.v $kawari/raster.v \
    $kawari/pixel_sequencer.v $kawari/matrix.v $kawari/lightpen.v $kawari/hires_dvi_sync_scaled.v \
    $kawari/hires_pixel_sequencer.v $kawari/hires_matrix.v $kawari/hires_addressgen.v $kawari/cycles.v \
    $kawari/comp_sync.v $kawari/bus_access.v $kawari/border.v $kawari/addressgen_spartan.v $kawari/vicii.v ]

  read_verilog c64_core.v c64_mem.v cart_slot.v slot_port.v trx_dbg.v
  read_xdc -mode out_of_context c64_core_ooc.xdc
}
set core_files [concat [trx_tree $core] [trx_tree $trxsid/rtl] [trx_tree $trxsid/mem] c64_core.v c64_mem.v cart_slot.v slot_port.v trx_dbg.v c64_core_ooc.xdc [lsort [glob mem/*.mem]]]
# SID: reDIP, 8 SIDs, by default (T25.4). TRX_SID=redip2 in the environment builds the
# two-SID reDIP (c64_top.v `ifdef SID_REDIP), TRX_SID=1 the T25.1 engine of cores/trx_sid
# (Alex, 2026-10-02: it did not fit next to T20/T21/T28).
set core_synth [list -include_dirs [list $core $kawari $core/c64_redip_sid/reDIP-SID/gateware]]
# T25.4 (Alex 2026-10-07): the 8-SID reDIP is the default, so CI builds it for the hearing test;
# TRX_SID=redip2 builds the 2-SID reDIP of before.
if {![info exists ::env(TRX_SID)] || $::env(TRX_SID) eq "" || $::env(TRX_SID) eq "redip8"} {
  # T25.4: reDIP's pipelines for 8 SIDs behind the T25.1 decode and mixer
  lappend core_synth -verilog_define SID_MULTI
  puts "=== SID: reDIP, 8 SIDs (TRX_SID=redip8)"
} elseif {$::env(TRX_SID) ne "redip2" && $::env(TRX_SID) ne "0"} {
  puts "=== SID: T25.1 engine (TRX_SID)"
} else {
  lappend core_synth -verilog_define SID_REDIP
  puts "=== SID: reDIP"
}
# The real expansion port (T16): T16_SLOT=0 in the environment builds the P1
# idle port (slot_port.v never leaves its idle levels).
if {[info exists ::env(T16_SLOT)] && $::env(T16_SLOT) eq "0"} {
  lappend core_synth -generic SLOT_EN=0
  puts "=== expansion port: idle (T16_SLOT=0)"
}
lassign [trx_ooc c64_core $part $core_files $core_read $core_synth] core_dcp core_stub

create_project -in_memory -part $part
set_property target_language Verilog [current_project]

# GTP transmitters: 1.5 Gb/s from 150 MHz picks the same PLL dividers
# (N = 20, M = 1, D = 4) as the real 1.481-1.491 Gb/s from 148.06-149.14 MHz.
trx_ip gt_hdmi xilinx.com:ip:gtwizard [list \
  CONFIG.identical_val_tx_line_rate {1.5} \
  CONFIG.identical_val_tx_reference_clock {150.000} \
  CONFIG.identical_val_rx_line_rate {1.5} \
  CONFIG.identical_val_rx_reference_clock {150.000} \
  CONFIG.identical_val_no_rx {true} \
  CONFIG.gt0_val {true} CONFIG.gt1_val {true} CONFIG.gt2_val {true} CONFIG.gt3_val {true} \
  CONFIG.gt0_val_tx_refclk {REFCLK0_Q0} CONFIG.gt1_val_tx_refclk {REFCLK0_Q0} \
  CONFIG.gt2_val_tx_refclk {REFCLK0_Q0} CONFIG.gt3_val_tx_refclk {REFCLK0_Q0} \
  CONFIG.gt0_val_encoding {None} CONFIG.gt0_val_tx_data_width {20} CONFIG.gt0_val_tx_int_datawidth {20} \
  CONFIG.gt0_val_txbuf_en {true} CONFIG.gt0_usesharedlogic {1} CONFIG.gt_val_drp {false} \
]

# The DDR2 controller is ours (ddr2_ctrl.v: ddr2_lean.v + cal_seq.v, no IP, no MIG).

# --- board glue ---
# c64 is a black box until read_checkpoint -cell fills it in after synthesis.
set_msg_config -id {Project 1-486} -new_severity INFO
read_verilog -sv hdmi_gtp.v
read_verilog -sv [list {*}[glob $core/hdmi/*.sv] $core/hdmi/hdmi.v]
read_verilog $core_stub top.v vic_scaler.v pal_ram.v cdce_i2c.v mode_ctl.v c64u_keyboard.v bling_keyboard.v jtag_dbg.v ddr2_port.v ddr2_ctrl.v ddr2_lean.v cal_seq.v i2s_out.v sock_audio.v \
  con_ring.v udp_stream.v udp_audio.v stream_mux.v app_i2c.v overlay.v usb_phy.v iec_bus.v \
  cart_fill.v trx_io.v trx_sys.v dma_win.v pix_layer.v c64_text.v trx_engine.v drv_dbg.v app_input.v rv_dbg.v
read_verilog bling_io.v case_leds.v reu_ddr_map.v
# Gideon's RISC-V and the blocks around it (cores/gideon), our boot ROM package
# for his bootrom_8k, his 1541 drive (drive_unit.vhd, drive_mem.vhd), his
# command interface (the UCI, uci_unit.vhd), and ultimate_sys.vhd that puts
# them together.
set g ../../cores/gideon/fpga
read_vhdl -library work [list \
  $g/ip/busses/vhdl_source/io_bus_pkg.vhd $g/ip/busses/vhdl_source/mem_bus_pkg.vhd \
  $g/ip/busses/vhdl_source/io_bus_splitter.vhd $g/ip/busses/vhdl_source/mem_bus_arbiter_pri_32.vhd \
  $g/cpu_unit/rvlite/vhdl_source/core_pkg.vhd $g/cpu_unit/rvlite/vhdl_source/alu_pkg.vhd \
  $g/cpu_unit/rvlite/vhdl_source/alu_branch.vhd $g/cpu_unit/rvlite/vhdl_source/alu_logical.vhd \
  $g/cpu_unit/rvlite/vhdl_source/multiply.vhd $g/cpu_unit/rvlite/vhdl_source/dsram.vhd \
  $g/cpu_unit/rvlite/vhdl_source/gprf.vhd $g/cpu_unit/rvlite/vhdl_source/csr.vhd \
  $g/cpu_unit/rvlite/vhdl_source/decode_comb.vhd $g/cpu_unit/rvlite/vhdl_source/decode.vhd \
  $g/cpu_unit/rvlite/vhdl_source/fetch.vhd $g/cpu_unit/rvlite/vhdl_source/execute.vhd \
  $g/cpu_unit/rvlite/vhdl_source/writeback.vhd $g/cpu_unit/rvlite/vhdl_source/core.vhd \
  $g/cpu_unit/rvlite/vhdl_source/icache.vhd $g/cpu_unit/rvlite/vhdl_source/bus_converter.vhd \
  bootrom_pkg.vhd $g/cpu_unit/rvlite/vhdl_source/bootrom_8k.vhd \
  $g/cpu_unit/rvlite/vhdl_source/rvlite_wrapper.vhd \
  $g/ip/clock/vhdl_source/fractional_div.vhd $g/ip/srl_fifo/vhdl_source/srl_fifo-xilinx.vhd \
  $g/ip/sync_fifo/vhdl_source/sync_fifo.vhd \
  $g/io/uart_lite/vhdl_source/tx.vhd $g/io/uart_lite/vhdl_source/rx.vhd \
  $g/io/uart_lite/vhdl_source/uart_peripheral_io.vhd \
  $g/io/uart_lite/vhdl_source/slip_encoder.vhd $g/io/uart_lite/vhdl_source/slip_decoder.vhd \
  $g/io/uart_lite/vhdl_source/tx_dma.vhd $g/io/uart_lite/vhdl_source/rx_dma.vhd \
  $g/io/uart_lite/vhdl_source/uart_dma.vhd \
  $g/io/spi/vhdl_source/spi.vhd \
  $g/io/spi/vhdl_source/spi_peripheral_io.vhd \
  $g/io/itu/vhdl_source/itu_pkg.vhd $g/io/itu/vhdl_source/itu.vhd \
  $g/altera/u2p_io.vhd \
  $g/ip/async_fifo/vhdl_source/gray_code_pkg.vhd $g/ip/async_fifo/vhdl_source/async_fifo_ft.vhd \
  $g/ip/synchroniser/vhdl_source/level_synchronizer.vhd \
  $g/ip/synchroniser/vhdl_source/pulse_synchronizer.vhd \
  $g/ip/memory/vhdl_source/spram.vhd \
  $g/ip/free_queue/vhdl_source/block_bus_pkg.vhd $g/ip/free_queue/vhdl_source/free_queue.vhd \
  $g/io/rmii/vhdl_source/crc32.vhd $g/io/rmii/vhdl_source/rmii_transceiver.vhd \
  $g/io/rmii/vhdl_source/eth_filter.vhd $g/io/rmii/vhdl_source/eth_transmit.vhd \
  $g/io/rmii/vhdl_source/ethernet_interface.vhd \
  $g/ip/busses/vhdl_source/endianness_pkg.vhd \
  $g/ip/memory/vhdl_source/dpram_8x16.vhd $g/ip/memory/vhdl_source/dpram_8x32.vhd usb_dpram.vhd \
  $g/altera/distributed_stack.vhd $g/altera/align_read_to_bram.vhd \
  $g/ip/nano_cpu/vhdl_source/nano_cpu_pkg.vhd $g/ip/nano_cpu/vhdl_source/nano_alu.vhd \
  $g/ip/nano_cpu/vhdl_source/nano_cpu.vhd $g/ip/nano_cpu/vhdl_source/nano.vhd \
  $g/io/usb2/vhdl_source/usb_pkg.vhd $g/io/usb2/vhdl_source/usb_cmd_pkg.vhd \
  $g/io/usb2/vhdl_source/timer.vhd $g/io/usb2/vhdl_source/token_crc.vhd \
  $g/io/usb2/vhdl_source/token_crc_19.vhd $g/io/usb2/vhdl_source/token_crc_check.vhd \
  $g/io/usb2/vhdl_source/data_crc.vhd $g/io/usb2/vhdl_source/ulpi_bus.vhd \
  $g/io/usb2/vhdl_source/ulpi_rx.vhd $g/io/usb2/vhdl_source/ulpi_tx.vhd \
  $g/io/usb2/vhdl_source/usb_host_interface.vhd $g/io/usb2/vhdl_source/host_sequencer.vhd \
  $g/io/usb2/vhdl_source/bridge_to_mem_ctrl.vhd $g/io/usb2/vhdl_source/mem_addr_counter.vhd \
  $g/io/usb2/vhdl_source/mem_remain_counter.vhd $g/io/usb2/vhdl_source/usb_memory_ctrl.vhd \
  $g/io/usb2/vhdl_source/nano_minimal_io.vhd $g/io/usb2/vhdl_source/usb_cmd_nano.vhd \
  $g/io/usb2/vhdl_source/usb_debug.vhd $g/io/usb2/vhdl_source/usb_host_nano.vhd \
  $g/6502n/vhdl_source/pkg_6502_defs.vhd $g/6502n/vhdl_source/pkg_6502_opcodes.vhd \
  $g/6502n/vhdl_source/pkg_6502_decode.vhd $g/6502n/vhdl_source/alu.vhd \
  $g/6502n/vhdl_source/bit_cpx_cpy.vhd $g/6502n/vhdl_source/implied.vhd $g/6502n/vhdl_source/shifter.vhd \
  $g/6502n/vhdl_source/data_oper.vhd $g/6502n/vhdl_source/proc_control.vhd \
  $g/6502n/vhdl_source/proc_interrupt.vhd $g/6502n/vhdl_source/proc_registers.vhd \
  $g/6502n/vhdl_source/proc_core.vhd $g/6502n/vhdl_source/cpu6502.vhd \
  $g/ip/memory/vhdl_source/pseudo_dpram_8x32.vhd pseudo_dpram_8x32_x7.vhd \
  $g/io/sigma_delta_dac/vhdl_source/noise_generator.vhd \
  $g/1541/vhdl_source/c1541_pkg.vhd $g/1541/vhdl_source/via6522.vhd $g/1541/vhdl_source/cpu_part_1541.vhd \
  $g/1541/vhdl_source/c1541_timing.vhd $g/1541/vhdl_source/floppy_param_mem.vhd \
  $g/1541/vhdl_source/floppy_mem.vhd $g/1541/vhdl_source/floppy_stream.vhd $g/1541/vhdl_source/floppy.vhd \
  $g/1541/vhdl_source/floppy_sound.vhd $g/1541/vhdl_source/drive_registers.vhd \
  $g/1541/vhdl_source/c1541_drive.vhd drive_mem.vhd drive_unit.vhd \
  $g/1541/vhdl_source/bin2gcr.vhd $g/1541/vhdl_source/gcr2bin.vhd $g/1541/vhdl_source/gcr_codec.vhd \
  $g/ip/busses/vhdl_source/mem_bus_arbiter_pri.vhd $g/ip/busses/vhdl_source/mem_to_mem32.vhd \
  $g/ip/busses/vhdl_source/slot_bus_pkg.vhd $g/ip/synchroniser/vhdl_source/synchronizer_gzw.vhd \
  $g/ip/busses/vhdl_source/io_bus_bridge2.vhd $g/ip/memory/vhdl_source/dpram.vhd \
  $g/io/command_interface/vhdl_source/command_if_pkg.vhd \
  $g/io/command_interface/vhdl_source/command_protocol.vhd \
  $g/io/command_interface/vhdl_source/command_interface.vhd uci_unit.vhd \
  flash_gate.vhd ultimate_sys.vhd ]
read_xdc ddr2_pins.xdc
read_xdc pins.xdc
read_xdc gtp_pins.xdc
read_xdc usb_pins.xdc

# T46 (Alex 2026-11-13): the JTAG debug master (jtag_dbg, tools/trxdbg.py) only in a dev build. TRX_FEATURES
# names the optional blocks as in the MultiCore host; here jtag is the only one and it is off unless listed
# (TRX_FEATURES=jtag). A softload does not need it: the MultiCore host hands the app over in DDR2.
set top_defs {}
set trx_on {}
if {[info exists ::env(TRX_FEATURES)]} { set trx_on [split $::env(TRX_FEATURES) ", "] }
if {"jtag" ni $trx_on} { lappend top_defs -verilog_define NO_JTAG_DBG; puts "=== JTAG debug: off (TRX_FEATURES without jtag)" } \
else { puts "=== JTAG debug: on (TRX_FEATURES=jtag)" }
synth_design -top top -part $part -generic FPGA_TYPE=$fpga_type {*}$top_defs
read_checkpoint -cell c64 $core_dcp
report_utilization -file $bdir/utilization_synth.rpt
report_utilization -hierarchical -hierarchical_depth 8 -file $bdir/utilization_hier_synth.rpt
opt_design
# T29 (spec 12.10): the CPU core and the engine's slow-access state run on clk2x, but what the
# CPU is waiting on (RDY: VIC BA, pause, REU DMA, the cartridge's wait) is a function of clk-domain
# registers that settles over a whole clk cycle. The core takes RDY, and the engine
# completes a slow access, only on clk2x edges that gclk shares (g_edge: at normal speed the core's
# enable, in turbo pn and a stalled CPU), and in turbo mode the core's RDY is tied high (cpu6510.v
# rdy_latched): every path that ends on a clk2x register through cpu_rdy has two clk2x periods.
set rdy_nets [get_nets -quiet -hier -filter {NAME =~ c64/c64/cpu_rdy}]
if {[llength $rdy_nets] == 1} {
  set sid2x_c [get_clocks -of_objects [get_pins mmcm_sys/CLKOUT1]]
  set_multicycle_path 2 -setup -end -through $rdy_nets -to $sid2x_c
  set_multicycle_path 1 -hold  -end -through $rdy_nets -to $sid2x_c
  puts "=== T29: cpu_rdy paths to clk2x timed with two clk2x periods"
} else {
  puts "=== T29: WARNING cpu_rdy net not found ([llength $rdy_nets]), no multicycle path"
}
# The C64's RAM/ROM block RAMs carry three clocks (gclk on port A, clk and clk2x on port B and
# the staging). With 70 block RAMs of 75 in use the placer put some of them into clock region
# X1Y0, which already holds the DDR2, GTP, USB and Ethernet clocks, and failed with "13 clocks in a
# clock region" (run 352, [Place 30-410]): keep them out of that region.
create_pblock pb_c64mem
add_cells_to_pblock pb_c64mem [get_cells c64/mem]
resize_pblock pb_c64mem -add {CLOCKREGION_X0Y0:CLOCKREGION_X0Y2 CLOCKREGION_X1Y1:CLOCKREGION_X1Y2}
# The app CPU's register file -> multiplier path (rvlite, 100 MHz) failed on
# placement luck alone (run 289); patch 0006 (docs/specs/OPT-rvmul.md) pipelines
# the multiplier, the directives stay for the rest of the design.
write_checkpoint -force $bdir/opt.dcp

# One implementation pass: place, phys_opt, route, phys_opt. Returns the worst setup slack.
proc impl_pass {place_dir} {
  place_design -directive $place_dir
  phys_opt_design -directive AggressiveExplore
  route_design -directive Explore
  phys_opt_design -directive AggressiveExplore
  set p [get_timing_paths -setup -max_paths 1]
  if {[llength $p] == 0} { return 1000.0 }
  return [get_property SLACK $p]
}

# The chip is ~99.9 % slices, so placement decides timing. A failed first pass gets a
# second one with directives that spread logic / pessimise net delay, from the same
# post-opt checkpoint; the better of the two is kept (docs/architecture-reset-2026-10-05.md).
set wns [impl_pass ExtraTimingOpt]
puts "=== impl pass 1 (ExtraTimingOpt): WNS $wns ns"
if {$wns < 0} {
  write_checkpoint -force $bdir/routed_pass1.dcp
  set best $wns
  set best_dcp $bdir/routed_pass1.dcp
  foreach dir {AltSpreadLogic_high ExtraNetDelay_high} {
    open_checkpoint $bdir/opt.dcp
    set w [impl_pass $dir]
    puts "=== impl pass ($dir): WNS $w ns"
    if {$w > $best} {
      set best $w
      write_checkpoint -force $bdir/routed_alt.dcp
      set best_dcp $bdir/routed_alt.dcp
    }
    if {$w >= 0} { break }
  }
  open_checkpoint $best_dcp
  puts "=== kept: $best_dcp (WNS $best ns)"
}
write_checkpoint -force $bdir/routed.dcp
write_bitstream -force $bdir/c64_basic.bit
puts "=== bitstream: $bdir/c64_basic.bit"
report_utilization -file $bdir/utilization.rpt
report_utilization -hierarchical -hierarchical_depth 8 -file $bdir/utilization_hier.rpt
report_timing_summary -max_paths 5 -file $bdir/timing.rpt
# Analysis for the next change: where it is tight and what Vivado would do about it.
if {[catch {report_design_analysis -congestion -complexity -timing -max_paths 10 -file $bdir/design_analysis.rpt} e]} {
  puts "report_design_analysis: $e"
}
# Clock-domain crossings (check only, not a gate yet): summary plus every crossing with its safety class.
if {[catch {report_cdc -details -file $bdir/cdc.rpt} e]} {
  puts "report_cdc: $e"
}
if {[catch {report_qor_suggestions -max_paths 20 -file $bdir/qor_suggestions.rpt} e]} {
  puts "report_qor_suggestions: $e"
}
# every endpoint that fails, worst first, one path each
report_timing -delay_type max -max_paths 60 -nworst 1 -slack_lesser_than 0 -sort_by slack -file $bdir/timing_neg.rpt
# the turbo CPU's block RAM port: address, write enable, data pins and the data out (T29 §12.3), whatever their slack
report_timing -delay_type max -max_paths 4 -nworst 1 -to [get_pins -quiet -hier -filter {NAME =~ c64/mem/*/ADDRBWRADDR*}] -file $bdir/timing_bram_addr.rpt
report_timing -delay_type max -max_paths 4 -nworst 1 -to [get_pins -quiet -hier -filter {NAME =~ c64/mem/*/WEBWE*}] -file $bdir/timing_bram_we.rpt
report_timing -delay_type max -max_paths 4 -nworst 1 -to [get_pins -quiet -hier -filter {NAME =~ c64/mem/*/DIBDI*}] -file $bdir/timing_bram_di.rpt
report_timing -delay_type max -max_paths 4 -nworst 1 -from [get_pins -quiet -hier -filter {NAME =~ c64/mem/*/CLKBWRCLK}] -file $bdir/timing_bram_do.rpt
report_timing -delay_type max -max_paths 20 -nworst 1 -sort_by slack -to [get_clocks -of_objects [get_pins mmcm_sys/CLKOUT1]] -file $bdir/timing_clk2x.rpt
puts "=== done"
