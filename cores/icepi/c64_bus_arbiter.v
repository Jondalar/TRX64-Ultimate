// ---------------------------------------------------------------------------
// Copyright 2026 Mateusz Nalewajski
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------

// (vendor) `default_nettype none
`timescale 1 ns / 1 ps
module c64_bus_arbiter (
  input  wire        vic_clk_dot4x,
  input  wire        vic_phi2_n,
  input  wire        vic_reset,
  input  wire [11:0] vic_ado,
  input  wire        vic_ras,
  input  wire        vic_cas,
  input  wire        vic_cas_glitch,
  input  wire        vic_write_db,
  input  wire        vic_write_ab,
  input  wire        vic_aec,
  input  wire        vic_ba,
  output reg         vic_we,
  output reg  [5:0]  vic_adi,
  output reg  [7:0]  vic_dbi,
  input  wire [7:0]  vic_dbo,
  output reg  [3:0]  vic_dbh,
  output reg         vic_cen,

  input  wire        cpu_reset,
  input  wire [15:0] cpu_addr,
  output reg  [7:0]  cpu_din,
  input  wire [7:0]  cpu_dout,
  input  wire [5:0]  cpu_pout,
  output reg  [5:0]  cpu_pin,
  input  wire        cpu_we,
  input  wire [15:0] cpu_pc,
  input wire         cpu_irq,
  input wire         cpu_nmi,

  output reg  [9:0]  ram_color_addr,
  output reg  [3:0]  ram_color_din,
  input  wire [3:0]  ram_color_dout,
  output reg         ram_color_select,
  output reg         ram_color_we,

  output reg  [15:0] ram_addr,
  input  wire [7:0]  ram_dout,
  output reg  [7:0]  ram_din,
  output reg         ram_we,
  output reg         ram_enable,
  input  wire        ram_ready,
  output reg         ram_select,

  output reg  [12:0] rom_basic_addr,
  output reg         rom_basic_enable,
  input  wire        rom_basic_ready,
  input  wire [7:0]  rom_basic_dout,
  output reg         rom_basic_select,

  output reg  [12:0] rom_kernal_addr,
  output reg         rom_kernal_enable,
  input  wire        rom_kernal_ready,
  input  wire [7:0]  rom_kernal_dout,
  output reg         rom_kernal_select,

  output reg  [11:0] rom_char_addr,
  output reg         rom_char_enable,
  input  wire        rom_char_ready,
  input  wire [7:0]  rom_char_dout,
  output reg         rom_char_select,

  output reg  [9:0]  sid_addr,
  input  wire [7:0]  sid_dout,
  input  wire        sid_hit,     // a SID answers the read (else the open bus)
  output reg  [7:0]  sid_din,
  output reg         sid_cen,
  output reg         sid_we,

  output reg  [3:0]  cia1_addr,
  input  wire [7:0]  cia1_dout,
  output reg  [7:0]  cia1_din,
  output reg         cia1_cen,
  output reg         cia1_we,

  output reg  [3:0]  cia2_addr,
  input  wire [7:0]  cia2_dout,
  output reg  [7:0]  cia2_din,
  output reg         cia2_cen,
  output reg         cia2_we,
  output wire [7:0]  cia2_pa_in,
  input  wire [7:0]  cia2_pa_out,
  input  wire [7:0]  cia2_ddra,
  output wire [7:0]  cia2_pb_in,
  input  wire [7:0]  cia2_pb_out,
  input  wire [7:0]  cia2_ddrb,
  output wire        cia2_flag_n,
  input  wire        cia2_pc_n,

  output reg  [3:0]  reu_addr,
  output reg  [7:0]  reu_din,
  input  wire [7:0]  reu_dout,
  output reg         reu_we,
  output reg         reu_cen,

  input  wire [15:0] reu_dma_addr,
  input  wire        reu_dma_active,
  input  wire        reu_dma_we,

  input  wire        reu_present,

  input  wire        cart_present,
  input  wire        cart_game,
  input  wire        cart_exrom,
  // the cartridge's own lines without the app's Ultimax (patch 0020), for
  // the IO2 arbitration between the REU and the cartridge: a DMA cycle with
  // Ultimax set (the app's, a freeze commit) still reaches the REU
  input  wire        cart_io_present,
  input  wire        cart_io_game,
  input  wire        cart_io_exrom,
  output reg         cart_io1_cen,
  output reg         cart_io2_cen,
  // T37 SID sockets (the app's C64_SIDn_EN/BASE/MASK, synchronized by c64_core): sock_sel[n] is
  // a CPU-half access of socket n+1, I/O visible, A11..A4 & mask == base (as trx_sid.v). Guardrail
  // (Alex 2026-10-16): physical chip before FPGA. At $DE00-$DFFF a cartridge's IO1/IO2 wins over a
  // socket (Q4), a socket wins over the REU and our own responder (UCI); a read is answered from
  // cart_dout (the port's data, Q5) before the internal SID.
  // sock_we is the port's R/W for a socket cycle; it stays apart from cart_we, which feeds the
  // cartridge models and through them the REU arbitration back into bus_addr (a loop, run 833).
  input  wire  [1:0] sock_en,
  input  wire [15:0] sock_base,
  input  wire [15:0] sock_mask,
  output reg   [1:0] sock_sel,
  output reg         sock_we,
  input  wire        cart_io2_we_valid,
  output wire        cart_roml,
  output wire        cart_romh,
  input  wire [7:0]  cart_dout,
  output reg  [7:0]  cart_din,
  output wire        cart_ba,
  output reg         cart_we,
  output reg  [15:0] cart_addr,

  output reg         roml_select,
  output reg         romh_select,

  // a read cycle of the app's lens unit (T21 §3.4.1): the select the PLA
  // would have chosen is replaced by this one, 1 ROML, 2 ROMH, 3 I/O (0 off)
  input  wire  [1:0] force_sel,

  // a responder of our own in $DE00-$DFFF (the UCI, c64_core.v): io_ext_sel
  // when the bus master accesses that range with I/O mapped; a read takes
  // io_ext_dout while io_ext_oe, before the REU and the cartridge
  output wire        io_ext_sel,
  output wire        io_ext_we,
  // T62 G17: the bus master addresses $D7FF with I/O mapped
  output wire        io_dbg_sel,
  input  wire        io_ext_oe,
  input  wire [7:0]  io_ext_dout,
  // the turbo registers ($D030/$D031/$D07A/$D07B/$D0BC and mirrors, c64_turbo.v)
  // answer a CPU read of the VIC's page 0 while the machine is at normal speed
  input  wire        tr_oe,
  input  wire [7:0]  tr_dout,
  // U13: the monitor's look at an I/O address: the device io_decode() picks for a read of lk_addr with I/O mapped,
  // the socket select, whether our own responder's range (io_ext_sel) covers it, and the open bus
  input  wire [15:0] lk_addr,
  output wire [3:0]  lk_dev,
  output wire [1:0]  lk_sock,
  output wire        lk_xsel,
  output wire [7:0]  lk_last,

  input  wire        cass_sense_n,
  output reg         cass_motor_n,

  output wire        game,
  output wire        exrom,
  output wire        loram,
  output wire        hiram,
  output wire        charen,

  output wire        sdram_pending,

  output wire        iec_data_out,
  output wire        iec_clk_out,
  output wire        iec_atn_out,
  output wire  [2:0] cia2_iec_lines,         // T29: the lines as the CIA drives them {ATN, CLK, DATA}, '0' = pulled low

  input  wire        iec_data_in,
  input  wire        iec_clk_in,
  input  wire        iec_atn_in,

  output wire        c1541_iec_atn_out,
  output wire        c1541_iec_data_out,
  output wire        c1541_iec_clk_out,

  input  wire        c1541_iec_data_in,
  input  wire        c1541_iec_clk_in,

  input  wire [7:0]  c1541_par_data_in,
  input  wire        c1541_par_stb_in,
  output wire [7:0]  c1541_par_data_out,
  output wire        c1541_par_stb_out,

  input  wire        va_delay,
  input  wire        iec_master_disconnect
);

wire [11:0] vic_addr;

wire va15, va14;

wire vic_select_rom_char;
wire vic_select_romh;

reg ras_locked;
reg cas_locked;

reg [15:0] vic_ram_addr;
reg  [7:0] vic_addr_i;

reg  [1:0] va1514;

assign {va15, va14} = ~(~cia2_ddra[1:0] | cia2_pa_out[1:0]);  // weak pull-up

assign vic_select_rom_char = {va14, vic_ado[5:4]} == 3'b001;
assign vic_select_romh     = vic_ado[5:4] == 2'b11;
assign vic_addr            = vic_ras ? vic_ado : {vic_ado[11:8], vic_addr_i};

assign sdram_pending = !vic_ras && !vic_cas && ras_locked && !cas_locked;

always @(posedge vic_clk_dot4x) begin
  if (cpu_reset || vic_reset) begin
    ram_enable        <= 1'b0;
    rom_basic_enable  <= 1'b0;
    rom_kernal_enable <= 1'b0;
    rom_char_enable   <= 1'b0;

    va1514 <= 2'b0;

    ras_locked <= 1'b0;
    cas_locked <= 1'b0;

  end else begin
    // handle U26 flip-flop
    if (vic_ras && !vic_aec)
      vic_addr_i <= vic_ado[7:0];

    // this simulates delayed change for U14 output
    if (!vic_cas && !vic_aec)
      va1514 <= ~(~cia2_ddra[1:0] | cia2_pa_out[1:0]);  // weak pull-up

    // clear registered enable signals when ready
    if (ram_ready)
      ram_enable <= 1'b0;

    if (rom_basic_ready)
      rom_basic_enable <= 1'b0;

    if (rom_kernal_ready)
      rom_kernal_enable <= 1'b0;

    if (rom_char_ready)
      rom_char_enable <= 1'b0;

    // clear ras_locked once RAS is asserted
    if (vic_ras)
      ras_locked <= 1'b0;

    // clear cas_locked once CAS is asserted or SDRAM is not selected (CASRAM)
    if (vic_cas)
      cas_locked <= 1'b0;

    // lock row address
    if (!vic_ras && !ras_locked) begin
      ras_locked <= 1'b1;

      if (!vic_aec)
        vic_ram_addr[7:0] <= vic_ado[7:0];
    end

    // lock column address and strobe
    if (sdram_pending) begin
      cas_locked <= 1'b1;

      ram_enable        <= ram_select;
      rom_basic_enable  <= rom_basic_select;
      rom_kernal_enable <= rom_kernal_select;
      rom_char_enable   <= rom_char_select;

      if (!vic_aec) begin
        vic_ram_addr[13:8] <= vic_ado[5:0];

        // glitch happens when bits going through U14 flip
        vic_ram_addr[15:14] <= va_delay && ((va1514 ^ {va15, va14}) == 2'b11) && (va15 != va14) ? 2'b11 : {va15, va14};
      end
    end
  end
end

// serial port

wire cia2_iec_data_in;
wire cia2_iec_clk_in;
wire cia2_iec_atn_in;

wire cia2_iec_data_out;
wire cia2_iec_clk_out;

// CIA2 has pull-up, when PA is configured as input and inverter pulls line low
assign cia2_iec_data_in = (cia2_ddra[5] ? !cia2_pa_out[5] : 1'b0) || iec_master_disconnect;
assign cia2_iec_clk_in  = (cia2_ddra[4] ? !cia2_pa_out[4] : 1'b0) || iec_master_disconnect;
assign cia2_iec_atn_in  = (cia2_ddra[3] ? !cia2_pa_out[3] : 1'b0) || iec_master_disconnect;

assign cia2_iec_lines = {cia2_iec_atn_in, cia2_iec_clk_in, cia2_iec_data_in};
assign iec_data_out = c1541_iec_data_in && cia2_iec_data_in;
assign iec_clk_out  = c1541_iec_clk_in && cia2_iec_clk_in;
assign iec_atn_out  = cia2_iec_atn_in;

reg iec_data;
reg iec_clk;
reg iec_atn;

// break long combinatorial logic; sync to FPGA domain
always @(posedge vic_clk_dot4x) begin
  iec_data <= iec_data_in;
  iec_clk  <= iec_clk_in;
  iec_atn  <= iec_atn_in;
end

assign c1541_iec_data_out = iec_data;
assign c1541_iec_clk_out  = iec_clk;
assign c1541_iec_atn_out  = iec_atn;

assign cia2_iec_data_out = iec_data || iec_master_disconnect;
assign cia2_iec_clk_out  = iec_clk || iec_master_disconnect;

assign cia2_pa_in = {cia2_iec_data_out, cia2_iec_clk_out,
  (~cia2_ddra[5:3] | cia2_pa_out[5:3]), (~cia2_ddra[2:0] | cia2_pa_out[2:0])};

// parallel port

assign c1541_par_data_out = (~cia2_ddrb | cia2_pb_out);
assign c1541_par_stb_out  = cia2_pc_n;

assign cia2_pb_in  = c1541_par_data_in & c1541_par_data_out;  // the pins carry our own outputs too
assign cia2_flag_n = c1541_par_stb_in;

// bus logic

reg [15:0] bus_addr;
reg        bus_we;

wire [3:0] bus_addr_msn;

assign bus_addr_msn = bus_addr[15:12];

reg [7:0] last_bus;
reg [7:0] bus;

reg io_enable;
reg reu_override;

always @(posedge vic_clk_dot4x) begin
  if (!reu_cen)
    reu_override <= 1'b1;
  else if (!cart_io2_cen)
    reu_override <= 1'b0;
end

always @(posedge vic_clk_dot4x)
  last_bus <= bus;

always @(*) begin
  bus = last_bus;

  if (cpu_reset || vic_reset) begin
    /* no-op */
  end else if (reu_dma_we && reu_dma_active && vic_aec)
    bus = reu_dout;
  else if (cpu_we && !reu_dma_active && vic_aec)
    bus = cpu_dout;
  else if (rom_basic_select)
    bus = rom_basic_dout;
  else if (rom_kernal_select)
    bus = rom_kernal_dout;
  else if (rom_char_select)
    bus = rom_char_dout;
  else if (roml_select && cart_present)
    bus = cart_dout;
  else if (romh_select && cart_present)
    bus = cart_dout;
  else if (ram_color_select && vic_aec)
    bus[3:0] = ram_color_dout;
  else if (ram_select)
    bus = ram_dout;
  else if (io_enable) begin
    if (tr_oe && !reu_dma_active)
      bus = tr_dout;
    else if (vic_write_db)
      bus = vic_dbo;
    else if (sock_sel != 2'b00)
      bus = cart_dout;
    else if (!sid_cen && sid_hit)
      bus = sid_dout;
    else if (!cia1_cen)
      bus = cia1_dout;
    else if (!cia2_cen)
      bus = cia2_dout;
    else if (!reu_cen)
      bus = reu_dout;
    else if (!cart_io1_cen || !cart_io2_cen)
      bus = cart_dout;
  end

  // the responder of our own: a last stage (inside $DE00-$DFFF with I/O
  // mapped only the REU and the cartridge would answer)
  if (io_ext_oe && io_ext_sel && !bus_we)
    bus = io_ext_dout;
end


assign io_ext_sel = io_enable && bus_addr[15:9] == 7'b1101111 && sock_sel == 2'b00;
assign io_ext_we  = bus_we;
assign io_dbg_sel = io_enable && bus_addr == 16'hD7FF;

assign game  = cart_game  || !cart_present;
assign exrom = cart_exrom || !cart_present;

assign loram  = cpu_pout[0];
assign hiram  = cpu_pout[1];
assign charen = cpu_pout[2];

assign cart_roml = !roml_select || !cart_present;
assign cart_romh = !romh_select || !cart_present;

assign cart_ba = vic_ba && cart_present;

// the PLA for the CPU half (a REU DMA address goes through it as well)
wire pla_io, pla_ram, pla_basic, pla_kernal, pla_chr, pla_roml, pla_romh;
c64_pla pla (
  .game(game), .exrom(exrom), .charen(charen), .hiram(hiram), .loram(loram),
  .msn(bus_addr_msn), .we(bus_we), .ba(vic_ba),
  .io(pla_io), .ram(pla_ram), .basic(pla_basic), .kernal(pla_kernal), .chr(pla_chr),
  .roml(pla_roml), .romh(pla_romh)
);

// The I/O decode: the device an I/O-mapped access to a reaches, and the socket select (U13). The bus path below and
// the monitor's look both call it, so the look cannot pick another device than a read would.
localparam [3:0] IO_NONE = 4'd0, IO_VIC = 4'd1, IO_SID = 4'd2, IO_CRAM = 4'd3, IO_CIA1 = 4'd4, IO_CIA2 = 4'd5,
                 IO_CART1 = 4'd6, IO_CART2 = 4'd7, IO_REU = 4'd8;
function [5:0] io_decode(input [15:0] a, input we, input c_present, input r_present, input ci_present,
                         input ci_game, input ci_exrom, input c2_we_valid, input r_override,
                         input [1:0] s_en, input [15:0] s_base, input [15:0] s_mask);
  reg [3:0] d;
  reg [1:0] s;
  begin
    d = IO_NONE;
    case (a[11:8])
      4'h0, 4'h1, 4'h2, 4'h3: d = IO_VIC;
      4'h4, 4'h5, 4'h6, 4'h7: d = IO_SID;
      4'h8, 4'h9, 4'ha, 4'hb: d = IO_CRAM;
      4'hc: d = IO_CIA1;
      4'hd: d = IO_CIA2;
      4'he: if (c_present) d = IO_CART1;
      4'hf: begin
        if (!r_present && !c_present)
          d = IO_NONE;
        else if (!r_present && c_present)
          d = IO_CART2;
        else if (r_present && !ci_present)
          d = IO_REU;
        // here we assume we must arbitrate IO2 / REU access
        else if (!ci_game || !ci_exrom)
          d = IO_CART2;
        else if (we)
          d = c2_we_valid ? IO_CART2 : IO_REU;
        else if (r_override)
          d = IO_REU;
        else
          d = IO_CART2;
      end
      default: ;  /* read last bus state */
    endcase
    s[0] = s_en[0] && ((a[11:4] & s_mask[7:0])  == s_base[7:0]);
    s[1] = s_en[1] && ((a[11:4] & s_mask[15:8]) == s_base[15:8]);
    // a cartridge (plugged or from a .crt) keeps IO1/IO2: no socket select there (Q4, Alex 2026-10-16)
    if (d == IO_CART1 || d == IO_CART2)
      s = 2'b00;
    // physical chip before FPGA: the REU does not see a socket cycle
    if (s != 2'b00 && d == IO_REU)
      d = IO_NONE;
    io_decode = {s, d};
  end
endfunction

reg [3:0] io_dev;

assign {lk_sock, lk_dev} = io_decode(lk_addr, 1'b0, cart_present, reu_present, cart_io_present, cart_io_game,
                                     cart_io_exrom, cart_io2_we_valid, reu_override, sock_en, sock_base, sock_mask);
assign lk_xsel = lk_addr[15:9] == 7'b1101111 && lk_sock == 2'b00;
assign lk_last = last_bus;

// address decoding

always @(*) begin
  io_dev = IO_NONE;

  if (reu_dma_active) begin
    bus_addr = reu_dma_addr;
    bus_we   = reu_dma_we;
    ram_din  = reu_dout;

  end else begin
    bus_addr = cpu_addr;
    bus_we   = cpu_we;
    ram_din  = cpu_dout;
  end

  cpu_din  = bus;
  vic_dbi  = bus;
  vic_dbh  = bus[3:0];
  sid_din  = cpu_dout;  // only CPU writes to SID, helps with timing
  cia1_din = cpu_dout;  // only CPU writes to CIA, helps with timing
  cia2_din = cpu_dout;  // only CPU writes to CIA, helps with timing
  reu_din  = bus;
  cart_din = bus;

  vic_adi   = bus_addr[5:0];
  sid_addr  = bus_addr[9:0];
  cia1_addr = bus_addr[3:0];
  cia2_addr = bus_addr[3:0];
  reu_addr  = bus_addr[3:0];

  io_enable = 1'b0;

  vic_we  = 1'b0;
  sid_we  = 1'b0;
  cia1_we = 1'b0;
  cia2_we = 1'b0;
  reu_we  = 1'b0;

  vic_cen  = 1'b1;
  sid_cen  = 1'b1;
  cia1_cen = 1'b1;
  cia2_cen = 1'b1;
  reu_cen  = 1'b1;

  cart_io1_cen = 1'b1;
  cart_io2_cen = 1'b1;
  sock_sel     = 2'b00;
  sock_we      = 1'b0;

  ram_color_din = bus[3:0];

  rom_basic_addr  = bus_addr[12:0];
  rom_kernal_addr = bus_addr[12:0];
  rom_char_addr   = bus_addr[11:0];
  ram_color_addr  = bus_addr[9:0];
  ram_addr        = bus_addr;
  cart_addr       = bus_addr;

  ram_color_we = 1'b0;
  ram_we       = 1'b0;
  cart_we      = 1'b0;

  rom_basic_select  = 1'b0;
  rom_kernal_select = 1'b0;
  rom_char_select   = 1'b0;
  roml_select       = 1'b0;
  romh_select       = 1'b0;
  ram_color_select  = 1'b0;
  ram_select        = 1'b0;

  cass_motor_n = cpu_pout[5];
  cpu_pin      = {1'b1, cass_sense_n, 4'b1111};

  if (cpu_reset || vic_reset) begin
    /* no-op */
  end else if (!vic_write_ab) begin
    vic_we  = bus_we;
    sid_we  = bus_we;
    cia1_we = bus_we;
    cia2_we = bus_we;
    reu_we  = bus_we;

    ram_color_we = !vic_cas && bus_we;  // mimic real PLA behaviour
    ram_we       = bus_we;
    cart_we      = bus_we && cart_present;

    // the PLA (c64_pla.v; the turbo engine decodes the CPU's address with a copy)
    io_enable         = pla_io;
    ram_select        = pla_ram;
    rom_basic_select  = pla_basic;
    rom_kernal_select = pla_kernal;
    rom_char_select   = pla_chr;
    roml_select       = pla_roml;
    romh_select       = pla_romh;

    if (force_sel != 2'd0 && !bus_we) begin
      ram_select        = 1'b0;
      rom_basic_select  = 1'b0;
      rom_kernal_select = 1'b0;
      rom_char_select   = 1'b0;
      roml_select       = force_sel == 2'd1;
      romh_select       = force_sel == 2'd2;
      io_enable         = force_sel == 2'd3;
    end

    if (io_enable) begin
      {sock_sel, io_dev} = io_decode(bus_addr, bus_we, cart_present, reu_present, cart_io_present, cart_io_game,
                                     cart_io_exrom, cart_io2_we_valid, reu_override, sock_en, sock_base, sock_mask);
      vic_cen          = io_dev != IO_VIC;
      sid_cen          = io_dev != IO_SID;
      ram_color_select = io_dev == IO_CRAM;
      cia1_cen         = io_dev != IO_CIA1;
      cia2_cen         = io_dev != IO_CIA2;
      cart_io1_cen     = io_dev != IO_CART1;
      cart_io2_cen     = io_dev != IO_CART2;
      reu_cen          = io_dev != IO_REU;
      if (sock_sel != 2'b00)
        sock_we = bus_we;                   // the port's R/W for a socket cycle, with or without a cartridge
    end

  end else begin
    vic_dbh = ram_color_dout;

    rom_char_addr  = vic_addr;
    cart_addr      = vic_ram_addr;
    ram_color_addr = vic_addr[9:0];
    ram_addr       = vic_ram_addr;

    ram_color_select = 1'b1;

    if (!game && exrom) begin
      if (vic_select_romh) begin
        romh_select = 1'b1;
      end else begin
        ram_select = 1'b1;
      end
    end else begin
      if (vic_select_rom_char) begin
        rom_char_select = 1'b1;
      end else begin
        ram_select = 1'b1;
      end
    end
  end
end

endmodule
`default_nettype wire
// vim:ts=2 sw=2 tw=120 et
