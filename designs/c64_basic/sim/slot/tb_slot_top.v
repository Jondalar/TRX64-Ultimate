// SPDX-License-Identifier: GPL-3.0-or-later
// T16 M1 bench (docs/specs/T16-cartridge-port.md §6.3, §13): c64_core (6510,
// VIC, PLA, KERNAL) with its slot_port pins on a model of the board: the two
// 1.8 V transceivers (DIR/OE), the 3.3 V balls with their pull-ups, and a
// behavioural cartridge on the port side. One clock (31.5 MHz dot4x); the
// model works in its ticks (31.7 ns), so timing is checked in ticks.
//
// Cartridge (cart_mode): 0 none, 1 8K (EXROM low), 2 16K (both low), 3 an
// EasyFlash-like cart: 8 banks of ROML/ROMH, $DE00 bank (readable here),
// $DE02 mode (b0 GAME, b1 EXROM, b2 MODE; MODE 0 = jumper = ultimax), 256 B
// RAM at $DF00, reset to the jumper (ultimax) by the port's RESET; writes with
// ROML/ROMH low are logged as flash writes (chip, bank, offset, data).
// Its data is valid acc_ticks after its address or strobe last changed;
// before that it drives garbage, so a sample taken too early gives a wrong
// byte.
//
// Checks (counters, all must stay 0 except the n_* and min_* ones):
//   e_cont_port  the port's D driven by the transceiver and the cart at once
//   e_cont_fpga  the FPGA's D balls driven by the FPGA and the transceiver
//   e_turn       DATADIR changed with DATAOEn on in this or the last tick
//   e_adir       address transceiver on with ADDRDIR != 1
//   e_idle       a port output active while slot_port is not armed
//   e_wr_setup   a write strobe's PHI2 fall without data driven and stable
//                for 8 ticks (250 ns)
//   e_wr_hold    address, data, R/W or strobe changed in the tick after PHI2 fell
//   e_addr       the address moved while a strobe stayed low with PHI2 high
//   e_cpu_late / e_vic_late  a used sample younger than acc_ticks
//   e_cpu_data / e_vic_data  a used sample differs from the cart's byte
//   n_cpu_rd, n_vic_rd: used samples; min_cpu_age, min_vic_age: their
//   smallest age in ticks (address/strobe change to the sample)
//
// SID sockets (T37, spec §6): a behavioural SID on each socket (sid_mode: 0 no chip, 1 plain 6581 with
// register read-back, 2 FPGASID, 3 ARMSID, 4 SwinSID; the handshakes of u64_config.cc detectFPGASID /
// detectRemakes), its personality picked by SLOT_SID_ADDR (a second register file per socket), CS1n/CS2n
// and PHI2, R/W, A0-A4, D0-D7 as on the board; read data valid sid_acc ticks after the address/CS
// last changed. Checks (all stay 0): e_sid_cs_phi CS low with PHI2 low, e_sid_pre address or R/W
// moved in the tick before CS fell, e_sid_hold address, R/W or write data moved at PHI2's fall,
// e_sid_park CS low in the park state, e_sid_late/e_sid_data a used read younger than sid_acc or
// wrong, e_sid_idle a bus line, the clock or the cartridge's strobes active without a cause, e_sid_a
// SLOT_SID_ADDR not the address bit / the idle level. n_cs[n]: PHI2 falls with CS n low, n_cs_both with
// both low, n_sid_wr/n_sid_rd: accesses the chips saw.
module tb_slot_top (
    input  wire        clk,
    input  wire        clk2x,
    input  wire        rst,
    input  wire        cpu_reset_req,
    input  wire        halt_req,
    output wire        halted,
    input  wire [15:0] dbg_addr,
    input  wire        dbg_we,
    input  wire [7:0]  dbg_wdata,
    output wire [7:0]  dbg_rdata,
    // the lens unit (dma_win.v LK, T21 §3.4.1): the app's request and result, and the app's Ultimax
    input  wire        app_ultimax,
    input  wire        cpu_pause_req,
    input  wire        lk_go,
    input  wire [18:0] lk_req,
    output wire [31:0] lk_res,
    input  wire        dma_req,
    input  wire [15:0] dma_addr,
    input  wire        dma_we,
    input  wire  [7:0] dma_wdata,
    output wire        dma_done,
    output wire  [7:0] dma_rdata,
    input  wire  [3:0] bus_int, bus_ext,
    input  wire        rom_wr_en,
    // the SID sockets (T37): the app's registers, as ultimate_sys puts them out, and the chips
    input  wire  [1:0] sock_en,
    input  wire [15:0] sock_base, sock_mask,
    input  wire  [2:0] stereo_sel,
    input  wire  [2:0] sid_mode,
    input  wire  [3:0] sid_acc,
    // the cartridge model
    input  wire  [1:0] cart_mode,
    input  wire  [3:0] acc_ticks,
    input  wire        cart_btn,            // the cart's reset button
    input  wire        rom_we,
    input  wire [16:0] rom_addr,            // {bank[2:0], half, offset[12:0]}
    input  wire  [7:0] rom_wdata,
    input  wire  [3:0] log_idx,
    output wire [31:0] log_word,            // {chip, bank, offset 13, data}
    output reg   [4:0] log_n = 5'd0,
    output wire  [7:0] ef_bank_o, ef_mode_o,
    // the SID model
    output reg  [31:0] n_cs1 = 0, n_cs2 = 0, n_cs_both = 0, n_sid_wr = 0, n_sid_rd = 0, n_sid_a1 = 0, n_sid_both = 0,
    output reg  [31:0] e_sid_cs_phi = 0, e_sid_pre = 0, e_sid_hold = 0, e_sid_park = 0, e_sid_late = 0,
    output reg  [31:0] e_sid_data = 0, e_sid_idle = 0, e_sid_a = 0, e_sid_cont = 0,
    output reg   [7:0] sid_last_a = 8'd0,   // the A0-A4 and R/W of the last access {rw, sid, a, A4..A0}
    // observation
    output wire [31:0] slot_status,
    output wire [31:0] slot_cnt0, slot_cnt1,
    output wire        p_phi2, p_rst_o,
    output reg  [31:0] n_phi2 = 0,          // PHI2 rising edges at the port
    output reg  [31:0] n_park = 0,          // clocks in the park state
    output reg  [31:0] e_cont_port = 0, e_cont_fpga = 0, e_turn = 0, e_adir = 0, e_idle = 0,
    output reg  [31:0] e_wr_setup = 0, e_wr_hold = 0, e_addr = 0,
    output reg  [31:0] e_cpu_late = 0, e_vic_late = 0, e_cpu_data = 0, e_vic_data = 0,
    output reg  [31:0] n_cpu_rd = 0, n_vic_rd = 0, n_wr = 0,
    output reg   [7:0] min_cpu_age = 8'hFF, min_vic_age = 8'hFF,
    output wire  [9:0] pix_x,
    output wire  [8:0] pix_y,
    output wire  [7:0] cpu_pdir, cpu_pout
);
    wire [3:0] pix_color; wire [4:0] leds;
    wire [23:0] reu_cmd_addr; wire reu_cmd_we, reu_cmd_valid; wire [15:0] reu_wdata; wire [1:0] reu_wdata_we;
    wire signed [23:0] audio_l, audio_r;
    wire gate_ce, gclk;
    wire [15:0] pk_addr;
    wire  [1:0] pk_cls;
    wire        pk_hit, pk_mem, pk_ext, pkd_tgl;
    wire [23:0] pk_maddr, pkd_addr;
    wire  [7:0] pk_data;
    wire        rsp_cyc_req, rsp_cyc_we, core_ultimax;
    wire [15:0] rsp_cyc_addr;
    wire  [7:0] rsp_cyc_wdata;
    wire  [1:0] rsp_force;
    wire        lens_use = lk_res[31];
    reg  gate_l = 1'b1;
    always @(negedge clk) gate_l <= gate_ce;
    assign gclk = clk & gate_l;

    // ---- the FPGA's pins ---------------------------------------------------------
    wire [15:0] s_a;
    wire  [7:0] s_d_o;
    wire        s_ahi_t, s_rw, s_d_t, s_adir, s_aoen, s_ddir, s_doen;
    wire        s_roml, s_romh, s_io1, s_io2, s_phi2, s_dotclk, s_ba, s_rst_o;
    wire        s_sid1, s_sid2, s_sid_a;
    wire  [7:0] s_d_i;
    wire        c_game, c_exrom, c_rst_i;

    c64_core c64 (
        .dbg_req(1'b1),            // T29: the outside port is wanted in every clock here
        .vmode(2'd0),                   // T40: PAL
        .clk(clk), .gclk(gclk), .gate_ce(gate_ce), .rst(rst), .halt_req(halt_req), .halted(halted),
        .snd_clk(clk), .snd_rst(rst), .audio_sample_en(1'b0),
        .clk2x(clk2x),
        .audio_l(audio_l), .audio_r(audio_r),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(leds),
        .keys(64'd0), .joy1(5'd0), .joy2(5'd0), .restore(1'b0),
        .dbg_addr(dbg_addr), .dbg_we(dbg_we), .dbg_wdata(dbg_wdata), .dbg_rdata(dbg_rdata),
        .reu_cmd_addr(reu_cmd_addr), .reu_cmd_we(reu_cmd_we), .reu_cmd_valid(reu_cmd_valid),
        .reu_cmd_ready(1'b1), .reu_en(1'b1), .reu_size(3'd5), .reu_wdata(reu_wdata), .reu_wdata_we(reu_wdata_we),
        .reu_rdata(16'd0), .reu_rdata_valid(1'b0),
        .cpu_reset_req(cpu_reset_req), .cpu_pause_req(cpu_pause_req),
        .cart_type(8'd0), .cart_desc(344'd0), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0), .cart_active(),
        .cart_wclk(clk), .cart_req_tgl(), .cart_req_we(), .cart_req_addr(), .cart_req_data(),
        .cart_req_slot(), .cart_req_cached(), .cart_ack_tgl(1'b0),
        .cart_fl_enable(1'b0), .cart_fl_magic1(16'd0), .cart_fl_magic2(16'd0), .cart_fl_mask(16'd0),
        .cart_fl_id(24'd0), .cart_evt_tgl(), .cart_evt_word(), .cart_fl_ack_tgl(1'b0), .cart_inval_tgl(1'b0),
        .cart_ee_tgl(),
        .cart_ee_clk(clk), .cart_ee_addr(11'd0), .cart_ee_we(1'b0), .cart_ee_wdata(8'd0), .cart_ee_rdata(),
        .cart_fill_be(8'd0), .cart_fill_addr(9'd0), .cart_fill_data(64'd0),
        .dma_req(dma_req || rsp_cyc_req), .dma_addr(lens_use ? rsp_cyc_addr : dma_addr),
        .dma_we(lens_use ? rsp_cyc_we : dma_we), .dma_wdata(lens_use ? rsp_cyc_wdata : dma_wdata),
        .dma_done(dma_done), .dma_rdata(dma_rdata), .ultimax(core_ultimax), .dma_force(rsp_force),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .pk_ext(pk_ext),
        .iec_atn_o(), .iec_clk_o(), .iec_data_o(),
        .iec_atn_i(1'b1), .iec_clk_i(1'b1), .iec_data_i(1'b1),
        .sb_addr(12'd0), .sb_rdata(), .sb_frozen(), .dbg_rom(1'b0),
        .cap_every(8'd0), .cap_gen(), .cap_tgl(), .cap_nosnap(),
        .log_on(1'b0), .cap_block(1'b0),
        .sb_we(1'b0), .sb_waddr(12'd0), .sb_wdata(32'd0), .sb_ldreq(1'b0), .sb_ldmask(5'd0),
        .pos_set(1'b0), .pos_arm_in(1'b0), .pos_cyc(6'd0), .pos_release(1'b0),
        .resume_phase(20'd0), .rp_set(1'b0),
        .cram_addr(10'd0), .cram_we(1'b0), .cram_wdata(4'd0), .cram_rdata(),
        .bus_addr(), .io_ev(), .io_ev_addr(), .io_ev_data(),
        .io_ev_we(), .io_ev_io(), .io_oe(1'b0), .io_dout(8'd0), .io_irq(1'b0),
        .dr_we(1'b0), .dr_re(1'b0), .dr_addr(11'd0), .dr_wdata(32'd0), .dr_rdata(),
        .dbg_own(), .dbg_ev(), .fw_stop(1'b0), .drv_stop(1'b0), .drv_done(1'b0),
        .dbg_log_put(), .dbg_log_rec(),
        .slot_int(bus_int), .slot_ext(bus_ext), .slot_wr_en(rom_wr_en),
        .slot_status(slot_status), .slot_cnt0(slot_cnt0), .slot_cnt1(slot_cnt1), .slot_last(),
        .slot_a(s_a), .slot_ahi_t(s_ahi_t), .slot_rw(s_rw), .slot_d_o(s_d_o), .slot_d_t(s_d_t),
        .slot_d_i(s_d_i), .slot_adir(s_adir), .slot_aoen(s_aoen), .slot_ddir(s_ddir), .slot_doen(s_doen),
        .slot_roml(s_roml), .slot_romh(s_romh), .slot_io1(s_io1), .slot_io2(s_io2),
        .slot_phi2(s_phi2), .slot_dotclk(s_dotclk), .slot_ba(s_ba), .slot_rst_o(s_rst_o),
        .slot_game(c_game), .slot_exrom(c_exrom), .slot_dma(1'b1), .slot_irq(1'b1), .slot_nmi(1'b1),
        .slot_rst_i(c_rst_i),
        .sock_en(sock_en), .sock_base(sock_base), .sock_mask(sock_mask), .stereo_sel(stereo_sel),
        .slot_sid1(s_sid1), .slot_sid2(s_sid2), .slot_sid_a(s_sid_a)
    );
    // the responder's lens unit (no window, no session: sh_mode 0)
    // the DMA window's lens unit (no window traffic)
    dma_win win (
        .clk(clk),
        .dma_req_tgl(1'b0), .dma_addr(16'd0), .dma_we(1'b0), .dma_wdata(8'd0),
        .dma_bus(1'b0), .dma_rom(1'b0), .dma_done_tgl(), .dma_rdata(),
        .dbg_busy(1'b0), .port_use(), .port_addr(), .port_rom(), .port_we(), .port_wdata(),
        .port_rdata(8'd0),
        .cyc_req(rsp_cyc_req), .cyc_addr(rsp_cyc_addr), .cyc_we(rsp_cyc_we), .cyc_wdata(rsp_cyc_wdata),
        .cyc_done(dma_done), .cyc_rdata(dma_rdata), .cyc_force(rsp_force),
        .cram_addr(), .cram_we(), .cram_wdata(), .cram_rdata(4'd0), .cram_use(),
        .halted(halted), .app_ultimax(app_ultimax), .core_ultimax(core_ultimax),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .cart_ext(pk_ext),
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
        .pkd_tgl(pkd_tgl), .pkd_addr(pkd_addr), .pkd_ack_tgl(pkd_tgl), .pkd_data(8'hEE)
    );
    assign cpu_pdir = c64.c64.st_pdir;
    assign cpu_pout = c64.c64.st_pout;
    assign p_phi2  = s_phi2;
    assign p_rst_o = s_rst_o;

    // ---- the board: transceivers, 3.3 V balls ------------------------------------------
    wire        a_on   = !s_aoen && s_adir;             // A0-A11 reach the port
    wire [15:0] pa     = {s_ahi_t ? 4'hF : s_a[15:12], a_on ? s_a[11:0] : 12'hFFF};
    wire        prw    = s_ahi_t ? 1'b1 : s_rw;
    wire        x_out  = !s_doen && s_ddir;             // transceiver drives the port's D
    wire        x_in   = !s_doen && !s_ddir;            // transceiver drives the FPGA's D
    wire        f_out  = !s_d_t;                        // the FPGA drives its D balls
    wire        c_drv;                                  // the cart drives the port's D
    wire  [7:0] c_dat;
    // nobody drives the port's D: the level stays as the last driver left it (the bus hold of the board's transceivers
    // and the C64 bus' capacitance; EE until anything drove it)
    reg   [7:0] keep   = 8'hEE;
    wire        sid_drv;                                // a SID drives the port's D
    wire  [7:0] sid_dat;
    wire        drvn   = x_out || c_drv || sid_drv;
    wire  [7:0] pd_drv = x_out ? s_d_o : c_drv ? c_dat : sid_dat;
    wire  [7:0] pd     = drvn ? pd_drv : keep;
    always @(posedge clk) if (drvn) keep <= pd_drv;
    assign s_d_i = x_in ? pd : f_out ? s_d_o : 8'hEE;
    wire        reset_n = !(s_rst_o || cart_btn);
    assign c_rst_i = !reset_n;

    // ---- the cartridge ---------------------------------------------------------------
    reg  [7:0] rom [0:131071];
    always @(posedge clk) if (rom_we) rom[rom_addr] <= rom_wdata;
    reg  [7:0] ef_ram [0:255];
    reg        phi2_q = 1'b0;
    reg  [5:0] ef_bank = 6'd0;
    reg  [7:0] ef_mode = 8'd0;
    assign ef_bank_o = {2'b00, ef_bank};
    assign ef_mode_o = ef_mode;
    wire ef = cart_mode == 2'd3;
    wire ef_game_a  = ef_mode[2] ? ef_mode[0] : 1'b1;   // asserted (low)
    wire ef_exrom_a = ef_mode[1];
    assign c_game  = cart_mode == 2'd2 ? 1'b0 : ef ? !ef_game_a : 1'b1;
    assign c_exrom = cart_mode == 2'd1 || cart_mode == 2'd2 ? 1'b0 : ef ? !ef_exrom_a : 1'b1;
    wire roml = !s_roml, romh = !s_romh, io1 = !s_io1, io2 = !s_io2;
    wire [2:0] bank3 = ef ? ef_bank[2:0] : 3'd0;
    wire       rom_sel = (cart_mode != 2'd0) && (roml || romh);
    wire [7:0] rom_byte = rom[{bank3, romh, pa[12:0]}];
    wire [7:0] io_byte  = io2 ? ef_ram[pa[7:0]] : pa[1] ? ef_mode : {2'b00, ef_bank};
    wire       io_sel   = ef && (io1 || io2);
    // age: ticks since the address, R/W or a strobe last changed
    reg [15:0] pa_q = 16'hFFFF;
    reg  [4:0] st_q = 5'h1F;
    reg  [7:0] age = 8'd0;
    wire [4:0] st = {prw, s_roml, s_romh, s_io1, s_io2};
    always @(posedge clk) begin
        pa_q <= pa;
        st_q <= st;
        age  <= (pa != pa_q || st != st_q) ? 8'd1 : (age == 8'hFF ? age : age + 1'b1);
    end
    wire [7:0] want = io_sel ? io_byte : rom_byte;
    wire       ready = (pa == pa_q && st == st_q) && age >= {4'd0, acc_ticks};
    assign c_drv = prw && (rom_sel || io_sel);
    assign c_dat = ready ? want : ~want;

    always @(posedge clk)
        if (reset_n && phi2_q && !s_phi2 && !prw && ef && io2) ef_ram[pa[7:0]] <= pd;
    // EF registers and RAM: written at PHI2's fall; flash writes at the strobe's rise
    reg [31:0] flog [0:15];
    assign log_word = flog[log_idx];
    reg        fw_q = 1'b0, fw_chip = 1'b0;
    reg [12:0] fw_off = 13'd0;
    reg  [7:0] fw_dat = 8'd0;
    always @(posedge clk) begin
        phi2_q <= s_phi2;
        if (!reset_n) begin
            ef_bank <= 6'd0;
            ef_mode <= 8'd0;
        end else if (phi2_q && !s_phi2 && !prw && ef) begin
            if (io1 && !pa[1]) ef_bank <= pd[5:0];
            if (io1 && pa[1])  ef_mode <= pd;
        end
        // flash write: strobe low with R/W low; the data and address as the
        // strobe rises (CE-controlled write)
        fw_q <= !prw && (roml || romh) && cart_mode != 2'd0;
        if (!prw && (roml || romh) && cart_mode != 2'd0) begin
            fw_chip <= romh;
            fw_off  <= pa[12:0];
            fw_dat  <= pd;
        end
        if (fw_q && !(!prw && (roml || romh))) begin
            if (log_n < 5'd16) flog[log_n[3:0]] <= {2'd0, fw_chip, 3'd0, bank3, 2'd0, fw_off, fw_dat};
            log_n <= log_n + 1'b1;
        end
    end


    // ---- the SID sockets: two chips on the port's bus (T37) ------------------------------------
    // registers: {socket, personality, A4..A0}; the personality is SLOT_SID_ADDR when the chip is addressed
    reg  [7:0] sid_r [0:127];
    integer    si;
    initial for (si = 0; si < 128; si = si + 1) sid_r[si] = 8'h00;
    wire       cs1 = !s_sid1, cs2 = !s_sid2, cs_any = cs1 || cs2;
    reg  [15:0] sp_pa = 16'hFFFF;                       // the port's address, R/W, D one tick ago
    reg         sp_rw = 1'b1, sp_cs1 = 1'b0, sp_cs2 = 1'b0, sp_phi2 = 1'b0, sp_a = 1'b0;
    reg   [7:0] sp_pd = 8'hEE;
    reg   [7:0] s_age = 8'd0;
    reg   [1:0] sg_diag = 2'b00;                        // per socket: FPGASID diagnostic mode
    reg   [1:0] sg_id   = 2'b00;                        // per socket: remake id handshake done
    reg   [1:0] sg_f1   = 2'b00;                        // per socket: first bytes of a handshake seen
    reg   [1:0] sg_s1   = 2'b00, sg_s2 = 2'b00;
    wire        sk_n   = cs2;                           // socket index of the selected chip (2 wins nothing: both is an error)
    wire  [6:0] sid_ix = {sk_n, s_sid_a, pa[4:0]};
    function [7:0] sid_rd(input [2:0] mode, input n, input diag, input id, input [6:0] ix, input [4:0] r);
        begin
            sid_rd = sid_r[ix];
            if (r == 5'd25 || r == 5'd26) sid_rd = 8'hFF;       // POT
            if (r == 5'd27 || r == 5'd28) sid_rd = 8'h00;       // OSC3, ENV3 of a silent chip
            if (mode == 3'd2 && diag && r == 5'd0) sid_rd = 8'h1D;
            if (mode == 3'd2 && diag && r == 5'd1) sid_rd = 8'hF5;
            if (mode == 3'd3 && id && r == 5'd27) sid_rd = "N";
            if (mode == 3'd3 && id && r == 5'd28) sid_rd = "O";
            if (mode == 3'd4 && id && r == 5'd27) sid_rd = "S";
            if (mode == 3'd4 && id && r == 5'd28) sid_rd = "W";
        end
    endfunction
    wire  [7:0] sid_want = sid_rd(sid_mode, sk_n, sg_diag[sk_n], sg_id[sk_n], sid_ix, pa[4:0]);
    wire        s_ready = (pa == sp_pa) && prw == sp_rw && cs1 == sp_cs1 && cs2 == sp_cs2 && s_sid_a == sp_a &&
                          s_age >= {4'd0, sid_acc};
    assign sid_drv = sid_mode != 3'd0 && cs_any && !(cs1 && cs2) && s_phi2 && prw;
    assign sid_dat = s_ready ? sid_want : ~sid_want;
    wire        sp_cs  = sp_cs1 || sp_cs2;
    wire [6:0]  sp_ix  = {sp_cs2, sp_a, sp_pa[4:0]};
    reg   [1:0] pk_h = 2'b00;
    reg   [2:0] ss_q = 3'd0;
    reg   [7:0] st_age = 8'd0;
    wire        sid_a_on = stereo_sel >= 3'd1 && stereo_sel <= 3'd5;
    reg   [2:0] ba_q = 3'd0;                            // bus activity of the last three ticks (window of a socket cycle)
    reg   [2:0] cs_w = 3'd0;                            // CS of the last three ticks
    wire        busact = !s_aoen || !s_ahi_t || !s_doen || f_out;
    wire        a_now  = !s_aoen && s_adir;
    always @(posedge clk) begin
        sp_pa <= pa; sp_rw <= prw; sp_cs1 <= cs1; sp_cs2 <= cs2; sp_phi2 <= s_phi2; sp_a <= s_sid_a;
        sp_pd <= pd;
        s_age <= (pa != sp_pa || prw != sp_rw || cs1 != sp_cs1 || cs2 != sp_cs2 || s_sid_a != sp_a) ? 8'd1 :
                 (s_age == 8'hFF ? s_age : s_age + 1'b1);
        ba_q  <= {ba_q[1:0], busact};
        cs_w  <= {cs_w[1:0], cs_any};
        // PHI2 fell: the chip took its write or served its read in the cycle just ended
        if (sp_phi2 && !s_phi2 && sp_cs) begin
            if (sp_cs1) n_cs1 <= n_cs1 + 1;
            if (sp_cs2) n_cs2 <= n_cs2 + 1;
            if (sp_cs1 && sp_cs2) n_cs_both <= n_cs_both + 1;
            if (sp_a) n_sid_a1 <= n_sid_a1 + 1;
            sid_last_a <= {sp_rw, sp_cs2, sp_a, sp_pa[4:0]};
            if (!sp_rw) begin
                n_sid_wr <= n_sid_wr + 1;
                if (sid_mode != 3'd0) begin
                    sid_r[sp_ix] <= sp_pd;
                    // the handshakes of detectFPGASID / detectRemakes
                    if (sid_mode == 3'd2) begin
                        if (sp_pa[4:0] == 5'd25) sg_f1[sp_cs2] <= sp_pd == 8'hEE;
                        if (sp_pa[4:0] == 5'd26 && sg_f1[sp_cs2] && sp_pd == 8'hAB) sg_diag[sp_cs2] <= 1'b1;
                    end
                    if (sid_mode >= 3'd3) begin
                        if (sp_pa[4:0] == 5'd29) begin sg_s1[sp_cs2] <= sp_pd == "S"; sg_s2[sp_cs2] <= 1'b0; sg_id[sp_cs2] <= 1'b0; end
                        if (sp_pa[4:0] == 5'd30) sg_s2[sp_cs2] <= sg_s1[sp_cs2] && sp_pd == "I";
                        if (sp_pa[4:0] == 5'd31 && sg_s2[sp_cs2] && sp_pd == "D") sg_id[sp_cs2] <= 1'b1;
                    end
                end
            end else begin
                n_sid_rd <= n_sid_rd + 1;
            end
        end
        // a socket write is a write of the internal SID bus as well (spec Q5: writes go to both)
        if (cs_any && !prw && s_phi2 && c64.c64.sid_we && !c64.c64.sid_cen) n_sid_both <= n_sid_both + 1;
        // contention: both chips selected is a bus error unless the test makes it on purpose (n_cs_both)
        if (cs1 && cs2 && sid_mode != 3'd0 && prw && s_phi2) e_sid_cont <= e_sid_cont + 1;
        // the selects
        if (cs_any && !s_phi2) e_sid_cs_phi <= e_sid_cs_phi + 1;
        if (cs_any && !sp_cs && (pa != sp_pa || prw != sp_rw)) e_sid_pre <= e_sid_pre + 1;
        if (sp_phi2 && !s_phi2 && sp_cs && (pa != sp_pa || prw != sp_rw || (!sp_rw && pd != sp_pd) || s_sid_a != sp_a))
            e_sid_hold <= e_sid_hold + 1;
        pk_h <= {pk_h[0], !c64.ce_d};
        if (pk_h == 2'b11 && (cs_any || s_phi2)) e_sid_park <= e_sid_park + 1;
        // a used read of a chip: the byte taken at the end of core tick 15 is the chip's, and not younger than sid_acc
        if (c64.ce && c64.slot.tk == 5'd15 && c64.slot.h_rd && sid_drv) begin
            if (!s_ready) e_sid_late <= e_sid_late + 1;
            if (s_d_i != sid_want) e_sid_data <= e_sid_data + 1;
        end
        // a bus line, the clock or a select active without a cause: the lines may lead a select by one tick and
        // follow it by one; the clock needs an enabled socket; K17 is the address bit in a window, idle high
        // with a split on, low without
        // (the tick before this one is judged: its neighbours are the tick before it and this one)
        if (ba_q[0] && !(cs_w[1] || cs_w[0] || cs_any) && !c64.slot.arm_q) e_sid_idle <= e_sid_idle + 1;
        ss_q <= stereo_sel;
        st_age <= (stereo_sel != ss_q) ? 8'd0 : (st_age == 8'hFF ? st_age : st_age + 1'b1);
        if (st_age > 8'd8 && stereo_sel == ss_q && !c64.slot.arm_q) begin
            if (!a_now && s_ahi_t && s_sid_a != sid_a_on) e_sid_a <= e_sid_a + 1;
            if (a_now && sid_a_on && s_sid_a != pa[4 + stereo_sel]) e_sid_a <= e_sid_a + 1;
            if (a_now && !sid_a_on && s_sid_a) e_sid_a <= e_sid_a + 1;
        end
    end

    // ---- checks -------------------------------------------------------------------------
    reg        ddir_q = 1'b1, doen_q = 1'b1;
    // the port's D as driven by the FPGA, and how long it has been stable
    reg  [7:0] xd_q = 8'd0;
    reg  [7:0] xd_age = 8'd0;
    always @(posedge clk) begin
        ddir_q <= s_ddir;
        doen_q <= s_doen;
        if (x_out && (c_drv || sid_drv)) e_cont_port <= e_cont_port + 1;
        if (c_drv && sid_drv) e_cont_port <= e_cont_port + 1;
        if (f_out && x_in) e_cont_fpga <= e_cont_fpga + 1;
        if (s_ddir != ddir_q && (!s_doen || !doen_q)) e_turn <= e_turn + 1;
        if (!s_aoen && !s_adir) e_adir <= e_adir + 1;
        if (!c64.slot.arm_q && (roml || romh || io1 || io2)) e_idle <= e_idle + 1;
        if (!c64.slot.arm_q && sock_en == 2'b00 && (s_phi2 || !s_doen || !s_aoen || f_out || !s_ahi_t))
            e_idle <= e_idle + 1;
        if (s_phi2 && !phi2_q) n_phi2 <= n_phi2 + 1;
        if (!c64.ce_d && c64.slot.act) n_park <= n_park + 1;

        xd_q   <= x_out ? s_d_o : 8'h00;
        xd_age <= (x_out && s_d_o == xd_q) ? (xd_age == 8'hFF ? xd_age : xd_age + 1'b1) : 8'd0;
        // a write with a strobe: PHI2 falls this tick
        if (phi2_q && !s_phi2 && !prw && (roml || romh || io1 || io2)) begin
            n_wr <= n_wr + 1;
            if (!x_out || xd_age < 8'd8) e_wr_setup <= e_wr_setup + 1;
        end
        // address stable while a strobe is low with PHI2 high (CPU half)
        if (s_phi2 && phi2_q && (c64.slot.h_rd || c64.slot.h_wr) && (roml || romh || io1 || io2) &&
            st == st_q && pa != pa_q)
            e_addr <= e_addr + 1;
    end
    // hold: the tick PHI2 falls (port tick 18) must still show the cycle's
    // address, R/W, strobe and data; compare the falling tick with the one before
    reg [15:0] pa_hi; reg [4:0] st_hi; reg [7:0] d_hi; reg x_hi;
    always @(posedge clk) begin
        if (s_phi2) begin pa_hi <= pa; st_hi <= st; d_hi <= s_d_o; x_hi <= x_out; end
        if (phi2_q && !s_phi2 && !prw && (st_hi[3:0] != 4'hF) &&
            (pa != pa_hi || st != st_hi || (x_hi && (!x_out || s_d_o != d_hi))))
            e_wr_hold <= e_wr_hold + 1;
    end

    // used samples: the slot takes D at the end of core ticks 15 and 31 (ce)
    wire samp = c64.ce && (c64.slot.tk == 5'd15 || c64.slot.tk == 5'd31);
    wire used_cpu = c64.slot.tk == 5'd15 && c64.slot.h_rd;
    wire used_vic = c64.slot.h_vu && romh && prw;
    always @(posedge clk)
        if (samp && (used_cpu || used_vic) && c_drv) begin
            if (used_cpu) begin
                n_cpu_rd <= n_cpu_rd + 1;
                if (age < min_cpu_age) min_cpu_age <= age;
                if (!ready) e_cpu_late <= e_cpu_late + 1;
                if (s_d_i != want) e_cpu_data <= e_cpu_data + 1;
            end else begin
                n_vic_rd <= n_vic_rd + 1;
                if (age < min_vic_age) min_vic_age <= age;
                if (!ready) e_vic_late <= e_vic_late + 1;
                if (s_d_i != want) e_vic_data <= e_vic_data + 1;
            end
        end
endmodule
