// SPDX-License-Identifier: GPL-3.0-or-later
// The app's keyboard-board registers (ultimate_sys hands over IO
// 0x10100600-0x101009FF, bl_addr = address bits 11:0):
//   0x800-0x803  Bling RX (software/system/u64.h BLING_RX_*): the bytes the
//                keyboard board sends on BLING_KEYB (bling_keyboard, sys_clk)
//                in a 16-byte FIFO
//                +0 DATA  oldest byte
//                +1 GET   write: drop it
//                +2 FLAGS b7 data available, b2 board installed, b1 SHIFT
//                         LOCK down, b0 shift lock off (write: the menu's
//                         scan sees no SHIFT from it)
//                +3 IRQEN b0: ITU high IRQ 4 while data is available
//   everything else (the app's LED registers 0x600-0x6FF and 0x900-0x9FF):
//                writes are ignored, reads give 0 (OPT-bling, A04: the LED
//                effects are out of the bitstream)
// BLING_LED carries a fixed WS2812 frame, refreshed all the time: 70 LEDs
// dark, the 71st (SHIFT LOCK) amber while locked. WS2812: 800 kHz, 0 = 0.35 us
// high, 1 = 0.8 us high, then 310 us low.
module bling_io #(
    parameter integer CLK_HZ = 100000000
) (
    input  wire        clk,
    input  wire        rst,
    input  wire        we,
    input  wire [11:0] addr,
    input  wire  [7:0] wdata,
    output reg   [7:0] rdata,               // one clock after addr
    output wire        irq,
    // from bling_keyboard (sys_clk): rx_tgl toggles with each byte in rx_byte
    input  wire        rx_tgl,
    input  wire  [7:0] rx_byte,
    input  wire        locked,
    output reg         lock_off = 1'b0,
    output reg         bling_led = 1'b0
);
    wire sel_rx = addr[11:8] == 4'h8;

    // ---- Bling RX ----
    (* ASYNC_REG = "TRUE" *) reg [1:0] tgl_s = 2'b00, lock_s = 2'b00;
    reg        tgl_d = 1'b0;
    reg  [7:0] fifo [0:15];
    reg  [4:0] wp = 5'd0, rp = 5'd0;
    reg        irqen = 1'b0;
    wire       avail = wp != rp;
    wire       full  = (wp ^ rp) == 5'b10000;
    always @(posedge clk) begin
        tgl_s  <= {tgl_s[0], rx_tgl};
        lock_s <= {lock_s[0], locked};
        tgl_d  <= tgl_s[1];
        if (tgl_d != tgl_s[1] && !full) begin   // rx_byte has been still for 2 clocks
            fifo[wp[3:0]] <= rx_byte;
            wp <= wp + 5'd1;
        end
        if (we && sel_rx) case (addr[1:0])
            2'd1: if (avail) rp <= rp + 5'd1;
            2'd2: lock_off <= wdata[0];
            2'd3: irqen <= wdata[0];
            default: ;
        endcase
        if (rst) begin
            wp <= 5'd0; rp <= 5'd0; irqen <= 1'b0; lock_off <= 1'b0;
        end
    end
    assign irq = irqen & avail;

    reg        rsel = 1'b0;
    reg  [7:0] rx_rdata = 8'h00;
    always @(posedge clk) begin
        rsel <= sel_rx;
        case (addr[1:0])
        2'd0: rx_rdata <= fifo[rp[3:0]];
        2'd2: rx_rdata <= {avail, 4'd0, 1'b1, lock_s[1], lock_off};
        2'd3: rx_rdata <= {7'd0, irqen};
        default: rx_rdata <= 8'h00;
        endcase
    end
    always @(*) rdata = rsel ? rx_rdata : 8'h00;

    // ---- SHIFT LOCK LED: the keyboard's 71st LED, G R B = 1E 24 00 ----
    localparam integer T_BIT = CLK_HZ / 800000;             // 1.25 us
    localparam integer T_0H  = CLK_HZ / 1000 * 35 / 100000; // 0.35 us
    localparam integer T_1H  = CLK_HZ / 1000 * 80 / 100000; // 0.8 us
    localparam integer T_RES = CLK_HZ / 1000 * 310 / 1000;  // 310 us
    localparam integer N_DARK = 70 * 24;                    // bits before the last LED
    localparam [23:0]  AMBER = 24'h1E2400;
    reg [14:0] t = 15'd0;                   // clocks in the bit, or in the reset gap
    reg [10:0] n = 11'd0;                   // bit of the frame
    reg        gap = 1'b1, lk = 1'b0;
    wire [4:0] j    = n[4:0] - N_DARK[4:0]; // bit of the last LED (N_DARK mod 32 = 16)
    wire       dbit = n >= N_DARK && lk && AMBER[5'd23 - j];
    always @(posedge clk) begin
        if (gap) begin
            bling_led <= 1'b0;
            t <= t + 15'd1;
            if (t == T_RES) begin gap <= 1'b0; t <= 15'd0; end
        end else begin
            if (n == 11'd0 && t == 15'd0) lk <= lock_s[1];
            bling_led <= t < (dbit ? T_1H : T_0H);
            if (t == T_BIT - 1) begin
                t <= 15'd0;
                if (n == N_DARK + 23) begin n <= 11'd0; gap <= 1'b1; end
                else n <= n + 11'd1;
            end else
                t <= t + 15'd1;
        end
        if (rst) begin gap <= 1'b1; t <= 15'd0; n <= 11'd0; bling_led <= 1'b0; end
    end
endmodule
