// SPDX-License-Identifier: GPL-3.0-or-later
// Equivalence bench (run.sh): the descriptor-driven cart_slot (T33) and the
// per-type cart_slot it replaced (cart_slot_legacy, from git) on the same
// random stream of C64 bus cycles, per CART_TYPE. Compared in every cycle:
// the lines, cartridge on, io2_we_valid, which read the cartridge answers and
// with what (register value, or the image / RAM address it reads), the RAM
// writes, the flash responder's select / chip / address, the EEPROM's input
// bits, and the registers (state bus).
//
// Run A: descriptors that reproduce the old model (legacy/*.json). Every
// difference is a failure, except the two the old model had and no
// descriptor can express (skip / ignore below, "A:").
// Run B: the app's descriptors (tools/cart_descriptors). The intended
// changes (T33 §2.3 D1, D3, D6, D7; IO reads of devices that are not there
// are open bus) are the only differences allowed, listed per type ("B:").
// The new behaviour itself is checked in cart_unit (tb_cart_unit.v).
//
// Also: the side-effect-free look (pk_*, T12) at the address of every read
// equals what the access reads (hit, register value, image / RAM address).
//
// The cache below the decode is not fed (no DDR2): what is compared is the
// address a read or write goes to, not the byte.
`timescale 1ns / 1ps
module tb_cart_equiv;
    `include "desc_app.vh"
    `include "desc_legacy.vh"

    reg clk = 1'b0;
    always #16 clk = ~clk;

    reg  [15:0] addr = 16'd0;
    reg   [7:0] din = 8'hFF;
    reg         we = 1'b0, roml_n = 1'b1, romh_n = 1'b1, io1_n = 1'b1, io2_n = 1'b1;
    reg         phi2_p = 1'b0, phi2_h = 1'b0, phi2_n = 1'b0, reset = 1'b1;
    reg   [7:0] cart_type = 8'd0;
    reg [343:0] desc = 344'd0;

    wire n_act, n_exrom, n_game, n_wev, o_act, o_exrom, o_game, o_wev;
    // the side-effect-free look (T12) at the address of the bus access: ROML 0, ROMH 1, IO1 2, IO2 3
    wire  [1:0] pk_cls = !io2_n ? 2'd3 : !io1_n ? 2'd2 : !romh_n ? 2'd1 : 2'd0;
    wire        pk_on = !roml_n || !romh_n || !io1_n || !io2_n;
    wire        pk_hit, pk_mem;
    wire [23:0] pk_maddr;
    wire  [7:0] pk_data;
    wire [15:0] n_bank, o_bank;

    cart_slot n (
        .clk(clk), .cart_type(cart_type), .desc(desc), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0),
        .cart_active(n_act),
        .addr(addr), .din(din), .we(we), .roml_n(roml_n), .romh_n(romh_n), .io1_n(io1_n), .io2_n(io2_n),
        .phi2_p(phi2_p), .phi2_h(phi2_h), .phi2_n(phi2_n), .reset(reset), .freeze(1'b0),
        .exrom(n_exrom), .game(n_game), .dout(), .nmi(), .irq(), .io2_we_valid(n_wev), .wait_cpu(),
        .req_tgl(), .req_we(), .req_addr(), .req_data(), .req_slot(), .req_cached(), .ack_tgl(1'b0),
        .fill_clk(clk), .fill_be(8'd0), .fill_addr(9'd0), .fill_data(64'd0),
        .fl_enable(1'b0), .fl_magic1(16'd0), .fl_magic2(16'd0), .fl_mask(16'd0), .fl_id(24'd0),
        .evt_tgl(), .evt_word(), .fl_ack_tgl(1'b0), .inval_tgl(1'b0), .ee_tgl(),
        .ee_clk(clk), .ee_addr(11'd0), .ee_we(1'b0), .ee_wdata(8'd0), .ee_rdata(),
        .st_bank(n_bank), .wtap(), .wtap_addr(), .wtap_data(),
        .pk_addr(addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr), .pk_data(pk_data)
    );
    cart_slot_legacy o (
        .clk(clk), .cart_type(cart_type), .cart_kill_tgl(1'b0), .cart_force_tgl(1'b0),
        .cart_active(o_act),
        .addr(addr), .din(din), .we(we), .roml_n(roml_n), .romh_n(romh_n), .io1_n(io1_n), .io2_n(io2_n),
        .phi2_p(phi2_p), .phi2_h(phi2_h), .phi2_n(phi2_n), .reset(reset), .freeze(1'b0),
        .exrom(o_exrom), .game(o_game), .dout(), .nmi(), .irq(), .io2_we_valid(o_wev), .wait_cpu(),
        .req_tgl(), .req_we(), .req_addr(), .req_data(), .req_slot(), .req_cached(), .ack_tgl(1'b0),
        .fill_clk(clk), .fill_be(8'd0), .fill_addr(9'd0), .fill_data(64'd0),
        .fl_enable(1'b0), .fl_magic1(16'd0), .fl_magic2(16'd0), .fl_mask(16'd0), .fl_id(24'd0),
        .evt_tgl(), .evt_word(), .fl_ack_tgl(1'b0), .inval_tgl(1'b0), .ee_tgl(),
        .ee_clk(clk), .ee_addr(11'd0), .ee_we(1'b0), .ee_wdata(8'd0), .ee_rdata(),
        .st_bank(o_bank), .wtap(), .wtap_addr(), .wtap_data()
    );

    // compare groups
    localparam G_LINES = 1, G_ACT = 2, G_BANK = 4, G_SERVE = 8, G_MEM = 16, G_FLASH = 32, G_EE = 64, G_WEV = 128;

    integer run = 0;            // 0: A, 1: B
    integer fails = 0, cycles = 0, skipped = 0, ignored = 0, peeks = 0;
    integer tfails;
    reg [8*24-1:0] tname;

    wire normal = cart_type[4:0] == 5'h01;
    wire mask00 = cart_type == 8'h08 || cart_type == 8'h69 || cart_type == 8'h28 || cart_type == 8'hA8 ||
                  cart_type == 8'h48;
    wire ar     = cart_type[4:0] == 5'h1B;

    // accesses not issued: they would change the registers in a way the
    // other side cannot follow (old model behaviour no descriptor has, or an
    // intended change of the app's descriptor)
    function skip(input [15:0] a, input w, input [7:0] d);
        begin
            skip = 1'b0;
            // A: the old Normal switched off on $40 to $DFFF while IO1 writes loaded R1: one register, two uses
            if (run == 0 && normal && w && a == 16'hDFFF && d[7:6] == 2'b01) skip = 1'b1;
            if (run == 1) begin
                // B (D1): the whole IO1 page is the one register; the old model wrote R1 above $DE00
                if (mask00 && w && a[15:8] == 8'hDE && a[7:0] != 8'h00) skip = 1'b1;
                // B (D1): EasyFlash decodes A1 only; the old model: $DE00 R0, every other address R1
                if (cart_type == 8'h11 && w && a[15:8] == 8'hDE && a[7:0] != 8'h00 && a[7:0] != 8'h02) skip = 1'b1;
                // B (D3): C64MegaCart: IO1 is the bank register on every address; mode 01 holds the
                // lines (old: 16K), mode 11 is Ultimax for CPU writes only (old: static)
                if (cart_type == 8'h68 && w && a[15:8] == 8'hDE && a[7:0] != 8'h00) skip = 1'b1;
                if (cart_type == 8'h68 && w && a[15:8] == 8'hDF && d[6]) skip = 1'b1;
            end
        end
    endfunction

    // compare groups not checked for this access
    function [7:0] ign(input [15:0] a, input w);
        begin
            ign = 8'd0;
            // A: GMod2's EEPROM bit: the old model answered every IO1 read with {DO, 7'b0}, the
            // descriptor's DEV only while CS is set, with bits 6-0 open bus (VICE gmod2.c)
            if (run == 0 && cart_type == 8'h48 && !w && a[15:8] == 8'hDE) ign = G_SERVE;
            // A and B: the old model's default rule copied $DE00/$DExx writes into R0/R1 for the
            // Action Replay too; its registers are in its own block, the descriptor has no rules
            if (ar) ign = G_BANK;
            if (run == 1 && !ar) begin
                // B (D1, D6): IO reads of what the cartridge does not have are open bus; the old
                // model answered IO1 with its registers and IO2 with RAM (EasyFlash keeps its RAM)
                if (!w && a[15:8] == 8'hDE) ign = G_SERVE;
                if (!w && a[15:8] == 8'hDF && cart_type != 8'h11) ign = G_SERVE | G_MEM;
                // B (D3): C64MegaCart IO2 writes no longer go to a RAM it does not have
                if (w && a[15:8] == 8'hDF && cart_type == 8'h68) ign = G_MEM;
                // B: Normal has no registers in IO1 (old: R0/R1 written, IO1 reads them back);
                // its R1 is the $DFFF switch-off latch now
                if (normal) ign = ign | G_BANK;
            end
        end
    endfunction

    // the register bits the descriptor keeps (EasyFlash: VICE's masks)
    wire [15:0] bmask = run == 1 ? {desc[3*8 +: 8], desc[2*8 +: 8]} : 16'hFFFF;

    reg [7:0] ign_now = 8'd0;
    task cmp(input [7:0] g, input [8*32-1:0] what, input ok);
        begin
            if (g & ign_now) ignored = ignored + 1;
            else if (!ok) begin
                fails = fails + 1; tfails = tfails + 1;
                if (tfails <= 10)
                    $display("  %0s run %s: %0s differs at $%h %s %h (new/old: lines %b%b/%b%b bank %h/%h)",
                             tname, run ? "B" : "A", what, addr, we ? "W" : "R", din,
                             n_exrom, n_game, o_exrom, o_game, n_bank, o_bank);
            end
        end
    endtask

    wire n_fcs = n.model.flash_cs;
    wire o_fcs = o.model.flash_lo_cs || o.model.flash_hi_cs;

    task check_cpu_half;
        begin
            cmp(G_LINES, "lines", n_exrom === o_exrom && n_game === o_game);
            cmp(G_WEV, "io2_we_valid", n_wev === o_wev);
            cmp(G_SERVE, "served", we || n.served === o.served);   // dout matters in a read only
            cmp(G_SERVE, "IO value", !(n.served && !n.mem_read) || n.c_dout === o.c_dout);
            cmp(G_MEM, "mem read", n.mem_read === o.mem_read);
            cmp(G_MEM, "mem write", n.mem_write === o.mem_write);
            cmp(G_MEM, "mem address", !(n.mem_read || n.mem_write) || n.maddr === o.maddr);
            cmp(G_FLASH, "flash select", n_fcs === o_fcs);
            cmp(G_FLASH, "flash chip", !n_fcs || n.model.flash_chip === o.model.flash_hi_cs);
            cmp(G_FLASH, "flash address", !n_fcs || n.model.flash_addr === o.model.flash_addr);
            // the look equals the access, for every read (both runs, the new cartridge only)
            // (except the Action Replay switched off by its $DE00 bit 2: the bus path still answers
            // $DE00/$DE01 there, as the old cart_slot did; T12's look of the AR block does not)
            if (!we && pk_on && !(ar && n.model.c64_action_replay_0.u4a_q[2])) begin
                if (pk_hit !== n.served) begin
                    fails = fails + 1; tfails = tfails + 1;
                    if (tfails <= 10) $display("  %0s: look hit %b, access served %b at $%h", tname, pk_hit, n.served, addr);
                end else if (pk_hit && (pk_mem ? (pk_maddr !== n.maddr) : (pk_data !== n.c_dout))) begin
                    fails = fails + 1; tfails = tfails + 1;
                    if (tfails <= 10) $display("  %0s: look %h/%h, access %h/%h at $%h", tname, pk_maddr, pk_data, n.maddr, n.c_dout, addr);
                end else
                    peeks = peeks + 1;
            end
            cmp(G_EE, "EEPROM", n.model.ee_enable === o.model.ee_enable && n.model.ee_cycle === o.model.ee_cycle &&
                                (!n.model.ee_enable || (n.model.c64_eeprom_0.sel === o.model.c64_eeprom_0.sel &&
                                 n.model.c64_eeprom_0.sclk === o.model.c64_eeprom_0.sclk &&
                                 n.model.c64_eeprom_0.din === o.model.c64_eeprom_0.din)));
        end
    endtask

    // one C64 cycle, the PLA from the old model's lines (they are compared)
    task bus(input [15:0] a, input w, input [7:0] d);
        reg umax;
        begin
            @(negedge clk);
            umax   = o_exrom && !o_game;
            addr   = a; we = w; din = w ? d : 8'hFF;
            roml_n = !(a[15:13] == 3'b100 && (umax || (!o_exrom && !w)));
            romh_n = !((a[15:13] == 3'b101 && !o_exrom && !o_game && !w) || (a[15:13] == 3'b111 && umax));
            io1_n  = a[15:8] != 8'hDE;
            io2_n  = a[15:8] != 8'hDF;
            ign_now = ign(a, w);
            phi2_p = 1'b1;
            @(negedge clk); phi2_p = 1'b0;
            repeat (8) @(negedge clk);
            check_cpu_half;
            phi2_h = 1'b1;
            @(negedge clk); phi2_h = 1'b0;
            repeat (6) @(negedge clk);
            phi2_n = 1'b1;
            @(negedge clk); phi2_n = 1'b0;
            we = 1'b0; roml_n = 1'b1; romh_n = 1'b1; io1_n = 1'b1; io2_n = 1'b1; din = 8'hFF;
            repeat (4) @(negedge clk);
            cmp(G_ACT, "cartridge on", n_act === o_act);
            cmp(G_BANK, "registers", (n_bank & bmask) === (o_bank & bmask));
            cmp(G_LINES, "lines after", n_exrom === o_exrom && n_game === o_game);
            repeat (10) @(negedge clk);
            cycles = cycles + 1;
        end
    endtask

    task start;
        begin
            reset = 1'b1;
            repeat (8) @(negedge clk);
            reset = 1'b0;
            repeat (8) @(negedge clk);
            cmp(G_ACT, "on after reset", n_act === o_act);
            if ($test$plusargs("dbg"))
                $display("start %h: d %h io %h entry %h mode %0d lines %b%b act %b", cart_type, n.d, n.model.io,
                         n.model.entry, n.model.mode, n_exrom, n_game, n_act);
        end
    endtask

    integer i, r, k;
    reg [15:0] a;
    reg        w;
    reg  [7:0] d;
    task stream(input integer count);
        begin
            for (i = 0; i < count; i = i + 1) begin
                if (i % 1500 == 0) start;
                r = $urandom % 100;
                k = $urandom;
                if (r < 28)      a = {8'hDE, (k & 3) != 3 ? {6'd0, k[9:8]} : k[7:0]};
                else if (r < 40) a = {8'hDF, k[3] ? (k[4] ? 8'hFF : 8'h00) : k[7:0]};
                else if (r < 62) a = {3'b100, k[12:0]};
                else if (r < 82) a = {k[13] ? 3'b111 : 3'b101, k[12:0]};
                else             a = {1'b0, k[14:0]};
                w = ($urandom % 100) < 45;
                d = $urandom;
                if (skip(a, w, d)) skipped = skipped + 1;
                else bus(a, w, d);
            end
        end
    endtask

    reg [7:0] types [0:16];
    integer t, count;
    initial begin
        types[0] = 8'h01; types[1] = 8'h41; types[2] = 8'h21; types[3] = 8'hA1; types[4] = 8'h05;
        types[5] = 8'h08; types[6] = 8'h69; types[7] = 8'h28; types[8] = 8'hA8; types[9] = 8'h0A;
        types[10] = 8'h88; types[11] = 8'h48; types[12] = 8'h68; types[13] = 8'h0F; types[14] = 8'h11;
        types[15] = 8'h1B; types[16] = 8'h3B;
        count = 12000;
        if ($value$plusargs("count=%d", count)) ;
        for (run = 0; run < 2; run = run + 1)
            for (t = 0; t < 17; t = t + 1) begin
                cart_type = types[t];
                desc = run ? cart_desc(cart_type) : cart_desc_legacy(cart_type);
                $sformat(tname, "$%h", cart_type);
                tfails = 0;
                stream(count);
                $display("run %s CART_TYPE $%h: %0s", run ? "B" : "A", cart_type, tfails ? "DIFFERS" : "same");
            end
        $display("%0d cycles, %0d accesses skipped, %0d compares not made (listed differences), %0d looks equal to the access",
                 cycles, skipped, ignored, peeks);
        if (fails) $display("FAILED: %0d differences", fails);
        else       $display("PASSED");
        $finish;
    end
endmodule
