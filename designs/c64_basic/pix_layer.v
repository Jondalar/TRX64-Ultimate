// SPDX-License-Identifier: GPL-3.0-or-later
// The pixel layer and the still capture (ARCH-lean-state-access.md §7.5,
// T12 §5): video client "v" of mig_arb.
//
// Pixel layer: an RGB565 picture in DDR2 (LVGL LV_COLOR_FORMAT_RGB565: one
// little-endian 16-bit word per pixel, R in bits 15:11, G 10:5, B 4:0), W x H
// source pixels (W <= 960), each shown as S x S output pixels (S = 1..3), its
// top left at output pixel (X0, Y0) of the mode's active area (U16 D6: cx / cy from xact0 / yact0, clipped to hact x vact), over the C64
// and overlay.v, under the freeze menu's text (C64 -> overlay.v -> this ->
// c64_text.v). Each channel is widened to 8 bits by
// repeating its top bits (R5 -> {R5, R5[4:2]}, G6 -> {G6, G6[5:4]}), so 0 and
// full scale stay 0 and 255. A pixel equal to the colour key (PX_KEY, when
// enabled) is transparent; the others are blended with the global alpha G:
//   out = bg + a (fg - bg) / 256,  a = G + G[7]
// per channel, so G = 255 gives the pixel and G = 0 the picture below,
// exactly. No per-pixel alpha. Outside the layer, and on a line whose fetch
// was late, the picture below passes unchanged (a late line is never shown in
// part: "underrun", counted).
// Scanout: two line buffers of 128 x 128 bit (one block RAM of 256 x 128,
// written by the fetcher in ui_clk, read in the pixel clock). When source
// line m starts on screen, the fetcher is asked for line m + 1 (line 0 at the
// frame start, cy = 0): ceil(W / 8) MIG reads back to back (120 at W = 960,
// 1.6 us of the S output lines it has, 17.7 us each at 1080p, longer in every other mode of U16 D2; in mig_arb the video client takes turns
// with the RISC-V/REU pair, and goes before them once a line fetch is 5.3 us
// old: v_urgent). A request that comes while the previous
// line is still being read stops that one (it is late anyway); the new one
// starts once the reads in flight have landed.
// Pixel repetition: every register of the pixel side changes only when ce is high (hdmi_timing.v).
// Page flip: a write of PX_BACK (its byte 3) asks for a flip; at the next
// frame start the shown buffer (PX_FRONT's, which does not follow: write-only) becomes
// PX_BACK's and the flip-done cause is raised.
//
// Still: the VIC's pixels (pix/px/py, the scaler's input) in the window the
// scaler shows (400 x 270 from raster X0S, Y0S) packed 32 to a 128-bit word
// (pixel k of the window, k = 400 y + x, at bits 4(k mod 32) + 3 .. 4(k mod
// 32) of word k / 32: two pixels per byte, the first in the low nibble),
// 3,375 words = 54,000 bytes per frame, written to STILL_BASE + slot x 64 KB.
// 60 Hz System Modes (v60, T40: 65 x 263 lines): the scaler's window is 400 x
// 240 from line 31 through 262 into lines 0-7 of the next frame. The still has
// the same 240 rows but all of one VIC frame, so that it is complete at line
// 262 and a stop on line 0 (the scrub's) does not lose it: lines 0-7 of the
// frame (written first) are rows 232-239 (words 2,900-2,999), lines 31-262
// rows 0-231 (words 0-2,899); 3,000 words = 48,000 bytes. The mode is taken
// when the capture starts and kept for the frame.
// The pixels shift into a 128-bit register; the finished word goes into ONE
// hand-off word to ui_clk (toggle + ack), which the DDR2 side must have taken
// before the next word is finished (32 VIC pixels, about 4 us at 7.88 MHz).
// A frame goes into the slot that does not hold the last complete frame; a
// frame that lost a word (the hand-off word was still waiting) is not reported
// and its slot is used again. The machine pause stops the VIC and with it the
// capture, so while paused STILL_STATUS names the slot of the last frame
// before the pause.
//
// Registers (page 6 of trx_io, 0x101A6000; 32 bit little endian, the T21
// rule: writing byte 3 commits the four bytes, reading byte 0 takes bytes
// 1-3; read data one clk_sys after the address). Only the two status registers
// read; every other register is write-only and reads 0 (OPT-pix.md):
//   060 STILL_CTRL   W   b0 capture (default 1)
//   064 STILL_BASE   W   DDR2 byte address of slot 0 (16-byte aligned; default
//                        0x05050000, T20 §5.1); slot 1 at +0x10000
//   068 STILL_STATUS R   b0 valid (a frame since capture on), b1 slot of the
//                        last complete frame, b2 that frame is 60 Hz: 400 x 240
//                        (0: 400 x 270), b7:3 0, b31:8 frames captured (wrapping)
//   080 PX_CTRL      W   b0 enable, b3:1 format (1 = RGB565; others: off),
//                        b5:4 scale S (0 and 1: 1; 2; 3); default 0x22
//   084 PX_FRONT     W   DDR2 byte address of the shown buffer (16-byte aligned)
//   088 PX_BACK      W   show this buffer from the next frame start
//   090 PX_SIZE      W   {height 16, width 16} source pixels (default 540, 960)
//   094 PX_STRIDE    W   bytes from line to line (multiple of 16; default 1920)
//   098 PX_POS       W   {y0 16, x0 16} output pixel of the top left (default 0)
//   09C PX_GALPHA    W   b7:0 global alpha (default 255)
//   0A0 PX_KEY       W   b15:0 colour key (an RGB565 value), b16 key on: pixels
//                        equal to it are transparent (default 0: off)
//   0A4 PX_STATUS    R   b0 flip pending, b1 vertical blank, b15:8 lines not
//                        shown for underrun (wrapping), b31:16 frames (wrapping)
// Events for trx_io's IRQ_CAUSE (clk_sys pulses): ev_vsync at every frame
// start (cy = 0, the vertical blanking of the mode, yact0 lines, before the first visible line), ev_flip when a
// flip was done.
module pix_layer #(
    parameter [9:0] X0S = 10'd96,       // still window, as vic_scaler's X0, Y0
    parameter [8:0] Y0S = 9'd16
) (
    input  wire         clk_sys,
    input  wire         we,
    input  wire         re,
    input  wire [11:0]  addr,
    input  wire [7:0]   wdata,
    output reg  [7:0]   rdata = 8'd0,
    output reg          ev_vsync = 1'b0,
    output reg          ev_flip = 1'b0,

    input  wire         ui_clk,
    input  wire         ui_rst,
    output wire [26:0]  v_addr,
    output wire [2:0]   v_cmd,
    output wire         v_en,
    input  wire         v_rdy,
    output wire [127:0] v_wdf_data,
    output wire [15:0]  v_wdf_mask,
    output wire         v_wdf_wren,
    input  wire [127:0] v_rd_data,
    input  wire         v_rd_data_valid,
    output reg          v_urgent = 1'b0,

    input  wire         vclk,
    input  wire [3:0]   pix,
    input  wire [9:0]   px,
    input  wire [8:0]   py,
    input  wire         v60,            // 65 x 263 VIC (any clock; as vic_scaler's)

    input  wire         clk,
    input  wire         ce,             // pixel enable (hdmi_timing.v)
    input  wire [11:0]  hact,           // the mode's active area and where it starts in cx / cy
    input  wire [10:0]  vact,
    input  wire [11:0]  xact0,
    input  wire [10:0]  yact0,
    input  wire [23:0]  rgb_in,
    input  wire         de_in, hs_in, vs_in,
    input  wire [11:0]  cx_in,
    input  wire [10:0]  cy_in,
    output reg  [23:0]  rgb,
    output reg          de, hs, vs,
    output reg  [11:0]  cx,
    output reg  [10:0]  cy
);
    // ======================= registers (clk_sys) ===============================
    reg        r_still_en = 1'b1;
    reg [31:0] r_still_base = 32'h05050000;
    reg [5:0]  r_ctrl = 6'h22;
    reg [31:0] r_front = 32'd0, r_back = 32'd0;
    reg [15:0] r_w = 16'd960, r_h = 16'd540;
    reg [15:0] r_stride = 16'd1920;
    reg [15:0] r_x0 = 16'd0, r_y0 = 16'd0;
    reg [7:0]  r_ga = 8'd255;
    reg [16:0] r_key = 17'd0;
    reg        cfg_tgl = 1'b0, front_tgl = 1'b0, back_tgl = 1'b0;
    reg [23:0] stg = 24'd0;
    reg [31:0] lat = 32'd0;

    // events from the other clocks
    wire       px_vs_tgl, px_flip_tgl, px_flip_seen, px_under_tgl, px_vblank, st_done_tgl;
    wire       st_done_slot, st_done_60;
    (* ASYNC_REG = "TRUE" *) reg [2:0] s_vs = 3'd0, s_flip = 3'd0, s_under = 3'd0, s_st = 3'd0;
    (* ASYNC_REG = "TRUE" *) reg [1:0] s_vb = 2'd0, s_seen = 2'd0;
    always @(posedge clk_sys) begin
        s_vs <= {s_vs[1:0], px_vs_tgl};
        s_flip <= {s_flip[1:0], px_flip_tgl};
        s_under <= {s_under[1:0], px_under_tgl};
        s_st <= {s_st[1:0], st_done_tgl};
        s_vb <= {s_vb[0], px_vblank};
        s_seen <= {s_seen[0], px_flip_seen};
    end
    reg [15:0] n_frames = 16'd0;
    reg [7:0]  n_under = 8'd0;
    reg [23:0] n_still = 24'd0;
    reg        still_valid = 1'b0, still_slot = 1'b0, still_60 = 1'b0;
    wire       flip_pending = back_tgl != s_seen[1];

    wire [11:0] reg_a = {addr[11:2], 2'b00};
    reg  [31:0] cur;                    // the register at reg_a: only the two status registers read
    always @(*)
        case (reg_a)
        12'h068: cur = {n_still, 5'd0, still_60, still_slot, still_valid};
        12'h0A4: cur = {n_frames, n_under, 6'd0, s_vb[1], flip_pending};
        default: cur = 32'd0;
        endcase

    wire [31:0] wv = {wdata, stg};
    always @(posedge clk_sys) begin
        ev_vsync <= s_vs[2] != s_vs[1];
        ev_flip  <= s_flip[2] != s_flip[1];
        if (s_vs[2] != s_vs[1]) n_frames <= n_frames + 16'd1;
        if (s_under[2] != s_under[1]) n_under <= n_under + 8'd1;
        if (s_st[2] != s_st[1]) begin
            n_still <= n_still + 24'd1;
            still_valid <= 1'b1;
            still_slot  <= st_done_slot;
            still_60    <= st_done_60;
        end

        if (we)
            case (addr[1:0])
            2'd0: stg[7:0]   <= wdata;
            2'd1: stg[15:8]  <= wdata;
            2'd2: stg[23:16] <= wdata;
            default: begin
                case (reg_a)
                12'h060: r_still_en   <= wv[0];
                12'h064: r_still_base <= {wv[31:4], 4'd0};
                12'h080: r_ctrl       <= wv[5:0];
                12'h084: r_front      <= {wv[31:4], 4'd0};
                12'h088: r_back       <= {wv[31:4], 4'd0};
                12'h090: begin r_w <= wv[15:0]; r_h <= wv[31:16]; end
                12'h094: r_stride     <= {wv[15:4], 4'd0};
                12'h098: begin r_x0 <= wv[15:0]; r_y0 <= wv[31:16]; end
                12'h09C: r_ga         <= wv[7:0];
                12'h0A0: r_key        <= wv[16:0];
                default: ;
                endcase
                if (reg_a == 12'h084) front_tgl <= ~front_tgl;
                else if (reg_a == 12'h088) back_tgl <= ~back_tgl;
                else cfg_tgl <= ~cfg_tgl;
            end
            endcase
        if (re && addr[1:0] == 2'd0) lat <= cur;
        case (addr[1:0])
        2'd0: rdata <= cur[7:0];
        2'd1: rdata <= lat[15:8];
        2'd2: rdata <= lat[23:16];
        default: rdata <= lat[31:24];
        endcase
    end

    // ======================= pixel side (clk) ==================================
    // settings: copied whenever the app commits one (the copy is taken again
    // at the next commit, so a value read while it changed is replaced), used
    // from the next frame start
    (* ASYNC_REG = "TRUE" *) reg [2:0] p_cfg = 3'd0, p_front = 3'd0, p_back = 3'd0;
    reg [5:0]  c_ctrl = 6'h22;
    reg [15:0] c_w = 16'd960, c_h = 16'd540, c_stride = 16'd1920, c_x0 = 16'd0, c_y0 = 16'd0;
    reg [7:0]  c_ga = 8'd255;
    reg [16:0] c_key = 17'd0;
    reg [31:0] c_front = 32'd0, c_back = 32'd0;
    reg        flip_req = 1'b0, flip_tgl = 1'b0, flip_seen = 1'b0, vs_tgl = 1'b0, under_tgl = 1'b0;
    always @(posedge clk) if (ce) begin
        p_cfg   <= {p_cfg[1:0], cfg_tgl};
        p_front <= {p_front[1:0], front_tgl};
        p_back  <= {p_back[1:0], back_tgl};
    end
    assign px_vs_tgl    = vs_tgl;
    assign px_flip_tgl  = flip_tgl;
    assign px_flip_seen = flip_seen;
    assign px_under_tgl = under_tgl;
    assign px_vblank    = cy_in < yact0;

    // per frame
    reg        f_en = 1'b0, par = 1'b0;
    reg [1:0]  f_s;
    reg [9:0]  f_w;
    reg [10:0] f_hm1;                   // layer height - 1, registered: keeps the subtract off
                                        // the m compare that gates the next line's request
    reg [11:0] f_xs;                    // cx of the first layer pixel
    reg [10:0] f_ys;                    // cy of the first layer line
    reg [15:0] f_stride;
    reg [8:0]  f_ga;                    // 0..256
    reg        f_ken;                   // colour key on
    reg [15:0] f_key;
    wire [1:0] s_new = (c_ctrl[5:4] == 2'd0) ? 2'd1 : c_ctrl[5:4];
    wire       en_new = c_ctrl[0] && c_ctrl[3:1] == 3'd1 && c_w != 0 && c_h != 0 &&
                        c_x0 < {4'd0, hact} && c_y0 < {5'd0, vact};

    // fetch requests (stable from their toggle to the next request)
    reg        rq_tgl = 1'b0;
    reg [6:0]  rq_n;                    // 128-bit words
    reg        rq_slot;
    reg [11:0] rq_tag;                  // {frame parity, line}
    reg [31:0] la;                      // address of the last line asked for; with rq_tgl it is the request's address

    // fetch done (ui_clk), into the line tags
    wire       fd_tgl, fd_slot;
    wire [11:0] fd_tag;
    (* ASYNC_REG = "TRUE" *) reg [2:0] p_fd = 3'd0;
    reg [11:0] vtag0 = 12'hFFF, vtag1 = 12'hFFF;

    // lines
    reg        row_on = 1'b0, first = 1'b0, line_ok = 1'b0, slot_cur = 1'b0;
    reg [10:0] m;
    reg [1:0]  sub;

    always @(posedge clk) if (ce) begin
        if (p_cfg[2] != p_cfg[1]) begin
            c_ctrl <= r_ctrl; c_w <= r_w; c_h <= r_h; c_stride <= r_stride;
            c_x0 <= r_x0; c_y0 <= r_y0; c_ga <= r_ga; c_key <= r_key;
        end
        p_fd <= {p_fd[1:0], fd_tgl};
        if (p_fd[2] != p_fd[1]) begin
            if (fd_slot) vtag1 <= fd_tag; else vtag0 <= fd_tag;
        end

        // frame start, once the last pixels of the frame are through the
        // pipeline (they use f_key up to cx 3, f_ga up to cx 5)
        if (cy_in == 11'd0 && cx_in == 12'd8) begin
            vs_tgl <= ~vs_tgl;
            par    <= ~par;
            f_en   <= en_new;
            f_s    <= s_new;
            f_w    <= (c_w > 16'd960) ? 10'd960 : c_w[9:0];
            f_hm1  <= ((c_h > {5'd0, vact}) ? vact : c_h[10:0]) - 11'd1;
            f_xs   <= xact0 + c_x0[11:0];
            f_ys   <= yact0 + c_y0[10:0];
            f_stride <= c_stride;
            f_ga   <= {1'b0, c_ga} + c_ga[7];
            f_ken  <= c_key[16];
            f_key  <= c_key[15:0];
            vtag0  <= 12'hFFF;
            vtag1  <= 12'hFFF;
            if (flip_req) begin
                c_front   <= c_back;
                flip_req  <= 1'b0;
                flip_tgl  <= ~flip_tgl;
                flip_seen <= p_back[2];
                la        <= c_back;
            end else
                la        <= c_front;
        end
        // after the frame start: a request in its clock waits for the next one
        if (p_front[2] != p_front[1]) c_front <= r_front;
        if (p_back[2] != p_back[1]) begin c_back <= r_back; flip_req <= 1'b1; end
        if (cy_in == 11'd0 && cx_in == 12'd12 && f_en) begin
            rq_n <= f_w[9:3] + (f_w[2:0] != 3'd0);
            rq_slot <= 1'b0; rq_tag <= {par, 11'd0}; rq_tgl <= ~rq_tgl;
        end

        if (cx_in == 12'd16) begin
            if (cy_in == 11'd0)
                row_on <= 1'b0;
            else if (f_en && cy_in == f_ys) begin
                row_on <= 1'b1; m <= 11'd0; sub <= 2'd0; first <= 1'b1;
            end else if (row_on) begin
                if (sub == f_s - 2'd1) begin
                    sub <= 2'd0;
                    if (m == f_hm1) row_on <= 1'b0;
                    else begin m <= m + 11'd1; first <= 1'b1; end
                end else begin
                    sub <= sub + 2'd1; first <= 1'b0;
                end
            end
        end
        if (cx_in == 12'd24) begin
            if (!row_on)
                line_ok <= 1'b0;
            else if (first) begin
                line_ok  <= (m[0] ? vtag1 : vtag0) == {par, m};
                slot_cur <= m[0];
                if ((m[0] ? vtag1 : vtag0) != {par, m}) under_tgl <= ~under_tgl;
                if (m != f_hm1) begin
                    la <= la + f_stride;
                    rq_slot <= ~m[0]; rq_tag <= {par, m + 11'd1}; rq_tgl <= ~rq_tgl;
                end
            end
        end
    end

    // pixels, one stage per clock from the pixel at cx_in: E0 counters, E1
    // line buffer, E2 its output register, E3 lane, E4 colour key, E5 fg - bg,
    // E6 product (DSP48 M register), E7 DSP48 P register, E8 out. The
    // difference and the product are registered on their own (DSP48 A/B, M,
    // P), so no path has more than one adder or one multiplier stage.
    (* ram_style = "block" *) reg [127:0] lb [0:255];
    reg        h_on = 1'b0;
    reg [9:0]  pidx;
    reg [1:0]  hsub;
    wire [9:0] pidx_n = pidx + 10'd1;
    always @(posedge clk) if (ce)
        if (cx_in == f_xs) begin
            h_on <= line_ok; pidx <= 10'd0; hsub <= 2'd0;
        end else if (h_on) begin
            if (hsub == f_s - 2'd1) begin
                hsub <= 2'd0;
                if (pidx_n == f_w) h_on <= 1'b0;
                else pidx <= pidx_n;
            end else
                hsub <= hsub + 2'd1;
        end

    reg [23:0] vrgb [1:8];
    reg [25:0] vctl [1:8];              // {de, hs, vs, cx, cy}
    reg [127:0] lb_q, lb_r;
    reg [2:0]  lane2, lane3;
    reg        on2, on3, on4, on5, on6, on7, on8;
    reg [15:0] fg4, fg5;
    reg signed [8:0]  d6 [0:2];
    reg signed [18:0] m7 [0:2], p8 [0:2];
    integer i;
    // RGB565 -> 888 by bit replication
    wire [23:0] fg888 = {fg5[15:11], fg5[15:13], fg5[10:5], fg5[10:9], fg5[4:0], fg5[4:2]};
    always @(posedge clk) if (ce) begin
        vrgb[1] <= rgb_in; vctl[1] <= {de_in, hs_in, vs_in, cx_in, cy_in};
        for (i = 2; i <= 8; i = i + 1) begin vrgb[i] <= vrgb[i-1]; vctl[i] <= vctl[i-1]; end
        // E1
        lb_q  <= lb[{slot_cur, pidx[9:3]}];
        lane2 <= pidx[2:0];
        on2   <= h_on && vctl[1][25];
        // E2
        lb_r  <= lb_q;
        lane3 <= lane2;
        on3   <= on2;
        // E3
        fg4 <= lb_r[16*lane3 +: 16];
        on4 <= on3;
        // E4
        fg5 <= fg4;
        on5 <= on4 && !(f_ken && fg4 == f_key);
        // E5
        for (i = 0; i < 3; i = i + 1)
            d6[i] <= $signed({1'b0, fg888[8*i +: 8]}) - $signed({1'b0, vrgb[5][8*i +: 8]});
        on6 <= on5;
        // E6, E7
        for (i = 0; i < 3; i = i + 1) begin
            m7[i] <= d6[i] * $signed({1'b0, f_ga});
            p8[i] <= m7[i];
        end
        on7 <= on6;
        on8 <= on7;
        // E8
        for (i = 0; i < 3; i = i + 1)
            rgb[8*i +: 8] <= on8 ? vrgb[8][8*i +: 8] + p8[i][15:8] : vrgb[8][8*i +: 8];
        {de, hs, vs, cx, cy} <= vctl[8];
    end

    // ======================= DDR2 side (ui_clk) ================================
    (* ASYNC_REG = "TRUE" *) reg [2:0] u_rq = 3'd0;
    (* ASYNC_REG = "TRUE" *) reg [1:0] u_st = 2'd0;
    reg        nxt = 1'b0, run = 1'b0;
    reg [31:0] n_addr;                  // the request, taken on its toggle
    reg [6:0]  n_n;
    reg        n_slot;
    reg [11:0] n_tag;
    reg [26:0] rd_a;                    // MIG address of the next read
    reg [6:0]  n_left;                  // reads still to issue
    reg [6:0]  rcv_left;                // words still to land for this line
    reg [4:0]  out = 5'd0;              // reads in flight
    reg [7:0]  wa;                      // line buffer address of the next word
    reg        cur_slot;
    reg [9:0]  age;                     // ui_clk since the line's fetch began
    reg [11:0] cur_tag;
    reg        done_tgl = 1'b0, done_slot = 1'b0;
    reg [11:0] done_tag = 12'hFFF;
    assign fd_tgl = done_tgl; assign fd_slot = done_slot; assign fd_tag = done_tag;

    // the still word from vclk: one entry, full while its toggle differs from the ack
    reg          st_tgl = 1'b0, st_ack = 1'b0;
    reg  [127:0] st_d;
    reg  [26:4]  st_a;
    wire         st_pend = u_st[1] != st_ack;

    wire issue = run && !nxt && n_left != 7'd0;
    assign v_en       = st_pend || issue;
    assign v_cmd      = st_pend ? 3'b000 : 3'b001;
    assign v_addr     = st_pend ? {1'b0, st_a, 3'b000} : rd_a;
    assign v_wdf_data = st_d;
    assign v_wdf_mask = 16'h0000;
    assign v_wdf_wren = st_pend;
    wire rd_go = v_rdy && !st_pend;

    always @(posedge ui_clk) begin
        u_rq  <= {u_rq[1:0], rq_tgl};
        u_st  <= {u_st[0], st_tgl};
        if (u_rq[2] != u_rq[1]) begin
            nxt <= 1'b1; run <= 1'b0;
            n_addr <= la; n_n <= rq_n; n_slot <= rq_slot; n_tag <= rq_tag;
        end

        out <= out + rd_go - v_rd_data_valid;
        if (v_rdy && st_pend) st_ack <= ~st_ack;
        if (rd_go) begin
            rd_a   <= rd_a + 27'd8;
            n_left <= n_left - 7'd1;
        end
        if (v_rd_data_valid) begin
            lb[wa] <= v_rd_data;
            wa     <= wa + 8'd1;
            if (run) begin
                rcv_left <= rcv_left - 7'd1;
                if (rcv_left == 7'd1) begin
                    run <= 1'b0;
                    done_tag <= cur_tag; done_slot <= cur_slot; done_tgl <= ~done_tgl;
                end
            end
        end
        // the next line once the last one's reads have landed
        if (nxt && u_rq[2] == u_rq[1] && out == 5'd0 && !v_rd_data_valid) begin
            nxt      <= 1'b0;
            run      <= n_n != 7'd0;
            rd_a     <= {1'b0, n_addr[26:4], 3'b000};
            n_left   <= n_n;
            rcv_left <= n_n;
            wa       <= {n_slot, 7'd0};
            cur_slot <= n_slot;
            cur_tag  <= n_tag;
        end
        // a line not in after 400 clocks (5.3 us of the 17.7 us an output
        // line takes at 1080p; longer in the other modes) goes before the RISC-V and the REU in mig_arb
        if (!run) age <= 10'd0;
        else if (age != 10'h3FF) age <= age + 10'd1;
        v_urgent <= run && age >= 10'd400;
        if (ui_rst) begin
            nxt <= 1'b0; run <= 1'b0; out <= 5'd0;
        end
    end

    // ======================= still capture (vclk) ==============================
    (* ASYNC_REG = "TRUE" *) reg [2:0] v_cfg = 3'd0;
    (* ASYNC_REG = "TRUE" *) reg [1:0] v_ack = 2'd0;
    reg        sv_en = 1'b1;
    reg [31:0] sv_base = 32'h05050000;
    reg [3:0]  pix_d;
    reg [9:0]  px_d = 10'd0;
    reg [8:0]  py_d = 9'd0;
    reg        cap = 1'b0, torn = 1'b0, slot = 1'b0, last_slot = 1'b1;
    reg [31:0] base;
    reg [127:0] sh;                     // the window's pixels, 32 to a word: pixel k at nibble k mod 32
    reg [4:0]  k = 5'd0;
    reg [11:0] widx = 12'd0;
    reg        sd_tgl = 1'b0, sd_slot = 1'b0, sd_60 = 1'b0;
    assign st_done_tgl = sd_tgl; assign st_done_slot = sd_slot; assign st_done_60 = sd_60;
    (* ASYNC_REG = "TRUE" *) reg [1:0] v60_vs = 2'b00;
    reg        m60 = 1'b0;                  // the mode of the frame being captured
    wire       v60_v = v60_vs[1];
    wire       f60   = cap ? m60 : v60_v;
    wire       st_free = st_tgl == v_ack[1];
    wire       emit = px != px_d;       // the pixel at px_d is final
    wire       in_x = px_d >= X0S && px_d < X0S + 10'd400;
    wire       in_y = f60 ? (py_d < 9'd8 || py_d >= 9'd31)
                            : (py_d >= Y0S && py_d < Y0S + 9'd270);
    wire       start = px_d == X0S && py_d == (v60_v ? 9'd0 : Y0S);
    wire       cap_n = start ? sv_en : cap;
    wire [4:0] kk = start ? 5'd0 : k;
    wire [127:0] sh_n = {pix_d, sh[127:4]};  // the word, when pix_d is its 32nd pixel
    always @(posedge vclk) begin
        v_cfg  <= {v_cfg[1:0], cfg_tgl};
        v_ack  <= {v_ack[0], st_ack};
        if (v_cfg[2] != v_cfg[1]) begin sv_en <= r_still_en; sv_base <= r_still_base; end
        pix_d <= pix; px_d <= px; py_d <= py;
        v60_vs <= {v60_vs[0], v60};

        if (emit && in_x && (in_y || start)) begin
            if (start) begin
                cap  <= sv_en; torn <= 1'b0; slot <= ~last_slot;
                base <= sv_base + {15'd0, ~last_slot, 16'd0};
                widx <= v60_v ? 12'd2900 : 12'd0; k <= 5'd0;
                m60  <= v60_v;
            end
            if (cap_n) begin
                k <= kk + 5'd1;
                sh <= sh_n;
                if (kk == 5'd31) begin
                    widx <= (f60 && widx == 12'd2999) ? 12'd0 : widx + 12'd1;
                    if (st_free) begin
                        st_d   <= sh_n;
                        st_a   <= base[26:4] + {11'd0, widx};
                        st_tgl <= ~st_tgl;
                    end else
                        torn <= 1'b1;
                    if (widx == (f60 ? 12'd2899 : 12'd3374)) begin
                        cap <= 1'b0;
                        if (!torn && st_free) begin
                            last_slot <= slot;
                            sd_slot   <= slot;
                            sd_60     <= f60;
                            sd_tgl    <= ~sd_tgl;
                        end
                    end
                end
            end
        end
    end
endmodule
