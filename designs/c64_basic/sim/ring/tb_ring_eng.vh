// SPDX-License-Identifier: GPL-3.0-or-later
    // ---- the engine, cart_fill, mig_arb (CPU/REU/drive/video idle), the MIG model ----
    wire         ui_rst, calib;
    wire [26:0]  app_addr, e_addr, c_addr;
    wire [2:0]   app_cmd, e_cmd, c_cmd;
    wire         app_en, app_rdy, app_wdf_wren, app_wdf_rdy, app_rd_data_valid;
    wire [127:0] app_wdf_data, app_rd_data, e_wdf_data, c_wdf_data, c_rd_data;
    wire [15:0]  app_wdf_mask, e_wdf_mask, c_wdf_mask;
    wire         e_en, e_rdy, e_wdf_wren, e_rd_valid;
    wire         c_en, c_rdy, c_wdf_wren, c_wdf_rdy, c_rd_valid;
    wire         cart_rbw, ob_v;
    wire [14:0]  ob_addr;
    wire  [7:0]  ob_old;
    trx_engine eng (
        .clk(clk),
        .r_we(p2_we), .r_re(p2_re), .r_addr(p2_addr), .r_wdata(p2_wdata), .r_rdata(p2_rdata),
        .log_on(log_on), .seq_busy(seq_busy), .mk(mk), .mk_snap(mk_snap), .mk_cap(mk_cap),
        .mk_skip(mk_skip), .mk_intg(mk_intg), .reu_busy(reu_busy), .mk_x(mk_x), .mk_line(mk_line),
        .phi2_tick(phi2_tick), .cap_gen(cap_gen),
        .tr_ib(tr_ib), .tr_pc(tr_pc), .tr_op(tr_op), .tr_b1(tr_b1), .tr_b2(tr_b2), .tr_regs(tr_regs),
        .sb_req(seq_sb_req), .sb_addr(seq_sb_addr), .sb_gnt(seq_sb_gnt), .sb_rdata(sb_rdata),
        .wa_we(wa_we), .wa_addr(wa_addr), .wa_new(wa_new), .wa_old(wa_old),
        .wb_we(p_we && !p_rom), .wb_addr(p_addr), .wb_new(p_wdata), .wb_old(ram_rdata),
        .wc_we(wc_we), .wc_addr(wc_addr), .wc_new(wc_new), .wc_old(wc_old),
        .wd_we(cram_we), .wd_addr(cram_addr), .wd_new(cram_wdata), .wd_old(cram_rdata),
        .wk_we(wk_we), .wk_addr(wk_addr), .wk_new(wk_new), .cart_rbw(cart_rbw),
        .eng_tgl(eng_tgl), .gap_tgl(gap_tgl),
        .ui_clk(ui_clk), .ui_rst(ui_rst || !calib),
        .e_addr(e_addr), .e_cmd(e_cmd), .e_en(e_en), .e_rdy(e_rdy), .e_wdf_data(e_wdf_data),
        .e_wdf_mask(e_wdf_mask), .e_wdf_wren(e_wdf_wren), .e_rd_data(c_rd_data),
        .e_rd_data_valid(e_rd_valid), .ob_v(ob_v), .ob_addr(ob_addr), .ob_old(ob_old)
    );
    cart_fill cfill (
        .ui_clk(ui_clk), .ui_rst(ui_rst || !calib), .rom_sel(2'd0),   // Gideon's base: the bench holds the image at 0x3C00000
        .req_tgl(cart_req_tgl), .req_we(cart_req_we), .req_addr(cart_req_addr),
        .req_data(cart_req_data), .req_slot(cart_req_slot), .req_cached(cart_req_cached),
        .ack_tgl(cart_ack_tgl),
        .c_be(cart_fill_be), .c_addr(cart_fill_addr), .c_data(cart_fill_data),
        .app_addr(c_addr), .app_cmd(c_cmd), .app_en(c_en), .app_rdy(c_rdy),
        .app_wdf_data(c_wdf_data), .app_wdf_mask(c_wdf_mask), .app_wdf_wren(c_wdf_wren),
        .app_wdf_rdy(c_wdf_rdy), .app_rd_data(c_rd_data), .app_rd_data_valid(c_rd_valid),
        .rbw(cart_rbw), .ob_v(ob_v), .ob_addr(ob_addr), .ob_old(ob_old),
        .pk_tgl(1'b0), .pk_addr(23'd0), .pk_ack_tgl(), .pk_data()
    );
    wire [2:0]  done_tgl;
    wire [63:0] rdata;
    wire [127:0] rdata_line;
    mig_arb arb (.ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(calib),
        .req_tgl(3'b000), .addr(78'd0), .we(3'b000), .wdata(96'd0), .be(12'd0),
        .done_tgl(done_tgl), .rdata(rdata), .rdata_line(rdata_line),
        .c_addr(c_addr), .c_cmd(c_cmd), .c_en(c_en), .c_rdy(c_rdy), .c_wdf_data(c_wdf_data),
        .c_wdf_mask(c_wdf_mask), .c_wdf_wren(c_wdf_wren), .c_wdf_rdy(c_wdf_rdy),
        .c_rd_data(c_rd_data), .c_rd_data_valid(c_rd_valid),
        .v_addr(27'd0), .v_cmd(3'd0), .v_en(1'b0), .v_rdy(), .v_wdf_data(128'd0), .v_wdf_mask(16'd0),
        .v_wdf_wren(1'b0), .v_wdf_rdy(), .v_rd_data_valid(), .v_urgent(1'b0),
        .e_addr(e_addr), .e_cmd(e_cmd), .e_en(e_en), .e_rdy(e_rdy), .e_wdf_data(e_wdf_data),
        .e_wdf_mask(e_wdf_mask), .e_wdf_wren(e_wdf_wren), .e_rd_data_valid(e_rd_valid),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid));
    mig_ui_model #(.MEM_WORDS(1 << 22)) mig (.ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(calib),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid));

    // the bench's own count of the taps' writes, and of them at each marker's
    // boundary (a write at the marker's clock is the next frame's)
    wire on_ = eng.on;
    wire [2:0] te = eng.tap_en;
    wire [1:0] w0 = (wa_we && on_) + (p_we && !p_rom && on_ && te[0]);
    wire [1:0] w1 = (wc_we && on_ && te[2]) + (cram_we && on_ && te[0] && te[2]) + (wk_we && on_ && te[1]);
    always @(posedge clk) begin
        n_s0 <= n_s0 + w0;
        n_s1 <= n_s1 + w1;
        if (mk && on_) begin
            n_s0_mk <= n_s0;
            n_s1_mk <= n_s1;
            n_mk    <= n_mk + 1;
        end
    end
