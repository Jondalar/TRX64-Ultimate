// SPDX-License-Identifier: GPL-3.0-or-later
// The C64U's physical expansion port (T16 M1, docs/specs/T16-cartridge-port.md
// §13): our C64 core drives it as a C64 board does while the app's
// C64_BUS_EXTERNAL selects ROM, IO1 or IO2 there; otherwise it stays idle
// (static levels, no traffic).
//
// Inside c64_core, on its ungated clk. "Core tick N" is the clock N dot4x
// clocks after the core's phi2_p clock (tk); the core's own registers move
// only with ce (the gate). Every port output is a register loaded every clk
// from the core's signals of this clock, so the port shows core tick N during
// port tick N + 1. Port schedule (§13.3):
//   PHI2 high port 2-17; address, R/W and strobes of the CPU half 2-18 (core
//   ticks 1-17 are the CPU branch of the PLA), the VIC's from 19, A12-A15 =
//   1111 while the VIC drives the address. Data transceiver: DIR set at
//   port 2 (0 for a read the port serves or a VIC fetch in ultimax) and at
//   port 20 (0 in ultimax), OE off on and before every DIR change; OE on
//   3-17 for a routed read or a VIC ultimax fetch, 3-18 for a write; 21-31 for
//   a VIC ultimax fetch. The FPGA drives D only in a write's 3-18 (DIR = 1
//   since port 2) and, outside ultimax, in port 27-31 of the VIC half with the
//   VIC's byte (T74: the open bus the next PHI2 half samples when no cart
//   answers; OE on 27-31, DIR 1 since port 20). Sample D at the end of core ticks 15 (the CPU latches at
//   16) and 31 (the VIC latches at 0).
// Pause (Q3): when the gate shuts, the next clock repeats the hold tick, then
// the park state holds (PHI2/DOTCLK 0, strobes and R/W released, data off);
// the first clock after the gate opens is still parked (park = !ce_d), so a
// resume never repeats a strobe.
// SID sockets (T37, spec §4.1; Q2/Q4/Q5/Q6 answered 2026-10-16): the sockets are two more
// chip selects on this bus, armed per cycle (Quiet): a socket hit seen at core tick 1 turns the
// address, R/W and data transceivers on for that CPU cycle only (port tick 2-18), SLOT_SIDn low at
// port 3-17. PHI2/DOTCLK run for as long as a socket is enabled (a SID needs its clock), parked in the
// pause like everything else. With a cartridge armed the every-cycle drive covers the sockets, too.
// Write inhibit: ROML/ROMH stay high in write cycles unless rom_wr_en.
// Guard: the DMA pin low while armed, or the registered controls breaking
// the turnaround rules, latch a fault: idle until the next C64 reset.
module slot_port #(
    parameter EN = 1                        // 0: the port stays idle (P1)
) (
    input  wire        clk,
    input  wire        rst,                 // the VIC restarts its phase
    input  wire        ce,                  // the gate: gclk's edge at the end of this clk
    input  wire        ce_d,                // ce of the clk before (the core moved into this one)
    input  wire        halted,              // the machine is paused
    // the app's selection (quasi-static): b0 IO1, b1 IO2, b2 ROM, b3 IRQ
    input  wire  [3:0] bus_int,             // C64_BUS_INTERNAL
    input  wire  [3:0] bus_ext,             // C64_BUS_EXTERNAL
    input  wire        cart_int,            // a CRT is configured (cart_slot)
    input  wire        rom_wr_en,           // SLOT_CTRL b1
    input  wire        ultimax,             // the app's freeze ultimax: stays internal (Q4)
    input  wire        own_reset,           // our own C64 reset request (not the sensed one)
    // the core's cartridge side, this clock (c64_top ext_cart_*)
    input  wire        phi2_p,
    input  wire        cpu_reset,
    input  wire        vic,                 // the VIC drives the address
    input  wire [11:0] vaddr,               // the VIC's address (valid from RAS)
    input  wire        ba,
    input  wire [15:0] addr,
    input  wire  [7:0] din,
    input  wire        we,
    input  wire        roml_n, romh_n, io1_n, io2_n,
    // SID sockets: this cycle's hits (arbiter, valid from core tick 1), the enables and
    // C64_STEREO_ADDRSEL (0 Off, 1-5 = A5-A9), quasi-static, synchronized outside
    input  wire  [1:0] sock_sel,
    input  wire  [1:0] sock_en,
    input  wire  [2:0] stereo_sel,
    // into the core
    output wire        rom_port,            // GAME/EXROM and ROML/ROMH reads from the port
    output wire        io1_rd,              // IO1 reads from the port
    output wire        io2_rd,
    output wire        io2_ext,             // the port claims IO2 writes
    output wire        irq_ext,             // IRQ/NMI from the port count
    output wire        present,             // an external resource is selected
    output wire        game, exrom,         // the pins, synchronized (1 = high)
    output wire        irq, nmi,            // the pins, synchronized (1 = asserted)
    output reg   [7:0] dout = 8'hFF,        // the last D sample
    output wire        reset_req,           // hold the C64 in reset (the port's RESET is low)
    output wire [31:0] status,
    output wire [31:0] cnt0,                // {routed reads, writes with a port strobe}
    output wire [31:0] cnt1,                // {armed cycles, VIC ultimax fetches}
    output reg  [31:0] last = 32'd0,        // {R/W, strobe, data, address} of the last port access
    // pins
    output reg  [15:0] p_addr = 16'hFFFF,
    output reg         p_ahi_t = 1'b1,      // 1: A12-A15 and R/W released
    output reg         p_rw = 1'b1,
    output reg   [7:0] p_d_o = 8'hFF,
    output reg         p_d_t = 1'b1,        // 1: the FPGA does not drive D0-D7
    input  wire  [7:0] p_d_i,
    output wire        p_adir,
    output reg         p_aoen = 1'b1,
    output reg         p_ddir = 1'b1,       // 1: FPGA to port
    output reg         p_doen = 1'b1,
    output reg         p_roml = 1'b1,
    output reg         p_romh = 1'b1,
    output reg         p_io1 = 1'b1,
    output reg         p_io2 = 1'b1,
    output reg         p_phi2 = 1'b0,
    output reg         p_dotclk = 1'b0,
    output reg         p_sid1 = 1'b1,       // SLOT_SID1n / SLOT_SID2n: low = selected
    output reg         p_sid2 = 1'b1,
    output reg         p_sid_a = 1'b0,      // SLOT_SID_ADDR
    output reg         p_ba = 1'b1,
    output reg         p_rst_o = 1'b0,
    input  wire        p_game, p_exrom, p_dma, p_irq, p_nmi, p_rst_i
);
    wire en = EN != 0;
    assign p_adir = 1'b1;                   // v1: the address side never turns

    // ---- inputs: two-flip-flop synchronizers -------------------------------------
    (* ASYNC_REG = "TRUE" *) reg [5:0] pin_s1 = 6'b011111, pin_s2 = 6'b011111;
    always @(posedge clk) begin
        pin_s1 <= {p_rst_i, p_nmi, p_irq, p_dma, p_exrom, p_game};
        pin_s2 <= pin_s1;
    end
    wire game_s = pin_s2[0], exrom_s = pin_s2[1], dma_s = pin_s2[2];
    wire irq_s = pin_s2[3], nmi_s = pin_s2[4], rst_s = pin_s2[5];
    assign game  = game_s;
    assign exrom = exrom_s;
    assign irq   = !irq_s;
    assign nmi   = !nmi_s;

    // ---- selection: the masks taken in reset or while paused ----------------------
    reg  [3:0] m_int = 4'hF, m_ext = 4'h0;
    reg        fault = 1'b0;
    reg  [3:0] cause = 4'd0;
    always @(posedge clk)
        if (cpu_reset || halted) begin
            m_int <= bus_int;
            m_ext <= bus_ext;
        end
    wire rom_sel = en && !fault && m_ext[2] && !m_int[2];          // Both = Internal (Q7)
    assign present = en && !fault && (rom_sel || m_ext[0] || m_ext[1]);
    reg  armed = 1'b0;
    wire act = armed && !fault;
    assign rom_port = rom_sel && !ultimax;
    assign io1_rd   = act && m_ext[0] && !(m_int[0] && cart_int);
    assign io2_rd   = act && m_ext[1] && !(m_int[1] && cart_int);
    assign io2_ext  = act && m_ext[1];
    assign irq_ext  = en && m_ext[3];
    wire ult = rom_port && !game_s && exrom_s;                      // ultimax at the port

    // ---- the core's tick and this cycle's decisions (move with the gate) ---------
    // the sockets: static arming at a cycle boundary, like `armed`; fault silences them too
    wire sk_on = en && !fault && (sock_en != 2'b00);
    reg  sarmed = 1'b0;
    wire sact = sarmed && !fault;           // a socket is enabled: the clock runs, cycles can be armed
    wire any_act = act || sact;
    reg  [4:0] tk = 5'd0;
    reg        synced = 1'b0;               // tk follows the VIC's phase
    reg        h_wr = 1'b0, h_rd = 1'b0, h_vu = 1'b0, h_ws = 1'b0;
    reg  [1:0] h_s = 2'b00;                 // this cycle selects socket 1 / 2 (decided at core tick 1)
    wire park = !ce_d;
    wire sel_now = ((!roml_n || !romh_n) && rom_port) || (!io1_n && io1_rd) || (!io2_n && io2_rd);
    wire [1:0] sk_hit = (any_act && !vic) ? sock_sel : 2'b00;
    wire sk_now  = sk_hit != 2'b00;
    wire wr_now  = (act && !vic && we) || (sk_now && we);
    wire rd_now  = (act && !vic && !we && sel_now) || (sk_now && !we);
    wire vu_now  = act && vic && ult;       // the VIC owns this phi2 half (BA)
    // The hold tick (core tick 17, port 18) shows the cycle's address, R/W, strobes as they were at core
    // tick 16. A CPU cycle keeps them anyway (its branch of the PLA lasts to tick 17); an app DMA cycle
    // ends at tick 16 (ext_dma_cyc falls with phi2_n) and the core's signals jump back to the paused
    // CPU's, which the cart would see as the strobe rising together with PHI2 falling.
    reg [15:0] q_addr = 16'hFFFF;
    reg        q_we = 1'b0, q_roml_n = 1'b1, q_romh_n = 1'b1, q_io1_n = 1'b1, q_io2_n = 1'b1;
    always @(posedge clk)
        if (tk == 5'd16) begin
            q_addr <= addr; q_we <= we;
            q_roml_n <= roml_n; q_romh_n <= romh_n; q_io1_n <= io1_n; q_io2_n <= io2_n;
        end
    wire        hold_t  = tk == 5'd17;
    wire [15:0] c_addr  = hold_t ? q_addr : addr;
    wire        c_we    = hold_t ? q_we : we;
    wire        c_roml  = hold_t ? q_roml_n : roml_n;
    wire        c_romh  = hold_t ? q_romh_n : romh_n;
    wire        c_io1   = hold_t ? q_io1_n : io1_n;
    wire        c_io2   = hold_t ? q_io2_n : io2_n;
    wire roml_go = !c_roml && rom_port && (!c_we || rom_wr_en);
    wire romh_go = !c_romh && rom_port && (!c_we || rom_wr_en);
    wire io1_go  = !c_io1 && m_ext[0];
    wire io2_go  = !c_io2 && m_ext[1];
    always @(posedge clk) begin
        if (ce) begin
            tk <= phi2_p ? 5'd1 : tk + 1'b1;
            if (phi2_p) synced <= 1'b1;
            if (tk == 5'd0) begin
                armed  <= present && synced;
                sarmed <= sk_on && synced;
            end
            if (tk == 5'd1) begin
                h_wr <= wr_now;
                h_rd <= rd_now;
                h_vu <= vu_now;
                h_ws <= wr_now && (roml_go || romh_go || io1_go || io2_go || sk_now);
                h_s  <= sk_hit;
            end
            if (tk == 5'd18) begin
                h_wr <= 1'b0;
                h_rd <= 1'b0;
                h_ws <= 1'b0;
                h_s  <= 2'b00;
                h_vu <= act && ult;
            end
            if (tk == 5'd15 || tk == 5'd31) dout <= p_d_i;
        end
        if (rst) begin
            synced <= 1'b0;
            armed  <= 1'b0;
            sarmed <= 1'b0;
        end
    end

    // ---- the pins, every clk -------------------------------------------------------
    wire on_cpu = (tk >= 5'd2 && tk <= 5'd16 && (h_rd || h_wr || h_vu)) || (tk == 5'd17 && h_wr);
    wire on_vic = tk >= 5'd20 && tk <= 5'd30 && h_vu;
    // T74: the VIC's byte on D in the PHI1 half, as a C64's bus carries it (not in an ultimax VIC fetch, where the cart
    // drives). It is the open bus: the bus keeps the level it was left at (board 2026-10-10: an unanswered read returned
    // the last byte driven), so the next PHI2 half, where a read that no cart answers samples it, sees this byte instead
    // of the one the FPGA wrote there in the cycle before. DIR is 1 since port 20 (tk 19)
    // and OE is off again from tk 31 on, before the DIR turn at tk 1.
    wire on_vf = tk >= 5'd26 && tk <= 5'd30 && !h_vu;
    wire run  = act && !park;                // the cartridge's strobes, the VIC half
    wire runa = any_act && !park;            // the clock, the CPU half's bus, the socket selects
    // the socket cycle's window for the address side and the data transceiver: the decision at core tick 1
    // shows as port tick 2, off again after port tick 18
    wire sk_win = !park && ((tk == 5'd1 && sk_now) || ((h_s != 2'b00) && tk >= 5'd2 && tk <= 5'd17));
    wire [15:0] a_src = vic ? {4'hF, vaddr} : c_addr;
    reg  a_bit;
    always @(*)
        case (stereo_sel)
        3'd1: a_bit = a_src[5];
        3'd2: a_bit = a_src[6];
        3'd3: a_bit = a_src[7];
        3'd4: a_bit = a_src[8];
        3'd5: a_bit = a_src[9];
        default: a_bit = 1'b0;
        endcase
    wire sid_a_on = stereo_sel >= 3'd1 && stereo_sel <= 3'd5;
    always @(posedge clk) begin
        p_phi2   <= runa && tk >= 5'd1 && tk <= 5'd16;
        p_dotclk <= runa && tk[1];
        p_ba     <= act ? ba : 1'b1;
        p_aoen   <= !(act || sk_win);
        p_ahi_t  <= !(act || sk_win);
        p_rw     <= !(runa && c_we);
        p_roml   <= !(run && roml_go);
        p_romh   <= !(run && romh_go);
        p_io1    <= !(run && io1_go);
        p_io2    <= !(run && io2_go);
        p_sid1   <= !(runa && h_s[0] && tk >= 5'd2 && tk <= 5'd16);
        p_sid2   <= !(runa && h_s[1] && tk >= 5'd2 && tk <= 5'd16);
        p_doen   <= !(runa && (on_cpu || (act && on_vic) || (act && on_vf)));
        p_d_t    <= !(runa && ((h_wr && tk >= 5'd2 && tk <= 5'd17) || (act && on_vf)));
        if (!park) begin
            p_addr  <= a_src;
            // M3 (2026-10-15): the chosen bit with the address, idle high with a split, low without
            p_sid_a <= sid_a_on && (!(act || sk_win) || a_bit);
        end else if (!act) p_sid_a <= sid_a_on;     // the idle level follows the split in the pause, too
        else if (!sid_a_on) p_sid_a <= 1'b0;
        if (!park && !vic && tk >= 5'd1 && tk <= 5'd16) p_d_o <= din;
        if (!park && vic && tk >= 5'd24 && tk <= 5'd29) p_d_o <= din;       // the VIC's fetch, valid by tk 23
        if (!any_act) begin
            if (p_doen) p_ddir <= 1'b1;     // back to idle only after an OE-off clock
        end else if (!park) begin
            if (tk == 5'd1)  p_ddir <= !(rd_now || vu_now);
            if (tk == 5'd19) p_ddir <= !h_vu;
        end
    end

    // ---- port RESET: our own resets, + 32 us; the cart's sensed while armed --------
    reg [10:0] rcnt = 11'd2047;
    always @(posedge clk) begin
        if (own_reset)         rcnt <= 11'd2047;
        else if (rcnt != 11'd0) rcnt <= rcnt - 1'b1;
        p_rst_o <= en && rcnt[10];
    end
    // The C64 stays in reset while we drive the port's RESET and, for 32 us
    // after, while the line is still low (one RESET line, as on a C64 board).
    // After that a low line is the cart's: it resets the C64 while armed. A
    // sensed reset does not drive the line, so it cannot loop.
    assign reset_req = en && (rcnt[10] || (rst_s && (rcnt != 11'd0 || present)));

    // ---- guard (§6.2, §13.5): a second look at the registered controls -------------
    reg ddir_q = 1'b1, doen_q = 1'b1, arm_q = 1'b0, clk_q = 1'b0, sk_q = 1'b0;
    wire g_dma  = act && !dma_s;
    wire g_turn = p_ddir != ddir_q && (!p_doen || !doen_q);
    wire g_dout = !p_d_t && (!p_ddir || !ddir_q);
    // an active pin needs its cause: the cartridge's strobes an armed cartridge; the clock any armed
    // resource; the bus lines and the selects an armed cartridge or a socket cycle
    wire g_idle = (!arm_q && (!p_roml || !p_romh || !p_io1 || !p_io2)) ||
                  (!clk_q && p_phi2) ||
                  (!arm_q && !sk_q && (!p_doen || !p_aoen || !p_d_t || !p_ahi_t || !p_sid1 || !p_sid2));
    always @(posedge clk) begin
        ddir_q <= p_ddir;
        doen_q <= p_doen;
        arm_q  <= act;
        clk_q  <= any_act;
        sk_q   <= sk_win;
        if (cpu_reset) begin
            fault <= 1'b0;
            cause <= 4'd0;
        end
        if (!fault && en && (g_dma || g_turn || g_dout || g_idle)) begin
            fault <= 1'b1;
            cause <= g_dma ? 4'd1 : g_turn ? 4'd2 : g_dout ? 4'd3 : 4'd4;
        end
    end

    // ---- counters and the last access (SLOT_CNT0/1, SLOT_LAST) ---------------------
    reg [15:0] n_rd = 16'd0, n_wr = 16'd0, n_cyc = 16'd0, n_vf = 16'd0;
    always @(posedge clk)
        if (ce) begin
            if (tk == 5'd16 && h_rd) n_rd <= n_rd + 1'b1;
            if (tk == 5'd16 && h_ws) n_wr <= n_wr + 1'b1;
            if (tk == 5'd0 && act) n_cyc <= n_cyc + 1'b1;
            if (tk == 5'd31 && h_vu && !p_romh) n_vf <= n_vf + 1'b1;
            if (tk == 5'd16 && (h_rd || h_ws))
                last <= {h_rd, !p_roml ? 3'd1 : !p_romh ? 3'd2 : !p_io1 ? 3'd3 : !p_io2 ? 3'd4 :
                               !p_sid1 ? 3'd5 : !p_sid2 ? 3'd6 : 3'd0,
                         h_rd ? dout : p_d_o, p_addr};
        end
    assign cnt0 = {n_rd, n_wr};
    assign cnt1 = {n_cyc, n_vf};
    assign status = {1'b1, 4'd0, sact, sock_en, cause, 1'b0, rom_wr_en, fault, act, m_int, m_ext,
                     2'b00, rst_s, nmi_s, irq_s, dma_s, exrom_s, game_s};
endmodule
