// SPDX-License-Identifier: GPL-3.0-or-later
// T37 (docs/specs/T37-sid-sockets.md §4.3, §6): i2s_out and sock_audio against a model of the codec's ADC
// (slave of our BCLK/LRCK, 24 bit I2S, MSB one BCLK after the LRCK edge, SDO changing 30 ns after BCLK falls).
// The ADC delivers known samples L(n), R(n) per frame; the mixer output at cnt = 100 of each frame must equal
// the bit-exact mix of one of the last frames' pairs (the receiver and the mixer work on the frame before):
//   out_l = sat((L*S1L + R*S2L) >> 7), out_r = sat((L*S1R + R*S2R) >> 7), a socket that is not enabled adds 0.
// Phases: unity/half gains, full gains (saturation), socket 2 off, both off, SOCK_SWAP is parameter 1 in a
// second instance. Prints PASS when every checked frame matched.
`timescale 1ns/1ps
module tb;
    reg mclk = 0;
    always #40.69 mclk = ~mclk;                         // 12.288 MHz
    wire bclk, lrck, sdata, spk, frame_en;
    wire [7:0] cnt;
    i2s_out i2s (.mclk(mclk), .frame_en(frame_en), .in_l(24'sd0), .in_r(24'sd0), .spk_en(1'b0), .spk_gain(8'd0),
                 .bclk(bclk), .lrck(lrck), .sdata(sdata), .spk(spk), .cnt(cnt));

    // ---- the codec's ADC ------------------------------------------------------------------------
    function signed [23:0] smp_l(input integer n); smp_l = 24'sh400000 + n * 24'sh1111; endfunction
    function signed [23:0] smp_r(input integer n); smp_r = 24'sh300000 - n * 24'sh0777; endfunction
    reg sdo = 1'b0;
    integer f = 0, pos = 0;
    reg lrck_p = 1'b0, right = 1'b0;
    reg [23:0] word;
    always @(negedge bclk) begin
        if (lrck !== lrck_p) begin
            pos = 0; right = lrck;
            if (!lrck) f = f + 1;
        end else pos = pos + 1;
        lrck_p = lrck;
        word = right ? smp_r(f) : smp_l(f);
        #30 sdo <= (pos >= 1 && pos <= 24) ? word[24 - pos] : 1'b0;
    end

    // ---- the DUTs: the unit under test and the swapped one -----------------------------------------------
    reg [31:0] gains = 32'd0;                           // {S2 L, S2 R, S1 L, S1 R}
    reg  [1:0] en = 2'b00;
    wire signed [23:0] ol, orr, sol, sor;
    sock_audio                 dut  (.mclk(mclk), .cnt(cnt), .sdo(sdo), .gains(gains), .en(en), .out_l(ol),  .out_r(orr));
    sock_audio #(.SOCK_SWAP(1)) dut2 (.mclk(mclk), .cnt(cnt), .sdo(sdo), .gains(gains), .en(en), .out_l(sol), .out_r(sor));

    function signed [23:0] sat(input signed [40:0] v);
        sat = (v > 41'sd8388607) ? 24'sh7FFFFF : (v < -41'sd8388608) ? 24'sh800000 : v[23:0];
    endfunction
    function signed [23:0] mix(input signed [23:0] a, input signed [23:0] b, input [7:0] ga, input [7:0] gb,
                               input ea, input eb);
        reg signed [40:0] t;
        begin
            t = (ea ? a * $signed({1'b0, ga}) : 41'sd0) + (eb ? b * $signed({1'b0, gb}) : 41'sd0);
            mix = sat(t >>> 7);
        end
    endfunction

    // ---- the check ---------------------------------------------------------------------------------
    integer checked = 0, bad = 0, k, phase = 0, seen = 0;
    reg ok, ok2;
    reg [7:0] cnt_q = 8'd0;
    always @(posedge mclk) begin
        cnt_q <= cnt;
        if (cnt == 8'd100 && cnt_q == 8'd99 && seen > 4) begin
            ok = 0; ok2 = 0;
            for (k = f - 3; k <= f + 1; k = k + 1) begin
                if (ol === mix(smp_l(k), smp_r(k), gains[15:8], gains[31:24], en[0], en[1]) &&
                    orr === mix(smp_l(k), smp_r(k), gains[7:0], gains[23:16], en[0], en[1])) ok = 1;
                if (sol === mix(smp_r(k), smp_l(k), gains[15:8], gains[31:24], en[0], en[1]) &&
                    sor === mix(smp_r(k), smp_l(k), gains[7:0], gains[23:16], en[0], en[1])) ok2 = 1;
            end
            // the swapped one: socket 1 is the ADC's right channel
            checked = checked + 1;
            if (!ok || !ok2) begin
                bad = bad + 1;
                if (bad < 6) $display("FAIL phase %0d frame %0d: out %h %h, swapped %h %h", phase, f, ol, orr, sol, sor);
            end
        end
        if (cnt == 8'd100 && cnt_q == 8'd99) seen = seen + 1;
    end

    task phase_set(input integer p, input [31:0] g, input [1:0] e);
        begin
            phase = p; gains = g; en = e; seen = 0;
        end
    endtask
    initial begin
        phase_set(1, {8'h80, 8'h80, 8'h40, 8'h20}, 2'b11);      // S2L 0x80, S2R 0x80, S1L 0x40, S1R 0x20
        #(40 * 256 * 81.38 * 1.0);
        phase_set(2, 32'hFFFF_FFFF, 2'b11);                     // saturation
        #(40 * 256 * 81.38);
        phase_set(3, {8'h80, 8'h80, 8'h80, 8'h80}, 2'b01);      // socket 2 off
        #(40 * 256 * 81.38);
        phase_set(4, {8'h80, 8'h80, 8'h80, 8'h80}, 2'b00);      // both off
        #(40 * 256 * 81.38);
        phase_set(5, {8'h00, 8'h00, 8'h00, 8'h00}, 2'b11);      // gain 0
        #(40 * 256 * 81.38);
        $display("%0d frames checked, %0d wrong", checked, bad);
        if (bad == 0 && checked > 150) $display("PASS"); else $display("FAIL");
        $finish;
    end
endmodule
