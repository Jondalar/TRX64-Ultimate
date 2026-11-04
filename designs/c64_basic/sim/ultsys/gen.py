#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# gen.py <ultimate_sys_gen.v> <out dir>: from the ghdl-synthesized Verilog,
#  - tb_us_inst.vh: the instance of ultimate_sys with every input idle (1 for the ones that idle high)
#    and the C64 side's wires bound by name,
#  - rvlite_bfm.v: the RISC-V stand-in module in place of the stub's, same name and ports,
#  - ultimate_sys_nostub.v: the generated file without the stub module.
import re, sys
src = open(sys.argv[1]).read()
out = sys.argv[2]
m = re.search(r'^module (rvlite_wrapper_\w+)\n(.*?\);)\n', src, re.S | re.M)
name, hdr = m.group(1), m.group(2)
end = src.index('endmodule', m.start()) + len('endmodule')
nostub = src[:m.start()] + src[end:]
open(out + '/ultimate_sys_nostub.v', 'w').write(re.sub(r'\bevent\b', 'event_', nostub))
body = '''
  // io access the C++ bench makes: x_go pulses one request, x_done says its ack came
  reg x_go /*verilator public_flat_rw*/ = 1'b0;
  reg x_we /*verilator public_flat_rw*/ = 1'b0;
  reg [23:0] x_addr /*verilator public_flat_rw*/ = 24'd0;
  reg [7:0] x_wdata /*verilator public_flat_rw*/ = 8'd0;
  reg x_done /*verilator public_flat_rw*/ = 1'b0;
  reg [7:0] x_rdata /*verilator public_flat_rw*/ = 8'd0;
  reg        x_busy = 1'b0, started = 1'b0, rd = 1'b0, wr = 1'b0;
  assign \\io_req[read]  = rd;
  assign \\io_req[write]  = wr;
  assign \\io_req[address]  = x_addr;
  assign \\io_req[data]  = x_wdata;
  assign io_busy = 1'b0;
  assign \\mem_req[tag]  = 8'd0;
  assign \\mem_req[request]  = 1'b0;
  assign \\mem_req[read_writen]  = 1'b1;
  assign \\mem_req[address]  = 26'd0;
  assign \\mem_req[byte_en]  = 4'd0;
  assign \\mem_req[data]  = 32'd0;
  assign dbg_pc = 32'd0;
  always @(posedge clock) begin
    rd <= 1'b0; wr <= 1'b0;
    if (x_go && !x_busy) begin
      rd <= !x_we; wr <= x_we; x_busy <= 1'b1; started <= 1'b0; x_go <= 1'b0; x_done <= 1'b0;
    end else if (x_busy) begin
      started <= 1'b1;
      if (started && \\io_resp[ack] ) begin x_busy <= 1'b0; x_done <= 1'b1; x_rdata <= \\io_resp[data] ; end
    end
  end
endmodule
'''
open(out + '/rvlite_bfm.v', 'w').write('module ' + name + '\n' + hdr + body)
# the instance
um = re.search(r'^module ultimate_sys\n\s*\((.*?)\);\n', src, re.S | re.M)
ports = []
for p in um.group(1).split(','):
    p = p.strip()
    mm = re.match(r'(input|output)\s+(\[(\d+):0\]\s+)?(\w+)$', p)
    ports.append((mm.group(1), int(mm.group(3)) + 1 if mm.group(3) else 1, mm.group(4)))
bound = {
    'clock': 'cpu_clk', 'reset': 'cpu_rst', 'eth_clock': 'cpu_clk', 'eth_reset': 'cpu_rst', 'c64_clock': 'clk',
    'dma_req_tgl': 'dma_req_tgl', 'dma_addr': 'dma_addr', 'dma_we': 'dma_we', 'dma_wdata': 'dma_wdata',
    'dma_bus': 'dma_bus', 'dma_rom': 'dma_rom', 'dma_done_tgl': 'dma_done_tgl', 'dma_rdata': 'dma_rdata',
    'dbg_stop': 'dbg_stop_o', 'c64_pause_req': 'app_stop', 'c64_ultimax_req': 'app_ultimax',
    'c64_reset_req': 'app_reset',
}
bound['keys'] = 'ult_keys'
bound['joys'] = 'ult_joys'
bound['restore_pin'] = 'ult_restore'
bound['scanlines'] = 'ult_scan_o'
bound['c64_dbg_tgl'] = 'ult_dbg_tgl'
bound['c64_dbg_data'] = 'ult_dbg_data'
bound['dbg_stop'] = '(dbg_stop_o & ~ign_dbg)'   # ign_dbg: control run, the old behaviour (ultimate_sys never sees the stop)
high = {'ddr_ready', 'uart_rxd', 'wifi_rxd', 'sd_miso', 'sd_detectn', 'flash_miso', 'mdio_i', 'hdmi_hpd',
        'iec_atn_i', 'iec_clk_i', 'iec_data_i', 'iec_reset_n'}
lines = []
for d, w, n in ports:
    if n in bound:
        e = bound[n]
    elif d == 'input':
        e = ("{%d{1'b1}}" % w) if n in high else "%d'd0" % w
    else:
        e = ''
    lines.append('        .%s(%s)' % (n, e))
open(out + '/tb_us_inst.vh', 'w').write('    ultimate_sys ult (\n' + ',\n'.join(lines) + '\n    );\n')
print(name)
