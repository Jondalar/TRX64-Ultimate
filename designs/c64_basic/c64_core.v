// SPDX-License-Identifier: GPL-3.0-or-later
// The C64 part of c64_basic: icepi's c64_top with its unused interfaces
// tied off, and block RAM for its memory port. Synthesized out of context and
// cached (tools/vivado_lib.tcl), so a change in top.v does not resynthesize it.
//
// Machine pause (T12 B): the C64 runs on gclk, clk through a BUFGCE in top.v
// whose CE is gate_ce. halt_req stops that clock right after a phi2 falling
// edge, so the VIC, CPU (mid-instruction as it is), CIAs with their TOD, SID,
// REU, cartridge and keyboard scan all stop on the same cycle boundary; only
// c64_mem's second port (debug, the app's RAM DMA) stays on clk. A bus DMA
// byte while halted opens the clock until its cycle is over: the first one
// also holds the CPU (RDY at its next read, as under the REU) for the rest of
// the pause, so each byte costs the machine one cycle, the way a real DMA
// does. Resume waits until the clock has been stopped for a whole number of
// VIC frames, so the VIC stays where vic_scaler's 1080p frame expects it.
//
// System Mode (T40, vmode from mode_ctl.v: 0 PAL, 1 NTSC, 2 PAL-60, 3
// NTSC-50; it changes only while the machine is halted or the clock stopped):
// the mode in use (vm) follows it with the gate open: the VIC's timing
// restarts with the new chip (kawari reset_req for 8 clocks, its reset kept
// from the CPU and the other chips for 64), everything else keeps its state.
module c64_core #(
    // flags as icepi's firmware/main.h numbers them: CIA and SID model, dual
    // SID, pan, auto mono, VA delay and fire = space as in icepi's reset value;
    // joystick invert off (joy1 = port 1, joy2 = port 2); REU off (bit 12 comes from reu_en);
    // overlay, DolphinDOS ROM and IEC disconnect off; cartridge (bit 11) when
    // the app has set a cartridge type. Bit 0 = 1: 8521 CIAs, 0: 6526.
    parameter [15:0] C64_FLAGS = 16'b0000_0001_0011_1111,
    parameter        SLOT_EN = 1,               // the real expansion port (slot_port.v); 0: idle
    parameter [22:0] TURBO_HOLD = 23'h400000    // T29: 2^22 PHI2 cycles at normal speed after a reset
) (
    input  wire       clk,                  // dot4x: 31.5278 MHz PAL (T40: per System Mode)
    input  wire       gclk,                 // clk through the BUFGCE, CE = gate_ce
    output wire       gate_ce,
    input  wire       rst,                  // sync to clk
    input  wire       halt_req,             // pause the whole machine (clk)
    output wire       halted,               // it is paused (also during a DMA's cycle)
    input  wire [1:0] vmode,                // System Mode (T40), see above
    input  wire       snd_clk,              // the SID's sample side (audio MCLK, 12.29 MHz)
    input  wire       snd_rst,              // sync to snd_clk
    input  wire       audio_sample_en,      // snd_clk, 48 kHz (the I2S frame)
    output wire signed [23:0] audio_l, audio_r, // snd_clk
    // T25.1 SID engine (cores/trx_sid): clk2x = 2 x clk from the same MMCM,
    // not gated (the engine idles between C64 cycles); the app's UltiSID
    // registers (quasi-static), filter curve writes (sid_cv_clk = the app's
    // clock), envelope levels (asynchronous read)
    input  wire       clk2x,
    input  wire [15:0] sid_cfg_base,
    input  wire [15:0] sid_cfg_mask,
    input  wire [2:0] sid_cfg_split,
    // T37 SID sockets: the app's C64_SID1/2_EN/BASE/MASK and C64_STEREO_ADDRSEL (0 Off, 1-5 = A5-A9),
    // any clock, quasi-static (synchronized here)
    input  wire [1:0] sock_en,
    input  wire [15:0] sock_base,
    input  wire [15:0] sock_mask,
    input  wire [2:0] stereo_sel,
    input  wire [1:0] sid_cfg_8580,
    input  wire [1:0] sid_cfg_res,
    input  wire [3:0] sid_cfg_digi,
    input  wire [49:0] sid_cfg_rs8,
    output wire       sid_resid8,       // CAPS3 b0 of trx_io: the 8-SID reDIP with the T25.5 filter control
    input  wire       sid_cv_clk,
    input  wire       sid_cv_we,
    input  wire [11:0] sid_cv_addr,
    input  wire [7:0] sid_cv_wdata,
    input  wire [4:0] sid_lv_addr,
    output wire [7:0] sid_lv_data,
    input  wire [31:0] sid_mix_gains,       // U64_AUDIO_MIXER bytes 0-3 (T25.3; quasi-static)
    input  wire [31:0] sid_mix_gains2,      // bytes 8-11: Sampler L / R (T25.6; quasi-static)
    // T25.6 sampler (trx_sampler.v): enable (C64_SAMPLER_ENABLE, any clock), the app's byte window (toggle
    // handshake, trx_io page 8), the DDR2 read it asks the REU's port for (toggle handshake, top.v)
    input  wire       smp_en,
    input  wire       smp_a_tgl,
    input  wire       smp_a_we,
    input  wire [7:0] smp_a_addr,
    input  wire [7:0] smp_a_wdata,
    output wire       smp_a_rs_tgl,
    output wire [7:0] smp_a_rdata,
    output wire       smp_f_req_tgl,
    output wire [26:0] smp_f_addr,
    input  wire       smp_f_done_tgl,
    input  wire [31:0] smp_f_data,
    output wire [3:0] pix_color,            // VIC native pixel stream (clk)
    output wire [9:0] pix_x,
    output wire [8:0] pix_y,
    output wire [4:0] leds,
    input  wire [63:0] keys,                // board keyboard (clk), col*8 + row
    input  wire [4:0] joy1, joy2,           // up, down, left, right, fire (clk)
    input  wire       restore,
    input  wire       freeze_btn,           // a freezer cartridge's button, held (clk)
    input  wire [15:0] dbg_addr,            // debug port into the C64 RAM (clk)
    input  wire       dbg_we,
    input  wire [7:0] dbg_wdata,
    output wire [7:0] dbg_rdata,
    // REU memory (icepi's p1 port: 16-bit words, valid/ready, rdata_valid later)
    output wire [23:0] reu_cmd_addr,
    output wire       reu_cmd_we,
    output wire       reu_cmd_valid,
    input  wire       reu_cmd_ready,
    output wire [15:0] reu_wdata,
    output wire [1:0] reu_wdata_we,
    input  wire [15:0] reu_rdata,
    input  wire       reu_rdata_valid,
    input  wire       reu_en,               // the app's REU_ENABLE (clk, quasi-static): REU present
    input  wire [2:0] reu_size,             // the app's REU_SIZE: 0 = 128 KB ... 7 = 16 MB (address mask)
    input  wire       cpu_reset_req,        // hold the CPU in reset (clk)
    input  wire       cpu_pause_req,        // stop the CPU at the next read cycle (clk)
    // T29 turbo (c64_turbo.v): the app's setting (page "011" 0x02, 0x2D and
    // the strobe 0x2E as a toggle; quasi-static), the engine's status words
    // and the clear of its sticky "ran" bit (a clk pulse), whether the engine
    // drives the CPU now; dbg_req: the outside wants c64_mem's second port in this clock
    input  wire [7:0] turbo_en,
    input  wire [7:0] turbo_prefer,
    input  wire       turbo_upd_tgl,
    output wire [31:0] turbo_stat,
    output wire [31:0] turbo_w1,
    output wire [31:0] turbo_w2,
    input  wire       turbo_ran_clr,
    output wire       turbo_on,
    input  wire       dbg_req,
    // cartridge (cart_slot): the app's CART_TYPE (taken at every CPU reset and
    // with a force), kill/force toggles, ACTIVE back; its requests to DDR2
    // (line fetches, byte writes) for cart_fill, which writes the cache's block
    // RAM in cart_wclk; for trx_io in the app's clock: the flash responder's
    // configuration (static while used), its pending operation, ACK and cache
    // invalidation (toggles), EEPROM changes, the GMod2 EEPROM's second port
    input  wire [7:0] cart_type,
    input  wire       cart_kill_tgl,
    input  wire       cart_force_tgl,
    output wire       cart_active,
    input  wire       cart_wclk,
    output wire       cart_req_tgl,
    output wire       cart_req_we,
    output wire [23:0] cart_req_addr,
    output wire [7:0] cart_req_data,
    output wire [5:0] cart_req_slot,
    output wire       cart_req_cached,
    input  wire       cart_ack_tgl,
    input  wire [7:0] cart_fill_be,
    input  wire [8:0] cart_fill_addr,
    input  wire [63:0] cart_fill_data,
    input  wire       cart_fl_enable,
    input  wire [15:0] cart_fl_magic1,
    input  wire [15:0] cart_fl_magic2,
    input  wire [15:0] cart_fl_mask,
    input  wire [23:0] cart_fl_id,
    input  wire [343:0] cart_desc,      // the cartridge's descriptor (trx_io page 4; static around reset)
    output wire       cart_evt_tgl,
    output wire [33:0] cart_evt_word,
    input  wire       cart_fl_ack_tgl,
    input  wire       cart_inval_tgl,
    output wire       cart_ee_tgl,
    input  wire       cart_ee_clk,
    input  wire [10:0] cart_ee_addr,
    input  wire       cart_ee_we,
    input  wire [7:0] cart_ee_wdata,
    output wire [7:0] cart_ee_rdata,
    // the Ultimate app's DMA as C64 bus cycles (clk), while the CPU is paused:
    // req pulses with addr/we/wdata, done pulses with rdata
    input  wire       dma_req,
    input  wire [15:0] dma_addr,
    input  wire       dma_we,
    input  wire [7:0] dma_wdata,
    output wire       dma_done,
    output wire [7:0] dma_rdata,
    input  wire       ultimax,              // C64_MODE bit 1 (clk)
    input  wire [1:0] dma_force,            // the lens unit: select of a DMA read forced (c64_top ext_dma_force)
    // serial bus (clk), '0' = line low: the C64's own pulls out, the bus in
    output wire       iec_atn_o, iec_clk_o, iec_data_o,
    input  wire       iec_atn_i, iec_clk_i, iec_data_i,
    // state bus (clk; ARCH-lean-state-access §4-§5): word sb_addr = {chip,
    // word} of the capture shadows, sb_rdata two clocks later; map below.
    // dbg_rom: the second RAM port reads and writes the ROM block (c64_mem.v)
    input  wire [11:0] sb_addr,
    output reg  [31:0] sb_rdata,
    output wire       sb_frozen,            // the gate is shut and no DMA cycle is open
    input  wire       dbg_rom,
    // capture control: a frame capture every cap_every frames (0 = none);
    // cap_gen counts captures (frame or stop), cap_tgl toggles with each,
    // cap_nosnap counts armed frames whose snap point did not come
    input  wire [7:0] cap_every,
    output reg [15:0] cap_gen = 16'd0,
    output reg        cap_tgl = 1'b0,
    output reg [15:0] cap_nosnap = 16'd0,
    // frame markers for the write log (trx_engine.v, T20 §4.1): with log_on a
    // snap point is looked for every frame; mk pulses one clock after the
    // frame's boundary edge (the snap edge, or line 16 when no snap came:
    // mk_snap 0), mk_cap when that edge also loaded the shadows (a frame of
    // the cadence), mk_skip when it would have but cap_block (the engine still
    // copies the previous block) held it off; mk_x / mk_line the VIC position
    // of the edge; phi2_tick pulses one clock after every phi2 falling edge
    input  wire       log_on,
    input  wire       cap_block,
    output reg        mk = 1'b0,
    output reg        mk_snap = 1'b0,
    output reg        mk_cap = 1'b0,
    output reg        mk_skip = 1'b0,
    output reg        mk_intg = 1'b0,       // the snap is a fetch an interrupt drops
    output wire       reu_busy,             // the REU moves data (the frame is not exact)
    output reg  [9:0] mk_x = 10'd0,
    output reg  [8:0] mk_line = 9'd0,
    // restore (ARCH §5.6, T20 §8.1): staged writes into the words of the
    // hidden state (sb_we with its own address, while the gate is shut), and
    // sb_ldreq arming their load (b1 CPU, b3 CIA 1, b4 CIA 2: chip ids) at the
    // next opening edge; a stop on line 0 before the phi2 falling edge that
    // ends cycle pos_cyc (pos_arm, one shot), held until pos_release, so that
    // edge loads the state captured on the same edge of the snap; the resume
    // condition shifted by resume_phase dot4x clocks (0: whole frames)
    input  wire       sb_we,
    input  wire [11:0] sb_waddr,
    input  wire [31:0] sb_wdata,
    input  wire       sb_ldreq,
    input  wire [4:0] sb_ldmask,
    output reg  [4:0] sb_ldarm = 5'd0,
    input  wire       pos_set,
    input  wire       pos_arm_in,
    input  wire [5:0] pos_cyc,
    input  wire       pos_release,
    output reg        pos_arm = 1'b0,
    output reg        pos_hold = 1'b0,
    input  wire [19:0] resume_phase,
    input  wire       rp_set,               // resume_phase written: for the next resume only
    output reg  [19:0] rp_pend = 20'd0,
    output reg        phi2_tick = 1'b0,
    // the machine's own memory writes for the write log: *_we high in the
    // clock whose gclk edge writes, *_old the byte (nibble) it overwrote,
    // valid from the next clock on. C64 RAM (c64_mem port A), colour RAM (port
    // A), cartridge RAM (cart_slot's queue: its old byte comes from DDR2)
    output wire       wa_we,
    output wire [15:0] wa_addr,
    output wire [7:0] wa_new,
    output wire [7:0] wa_old,
    output wire       wc_we,
    output wire [9:0] wc_addr,
    output wire [3:0] wc_new,
    output wire [3:0] wc_old,
    output wire       wk_we,
    output wire [14:0] wk_addr,
    output wire [7:0] wk_new,
    // colour RAM second port (clk), low nibble, read data one clock later
    input  wire [9:0] cram_addr,
    input  wire       cram_we,
    input  wire [3:0] cram_wdata,
    output wire [3:0] cram_rdata,
    // the UCI's slot side (uci_unit.vhd, T28; clk): the bus address now (or the lens unit's look address
    // while the bus is elsewhere, U13);
    // io_ev pulses one clock after each phi2 falling edge with that edge's
    // address, data, direction and whether it was a real access to
    // $DE00-$DFFF with I/O mapped; a read takes io_dout while io_oe (before
    // the REU and the cartridge); io_irq is one more C64 IRQ source
    output wire [15:0] bus_addr,
    output reg        io_ev = 1'b0,
    output reg [15:0] io_ev_addr = 16'd0,
    output reg  [7:0] io_ev_data = 8'd0,
    output reg        io_ev_we = 1'b0,
    output reg        io_ev_io = 1'b0,
    // T62 G17: a CPU write to $D7FF (I/O mapped), one clock after the phi2 falling edge that ends it
    output reg        d7ff_ev = 1'b0,
    output reg  [7:0] d7ff_data = 8'd0,
    input  wire       io_oe,
    input  wire [7:0] io_dout,
    input  wire       io_irq,
    // debug (trx_dbg.v, T21; clk): its registers as 32-bit words from
    // trx_sys.v (page 0 from 0x118, page 3; read data from dr_addr), the
    // debugger's ownership (top.v takes the app's C64_STOP away with it), a
    // pulse per stop (IRQ cause b0), the app's C64_STOP for STATUS, the drive's
    // stop and step-done pulses (drv_dbg.v), log records for the engine
    input  wire       dr_we,
    input  wire       dr_re,
    input  wire [10:0] dr_addr,
    input  wire [31:0] dr_wdata,
    output wire [31:0] dr_rdata,
    output wire       dbg_own,
    output wire       dbg_ev,
    // a debugger stop (T12 §14.8): hold = the debugger holds the machine, stop = the
    // gate is shut by it and no step runs (ultimate_sys opens the app window's D page for
    // it), rel = RUN or a step is requested (one clock)
    output wire       dbg_hold,
    output wire       dbg_stop,
    output wire       dbg_rel,
    output wire       tr_ib,                // trace former feed (trx_dbg.v, trx_engine.v)
    output wire [15:0] tr_pc,
    output wire  [7:0] tr_op,
    output wire  [7:0] tr_b1,
    output wire  [7:0] tr_b2,
    output wire [39:0] tr_regs,
    input  wire       fw_stop,
    input  wire       drv_stop,
    input  wire       drv_done,
    output wire       dbg_log_put,
    output wire [63:0] dbg_log_rec,
    // the real expansion port (T16, slot_port.v; clk): the app's
    // C64_BUS_INTERNAL/EXTERNAL (quasi-static), SLOT_CTRL's ROM write enable,
    // SLOT_STATUS and the counters; the pins (top.v has the buffers)
    input  wire [3:0] slot_int,
    input  wire [3:0] slot_ext,
    input  wire       slot_wr_en,
    output wire [31:0] slot_status,
    output wire [31:0] slot_cnt0,
    output wire [31:0] slot_cnt1,
    output wire [31:0] slot_last,
    output wire [15:0] slot_a,
    output wire       slot_ahi_t,           // 1: A12-A15 and R/W released
    output wire       slot_rw,
    output wire [7:0] slot_d_o,
    output wire       slot_d_t,             // 1: D0-D7 not driven
    input  wire [7:0] slot_d_i,
    output wire       slot_adir, slot_aoen, slot_ddir, slot_doen,
    output wire       slot_roml, slot_romh, slot_io1, slot_io2,
    output wire       slot_phi2, slot_dotclk, slot_ba, slot_rst_o,
    output wire       slot_sid1, slot_sid2, slot_sid_a,    // T37: SLOT_SID1n/2n (low = selected), SLOT_SID_ADDR
    input  wire       slot_game, slot_exrom, slot_dma, slot_irq, slot_nmi, slot_rst_i,
    // a look at the cartridge and the REU without a bus cycle (clk; the
    // lens unit, T12 §11 Q2, T21 §3.4.1): a read of pk_addr as ROML (pk_cls 0), ROMH
    // (1), IO1 (2) or IO2 (3) would return nothing (pk_hit 0), a register
    // value (pk_mem 0: pk_data) or the byte at pk_maddr in DDR2 (cart_slot's
    // address: b22 cartridge RAM, else the ROM image). Combinational from the
    // paused machine's registers.
    input  wire [15:0] pk_addr,
    input  wire [1:0] pk_cls,
    output wire       pk_hit,
    output wire       pk_mem,
    output wire [23:0] pk_maddr,
    output wire [7:0] pk_data,
    output wire       pk_ext,              // the real expansion port serves this read (no look)
    // U13: classes 2 and 3 ask the whole I/O page by address: the device the arbiter's decode picks for a read of
    // pk_addr with I/O mapped, and its read view (c64_top, the cartridge, our own responder). pk_ext then: no read view
    // without a bus cycle (a socket chip, the real port, the reDIP build's SID). pk_on while the lens unit's look runs;
    // pk_cram: the low nibble is the colour RAM's (dma_win reads it through its port); pk_open: bits of pk_data are
    // the open bus; pk_wait: the UCI's answer for pk_addr is not there yet
    input  wire       pk_on,
    output wire       pk_cram,
    output wire       pk_open,
    output wire       pk_wait
);
    wire [23:0] mem_cmd_addr;
    wire        mem_cmd_we, mem_cmd_valid, mem_cmd_ready;
    wire [15:0] mem_wdata, mem_rdata;
    wire  [1:0] mem_wdata_we;
    wire        mem_wdata_ready, mem_rdata_valid;
    wire        usb_dp_0, usb_dn_0, usb_dp_1, usb_dn_1;

    // the real port's side (slot_port below)
    wire        s_rom, s_io1_rd, s_io2_rd, s_io2_ext, s_irq_ext, s_present, s_game, s_exrom;
    wire        s_irq, s_nmi, s_rst_req, xc_vic, xc_ba;
    wire [11:0] xc_vaddr;
    wire  [7:0] s_dout;
    wire        port_rd;                    // this read is the port's
    // T37: the app's socket registers into clk; sk_sel[n] = this bus cycle is an access of socket n+1
    (* ASYNC_REG = "TRUE" *) reg [36:0] sk_c1 = 37'd0, sk_c = 37'd0;
    always @(posedge clk) begin
        sk_c1 <= {stereo_sel, sock_mask, sock_base, sock_en};
        sk_c  <= sk_c1;
    end
    wire  [1:0] sk_sel;
    wire        sk_we;                      // a socket cycle writes (not xc_we: the cartridge models must not see it)
    wire        cpu_rst_in = cpu_reset_req || s_rst_req;

    // bit 11, the cartridge: a CRT, or a resource of the real port (taken at reset)
    wire [15:0] flags = C64_FLAGS | {3'd0, reu_en, cart_type[4:0] != 5'd0 || s_present, 11'd0};

    wire [15:0] xc_addr;
    wire  [7:0] xc_din, xc_dout;
    wire        xc_we, xc_roml, xc_romh, xc_io1, xc_io2, xc_phi2_p, xc_phi2_h, xc_phi2_n, xc_reset;
    wire        xc_exrom, xc_game, xc_nmi, xc_irq, xc_io2_we_valid, xc_wait;
    wire        x_io_sel, x_io_we, x_dbg_we;
    wire        l_rdy, l_cbus;              // the CPU's RDY, the CPU is the bus master
    wire        dbg_brk, dbg_keep, dbg_go, dbg_ib, dbg_armed;
    // T29: the turbo CPU's port into c64_mem's second port, the words of its state
    wire [15:0] tb_addr;
    wire [14:0] tb_raddr;
    wire        tb_we, tb_took;
    wire  [7:0] tb_wdata, tb_ram_lo, tb_ram_hi, tb_rom_lo, tb_rom_hi;
    wire [31:0] turbo_pack;
    reg  [31:0] g_t1 = 32'd0;                   // staging for a restore (chip 0 word 1)
    reg  [31:0] s_t1 = 32'd0;                   // its shadow
    wire        turbo_inh;
    wire [15:0] dbg_ipc;
    wire  [7:0] dbg_op;

    // the chips' state for the state bus (below)
    wire [383:0] l_vic;
    wire [127:0] l_cia1, l_cia2;
    wire  [31:0] l_cia1p, l_cia2p;
    wire  [39:0] l_cpu, l_cpunx;
    assign tr_pc = dbg_ipc;
    assign tr_op = dbg_op;
    assign tr_regs = l_cpunx;
    wire  [15:0] l_pc;
    wire         l_sync, l_intg, l_vok, l_vacc, l_reu;
    // G06: a byte the app steals from the running machine (nothing else holds the
    // CPU): like the REU's busy line, the snap waits and the frame's REU flag is set,
    // as the cycle is an input the frame's CPU stream does not explain
    wire         steal_run;
    wire [208:0] l_cpuf;                    // every flip-flop of the CPU, the CIAs
    wire [362:0] l_cia1f, l_cia2f;
    reg  [223:0] g_cpu = 224'd0;            // staging for their loads
    reg  [383:0] g_cia1 = 384'd0, g_cia2 = 384'd0;
    wire  [23:0] l_port;
    wire   [2:0] l_cart;
    wire         c_pk_hit, c_pk_mem;           // the cartridge look (pk_*)
    wire   [7:0] c_pk_data, c_pk_mask;
    // U13: the I/O look from c64_top (its device codes are c64_bus_arbiter.v io_decode()'s)
    wire         io_look = pk_cls[1];
    wire   [1:0] c_pk_cls = io_look ? {1'b1, pk_addr[8]} : pk_cls;   // the cartridge: IO1 or IO2 by the address
    wire   [3:0] lk_dev;
    wire   [1:0] lk_sock;
    wire         lk_xsel, lk_open, lk_na;
    wire   [7:0] lk_last, lk_data;
    wire  [15:0] l_bank;
    wire         t_a, t_c, t_k;
    assign wa_we = ce && t_a;
    assign wc_we = ce && t_c;
    assign wk_we = ce && t_k;

    // ---- the gate (clk) ---------------------------------------------------------
    // ce is the CE for the next gclk edge; ce_d the one for the edge just past,
    // so a gclk register read here is fresh when ce_d is set. Pulses into the
    // C64 are handed over only with ce set; pulses out of it count only with
    // ce_d set (a stopped register keeps showing its last value).
    // dot4x clocks per VIC frame: 312 x 63 x 32 (PAL, NTSC-50), 263 x 65 x 32
    // (NTSC, PAL-60)
    reg  [1:0]  vm = 2'd0;                  // the System Mode the VIC runs in
    wire [19:0] FRAME = (vm == 2'd1 || vm == 2'd2) ? 20'd547040 : 20'd628992;
    reg        ce = 1'b1, ce_d = 1'b1;
    reg        halted_r = 1'b0;
    reg        held = 1'b0;                 // a DMA came with a pause on: CPU held until resume
    reg [19:0] shut = 20'd0;                // gclk edges left out, modulo FRAME
    reg        dma_hold = 1'b0;             // DMA request not yet taken by the C64
    reg        rv_hold = 1'b0;              // REU read data not yet taken
    wire       dma_busy;
    wire       g_dma_done, g_reu_valid;
    wire       run = (!halt_req && !pos_hold && !dbg_hold) || rst || cpu_rst_in;
    wire       pos_hit;                     // this edge is the one before the stop position's phi2 fall
    reg  [19:0] res_at = 20'd628991;      // shut count at which a stop may end
    always @(posedge clk) res_at <= rp_pend == 20'd0 ? FRAME - 1 : rp_pend - 1'b1;
    wire       dma_in = dma_req || dma_hold;
    always @(posedge clk) begin
        ce_d     <= ce;
        dma_hold <= dma_in && !ce;
        rv_hold  <= (rv_hold || reu_rdata_valid) && !ce;
        if (!ce) shut <= shut >= FRAME - 1 ? 20'd0 : shut + 1'b1;
        if (dma_in && (!run || halted_r)) held <= 1'b1;
        if (ce) begin
            // stop with this edge, the phi2 falling one, unless a DMA needs more
            // or a debug step goes on; a debug stop source stops on its edge
            if (((!run || halted_r) && xc_phi2_n && !dma_busy && !dma_in && !dbg_keep && !turbo_on) ||
                ((pos_hit || dbg_brk) && !turbo_on)) begin
                ce       <= 1'b0;
                halted_r <= 1'b1;
            end
        end else if (dma_in) begin
            ce <= 1'b1;                     // the DMA's cycle (and the CPU's way to a read)
        end else if (dbg_go) begin
            ce   <= 1'b1;                   // a debug step: the gate stays open until its end
            held <= 1'b0;                   // and the CPU leaves a DMA's RDY hold
        end else if (run && shut == res_at) begin
            ce       <= 1'b1;               // whole frames left out (+ RESUME_PHASE): go on
            halted_r <= 1'b0;
            held     <= 1'b0;
            shut     <= 20'd0;              // the phase is taken once: the frame starts here
            rp_pend  <= 20'd0;
        end
        if (rp_set) rp_pend <= resume_phase;
        if (rst) begin
            ce <= 1'b1; halted_r <= 1'b0; held <= 1'b0; shut <= 20'd0; rp_pend <= 20'd0;
        end
    end

    // the stop position: ph counts the gclk edges since the last phi2 fall
    reg   [4:0] ph = 5'd0;
    always @(posedge clk) begin
        if (ce) ph <= xc_phi2_n ? 5'd0 : ph + 1'b1;
        if (pos_hit) begin
            pos_hold <= 1'b1;
            pos_arm  <= 1'b0;
        end
        if (pos_set) pos_arm <= pos_arm_in;
        if (pos_release) pos_hold <= 1'b0;
        // a load takes the first opening edge after it is armed
        if (ce) sb_ldarm <= 5'd0;
        if (sb_ldreq) sb_ldarm <= sb_ldmask;
        if (rst) begin pos_hold <= 1'b0; pos_arm <= 1'b0; sb_ldarm <= 5'd0; end
    end
    // the UCI's bus events: the edge that ends a CPU half (ce: gclk takes it)
    // U13: the UCI answers the slot bus address (uci_unit.vhd), its response and status through a pointer, so the look
    // reads it there: while the look asks $DE00-$DFFF its address goes to the UCI whenever the bus master's is not in
    // $DE00-$DFFF (a CPU or DMA read takes the UCI's byte only for an access there) or the machine is halted. The UCI
    // registers its answer twice: it counts after 4 clocks of that.
    wire       uci_sub = pk_on && io_look && pk_addr[15:9] == 7'b1101111 &&
                         (halted_r || xc_addr[15:9] != 7'b1101111);
    assign bus_addr = uci_sub ? pk_addr : xc_addr;
    reg  [2:0] sub_n = 3'd0;
    always @(posedge clk) sub_n <= !uci_sub ? 3'd0 : sub_n == 3'd7 ? 3'd7 : sub_n + 3'd1;
    wire       uci_ok = sub_n >= 3'd4;
    always @(posedge clk) begin
        io_ev <= ce && xc_phi2_n;
        d7ff_ev <= ce && xc_phi2_n && x_dbg_we;
        if (ce && xc_phi2_n && x_dbg_we) d7ff_data <= xc_din;
        if (ce && xc_phi2_n) begin
            io_ev_addr <= xc_addr;
            io_ev_data <= xc_din;
            io_ev_we   <= x_io_we;
            io_ev_io   <= x_io_sel;
        end
    end

    // T40: a new System Mode restarts the VIC's timing with the gate open
    reg  [5:0]  trst = 6'd0;                // clocks of the restart left
    always @(posedge clk) begin
        if (trst != 6'd0) begin
            if (ce) trst <= trst - 6'd1;
        end else if (ce && vm != vmode) begin
            vm   <= vmode;
            trst <= 6'd63;
        end
        if (rst) begin vm <= vmode; trst <= 6'd0; end
    end
    wire vic_trst      = trst > 6'd55;      // kawari reset_req: 8 gclk edges
    wire vic_trst_mask = trst != 6'd0;      // its reset (31 clocks after) kept from the CPU

    assign gate_ce = ce;
    assign halted  = halted_r;
    assign dma_done = g_dma_done && ce_d;
    assign reu_cmd_valid = g_reu_valid && ce;

    // T25.6: the Ultimate Audio sampler on the slot bus ($DF20-$DFFF), next to the UCI (or-ed above)
    wire        smp_oe, smp_irq, smp_l_oe;
    wire  [7:0] smp_dout, smp_l_dout;
    wire signed [22:0] smp_l, smp_r;
    trx_sampler smp (
        .clk(clk), .clk2x(clk2x), .rst(rst), .vmode(vm), .c64_en(smp_en),
        .bus_addr(xc_addr), .io_ev(io_ev), .io_ev_addr(io_ev_addr), .io_ev_data(io_ev_data),
        .io_ev_we(io_ev_we), .io_ev_io(io_ev_io), .oe(smp_oe), .dout(smp_dout), .irq(smp_irq),
        .la(pk_addr), .l_oe(smp_l_oe), .l_dout(smp_l_dout),
        .a_tgl(smp_a_tgl), .a_we(smp_a_we), .a_addr(smp_a_addr), .a_wdata(smp_a_wdata),
        .a_rs_tgl(smp_a_rs_tgl), .a_rdata(smp_a_rdata),
        .f_req_tgl(smp_f_req_tgl), .f_addr(smp_f_addr), .f_done_tgl(smp_f_done_tgl), .f_data(smp_f_data),
        .smp_l(smp_l), .smp_r(smp_r));

    c64_top #(
        .C1541_EXTERNAL_ROM(0),
        .C1541(0),                          // the app's drive is outside (iec_bus.v)
        .TURBO_HOLD(TURBO_HOLD),
        .SYS_CLK_FREQUENCY(31_527_778),
        .TMDS_CLK_FREQUENCY(27_586_806)
    ) c64 (
        .clk(gclk), .rst(rst),
        .vic_reset_req(1'b0), .cpu_reset_req(cpu_rst_in), .cpu_pause_req(cpu_pause_req || held || dma_busy),
        .gate_ce(ce), .turbo_inhibit(turbo_inh),
        .tcfg_en(turbo_en), .tcfg_prefer(turbo_prefer), .tcfg_upd_tgl(turbo_upd_tgl),
        .tb_addr(tb_addr), .tb_raddr(tb_raddr), .tb_we(tb_we), .tb_wdata(tb_wdata),
        .tb_ram_lo(tb_ram_lo), .tb_ram_hi(tb_ram_hi), .tb_rom_lo(tb_rom_lo), .tb_rom_hi(tb_rom_hi), .tb_took(tb_took),
        .turbo_stat(turbo_stat), .turbo_w1(turbo_w1), .turbo_w2(turbo_w2), .turbo_pack(turbo_pack),
        .turbo_ld(sb_ldarm[0]), .turbo_ld_pack(g_t1),
        .turbo_ran_clr(turbo_ran_clr), .turbo_on(turbo_on),
        .vmode(vm), .vic_trst(vic_trst), .vic_trst_mask(vic_trst_mask),

        .mem_cmd_addr(mem_cmd_addr), .mem_cmd_we(mem_cmd_we), .mem_cmd_valid(mem_cmd_valid),
        .mem_cmd_ready(mem_cmd_ready), .mem_wdata(mem_wdata), .mem_wdata_we(mem_wdata_we),
        .mem_wdata_ready(mem_wdata_ready), .mem_rdata(mem_rdata), .mem_rdata_valid(mem_rdata_valid),

        .reu_mem_cmd_addr(reu_cmd_addr), .reu_mem_cmd_we(reu_cmd_we),
        .reu_mem_cmd_valid(g_reu_valid), .reu_mem_cmd_ready(reu_cmd_ready),
        .reu_mem_wdata(reu_wdata), .reu_mem_wdata_we(reu_wdata_we), .reu_mem_wdata_ready(1'b1),
        .reu_mem_rdata(reu_rdata), .reu_mem_rdata_valid((reu_rdata_valid || rv_hold) && ce),

        .leds(leds),
        // kawari's own 576p DVI output is not used (vic_scaler takes the
        // native pixels); this clock times the SID's sample side.
        .tmds_clk(snd_clk), .tmds_rst(snd_rst),
        .tmds_0(), .tmds_1(), .tmds_2(),

        .usb_clk(gclk), .usb_rst(1'b1),
        .usb_dp_0(usb_dp_0), .usb_dn_0(usb_dn_0), .usb_pullup_dp_0(), .usb_pullup_dn_0(),
        .usb_dp_1(usb_dp_1), .usb_dn_1(usb_dn_1), .usb_pullup_dp_1(), .usb_pullup_dn_1(),

        .flags(flags), .reu_size(reu_size),

        .iec_data_out(iec_data_o), .iec_clk_out(iec_clk_o), .iec_atn_out(iec_atn_o),
        .iec_data_in(iec_data_i), .iec_clk_in(iec_clk_i), .iec_atn_in(iec_atn_i),

        .drive_rom_en(), .drive_rom_addr(), .drive_rom_dout(32'hFFFFFFFF),
        .drive_shmem_en(), .drive_shmem_addr(), .drive_shmem_din(),
        .drive_shmem_dout(32'h0), .drive_shmem_we(),
        .block_lba(), .block_cnt(), .block_rd(), .block_wr(), .block_ack(1'b0),
        .img_id(16'h0), .img_readonly(1'b1), .img_present(1'b0), .img_mounted(1'b0),

        .overlay_pixel(24'h0), .overlay_pixel_x(), .overlay_pixel_y(), .overlay_pixel_valid(1'b0),
        .hid_key_report_out(), .hid_key_modifiers_out(), .hid_key_0_out(),

        .tape_play_toggle(1'b0), .tape_cass_sense_n(), .tape_cass_motor_n(),
        .tap_fifo_rd_data(8'h0), .tap_fifo_rd_valid(1'b0), .tap_fifo_rd_en(),
        .ps2_fifo_rd_en(), .ps2_fifo_rd_data(8'h0), .ps2_fifo_rd_valid(1'b0),

        .cart_flags(16'h0300),          // icepi's own cartridge: unused (ext_cart_en)
        .flash_enable(1'b0), .flash_magic1(16'h0), .flash_magic2(16'h0), .flash_mask(16'h0),
        .flash_id_mfr(8'h0), .flash_id_dev(8'h0), .flash_id_ofs(8'h0),
        .flash_evt_0(), .flash_evt_pulse(), .flash_ack(1'b0), .eeprom_written(),

        .kbd_overlay_pulse(), .kbd_reset_pulse(), .kbd_tape_play_pulse(),

        .vic_pix_color(pix_color), .vic_pix_x(pix_x), .vic_pix_y(pix_y),

        .ext_keys(keys), .ext_joy1(joy1), .ext_joy2(joy2), .ext_restore(restore),
        .ext_audio_sample_en(audio_sample_en), .audio_word_0(audio_l), .audio_word_1(audio_r),
        .clk2x(clk2x), .sid_cfg_base(sid_cfg_base), .sid_cfg_mask(sid_cfg_mask),
        .sid_cfg_split(sid_cfg_split), .sid_cfg_8580(sid_cfg_8580), .sid_cfg_res(sid_cfg_res),
        .sid_cfg_digi(sid_cfg_digi), .sid_cfg_rs8(sid_cfg_rs8), .sid_cv_clk(sid_cv_clk), .sid_cv_we(sid_cv_we),
        .sid_cv_addr(sid_cv_addr), .sid_cv_wdata(sid_cv_wdata),
        .sid_lv_addr(sid_lv_addr), .sid_lv_data(sid_lv_data), .sid_mix_gains(sid_mix_gains),
        .sid_mix_gains2(sid_mix_gains2), .smp_l(smp_l), .smp_r(smp_r),
        .ext_io2_wr(), .ext_io_addr(), .ext_io_data(),
        .ext_dma_req(dma_in && ce), .ext_dma_addr(dma_addr), .ext_dma_we(dma_we),
        .ext_dma_wdata(dma_wdata), .ext_dma_done(g_dma_done), .ext_dma_rdata(dma_rdata),
        .ext_ultimax(ultimax), .ext_dma_force(dma_force),
        .ext_io_sel(x_io_sel), .ext_io_we(x_io_we), .ext_dbg_we(x_dbg_we), .ext_io_oe(io_oe | smp_oe), .ext_io_dout(smp_oe ? smp_dout : io_dout),
        .ext_io_irq(io_irq | smp_irq),
        .ext_cart_en(1'b1),
        .ext_cart_addr(xc_addr), .ext_cart_din(xc_din), .ext_cart_we(xc_we),
        .ext_cart_roml(xc_roml), .ext_cart_romh(xc_romh), .ext_cart_io1_cen(xc_io1),
        .ext_cart_io2_cen(xc_io2), .ext_cart_phi2_p(xc_phi2_p), .ext_cart_phi2_h(xc_phi2_h),
        .ext_cart_phi2_n(xc_phi2_n), .ext_cart_reset(xc_reset),
        .ext_cart_vic(xc_vic), .ext_cart_vaddr(xc_vaddr), .ext_cart_ba(xc_ba),
        .ext_cart_exrom(s_rom ? s_exrom : xc_exrom), .ext_cart_game(s_rom ? s_game : xc_game),
        .ext_cart_dout(port_rd ? s_dout : xc_dout),
        .ext_cart_nmi(xc_nmi || (s_irq_ext && s_nmi)), .ext_cart_irq(xc_irq || (s_irq_ext && s_irq)),
        .ext_cart_io2_we_valid(xc_io2_we_valid || s_io2_ext),
        .ext_cart_wait(xc_wait), .ext_dma_busy(dma_busy),
        .sock_en(sk_c[1:0]), .sock_base(sk_c[17:2]), .sock_mask(sk_c[33:18]), .ext_cart_sock(sk_sel), .ext_cart_sock_we(sk_we),
        .sb_vic(l_vic), .sb_cia1(l_cia1), .sb_cia2(l_cia2), .sb_cia1_port(l_cia1p),
        .sb_cia2_port(l_cia2p), .sb_cpu_regs(l_cpu), .sb_cpu_next(l_cpunx), .sb_cpu_pc(l_pc), .sb_cpu_sync(l_sync), .sb_cpu_intg(l_intg),
        .sb_cpu_rdy(l_rdy), .sb_cpu_bus(l_cbus),
        .sb_ld_cpu(sb_ldarm[1]), .sb_stg_cpu(g_cpu[208:0]), .sb_cur_cpu(l_cpuf),
        .sb_ld_cia1(sb_ldarm[3]), .sb_stg_cia1(g_cia1[362:0]), .sb_cur_cia1(l_cia1f),
        .sb_ld_cia2(sb_ldarm[4]), .sb_stg_cia2(g_cia2[362:0]), .sb_cur_cia2(l_cia2f),
        .sb_vic_snap_ok(l_vok), .sb_vic_access(l_vacc), .sb_reu_busy(l_reu),
        .sb_cpu_port(l_port), .sb_cart(l_cart),
                .lk_addr(pk_addr), .lk_dev(lk_dev), .lk_sock(lk_sock), .lk_xsel(lk_xsel), .lk_last(lk_last),
        .lk_data(lk_data), .lk_open(lk_open), .lk_na(lk_na),
        .cram_clk(clk), .cram_addr(cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata),
        .cram_rdata(cram_rdata),
        .cram_tap_we(t_c), .cram_tap_addr(wc_addr), .cram_tap_din(wc_new), .cram_tap_old(wc_old)
    );

    cart_slot cart (
        .clk(gclk),
        .cart_type(cart_type), .desc(cart_desc), .cart_kill_tgl(cart_kill_tgl), .cart_force_tgl(cart_force_tgl),
        .cart_active(cart_active),
        .addr(xc_addr), .din(xc_din), .we(xc_we), .roml_n(xc_roml), .romh_n(xc_romh),
        .io1_n(xc_io1), .io2_n(xc_io2), .phi2_p(xc_phi2_p), .phi2_h(xc_phi2_h), .phi2_n(xc_phi2_n),
        .reset(xc_reset), .exrom(xc_exrom), .game(xc_game), .dout(xc_dout), .nmi(xc_nmi),
        .irq(xc_irq), .io2_we_valid(xc_io2_we_valid), .wait_cpu(xc_wait),
        .req_tgl(cart_req_tgl), .req_we(cart_req_we), .req_addr(cart_req_addr),
        .req_data(cart_req_data), .req_slot(cart_req_slot), .req_cached(cart_req_cached),
        .ack_tgl(cart_ack_tgl),
        .fill_clk(cart_wclk), .fill_be(cart_fill_be), .fill_addr(cart_fill_addr),
        .fill_data(cart_fill_data), .st_bank(l_bank),
        .fl_enable(cart_fl_enable), .fl_magic1(cart_fl_magic1), .fl_magic2(cart_fl_magic2),
        .fl_mask(cart_fl_mask), .fl_id(cart_fl_id), .evt_tgl(cart_evt_tgl), .evt_word(cart_evt_word),
        .fl_ack_tgl(cart_fl_ack_tgl), .inval_tgl(cart_inval_tgl), .ee_tgl(cart_ee_tgl),
        .ee_clk(cart_ee_clk), .ee_addr(cart_ee_addr), .ee_we(cart_ee_we), .ee_wdata(cart_ee_wdata),
        .ee_rdata(cart_ee_rdata), .freeze(freeze_btn),
        .wtap(t_k), .wtap_addr(wk_addr), .wtap_data(wk_new),
        .pk_addr(pk_addr), .pk_cls(c_pk_cls), .pk_hit(c_pk_hit), .pk_mem(c_pk_mem), .pk_maddr(pk_maddr),
        .pk_data(c_pk_data), .pk_mask(c_pk_mask)
    );

    // ---- the look ------------------------------------------------------------------------------------------------
    // ROML/ROMH (classes 0, 1): the cartridge's, or the real port's (no look). The I/O page (classes 2, 3, U13): the
    // device the arbiter's io_decode() picks, in the arbiter's priority: the colour RAM before the I/O mux, then the
    // VIC (turbo registers included), a socket chip, the SID, the CIAs, the REU and the cartridge (c64_top's lk_data
    // for its own devices), and over $DE00-$DFFF without a socket our own responder (sampler, UCI) last.
    reg        lk_x, lk_m, lk_c, lk_o, lk_w;
    reg  [7:0] lk_d;
    always @(*) begin
        lk_x = 1'b0; lk_m = 1'b0; lk_c = 1'b0; lk_w = 1'b0;
        lk_o = lk_open; lk_d = lk_data;
        if (lk_dev == 4'd3)
            lk_c = 1'b1;                                    // colour RAM
        else if (lk_dev == 4'd1)
            ;                                               // VIC
        else if (lk_sock != 2'b00 || lk_na)
            lk_x = 1'b1;                                    // a socket chip, or no read view here
        else if (lk_dev == 4'd6 || lk_dev == 4'd7) begin    // the cartridge
            if (pk_addr[8] ? s_io2_rd : s_io1_rd)
                lk_x = 1'b1;                                // the real port's
            else if (c_pk_hit && c_pk_mem)
                lk_m = 1'b1;                                // its RAM: dma_win reads DDR2 at pk_maddr
            else if (c_pk_hit) begin
                lk_o = c_pk_mask != 8'hFF;                  // the EEPROM's DO bit drives one bit
                lk_d = (c_pk_data & c_pk_mask) | (lk_last & ~c_pk_mask);
            end                                             // else the open bus (lk_data)
        end
        if (lk_xsel) begin
            if (smp_l_oe) begin
                lk_x = 1'b0; lk_m = 1'b0; lk_o = 1'b0; lk_d = smp_l_dout;
            end else if (!uci_ok)
                lk_w = 1'b1;
            else if (io_oe) begin
                lk_x = 1'b0; lk_m = 1'b0; lk_o = 1'b0; lk_d = io_dout;
            end
        end
    end
    assign pk_hit  = io_look ? 1'b1 : c_pk_hit;
    assign pk_mem  = io_look ? lk_m : c_pk_mem;
    assign pk_data = io_look ? lk_d : c_pk_data;
    // a cartridge on the real port cannot be read without a bus cycle
    assign pk_ext  = io_look ? lk_x : s_rom;
    assign pk_cram = io_look && lk_c;
    assign pk_open = io_look && lk_o;
    assign pk_wait = io_look && lk_w;

    // the real expansion port: ROML/ROMH/IO1/IO2 reads it serves, and reads of a SID socket (T37), take its data
    assign port_rd = ((!xc_roml || !xc_romh) && s_rom) || (!xc_io1 && s_io1_rd) || (!xc_io2 && s_io2_rd) ||
                     (sk_sel != 2'b00);
    slot_port #(.EN(SLOT_EN)) slot (
        .clk(clk), .rst(rst), .ce(ce), .ce_d(ce_d), .halted(halted_r),
        .bus_int(slot_int), .bus_ext(slot_ext), .cart_int(cart_type[4:0] != 5'd0),
        .rom_wr_en(slot_wr_en), .ultimax(ultimax), .own_reset(cpu_reset_req || rst),
        .phi2_p(xc_phi2_p), .cpu_reset(xc_reset), .vic(xc_vic), .vaddr(xc_vaddr), .ba(xc_ba),
        .addr(xc_addr), .din(xc_din), .we(xc_we || sk_we),
        .roml_n(xc_roml), .romh_n(xc_romh), .io1_n(xc_io1), .io2_n(xc_io2),
        .rom_port(s_rom), .io1_rd(s_io1_rd), .io2_rd(s_io2_rd), .io2_ext(s_io2_ext),
        .irq_ext(s_irq_ext), .present(s_present), .game(s_game), .exrom(s_exrom),
        .irq(s_irq), .nmi(s_nmi), .dout(s_dout), .reset_req(s_rst_req),
        .sock_sel(sk_sel), .sock_en(sk_c[1:0]), .stereo_sel(sk_c[36:34]),
        .p_sid1(slot_sid1), .p_sid2(slot_sid2), .p_sid_a(slot_sid_a),
        .status(slot_status), .cnt0(slot_cnt0), .cnt1(slot_cnt1), .last(slot_last),
        .p_addr(slot_a), .p_ahi_t(slot_ahi_t), .p_rw(slot_rw), .p_d_o(slot_d_o), .p_d_t(slot_d_t),
        .p_d_i(slot_d_i), .p_adir(slot_adir), .p_aoen(slot_aoen), .p_ddir(slot_ddir), .p_doen(slot_doen),
        .p_roml(slot_roml), .p_romh(slot_romh), .p_io1(slot_io1), .p_io2(slot_io2),
        .p_phi2(slot_phi2), .p_dotclk(slot_dotclk), .p_ba(slot_ba), .p_rst_o(slot_rst_o),
        .p_game(slot_game), .p_exrom(slot_exrom), .p_dma(slot_dma), .p_irq(slot_irq),
        .p_nmi(slot_nmi), .p_rst_i(slot_rst_i)
    );

    c64_mem mem (
        .clk(gclk), .dbg_clk(clk), .clk2x(clk2x), .dbg_req(dbg_req),
        .tb_addr(tb_addr), .tb_raddr(tb_raddr), .tb_we(tb_we), .tb_wdata(tb_wdata),
        .tb_ram_lo(tb_ram_lo), .tb_ram_hi(tb_ram_hi), .tb_rom_lo(tb_rom_lo), .tb_rom_hi(tb_rom_hi), .tb_took(tb_took),
        .cmd_addr(mem_cmd_addr), .cmd_we(mem_cmd_we), .cmd_valid(mem_cmd_valid),
        .cmd_ready(mem_cmd_ready), .wdata(mem_wdata), .wdata_we(mem_wdata_we),
        .wdata_ready(mem_wdata_ready), .rdata(mem_rdata), .rdata_valid(mem_rdata_valid),
        .dbg_addr(dbg_addr), .dbg_rom(dbg_rom), .dbg_we(dbg_we), .dbg_wdata(dbg_wdata),
        .dbg_rdata(dbg_rdata),
        .wl_we(t_a), .wl_addr(wa_addr), .wl_new(wa_new), .wl_old(wa_old)
    );

    // ---- state bus (ARCH-lean-state-access §4-§5) -------------------------------
    // Every bit the bus reads has a shadow flip-flop on clk (not gclk), loaded
    // with clock enable sb_capture and nothing in front of its D: one clock
    // after a frame's snap edge while the machine runs, and on every clock while
    // the gate is shut (so the shadows follow a DMA cycle or a step). A read is
    // the shadows through a mux: no strobe reaches a chip, so it has no side
    // effect (no ICR clear, no TOD latch, no collision clear).
    //
    // Word map (sb_addr[11:7] chip, [6:0] word; bytes little endian):
    //   chip 0 glue   w0 {cap_gen, 8'd0, 3'd0, held, cart present, EXROM, GAME, halted}
    //                    (cartridge lines are the cartridge's own, before ultimax)
    //                 w1 T29 turbo state {hold[22:8] b31:17, $D031 b16:9, $D030 bit 0 b8,
    //                    enable word b7:5, PREFER bit 7 b4, PREFER index b3:0} (c64_turbo.v
    //                    st_pack; a restore stages it through w1 and loads it with SB_LOAD b0)
    //   chip 1 6510   w0 {S, Y, X, A}   w1 {7'd0, opcode fetch, P, PC}
    //                 w2 {8'd0, $01 as read, $01 latch, $00}
    //                 w3 {7'd0, the last edge was an instruction boundary, opcode
    //                    at IPC, IPC} (trx_dbg.v; live, not a shadow: they only
    //                    move with the machine)
    //                 w4 {S, Y, X, A}  w5 {24'd0, P}  as the CPU's next update
    //                    leaves them (live, meaningful while the gate is shut: at
    //                    an IB, the registers with the previous instruction's
    //                    write-back done, which the update 9 clocks after the
    //                    phi2 fall makes)
    //   chip 2 VIC    w0-w11 $D000-$D02F as a read returns them, $D000 in w0 bits 7:0
    //                    ($D011 bit 7/$D012 = the raster line; $D019/$D01E/$D01F
    //                    not cleared)
    //   chip 3 CIA 1  w0-w3 registers 0-15 as a read returns them (register 4n+j
    //                    in byte j of word n: ports as latched, timer counters,
    //                    TOD as a read shows it (latch or clock), ICR flags)
    //                 w4 {DDRB, DDRA, PB out, PA out} as they drive the pins
    //   chip 4 CIA 2  as chip 3
    //   chip 7 cart   w0 {16'd0, reg_1, reg_0} of icepi's cartridge model (its
    //                    banking registers; meaning per type, c64_cartridge.v)
    //   chip 1 6510   w8-w14  every flip-flop of the CPU (209 bits, layout in
    //                    cpu6510.v / proc_core.vhd), bit 0 in w8 bit 0
    //   chip 3/4 CIA  w8-w19  every flip-flop of the CIA (363 bits, layout in
    //                    c64_redip_cia.v / cia_core.sv)
    //   other chips/words read 0. Staged writes go to the words w8 and up of
    //   chips 1, 3, 4 (while the gate is shut); a load (sb_ldarm) makes those
    //   chips take them on the next opening edge (ARCH §5.2/§5.6).
    reg  [383:0] s_vic;
    reg  [127:0] s_cia1, s_cia2;
    reg   [31:0] s_cia1p, s_cia2p;
    reg   [39:0] s_cpu;
    reg   [15:0] s_pc;
    reg          s_sync;
    reg   [23:0] s_port;
    reg    [2:0] s_cart;
    reg   [15:0] s_bank;
    integer      gk;
    wire         stg_ok;
    reg  [223:0] s_cpuf = 224'd0;
    reg  [383:0] s_cia1f = 384'd0, s_cia2f = 384'd0;

    // capture control: arm every frame at line 16 (every cap_every frames
    // for a capture, every frame for the log's markers), snap at the first
    // phi2 falling edge on line 0 from x 96 (cycle 12) on that ends an opcode
    // fetch (an instruction boundary; with an interrupt decided, the one
    // before its sequence: mk_intg) while the snap condition of ARCH §5.4
    // holds: border flip-flops set, no sprite pixel on its way, no VIC access
    // in the CPU cycle that ends, the REU idle. A snap is the edge a stop
    // would have stopped on (ARCH §4.1). No snap by line 16: the frame's
    // boundary is there, without a capture (mk_snap 0).
    // A frame of the cadence loads the shadows at its snap unless cap_block.
    wire  [8:0] l_line = {l_vic[17*8+7], l_vic[18*8 +: 8]};   // $D011 b7, $D012
    assign pos_hit = pos_arm && ce && !halted_r && ph == 5'd30 && l_line == 9'd0 &&
                     pix_x[9:3] == {1'b0, pos_cyc};
    reg   [8:0] line_q = 9'd0;
    reg   [7:0] fcnt = 8'd0;
    reg         armed = 1'b0, cap_this = 1'b0, cap_q = 1'b0, halted_q = 1'b0;
    wire        snap = armed && ce && xc_phi2_n && run && !halted_r && l_line == 9'd0 &&
                       pix_x >= 10'd96 && l_sync && l_vok && !l_vacc && !l_reu && !steal_run && !turbo_on;
    wire        snap_cap = snap && cap_this && !cap_block;
    wire        l16 = l_line == 9'd16 && line_q != 9'd16;
    wire        sb_capture = cap_q || halted_r;
    always @(posedge clk) begin
        line_q    <= l_line;
        halted_q  <= halted_r;
        cap_q     <= snap_cap;
        phi2_tick <= ce && xc_phi2_n;
        mk        <= snap || (l16 && armed);
        mk_snap   <= snap;
        mk_cap    <= snap_cap;
        mk_skip   <= snap && cap_this && cap_block;
        mk_intg   <= snap && l_intg;
        if (snap || l16) begin
            mk_x    <= pix_x;
            mk_line <= l_line;
        end
        if (snap || pos_hit) armed <= 1'b0;   // a position stop stands for a snap edge
        if (l16) begin
            if (armed && cap_this) cap_nosnap <= cap_nosnap + 1'b1;
            armed    <= log_on || cap_every != 8'd0;
            cap_this <= 1'b0;
            if (cap_every != 8'd0) begin
                if (fcnt + 1'b1 >= cap_every) begin
                    fcnt     <= 8'd0;
                    cap_this <= 1'b1;
                end else
                    fcnt <= fcnt + 1'b1;
            end
        end
        if (snap_cap || (halted_r && !halted_q)) begin
            cap_gen <= cap_gen + 1'b1;
            cap_tgl <= ~cap_tgl;
        end
        if (sb_capture) begin
            s_vic  <= l_vic;
            s_cia1 <= l_cia1;  s_cia1p <= l_cia1p;
            s_cia2 <= l_cia2;  s_cia2p <= l_cia2p;
            s_cpu  <= l_cpu;   s_pc <= l_pc;  s_sync <= l_sync;  s_port <= l_port;
            s_cart <= l_cart;  s_bank <= l_bank;
            s_t1 <= turbo_pack;
            s_cpuf[208:0] <= l_cpuf;
            s_cia1f[362:0] <= l_cia1f;
            s_cia2f[362:0] <= l_cia2f;
        end
        // staged writes (the hidden-state words only): a clock enable per
        // word, the data straight from sb_wdata (no mux in front)
        if (stg_ok && sb_waddr == {5'd0, 7'd1}) g_t1 <= sb_wdata;
        for (gk = 0; gk < 12; gk = gk + 1) begin
            if (gk < 7 && stg_ok && sb_waddr == {5'd1, 7'd8 + gk[6:0]}) g_cpu[gk * 32 +: 32] <= sb_wdata;
            if (stg_ok && sb_waddr == {5'd3, 7'd8 + gk[6:0]}) g_cia1[gk * 32 +: 32] <= sb_wdata;
            if (stg_ok && sb_waddr == {5'd4, 7'd8 + gk[6:0]}) g_cia2[gk * 32 +: 32] <= sb_wdata;
        end
    end
    // ---- debug (T21): stops, steps, counters, bitmap, watch units -----------------
    trx_dbg dbg (
        .clk(clk), .rst(rst), .ce(ce), .phi2n(xc_phi2_n), .halted(halted_r), .held(held),
        .addr(xc_addr), .data(xc_din), .we(x_io_we), .sync(l_sync), .intg(l_intg), .rdy(l_rdy),
        .cpu_bus(l_cbus), .regs(l_cpu), .pos({l_line, pix_x[9:3]}), .fw_stop(fw_stop), .armed(dbg_armed),
        .drv_stop(drv_stop), .drv_done(drv_done),
        .stop(dbg_stop), .rel(dbg_rel), .hold(dbg_hold), .brk(dbg_brk), .keep(dbg_keep), .go(dbg_go), .own(dbg_own), .ev(dbg_ev),
        .ipc(dbg_ipc), .op(dbg_op), .at_ib(dbg_ib),
        .tr_ib(tr_ib), .tr_b1(tr_b1), .tr_b2(tr_b2), .log_put(dbg_log_put), .log_rec(dbg_log_rec),
        .r_we(dr_we), .r_re(dr_re), .r_addr(dr_addr), .r_wdata(dr_wdata), .r_rdata(dr_rdata)
    );

    // T29 (spec §12.2 D7): a stop, a position stop, an armed debugger, a held
    // CPU or DMA, a VIC restart: the engine falls back to normal speed first
    assign turbo_inh = halt_req || pos_hold || dbg_hold || pos_arm || dbg_armed || cpu_rst_in ||
                       halted_r || held || dma_in || trst != 6'd0 || rst;

    assign sb_frozen = halted_r && !ce && !ce_d;
    assign stg_ok    = sb_we && sb_frozen;
    assign steal_run = dma_busy && !halted_r && !held && !cpu_pause_req;
    assign reu_busy  = l_reu || steal_run;

    // CIA register r (bits 127-8r) in byte r%4 of word r/4
    function [31:0] cia_word(input [127:0] r, input [1:0] n);
        cia_word = {r[127 - 8*(4*n+3) -: 8], r[127 - 8*(4*n+2) -: 8],
                    r[127 - 8*(4*n+1) -: 8], r[127 - 8*(4*n) -: 8]};
    endfunction

    // the word maps of chips 1, 3, 4 as tables (missing words read 0)
    wire [31:0] w_cpu [0:15];
    wire [31:0] w_cia1 [0:31];
    wire [31:0] w_cia2 [0:31];
    genvar gi;
    generate
        for (gi = 0; gi < 16; gi = gi + 1) begin : g_wcpu
            if (gi == 0)                 assign w_cpu[gi] = s_cpu[31:0];
            else if (gi == 1)            assign w_cpu[gi] = {7'd0, s_sync, s_cpu[39:32], s_pc};
            else if (gi == 2)            assign w_cpu[gi] = {8'd0, s_port};
            else if (gi == 3)            assign w_cpu[gi] = {7'd0, dbg_ib, dbg_op, dbg_ipc};
            else if (gi == 4)            assign w_cpu[gi] = l_cpunx[31:0];
            else if (gi == 5)            assign w_cpu[gi] = {24'd0, l_cpunx[39:32]};
            else if (gi >= 8 && gi < 15) assign w_cpu[gi] = s_cpuf[(gi - 8) * 32 +: 32];
            else                         assign w_cpu[gi] = 32'd0;
        end
        for (gi = 0; gi < 32; gi = gi + 1) begin : g_wcia
            if (gi < 4) begin
                assign w_cia1[gi] = cia_word(s_cia1, gi);
                assign w_cia2[gi] = cia_word(s_cia2, gi);
            end else if (gi == 4) begin
                assign w_cia1[gi] = s_cia1p;
                assign w_cia2[gi] = s_cia2p;
            end else if (gi >= 8 && gi < 20) begin
                assign w_cia1[gi] = s_cia1f[(gi - 8) * 32 +: 32];
                assign w_cia2[gi] = s_cia2f[(gi - 8) * 32 +: 32];
            end else begin
                assign w_cia1[gi] = 32'd0;
                assign w_cia2[gi] = 32'd0;
            end
        end
    endgenerate

    reg  [11:0] sb_addr_q = 12'd0;
    always @(posedge clk) begin
        sb_addr_q <= sb_addr;
        case (sb_addr_q[11:7])
        5'd0: sb_rdata <= sb_addr_q[6:0] == 7'd0 ? {cap_gen, 8'd0, 3'd0, held, s_cart, halted_r} :
                          sb_addr_q[6:0] == 7'd1 ? s_t1 : 32'd0;
        5'd1: sb_rdata <= sb_addr_q[6:4] == 3'd0 ? w_cpu[sb_addr_q[3:0]] : 32'd0;
        5'd2: sb_rdata <= sb_addr_q[6:0] < 7'd12 ? s_vic[sb_addr_q[3:0] * 32 +: 32] : 32'd0;
        5'd3: sb_rdata <= sb_addr_q[6:5] == 2'd0 ? w_cia1[sb_addr_q[4:0]] : 32'd0;
        5'd4: sb_rdata <= sb_addr_q[6:5] == 2'd0 ? w_cia2[sb_addr_q[4:0]] : 32'd0;
        5'd7: sb_rdata <= sb_addr_q[6:0] == 7'd0 ? {16'd0, s_bank} : 32'd0;
        default: sb_rdata <= 32'd0;
        endcase
    end

`ifdef SID_MULTI
assign sid_resid8 = 1'b1;
`else
assign sid_resid8 = 1'b0;
`endif

endmodule
