// SPDX-License-Identifier: GPL-3.0-or-later
// The SID sockets' audio (T37, spec §4.3): the real SIDs' analog output goes into the codec's AUXIN, the
// ADC data comes back on AUDIO_SDO, and this block mixes it as the app's mixer sources "Socket 1" and
// "Socket 2". Everything runs in mclk, framed by i2s_out's counter (256 mclk per frame, BCLK = mclk / 4,
// the codec is the slave of our BCLK and LRCK, 24 bit I2S, MSB one BCLK after the LRCK edge).
//
// Receiver: bit period b = cnt[7:2] (0-63; b[5] = right slot); data bits are periods 1-24 of each slot; the
// codec changes SDO after BCLK falls (at cnt = 4b), so the bit is taken near the middle of the period
// (the pad is registered at cnt[1:0] = RX_PH - 1, consumed at RX_PH).
// Mixer: ADC left is socket 1, ADC right socket 2 (SOCK_SWAP = 1: the other way; which ADC channel is
// which socket is open, M5, H: the code enables AUXIN to both ADCs, Gideon's core shows the same level on
// L and R). The app's bytes 4-7 of U64_AUDIO_MIXER, per socket right then left gain, 0x80 = 0 dB, 0xFF = about
// +6 dB, saturated; a socket that is not enabled adds nothing. One multiplier (one DSP) does the four
// products in the first mclk clocks of a frame, from the pair of the frame before: the result is stable from
// cnt = 8 to the frame's end, before frame_en (cnt = 127) and the I2S frame start.
module sock_audio #(
    parameter RX_PH = 3,
    parameter SOCK_SWAP = 0
) (
    input  wire               mclk,
    input  wire         [7:0] cnt,          // i2s_out's frame counter
    input  wire               sdo,          // AUDIO_SDO (codec ADC data)
    input  wire        [31:0] gains,        // {S2 L, S2 R, S1 L, S1 R} (async, static)
    input  wire         [1:0] en,           // the sockets' enables (async, static)
    output reg  signed [23:0] out_l = 24'sd0,
    output reg  signed [23:0] out_r = 24'sd0
);
    // ---- the app's bytes into mclk ---------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) reg [33:0] cfg_1 = 34'd0, cfg_2 = 34'd0;
    always @(posedge mclk) begin
        cfg_1 <= {en, gains};
        cfg_2 <= cfg_1;
    end
    wire [7:0] g_s1r = cfg_2[7:0],   g_s1l = cfg_2[15:8];
    wire [7:0] g_s2r = cfg_2[23:16], g_s2l = cfg_2[31:24];

    // ---- the receiver -----------------------------------------------------------------------------
    reg         sdo_q = 1'b0;
    reg  [23:0] sh = 24'd0;
    reg  signed [23:0] adc_l = 24'sd0, adc_r = 24'sd0;
    wire [5:0]  b   = cnt[7:2];
    wire        rt  = b[5];
    always @(posedge mclk) begin
        sdo_q <= sdo;
        if (cnt[1:0] == RX_PH[1:0]) begin
            if (b[4:0] >= 5'd1 && b[4:0] <= 5'd24) sh <= {sh[22:0], sdo_q};
            if (b[4:0] == 5'd24) begin
                if (!rt) adc_l <= {sh[22:0], sdo_q};
                else     adc_r <= {sh[22:0], sdo_q};
            end
        end
    end

    // ---- the mixer: four products through one multiplier ------------------------------------------------
    // The pair of the receiver is stable from cnt = 228 to cnt = 99 of the next frame, so the schedule below
    // (cnt 1-7) reads it directly.
    wire signed [23:0] s_1 = SOCK_SWAP ? adc_r : adc_l;
    wire signed [23:0] s_2 = SOCK_SWAP ? adc_l : adc_r;
    reg  signed [23:0] a_sel = 24'sd0;
    reg         [7:0]  g_sel = 8'd0;
    reg  signed [32:0] prod = 33'sd0;
    reg  signed [33:0] acc = 34'sd0;
    wire signed [33:0] shr = acc >>> 7;                     // 0x80 = unity
    // fits 24 bits when bits 33:23 are all the sign
    wire               fits = shr[33:23] == 11'h000 || shr[33:23] == 11'h7FF;
    wire signed [23:0] sat  = fits ? shr[23:0] : shr[33] ? 24'sh800000 : 24'sh7FFFFF;
    // steps cnt 1-4 pick (socket, side) = (1, L), (2, L), (1, R), (2, R); free-running selects, the enable of the socket gates the gain
    wire         is2   = (cnt == 8'd2) || (cnt == 8'd4);
    wire         isr   = (cnt == 8'd3) || (cnt == 8'd4);
    wire   [7:0] g_raw = is2 ? (isr ? g_s2r : g_s2l) : (isr ? g_s1r : g_s1l);
    always @(posedge mclk) begin
        a_sel <= is2 ? s_2 : s_1;
        g_sel <= (is2 ? cfg_2[33] : cfg_2[32]) ? g_raw : 8'd0;
        prod  <= a_sel * $signed({1'b0, g_sel});
        case (cnt)
        8'd3: acc <= prod;
        8'd4: acc <= acc + prod;
        8'd5: begin out_l <= sat; acc <= prod; end
        8'd6: acc <= acc + prod;
        8'd7: out_r <= sat;
        default: ;
        endcase
    end
endmodule
