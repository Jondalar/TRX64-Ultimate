// SPDX-License-Identifier: GPL-3.0-or-later
// c64_basic: the icepi-zero-c64 C64 core (6502n, vicii-kawari, reDIP SID and
// CIA) on the C64 Ultimate, with HDMI out and nothing else.
//
// c64_top.v and everything below it is vendored in cores/icepi; c64_core.v
// wraps it. This file gives it what the icepi board gave it:
//   - clock: dot4x, 31.53 MHz (PAL); the app's System Mode switches it live to
//     NTSC, PAL-60 or NTSC-50 (T40, mode_ctl.v).
//   - memory: block RAM (c64_mem, inside c64_core) in place of the SDRAM port.
// and replaces kawari's 576p output with the HDMI modes of U16 (576p, 720p,
// 1080p at 50 Hz; 480p, 720p, 1080p, 800 x 600 at 60 Hz; the family follows
// the System Mode; power-up 1080p, the app asks for its mode at 0x4F00 of the
// overlay window, mode_req.v), every one locked to the VIC frame:
//   - hdmi_timing.v runs the mode's frame, one VIC frame long; vic_scaler.v
//     takes the VIC's native pixels and scales them onto it at the app's scale
//     (Integer, Fit, Fill, 0x4F01).
//   - The TMDS clock (122.7-149.1 MHz, tools/hdmi_modes.py) comes from the
//     board's CDCEL824 (cdce_mode.v, N/M per row) fed with 37.89 MHz = 125/104
//     of dot4x from a second MMCM; it is also the GTP reference (two TMDS bits
//     per pix_clk on each lane), so picture and VIC stay locked. Rows with a
//     pixel clock below 25 MHz x gear repeat each pixel (gear 2, 3 or 5:
//     hdmi_timing's ce, tmds_rep.v).
// Keyboard: the C64U's own, scanned over I2C (c64u_keyboard); joysticks and
// RESTORE straight from their pins. Tied off: tape. The expansion port and the
// SID sockets: docs/specs/T16-cartridge-port.md §13. The serial bus
// joins the C64, the app's drive A and the board's IEC port (iec_bus.v).
//
// Next to the C64: Gideon's RISC-V (rvlite, 100 MHz) with the stubs the U64-II
// "ultimate" app needs to boot (ultimate_sys.vhd, docs/ultimate-app-hw-spec.md
// step A). It sees the bottom 64 MB of the DDR2; its boot ROM takes the app
// over JTAG, or from flash when the bitstream in flash is this one (T15); its
// console UART goes to the UART pins and into a ring that
// tools/trxdbg.py reads (`console`). The app's menu overlay (overlay.v) is
// drawn over the scaled picture, the pixel layer (pix_layer.v) over that, and
// the freeze menu's C64 text (c64_text.v) on top.
//
// LEDs: board LED off. The case LEDs show what the app
// selects (case_leds.v); until it does, case LED 1 on = C64 out of reset,
// case LED 2 on = transmitters running and the picture locked to the VIC.
// The keyboard board's bytes and the SHIFT LOCK LED are bling_io.v; the case LED strip and keyboard lighting are out (OPT-bling).
module top #(
    // capability bits 29:28 (itu.h CAPAB_FPGA_TYPE): 2 = XC7A50T (C64 Ultimate),
    // 3 = XC7A100T (Ultimate 64 Elite II). The app and the boot ROM take the
    // flash layout from it; build.tcl sets it from the part (T53).
    parameter [1:0] FPGA_TYPE = 2'd2
) (
    input  wire       RMII_REFCLK,          // 50 MHz from the Ethernet PHY
    input  wire       HDMI_REFCLK_P, HDMI_REFCLK_N,
    output wire [2:0] HDMI_DATA_P, HDMI_DATA_N,
    output wire       HDMI_CLK_P, HDMI_CLK_N,
    output wire       HDMI_CLK_SYNTH,       // reference into the HDMI synthesizer
    output wire       CLOCK_C64_SYNTH,      // reference into the C64 synthesizer
    inout  wire       I2C18_SCL, I2C18_SDA,
    inout  wire       HDMI_SCL, HDMI_SDA,
    inout  wire       I2C33_SCL, I2C33_SDA,
    output wire       HDMI_DDC_EN,
    input  wire       HDMI_HPD,
    output wire       LED_BOARDn, LED_CASE1n, LED_CASE2n,
    output wire       LED_CASE3n,           // third case LED line: held off (active high)
    // expansion port and SID sockets (T16 §3.1, §13.1)
    inout  wire [15:0] SLOT_ADDR,           // A0-A11 behind the address transceiver, A12-A15 direct
    inout  wire [7:0] SLOT_DATA,
    output wire       SLOT_ADDRDIR, SLOT_ADDROEn, SLOT_DATADIR, SLOT_DATAOEn,
    output wire       SLOT_ROMLn, SLOT_ROMHn, SLOT_IO1n, SLOT_IO2n,
    output wire       SLOT_PHI2, SLOT_DOTCLK, SLOT_BA,
    inout  wire       SLOT_RWn,
    input  wire       SLOT_GAMEn, SLOT_EXROMn, SLOT_DMAn, SLOT_IRQn, SLOT_NMIn,
    input  wire       SLOT_RST_I,               // 1 = the port's RESET is low
    output wire       SLOT_RST_O,               // 1 = pull the port's RESET low
    input  wire       CIA_RSTn,                 // not driven (§13.6)
    output wire       SLOT_SID1n, SLOT_SID2n, SLOT_SID_ADDR, SLOT_PWR,
    input  wire [4:0] KBJOY_1_JOY, KBJOY_2_JOY, // up, down, left, right, fire; low = pressed
    input  wire       KBJOY_RESTORE,            // low = pressed
    input  wire       KBJOY_INT,                // keyboard controller interrupt (1.8 V)
    input  wire       BLING_KEYB,               // serial line from the keyboard (Bling) board
    output wire       CIA_BUFFER_EN, CIA_PWR_EN, // C64-side port buffers and their supply
    output wire       AUDIO_MCLK, AUDIO_BCLK, AUDIO_LRCLK, AUDIO_SDI,  // I2S to the codec
    input  wire       AUDIO_SDO,                                        // I2S from the codec's ADC (T37: the SID sockets)
    output wire       SPEAKER_DATA,         // I2S data to the speaker amplifier (same BCLK/LRCK)
    output wire       UART_TXD,             // RISC-V console, 115200 8N1
    input  wire       UART_RXD,
    // Ethernet PHY (KSZ8081, RMII; its 50 MHz reference is RMII_REFCLK)
    input  wire       RMII_CRS_DV,
    input  wire [1:0] RMII_RX_DATA,
    output wire       RMII_TX_EN,
    output wire [1:0] RMII_TX_DATA,
    output wire       MDIO_CLK,
    output wire       ETH_LED_YELn,         // T62 G19: low = on (Ethernet carrier seen in the last 2.7 s)
    inout  wire       MDIO_DATA,
    // ESP32 (control module) UART, for the app's WiFi/power/rocker link
    output wire       WIFI_TXD, WIFI_RTS,
    output wire       SD_SSn, SD_CLK, SD_MOSI,  // SD card, SPI mode
    input  wire       SD_MISO, SD_CARDDETn,
    output wire       FLASH_CSn, FLASH_MOSI,    // configuration flash; SCK is CCLK
    input  wire       FLASH_MISO,
    input  wire       WIFI_RXD, WIFI_CTS,
    input  wire       ULPI_CLOCK, ULPI_NXT, ULPI_DIR,   // USB PHY (usb_phy.v)
    output wire       ULPI_STP, ULPI_RESET, ULPI_REFCLK,
    inout  wire [7:0] ULPI_DATA,
    output wire       HUB_RESETn, HUB_CLOCK,
    // serial bus (IEC) port: _O = 1 pulls the line low, _I = 1 while it is low
    input  wire       IEC_ATN_I, IEC_CLOCK_I, IEC_DATA_I, IEC_RESET_I,   // IEC_RESET_I: T62 G19, 1 while the bus RESET line is low
    output wire       IEC_ATN_O, IEC_CLOCK_O, IEC_DATA_O, IEC_RESET_O, IEC_SRQ_O,
    // DDR2 (constrained in pins.xdc: Gideon's HSTL standards, bank 35)
    inout  wire [15:0] ddr2_dq,
    inout  wire [1:0]  ddr2_dqs_n, ddr2_dqs_p,
    output wire [12:0] ddr2_addr,
    output wire        SDRAM_A13,
    output wire [2:0]  ddr2_ba,
    output wire        ddr2_ras_n, ddr2_cas_n, ddr2_we_n,
    output wire [0:0]  ddr2_ck_p, ddr2_ck_n, ddr2_cke, ddr2_cs_n, ddr2_odt,
    output wire [1:0]  ddr2_dm,
    output wire        BLING_LED,           // keyboard LEDs, WS2812 chain
    output wire        LED_DATA, LED_CLOCK  // case LED strip, WS2812 or APA102
);
`include "hdmi_modes.vh"                // U16: the HDMI rows (hm_rate, hm_rate_alt, hm_aspect here)
    // The expansion port (T16 §13): c64_core's slot_port.v drives it; idle
    // (transceivers off with DIR towards the port, strobes high, PHI2/DOTCLK
    // low, BA high, A12-A15 and R/W released) unless the app's
    // C64_BUS_EXTERNAL selects it or a SID socket is enabled (T37: SLOT_SID1n/2n and
    // SLOT_SID_ADDR are slot_port's). Port power on as in Gideon's design.
    wire [15:0] slot_a;
    wire  [7:0] slot_d_o;
    wire        slot_ahi_t, slot_rw, slot_d_t, slot_rst_o;
    wire  [3:0] bus_int_cpu, bus_ext_cpu;
    wire        slot_wr_en;
    wire [31:0] slot_status, slot_cnt0, slot_cnt1, slot_last;
    assign SLOT_ADDR[11:0]  = slot_a[11:0];
    assign SLOT_ADDR[15:12] = slot_ahi_t ? 4'bzzzz : slot_a[15:12];
    assign SLOT_RWn         = slot_ahi_t ? 1'bz : slot_rw;
    assign SLOT_DATA        = slot_d_t ? 8'bzzzzzzzz : slot_d_o;
    assign SLOT_RST_O    = slot_rst_o;
    wire        slot_sid1, slot_sid2, slot_sid_a;
    assign SLOT_SID1n    = slot_sid1;
    assign SLOT_SID2n    = slot_sid2;
    assign SLOT_SID_ADDR = slot_sid_a;
    assign SLOT_PWR      = 1'b1;
    // Gideon's design holds both high (JTAG SAMPLE, 2026-09-30); without them
    // the joystick and keyboard lines never reach the FPGA.
    assign CIA_BUFFER_EN = 1'b1;
    assign CIA_PWR_EN    = 1'b1;
    assign SDRAM_A13    = 1'b0;             // 1 Gbit part: A13 unused

    // ---- 50 MHz and power-on reset -------------------------------------------
    wire clk50_ibuf, clk50;
    IBUF  ib50 (.I(RMII_REFCLK), .O(clk50_ibuf));
    BUFG  bg50 (.I(clk50_ibuf), .O(clk50));
    reg [15:0] por = 0;
    wire por_rst = ~por[15];
    always @(posedge clk50) if (!por[15]) por <= por + 1;

    // ---- sys = dot4x, sid2x = 2 x dot4x -------------------------------------------
    // PAL from power-on: 50 x 56.75 / 3 / 30 = 31.5278 MHz, with an integer
    // divider, so that a second output can run at exactly twice that,
    // edge-aligned: the T25.1 SID engine's clock (cores/trx_sid). The other
    // System Modes (T40) reprogram it through its DRP (mode_ctl.v). While that
    // runs, sys and sid2x are stopped by their BUFGCEs (the MMCM's outputs are
    // not clean until it has locked); their enable is taken over on an ungated
    // copy of the output (sys_free).
    wire fb1, sys_mmcm, sys_clk, sys_locked, sid2x_mmcm, sid2x_clk, sys_free;
    wire        mmcm_drst, mmcm_den, mmcm_dwe, mmcm_drdy;
    wire  [6:0] mmcm_daddr;
    wire [15:0] mmcm_di, mmcm_do;
    MMCME2_ADV #(
        .CLKIN1_PERIOD(20.0), .DIVCLK_DIVIDE(3), .CLKFBOUT_MULT_F(56.75),   // VCO 945.833 MHz
        .CLKOUT0_DIVIDE_F(30.0),                                           // 31.5278 MHz
        .CLKOUT1_DIVIDE(15)                                                // 63.0556 MHz
    ) mmcm_sys (
        .CLKIN1(clk50_ibuf), .CLKIN2(1'b0), .CLKINSEL(1'b1), .CLKFBIN(fb1), .CLKFBOUT(fb1),
        .CLKOUT0(sys_mmcm), .LOCKED(sys_locked), .RST(por_rst | mmcm_drst), .PWRDWN(1'b0),
        .CLKOUT0B(), .CLKOUT1(sid2x_mmcm), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(),
        .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(), .CLKFBOUTB(),
        .DCLK(clk50), .DADDR(mmcm_daddr), .DI(mmcm_di), .DO(mmcm_do), .DEN(mmcm_den),
        .DWE(mmcm_dwe), .DRDY(mmcm_drdy),
        .PSCLK(1'b0), .PSEN(1'b0), .PSINCDEC(1'b0), .PSDONE(),
        .CLKINSTOPPED(), .CLKFBSTOPPED()
    );
    wire mc_sys_en;                     // mode_ctl (clk50)
    BUFG bg_sysfree (.I(sys_mmcm), .O(sys_free));
    (* ASYNC_REG = "TRUE" *) reg [2:0] sys_en_s = 3'b111;
    always @(posedge sys_free) sys_en_s <= {sys_en_s[1:0], mc_sys_en};
    BUFGCE bg_sys (.I(sys_mmcm), .CE(sys_en_s[2]), .O(sys_clk));
    BUFGCE bg_sid2x (.I(sid2x_mmcm), .CE(sys_en_s[2]), .O(sid2x_clk));
    // the C64's own copy, stopped by the machine pause (c64_core.v)
    wire c64_gclk, c64_ce;
    BUFGCE bg_c64 (.I(sys_mmcm), .CE(c64_ce & sys_en_s[2]), .O(c64_gclk));

    // synthesizer reference: 125/104 of dot4x; the HDMI synthesizer makes x4.
    // Fed from the ungated copy: during a mode switch it must lock again
    // while sys itself is still stopped (mode_ctl.v waits for both locks).
    wire fb2, ref_mmcm, ref_clk, ref_locked;
    MMCME2_BASE #(
        .CLKIN1_PERIOD(31.718), .DIVCLK_DIVIDE(1), .CLKFBOUT_MULT_F(31.25), // VCO 985.24 MHz
        .CLKOUT0_DIVIDE_F(26.0)                                            // 37.894 MHz
    ) mmcm_ref (
        .CLKIN1(sys_free), .CLKFBIN(fb2), .CLKFBOUT(fb2), .CLKOUT0(ref_mmcm),
        .LOCKED(ref_locked), .RST(~sys_locked), .PWRDWN(1'b0),
        .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(),
        .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(), .CLKFBOUTB()
    );
    BUFG bg_ref (.I(ref_mmcm), .O(ref_clk));
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) fwd_hdmi (.Q(HDMI_CLK_SYNTH), .C(ref_clk), .CE(1'b1),
        .D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0));
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) fwd_c64 (.Q(CLOCK_C64_SYNTH), .C(ref_clk), .CE(1'b1),
        .D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0));

    wire clocks_ok = sys_locked & ref_locked;
    wire sys_rst, mc_reconf;
    // a mode switch's lock loss (mc_reconf) is no reset of the sys domain
    rst_sync rs_sys (.clk(sys_clk), .arst(~clocks_ok & ~mc_reconf), .rst(sys_rst));

    // ---- DDR2 ------------------------------------------------------------------
    // 200 MHz for ddr2_lean's PLL and IDELAYCTRL, the audio MCLK (1000 / 81.375 =
    // 12.2887 MHz, 256 x 48.003 kHz) and the RISC-V's 100 MHz: they share one
    // MMCM (the 50T has five; mmcm_sys, mmcm_ref, mmcm_pix and this one are ours,
    // the fifth was the MIG's and is free since OPT-ddr2 §9).
    wire fb3, clk200_mmcm, clk200, clk200_locked, mclk_mmcm, mclk, cpu_mmcm, cpu_clk;
    MMCME2_BASE #(
        .CLKIN1_PERIOD(20.0), .DIVCLK_DIVIDE(1), .CLKFBOUT_MULT_F(20.0),   // VCO 1000 MHz
        .CLKOUT0_DIVIDE_F(81.375),                                        // 12.2887 MHz
        .CLKOUT1_DIVIDE(5),                                               // 200 MHz
        .CLKOUT2_DIVIDE(10)                                               // 100 MHz
    ) mmcm_ddr (
        .CLKIN1(clk50_ibuf), .CLKFBIN(fb3), .CLKFBOUT(fb3), .CLKOUT0(mclk_mmcm),
        .LOCKED(clk200_locked), .RST(por_rst), .PWRDWN(1'b0),
        .CLKOUT0B(), .CLKOUT1(clk200_mmcm), .CLKOUT1B(), .CLKOUT2(cpu_mmcm), .CLKOUT2B(), .CLKOUT3(),
        .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(), .CLKFBOUTB()
    );
    BUFG bg200 (.I(clk200_mmcm), .O(clk200));
    BUFG bgmclk (.I(mclk_mmcm), .O(mclk));
    BUFG bgcpu (.I(cpu_mmcm), .O(cpu_clk));
    rst_sync rs_mclk (.clk(mclk), .arst(~clk200_locked), .rst(mclk_rst));

    wire         ui_clk, ui_rst, ddr_calib;
    wire [26:0]  app_addr;
    wire [2:0]   app_cmd;
    wire         app_en, app_rdy, app_wdf_wren, app_wdf_rdy, app_rd_data_valid;
    wire [127:0] app_wdf_data, app_rd_data;
    wire [15:0]  app_wdf_mask;
    // cart_fill's commands for mig_arb (c_)
    wire [26:0]  c_addr;
    wire [2:0]   c_cmd;
    wire         c_en, c_rdy, c_wdf_wren, c_wdf_rdy, c_rd_valid;
    wire [127:0] c_wdf_data, c_rd_data;
    wire [15:0]  c_wdf_mask;
    // the video client's (pix_layer, v_); its read data comes on c_rd_data
    wire [26:0]  v_addr;
    wire [2:0]   v_cmd;
    wire         v_en, v_rdy, v_wdf_wren, v_rd_valid, v_urgent;
    wire [127:0] v_wdf_data;
    wire [15:0]  v_wdf_mask;
    // the engine's (trx_engine, e_; frame ring and mover); read data on c_rd_data
    wire [26:0]  e_addr;
    wire [2:0]   e_cmd;
    wire         e_en, e_rdy, e_wdf_wren, e_rd_valid;
    wire [127:0] e_wdf_data;
    wire [15:0]  e_wdf_mask;
    // cart_fill reads a cartridge RAM byte before writing it, for the log
    wire         cart_rbw, ob_v;
    wire [14:0]  ob_addr;
    wire [7:0]   ob_old;
    // cartridge: ROM image (from the app: DDR2 0x3C00000, the REU block 0x1000000 or the
    // built-in slot 0xF00000, trx_io ROM_SEL, cart_fill) and RAM (0xEF0000) in
    // DDR2; cart_fill serves the core's cache with them
    wire         cart_req_tgl, cart_req_we, cart_req_cached, cart_ack_tgl;
    wire [23:0]  cart_req_addr;
    wire [7:0]   cart_req_data, cart_fill_be;
    wire [5:0]   cart_req_slot;
    wire [8:0]   cart_fill_addr;
    wire [63:0]  cart_fill_data;
    // our DDR2 controller (ddr2_lean + the calibration sequence cal_seq, ddr2_ctrl.v):
    // MIG's app_* interface, ui_clk 75 MHz from the PLL inside ddr2_lean. ddr_calib rises
    // when the calibration ended ok and never when it found no window; ddr_cal is the
    // result (ddr2_ctrl.v: CAL0 and CAL1, done / ok are bits of CAL0) for trx_io page 0
    // and the JTAG registers 0x100050-0x100052.
    wire [63:0]  ddr_cal;
    ddr2_ctrl ddr (
        .ddr2_dq(ddr2_dq), .ddr2_dqs_n(ddr2_dqs_n), .ddr2_dqs_p(ddr2_dqs_p),
        .ddr2_addr(ddr2_addr), .ddr2_ba(ddr2_ba), .ddr2_ras_n(ddr2_ras_n),
        .ddr2_cas_n(ddr2_cas_n), .ddr2_we_n(ddr2_we_n), .ddr2_ck_p(ddr2_ck_p),
        .ddr2_ck_n(ddr2_ck_n), .ddr2_cke(ddr2_cke), .ddr2_cs_n(ddr2_cs_n),
        .ddr2_dm(ddr2_dm), .ddr2_odt(ddr2_odt),
        .sys_clk_i(clk200), .sys_rst(~clk200_locked),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask),
        .app_wdf_wren(app_wdf_wren),
        .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid),
        .app_rdy(app_rdy), .app_wdf_rdy(app_wdf_rdy),
        .ui_clk(ui_clk), .ui_clk_sync_rst(ui_rst), .init_calib_complete(ddr_calib),
        .cal_done(), .cal_ok(), .cal_res(ddr_cal)
    );

    // DDR2 map: the RISC-V app's 64 MB at the bottom (its own addresses), with the
    // REU inside it at 16 MB (word 0x0800000, Gideon's REU_MEMORY_BASE), all of its 16 MB (T60),
    // which is also where the app puts a cartridge's ROM image (cart_fill ROM_SEL 1: the
    // expansion-port block, REU or cartridge, never both). mig_arb serves cart_fill first,
    // then drive A, then in turn the RISC-V (cpu_mem_bridge) and one port
    // shared by the REU (first) and the JTAG debug window (ddr2_port).
    localparam [25:0] REU_BASE = 26'h0800000;
    reg         ddr_req = 1'b0;
    reg  [25:0] ddr_addr;
    reg         ddr_we;
    reg  [31:0] ddr_wdata;
    reg  [3:0]  ddr_be;
    reg         ddr_for_reu;
    // trx_sampler (in c64_core, T25.6): the app's byte window (trx_io page 8) and its DDR2 reads
    wire        smp_en;                             // C64_SAMPLER_ENABLE
    wire        smp_a_tgl, smp_a_we, smp_a_rs_tgl, smp_f_tgl, smp_f_done_tgl;
    wire  [7:0] smp_a_addr, smp_a_wdata, smp_a_rdata;
    wire [26:0] smp_f_addr;
    reg  [31:0] smp_f_data = 32'd0;
    reg         ddr_for_smp = 1'b0;     // the sampler's read (T25.6): after the REU, before the JTAG window
    reg         smp_pending = 1'b0, smp_done_r = 1'b0;
    (* ASYNC_REG = "TRUE" *) reg [2:0] smp_rq_s = 3'b000;
    wire        smp_rq_ev = smp_rq_s[2] != smp_rq_s[1];
    assign      smp_f_done_tgl = smp_done_r;
    reg         ddr_wide;               // JTAG 32-bit access
    wire        ddr_ack;
    wire [31:0] ddr_rdata;
    wire [15:0] ddr_rdata16 = ddr_addr[0] ? ddr_rdata[31:16] : ddr_rdata[15:0];

    wire [23:0] reu_cmd_addr;
    wire        reu_cmd_we, reu_cmd_valid;
    wire [15:0] reu_wdata;
    wire [1:0]  reu_wdata_we;
    reg  [15:0] reu_rdata;
    reg         reu_rdata_valid;
    wire        reu_cmd_ready = !ddr_req;       // taken the clk it is offered
    wire [25:0] reu_map_addr;
    wire [3:0]  reu_map_be;
    reu_ddr_map #(.BASE(REU_BASE)) reu_map (.cmd_addr(reu_cmd_addr), .wdata_we(reu_wdata_we),
        .ddr_addr(reu_map_addr), .ddr_be(reu_map_be));
    reg         jtag_ddr_pending = 1'b0;
    reg         jtag_ddr_we;
    reg         jtag_ddr_wide;
    reg  [25:0] jtag_ddr_addr;
    reg  [31:0] jtag_ddr_wdata;
    reg         jtag_ddr_done;
    reg  [31:0] jtag_ddr_rdata;

    always @(posedge sys_clk) begin
        reu_rdata_valid <= 1'b0;
        jtag_ddr_done   <= 1'b0;
        smp_rq_s <= {smp_rq_s[1:0], smp_f_tgl};
        if (smp_rq_ev) smp_pending <= 1'b1;
        if (!ddr_req) begin
            // 16-bit clients put their word into the half addr[0] picks
            if (reu_cmd_valid) begin
                ddr_req <= 1'b1; ddr_for_reu <= 1'b1; ddr_for_smp <= 1'b0; ddr_wide <= 1'b0;
                ddr_addr  <= reu_map_addr;
                ddr_we    <= reu_cmd_we;
                ddr_wdata <= {reu_wdata, reu_wdata};
                ddr_be    <= reu_map_be;
            end else if (smp_pending) begin
                // the sampler's 32-bit read: byte address -> 16-bit word address
                ddr_req <= 1'b1; ddr_for_reu <= 1'b0; ddr_for_smp <= 1'b1; ddr_wide <= 1'b1;
                ddr_addr  <= smp_f_addr[26:1];
                ddr_we    <= 1'b0;
                ddr_be    <= 4'b1111;
                smp_pending <= 1'b0;
            end else if (jtag_ddr_pending) begin
                ddr_req <= 1'b1; ddr_for_reu <= 1'b0; ddr_for_smp <= 1'b0; ddr_wide <= jtag_ddr_wide;
                ddr_addr  <= jtag_ddr_addr;
                ddr_we    <= jtag_ddr_we;
                ddr_wdata <= jtag_ddr_wide ? jtag_ddr_wdata : {2{jtag_ddr_wdata[15:0]}};
                ddr_be    <= jtag_ddr_wide ? 4'b1111 : jtag_ddr_addr[0] ? 4'b1100 : 4'b0011;
            end
        end else if (ddr_ack) begin
            ddr_req <= 1'b0;
            if (ddr_for_reu) begin
                if (!ddr_we) begin reu_rdata <= ddr_rdata16; reu_rdata_valid <= 1'b1; end
            end else if (ddr_for_smp) begin
                smp_f_data  <= ddr_rdata;
                smp_done_r  <= ~smp_done_r;
            end else begin
                jtag_ddr_done  <= 1'b1;
                jtag_ddr_rdata <= ddr_wide ? ddr_rdata : {16'd0, ddr_rdata16};
            end
        end
    end

    wire        port_tgl, port_done_tgl;
    wire [31:0] port_done_rdata;
    ddr2_port ddr_port (
        .clk(sys_clk), .req(ddr_req), .addr(ddr_addr), .we(ddr_we), .wdata(ddr_wdata),
        .be(ddr_be), .ack(ddr_ack), .rdata(ddr_rdata),
        .req_tgl(port_tgl), .done_tgl(port_done_tgl), .done_rdata(port_done_rdata)
    );

    // ---- the RISC-V (Ultimate app) ----------------------------------------------
    wire        cpu_rst;
    reg  [4:0]  machine_ctl = 5'b00000;     // JTAG register 9 (below), sys_clk
    rst_sync rs_cpu (.clk(cpu_clk), .arst(~clk200_locked | machine_ctl[2]), .rst(cpu_rst));
    (* ASYNC_REG = "TRUE" *) reg [1:0] calib_cpu = 2'b00;
    always @(posedge cpu_clk) calib_cpu <= {calib_cpu[0], ddr_calib};

    wire        m_request, m_rwn, m_rack;
    wire [7:0]  m_tag, m_rack_tag, m_dack_tag;
    wire [25:0] m_address;
    wire [3:0]  m_byte_en;
    wire [31:0] m_wdata, m_rdata;
    wire        con_valid;
    wire [7:0]  con_data;
    wire [31:0] con_rdata, con_count;       // con_ring, after the debug port
    wire        mdio_in, mdio_out, eth_rst;
    // ports of ultimate_sys: Ethernet byte streams, C64 control, UDP stream
    wire [7:0] erx_data, mac_tx_data, st_tx_data;
    wire       erx_sof, erx_eof, erx_valid;
    wire       mac_tx_eof, mac_tx_valid, st_tx_eof, st_tx_valid, st_req;
    wire       mac_tx_ready;
    wire       app_mdc;
    reg [31:0] mdio_ovr = 32'd0;            // sys_clk; static levels for the pins
    wire [15:0] st_frames;
    // video and audio stream senders, merged by stream_mux (T13)
    wire [7:0]  vs_data, as_data;
    wire        vs_req, vs_grant, vs_valid, vs_eof, vs_ready;
    wire        as_req, as_grant, as_valid, as_eof, as_ready;
    wire [15:0] as_pkts;
    // the app's I2C master (T14): register port from ultimate_sys, bus side below
    wire        ai2c_we, ai2c_re, ai2c_own, ai2c_scan, kb_pause, kb_parked;
    wire        mc_i2c_hold, ai2c_parked, mc_kb_pause;   // T40 mode switch (mode_ctl)
    wire        mc_halt;
    wire  [1:0] vmode;                      // System Mode in use (mode_ctl, clk50)
    wire        v60 = (vmode == 2'd1) || (vmode == 2'd2);   // 65 x 263: NTSC, PAL-60
    wire  [7:0] video_fmt;                  // C64_VIDEOFORMAT (ultimate_sys, cpu_clk)
    wire  [7:0] turbo_en, turbo_prefer;     // T29: TURBOREGS_EN, SPEED_PREFER (ultimate_sys, cpu_clk)
    wire        turbo_upd;                  // toggles with SPEED_UPDATE
    wire [31:0] turbo_stat, turbo_w1, turbo_w2;
    wire        turbo_ran_clr, turbo_on;
    wire [31:0] mc_status, mc_status2;
    wire [3:0]  ai2c_addr;
    wire [7:0]  ai2c_wdata, ai2c_rdata;
    wire [2:0]  ai2c_scl_lo, ai2c_sda_lo;
    wire [7:0]  stream_ena, udp_addr, udp_wdata;
    wire        udp_we;
    wire        app_c64_reset, app_c64_pause;
    // T38: the app's keyboard/joystick state (cpu_clk), merged in app_input below
    wire [63:0] app_keys;
    wire  [4:0] app_joy1, app_joy2;
    wire        app_reset, app_restore, app_freeze;
    wire        core_reset_req;             // the C64's chips in reset (T39)
    wire        c64_halted;                 // machine pause (c64_core.v), sys_clk
    wire        speaker_en;
    wire  [3:0] speaker_vol;            // U2PIO_SPEAKER_EN volume, unused (the mixer sets it)
    wire  [7:0] mix_l, mix_r, mix_spk;
    // the app's UltiSID registers, filter curve writes, envelope levels (T25.1)
    wire [15:0] sid_base, sid_mask;
    wire [15:0] sock_base, sock_mask;        // T37: the SID sockets (ultimate_sys, cpu_clk)
    wire  [1:0] sock_en;
    wire  [2:0] stereo_sel;
    wire [31:0] sock_gains;
    wire  [2:0] sid_split;
    wire  [1:0] sid_waves, sid_res;
    wire  [3:0] sid_digi;
    wire [49:0] sid_rs8;                 // trx_io page 0 0x040: the reSID8 filter control (T25.5)
    wire        sid_resid8;              // the engine is the 8-SID reDIP (CAPS3 b0)
    wire        sidcv_we;
    wire [11:0] sidcv_addr;
    wire  [7:0] sidcv_wdata, sid_lv_data;
    wire  [4:0] sid_lv_addr;
    wire [31:0] sid_gains, sid_gains2;
    wire [7:0]  app_cart_type;
    wire        app_reu_en;
    wire [2:0]  app_reu_size;
    wire        app_cart_kill_tgl, app_cart_force_tgl, cart_active;
    wire        app_ultimax;
    wire        dma_req_tgl, dma_we, dma_bus, dma_rom;
    wire        dbg_stop;                   // the debugger's stop (trx_dbg.v, T12 §14.8): unused by ultimate_sys since G06 (the app window's D page is a bus cycle stopped or running)
    wire [15:0] dma_addr;
    wire [7:0]  dma_wdata;
    wire       dma_done_tgl;
    wire [7:0] dma_rdata_q;
    // SD card detect into the CPU clock; the flash clock leaves through the
    // configuration clock pin (the first three edges after startup are lost,
    // the app's flash probe begins with a dummy byte)
    // the key matrix for the menu's scan (bits change slowly, as for the C64)
    (* ASYNC_REG = "TRUE" *) reg [63:0] keys_cpu_s1, keys_cpu_s2;
    (* ASYNC_REG = "TRUE" *) reg [1:0] sd_detectn_s = 2'b11;
    always @(posedge cpu_clk) sd_detectn_s <= {sd_detectn_s[0], SD_CARDDETn};
    wire flash_sck;
    STARTUPE2 #(.PROG_USR("FALSE")) startup (
        .CFGCLK(), .CFGMCLK(), .EOS(), .PREQ(),
        .CLK(1'b0), .GSR(1'b0), .GTS(1'b0), .KEYCLEARB(1'b1), .PACK(1'b0),
        .USRCCLKO(flash_sck), .USRCCLKTS(1'b0), .USRDONEO(1'b1), .USRDONETS(1'b1));
    // the build stamp (pins.xdc USR_ACCESS TIMESTAMP) for the boot ROM's
    // identity check (trx_io BUILD, T15 §4.2); static once configured
    wire [31:0] usr_access;
    USR_ACCESSE2 usr (.CFGCLK(), .DATA(usr_access), .DATAVALID());

    wire        ovl_we;
    wire        pal_we;                 // T59: C64_PALETTE writes (ultimate_sys, cpu_clk)
    wire  [5:0] pal_addr;
    wire  [7:0] pal_wdata;
    wire [14:0] ovl_addr;
    wire [7:0]  ovl_wdata, ovl_rdata;
    wire        ovl_own;                // the menu has the keyboard (cpu_clk)
    (* ASYNC_REG = "TRUE" *) reg [1:0] ovl_own_s = 2'b00;
    always @(posedge sys_clk) ovl_own_s <= {ovl_own_s[0], ovl_own};
    // LEDs and the keyboard board's bytes (bling_io.v), case LED selector
    wire        bl_we, bl_irq, caseled_set, busy_any, busy_usb;
    wire [11:0] bl_addr;
    wire [7:0]  bl_wdata, bl_rdata, caseled_sel;
    // our IO block 0x101A0000 and the EEPROM window (trx_io.v); the cartridge's
    // flash responder and GMod2 EEPROM port into cart_slot
    wire        cw_we, cw_re, cw_ack, cw_irq, cart_ee_we;
    // the UCI (uci_unit.vhd in ultimate_sys; T28): its slot side on the C64
    // bus (c64_core.v), sys_clk; uci_freeze holds the CPU (a C64 program's
    // request through the control register, as Gideon's dma_n)
    wire [15:0] uci_bus_addr, uci_ev_addr;
    wire  [7:0] uci_ev_data, uci_dout;
    wire        uci_ev, uci_ev_we, uci_ev_io, uci_oe, uci_c64_irq, uci_freeze;
    // its pages in the C64's clock (trx_sys.v, below): byte handshake, the
    // machine's stop and capture events for its interrupt causes
    wire        trx_rq_tgl, trx_rq_we, trx_rs_tgl, trx_j_rvalid;
    wire [15:0] trx_rq_addr;
    wire  [7:0] trx_rq_wdata, trx_rs_rdata;
    wire [31:0] trx_j_rdata;
    wire        cap_tgl, eng_tgl, gap_tgl;
    reg         stop_tgl = 1'b0;
    wire [16:0] cw_addr;
    wire [7:0]  cw_wdata, cw_rdata, cart_ee_wdata, cart_ee_rdata;
    wire [10:0] cart_ee_addr;
    wire        cart_fl_enable, cart_evt_tgl, cart_fl_ack_tgl, cart_inval_tgl, cart_ee_tgl;
    wire [15:0] cart_fl_magic1, cart_fl_magic2, cart_fl_mask;
    wire [23:0] cart_fl_id;
    wire [343:0] cart_desc;                     // the cartridge's descriptor (trx_io page 4)
    wire [33:0] cart_evt_word;
    wire        px_we, px_re, px_ev_vsync, px_ev_flip;  // trx_io page 6: pix_layer, c64_text
    wire [7:0]  px_rdata, pxl_rdata, tx_rdata;
    assign px_rdata = pxl_rdata | tx_rdata;
    wire        ulpi_clk, ulpi_nxt, ulpi_dir, ulpi_stp, ulpi_data_t, ulpi_reset_req, hub_reset_n;
    wire [7:0]  ulpi_data_i, ulpi_data_o;
    wire [31:0] usb_khz;
    wire [15:0] usb_dir_edges;
    reg  [1:0]  usb_ctl = 2'b00;            // JTAG register 0x10 (sys_clk)
    usb_phy usb (
        .clk50(clk50), .pll_rst(por_rst), .cpu_clk(cpu_clk), .cpu_rst(cpu_rst),
        .ULPI_CLOCK(ULPI_CLOCK), .ULPI_NXT(ULPI_NXT), .ULPI_DIR(ULPI_DIR), .ULPI_STP(ULPI_STP),
        .ULPI_DATA(ULPI_DATA), .ULPI_RESET(ULPI_RESET), .ULPI_REFCLK(ULPI_REFCLK),
        .HUB_RESETn(HUB_RESETn), .HUB_CLOCK(HUB_CLOCK),
        .ulpi_clk(ulpi_clk), .ulpi_nxt(ulpi_nxt), .ulpi_dir(ulpi_dir), .ulpi_data_i(ulpi_data_i),
        .ulpi_stp(ulpi_stp), .ulpi_data_o(ulpi_data_o), .ulpi_data_t(ulpi_data_t),
        .ulpi_reset_req(ulpi_reset_req), .hub_reset_n(hub_reset_n),
        .ctl(usb_ctl), .khz(usb_khz), .dir_edges(usb_dir_edges)
    );
    // serial bus (iec_bus below), '0' = line low
    wire c64_atn_o, c64_clk_o, c64_data_o, c64_atn_i, c64_clk_i, c64_data_i;
    wire drv_clk_o, drv_data_o, drv_atn_i, drv_clk_i, drv_data_i;
    wire [2:0] drive_leds_n;                // drive A: activity, motor, dirty (cpu_clk)
    wire [255:0] drive_dbg;                 // drive A status for the debug map (cpu_clk)
    reg          iec_port_off = 1'b0;       // JTAG register 0x24 bit 0 (sys_clk)
    // drive A status (drive_unit.vhd), sampled; a word may tear while it changes
    (* ASYNC_REG = "TRUE" *) reg [255:0] drive_dbg_s1, drive_dbg_s2;
    always @(posedge sys_clk) begin drive_dbg_s1 <= drive_dbg; drive_dbg_s2 <= drive_dbg_s1; end
    // the serial bus for 0x24: {board pins in (ATN, CLK, DATA; 1 = low), out,
    //   C64 pulls, drive pulls (CLK, DATA); '0' = pulling}
    (* ASYNC_REG = "TRUE" *) reg [10:0] iec_s1, iec_s2;
    always @(posedge sys_clk) begin
        iec_s1 <= {IEC_ATN_I, IEC_CLOCK_I, IEC_DATA_I, IEC_ATN_O, IEC_CLOCK_O, IEC_DATA_O,
                   c64_atn_o, c64_clk_o, c64_data_o, drv_clk_o, drv_data_o};
        iec_s2 <= iec_s1;
    end
    // drive A stops with the machine pause (its ticks, drive_unit.vhd)
    (* ASYNC_REG = "TRUE" *) reg [1:0] drv_halt_s = 2'b00;
    always @(posedge cpu_clk) drv_halt_s <= {drv_halt_s[0], c64_halted};
    // drive A's debug (drv_dbg.v, T21): its halt is the machine pause's unless
    // the debugger holds or steps the drive
    wire        drv_dbg_halt, drv_trace_valid, drv_stop_ev, drv_done_ev;
    wire [31:0] drv_trace, drv_dr_rdata;
    // GAME/EXROM pins and the port RESET sense for the app (CART_DETECT,
    // C64_CLOCK_DETECT b4), synchronized to its clock
    (* ASYNC_REG = "TRUE" *) reg [2:0] slot_pins_s1 = 3'b011, slot_pins_cpu = 3'b011;
    always @(posedge cpu_clk) begin
        slot_pins_s1  <= {SLOT_RST_I, SLOT_EXROMn, SLOT_GAMEn};
        slot_pins_cpu <= slot_pins_s1;
    end

    wire        drv_tgl, drv_done_tgl, drv_we;  // drive A's DDR2 port (cpu_clk)
    wire [25:0] drv_addr;
    wire [31:0] drv_wdata;
    wire [3:0]  drv_be;
    wire [127:0] drv_done_rdata;
    wire [127:0] rv_tap;                    // the RISC-V's state (ultimate_sys, cpu_clk)
    // T62: RESTORE level for U64_RESTORE_REG (G04), the C64's write to $D7FF as toggle + held byte (G17,
    // c64_core d7ff_*; the byte is stable long before ultimate_sys has seen the toggle), the scan lines bit (G09),
    // the IEC RESET line into the cpu clock (G19; ignored while the board port is switched off)
    wire        scanlines;
    wire        d7ff_ev;
    wire  [7:0] d7ff_data;
    reg         d7ff_tgl = 1'b0;
    reg   [7:0] d7ff_hold = 8'd0;
    always @(posedge sys_clk)
        if (d7ff_ev) begin d7ff_tgl <= ~d7ff_tgl; d7ff_hold <= d7ff_data; end
    (* ASYNC_REG = "TRUE" *) reg restore_cpu_s1, restore_cpu_s2;
    (* ASYNC_REG = "TRUE" *) reg [1:0] iec_rst_s1 = 2'b00, iec_rst_s2 = 2'b00;
    wire iec_rst_n_cpu = ~(iec_rst_s2[1] & ~iec_rst_s2[0]);
    // ETH_LED_YELn: on while a carrier start (RMII_CRS_DV) was seen in the last 2^27 clk50 cycles
    reg        eth_crs_d = 1'b0;
    reg [27:0] eth_age = {28{1'b1}};
    always @(posedge clk50) begin
        eth_crs_d <= RMII_CRS_DV;
        if (RMII_CRS_DV && !eth_crs_d) eth_age <= 28'd0;
        else if (!eth_age[27]) eth_age <= eth_age + 1'b1;
    end
    assign ETH_LED_YELn = eth_age[27];
    ultimate_sys #(.g_capabilities({2'b00, FPGA_TYPE, 28'h5E40202})) ult (
        .clock(cpu_clk), .reset(cpu_rst),
        .mem_request(m_request), .mem_tag(m_tag), .mem_read_writen(m_rwn),
        .mem_address(m_address), .mem_byte_en(m_byte_en), .mem_wdata(m_wdata),
        .mem_rdata(m_rdata), .mem_rack(m_rack), .mem_rack_tag(m_rack_tag),
        .mem_dack_tag(m_dack_tag),
        .ddr_ready(calib_cpu[1]),
        .uart_txd(UART_TXD), .uart_rxd(UART_RXD),
        .con_valid(con_valid), .con_data(con_data),
        .eth_clock(clk50), .eth_reset(eth_rst),
        .eth_tx_data(mac_tx_data), .eth_tx_eof(mac_tx_eof), .eth_tx_valid(mac_tx_valid),
        .eth_tx_ready(mac_tx_ready),
        .eth_rx_data(erx_data), .eth_rx_sof(erx_sof), .eth_rx_eof(erx_eof), .eth_rx_valid(erx_valid),
        .mdc(app_mdc), .mdio_i(mdio_in), .mdio_o(mdio_out),
        .wifi_txd(WIFI_TXD), .wifi_rxd(WIFI_RXD), .wifi_rts(WIFI_RTS), .wifi_cts(WIFI_CTS),
        .sd_ssn(SD_SSn), .sd_clk(SD_CLK), .sd_mosi(SD_MOSI), .sd_miso(SD_MISO),
        .sd_detectn(sd_detectn_s[1]),
        .keys(keys_cpu_s2), .joys(joys_cpu_s2),
        .restore_pin(restore_cpu_s2), .scanlines(scanlines), .c64_dbg_tgl(d7ff_tgl), .c64_dbg_data(d7ff_hold),
        .iec_reset_n(iec_rst_n_cpu),
        .app_keys(app_keys), .app_reset(app_reset), .app_restore(app_restore), .app_freeze(app_freeze),
        .app_joy1(app_joy1), .app_joy2(app_joy2), .c64_reset_sense(core_reset_req),
        .hdmi_hpd(HDMI_HPD), .hdmi_ddc_en(HDMI_DDC_EN),
        .flash_csn(FLASH_CSn), .flash_sck(flash_sck), .flash_mosi(FLASH_MOSI),
        .flash_miso(FLASH_MISO),
        .c64_reset_req(app_c64_reset), .c64_pause_req(app_c64_pause), .c64_ultimax_req(app_ultimax),
        .slot_pins(slot_pins_cpu), .c64_bus_int(bus_int_cpu), .c64_bus_ext(bus_ext_cpu),
        .video_fmt(video_fmt),
        .turbo_en(turbo_en), .turbo_prefer(turbo_prefer), .turbo_upd_tgl(turbo_upd),
        .speaker_en(speaker_en), .speaker_vol(speaker_vol),
        .mix_l(mix_l), .mix_r(mix_r), .mix_spk(mix_spk),
        .sid_base(sid_base), .sid_mask(sid_mask), .sid_split(sid_split), .sid_waves(sid_waves),
        .sock_base(sock_base), .sock_mask(sock_mask), .sock_en(sock_en), .stereo_sel(stereo_sel),
        .sock_gains(sock_gains),
        .sid_res(sid_res), .sid_digi(sid_digi), .sidcv_we(sidcv_we), .sidcv_addr(sidcv_addr),
        .sidcv_wdata(sidcv_wdata), .sid_lv_addr(sid_lv_addr), .sid_lv_data(sid_lv_data),
        .sid_gains(sid_gains), .sid_gains2(sid_gains2), .smp_en(smp_en),
        .cart_type(app_cart_type), .cart_kill_tgl(app_cart_kill_tgl),
        .reu_enable(app_reu_en), .reu_size(app_reu_size),
        .cart_force_tgl(app_cart_force_tgl), .cart_active(cart_active),
        .dma_req_tgl(dma_req_tgl), .dma_addr(dma_addr), .dma_we(dma_we), .dma_wdata(dma_wdata),
        .dma_bus(dma_bus), .dma_rom(dma_rom),
        .dma_done_tgl(dma_done_tgl), .dma_rdata(dma_rdata_q), .dbg_stop(dbg_stop),
        .stream_ena(stream_ena), .udp_we(udp_we), .udp_addr(udp_addr), .udp_wdata(udp_wdata),
        .i2c_we(ai2c_we), .i2c_re(ai2c_re), .i2c_addr(ai2c_addr), .i2c_wdata(ai2c_wdata),
        .i2c_rdata(ai2c_rdata),
        .ovl_we(ovl_we), .ovl_addr(ovl_addr), .ovl_wdata(ovl_wdata), .ovl_rdata(ovl_rdata),
        .pal_we(pal_we), .pal_addr(pal_addr), .pal_wdata(pal_wdata),
        .bl_we(bl_we), .bl_addr(bl_addr), .bl_wdata(bl_wdata), .bl_rdata(bl_rdata), .bl_irq(bl_irq),
        .cw_we(cw_we), .cw_re(cw_re), .cw_addr(cw_addr), .cw_wdata(cw_wdata), .cw_rdata(cw_rdata),
        .cw_ack(cw_ack),
        .cw_irq(cw_irq),
        .c64_clock(sys_clk), .uci_bus_addr(uci_bus_addr), .uci_ev(uci_ev), .uci_ev_addr(uci_ev_addr),
        .uci_ev_data(uci_ev_data), .uci_ev_we(uci_ev_we), .uci_ev_io(uci_ev_io), .uci_dout(uci_dout),
        .uci_oe(uci_oe), .uci_c64_irq(uci_c64_irq), .uci_freeze(uci_freeze),
        .caseled_sel(caseled_sel), .caseled_set(caseled_set), .busy_any(busy_any), .busy_usb(busy_usb),
        .ulpi_clock(ulpi_clk), .ulpi_nxt(ulpi_nxt), .ulpi_dir(ulpi_dir), .ulpi_stp(ulpi_stp),
        .ulpi_data_i(ulpi_data_i), .ulpi_data_o(ulpi_data_o), .ulpi_data_t(ulpi_data_t),
        .ulpi_reset_req(ulpi_reset_req), .hub_reset_n(hub_reset_n),
        .iec_atn_i(drv_atn_i), .iec_clk_i(drv_clk_i), .iec_data_i(drv_data_i),
        .iec_clk_o(drv_clk_o), .iec_data_o(drv_data_o), .drive_leds_n(drive_leds_n), .drive_audio(),
        .drive_dbg(drive_dbg), .drive_halt(drv_dbg_halt),
        .drive_trace(drv_trace), .drive_trace_valid(drv_trace_valid),
        .rv_tap(rv_tap),
        .drv_mem_tgl(drv_tgl), .drv_mem_addr(drv_addr), .drv_mem_we(drv_we), .drv_mem_wdata(drv_wdata),
        .drv_mem_be(drv_be), .drv_mem_done(drv_done_tgl), .drv_mem_rdata(drv_done_rdata)
    );

    // ---- serial bus: the C64, the app's drive A, the board's IEC port -------------
    iec_bus iec (
        .clk(sys_clk), .c64_atn_o(c64_atn_o), .c64_clk_o(c64_clk_o), .c64_data_o(c64_data_o),
        .c64_atn_i(c64_atn_i), .c64_clk_i(c64_clk_i), .c64_data_i(c64_data_i),
        .cpu_clk(cpu_clk), .drv_clk_o(drv_clk_o), .drv_data_o(drv_data_o),
        .drv_atn_i(drv_atn_i), .drv_clk_i(drv_clk_i), .drv_data_i(drv_data_i),
        .port_off(iec_port_off),
        .port_atn_i(IEC_ATN_I), .port_clk_i(IEC_CLOCK_I), .port_data_i(IEC_DATA_I),
        .port_atn_o(IEC_ATN_O), .port_clk_o(IEC_CLOCK_O), .port_data_o(IEC_DATA_O)
    );
    assign IEC_RESET_O = 1'b0;
    assign IEC_SRQ_O   = 1'b0;
    // MDIO: open drain, released = pulled high. Debug register 7 can take
    // MDC/MDIO over from the app (tools/trxdbg.py mdio).
    IOBUF b_mdio (.IO(MDIO_DATA), .I(1'b0), .T(mdio_ovr[31] ? mdio_ovr[1] : mdio_out), .O(mdio_in));
    assign MDIO_CLK = mdio_ovr[31] ? mdio_ovr[0] : app_mdc;
    rst_sync rs_eth (.clk(clk50), .arst(cpu_rst), .rst(eth_rst));

    // ---- Ethernet: RMII transceiver, shared by the MAC and the UDP streams ----
    wire [7:0] x_data;
    wire       x_eof, x_valid, x_ready;
    rmii_transceiver rmii (
        .clock(clk50), .reset(eth_rst),
        .rmii_crs_dv(RMII_CRS_DV), .rmii_rxd(RMII_RX_DATA),
        .rmii_tx_en(RMII_TX_EN), .rmii_txd(RMII_TX_DATA),
        .eth_rx_data(erx_data), .eth_rx_sof(erx_sof), .eth_rx_eof(erx_eof), .eth_rx_valid(erx_valid),
        .eth_tx_data(x_data), .eth_tx_eof(x_eof), .eth_tx_valid(x_valid), .eth_tx_ready(x_ready),
        .ten_meg_mode(1'b0)
    );
    // whole frames in turn: whoever has one waiting gets the transmitter when it
    // is free, the MAC first, and keeps it until its last byte is taken
    reg own_mac = 1'b0, own_st = 1'b0;
    always @(posedge clk50)
        if (eth_rst) begin
            own_mac <= 1'b0; own_st <= 1'b0;
        end else if (!own_mac && !own_st) begin
            if (mac_tx_valid) own_mac <= 1'b1;
            else if (st_req)  own_st  <= 1'b1;
        end else if (own_mac && mac_tx_valid && mac_tx_eof && x_ready)
            own_mac <= 1'b0;
        else if (own_st && st_tx_valid && st_tx_eof && x_ready)
            own_st <= 1'b0;
    assign x_data  = own_st ? st_tx_data : mac_tx_data;
    assign x_eof   = own_st ? st_tx_eof  : mac_tx_eof;
    assign x_valid = own_st ? st_tx_valid : own_mac & mac_tx_valid;
    assign mac_tx_ready = own_mac & x_ready;

    // counters for the debug port (clk50; read across clocks, for a look only)
    reg [15:0] cnt_rx = 0, cnt_rx_ok = 0, cnt_tx = 0, cnt_crs = 0;
    reg        crs_d;
    always @(posedge clk50) begin
        crs_d <= RMII_CRS_DV;
        if (RMII_CRS_DV && !crs_d) cnt_crs <= cnt_crs + 1'b1;
        if (erx_valid && erx_eof) begin
            cnt_rx <= cnt_rx + 1'b1;
            if (erx_data[0]) cnt_rx_ok <= cnt_rx_ok + 1'b1;
        end
        if (own_mac && mac_tx_valid && mac_tx_eof && x_ready) cnt_tx <= cnt_tx + 1'b1;
    end


    wire        cpu_tgl, cpu_done_tgl, cpu_we;
    wire [25:0] cpu_addr;
    wire [31:0] cpu_wdata, cpu_done_rdata;
    wire [3:0]  cpu_be;
    cpu_mem_bridge bridge (
        .clk(cpu_clk), .rst(cpu_rst),
        .request(m_request), .tag(m_tag), .read_writen(m_rwn), .address(m_address),
        .byte_en(m_byte_en), .wdata(m_wdata), .rdata(m_rdata), .rack(m_rack),
        .rack_tag(m_rack_tag), .dack_tag(m_dack_tag),
        .req_tgl(cpu_tgl), .a_addr(cpu_addr), .a_we(cpu_we), .a_wdata(cpu_wdata), .a_be(cpu_be),
        .done_tgl(cpu_done_tgl), .done_rdata(cpu_done_rdata)
    );

    // client 0: RISC-V, client 1: REU / JTAG, client 2: drive A; cart_fill
    // before them
    mig_arb arb (
        .ui_clk(ui_clk), .ui_rst(ui_rst), .calib_done(ddr_calib),
        .req_tgl({drv_tgl, port_tgl, cpu_tgl}), .addr({drv_addr, ddr_addr, cpu_addr}),
        .we({drv_we, ddr_we, cpu_we}), .wdata({drv_wdata, ddr_wdata, cpu_wdata}),
        .be({drv_be, ddr_be, cpu_be}),
        .done_tgl({drv_done_tgl, port_done_tgl, cpu_done_tgl}), .rdata({port_done_rdata, cpu_done_rdata}),
        .rdata_line(drv_done_rdata),
        .c_addr(c_addr), .c_cmd(c_cmd), .c_en(c_en), .c_rdy(c_rdy), .c_wdf_data(c_wdf_data),
        .c_wdf_mask(c_wdf_mask), .c_wdf_wren(c_wdf_wren), .c_wdf_rdy(c_wdf_rdy),
        .c_rd_data(c_rd_data), .c_rd_data_valid(c_rd_valid),
        .v_addr(v_addr), .v_cmd(v_cmd), .v_en(v_en), .v_rdy(v_rdy), .v_wdf_data(v_wdf_data),
        .v_wdf_mask(v_wdf_mask), .v_wdf_wren(v_wdf_wren), .v_wdf_rdy(), .v_rd_data_valid(v_rd_valid),
        .v_urgent(v_urgent),
        .e_addr(e_addr), .e_cmd(e_cmd), .e_en(e_en), .e_rdy(e_rdy), .e_wdf_data(e_wdf_data),
        .e_wdf_mask(e_wdf_mask), .e_wdf_wren(e_wdf_wren), .e_rd_data_valid(e_rd_valid),
        .app_addr(app_addr), .app_cmd(app_cmd), .app_en(app_en), .app_rdy(app_rdy),
        .app_wdf_data(app_wdf_data), .app_wdf_mask(app_wdf_mask), .app_wdf_wren(app_wdf_wren),
        .app_wdf_rdy(app_wdf_rdy), .app_rd_data(app_rd_data), .app_rd_data_valid(app_rd_data_valid)
    );

    // the lens unit's look at the cartridge's DDR2 bytes (dma_win, T12 §11 Q2, T21 §3.4.1)
    wire        pkd_tgl, pkd_ack_tgl;
    wire [23:0] pkd_addr;
    wire  [7:0] pkd_data;
    wire [1:0] cart_rom_sel;                    // trx_io ROM_SEL (T60)
    cart_fill #(.ROM_BASE_BLOCK({1'b0, REU_BASE})) cfill (
        .ui_clk(ui_clk), .ui_rst(ui_rst || !ddr_calib), .rom_sel(cart_rom_sel),
        .req_tgl(cart_req_tgl), .req_we(cart_req_we), .req_addr(cart_req_addr),
        .req_data(cart_req_data), .req_slot(cart_req_slot), .req_cached(cart_req_cached),
        .ack_tgl(cart_ack_tgl),
        .c_be(cart_fill_be), .c_addr(cart_fill_addr), .c_data(cart_fill_data),
        .app_addr(c_addr), .app_cmd(c_cmd), .app_en(c_en), .app_rdy(c_rdy),
        .app_wdf_data(c_wdf_data), .app_wdf_mask(c_wdf_mask), .app_wdf_wren(c_wdf_wren),
        .app_wdf_rdy(c_wdf_rdy), .app_rd_data(c_rd_data), .app_rd_data_valid(c_rd_valid),
        .rbw(cart_rbw), .ob_v(ob_v), .ob_addr(ob_addr), .ob_old(ob_old),
        .pk_tgl(pkd_tgl), .pk_addr(pkd_addr), .pk_ack_tgl(pkd_ack_tgl), .pk_data(pkd_data)
    );

    // our IO block (ARCH §10): ID and interrupt, the cartridge page (flash
    // responder configuration, pending operation, ACK, cache invalidation,
    // write counter), the GMod2 EEPROM window. The responder configuration
    // goes into the C64 clock as it is: the app writes it before the cartridge
    // starts and leaves it alone while it runs.
    trx_io tio (
        .clk(cpu_clk), .we(cw_we), .re(cw_re), .addr(cw_addr), .wdata(cw_wdata), .rdata(cw_rdata),
        .ack(cw_ack), .irq(cw_irq),
        .rq_tgl(trx_rq_tgl), .rq_addr(trx_rq_addr), .rq_we(trx_rq_we), .rq_wdata(trx_rq_wdata),
        .rs_tgl(trx_rs_tgl), .rs_rdata(trx_rs_rdata), .stop_tgl(stop_tgl), .cap_tgl(cap_tgl),
        .eng_tgl(eng_tgl), .gap_tgl(gap_tgl),
        .fl_enable(cart_fl_enable), .fl_magic1(cart_fl_magic1), .fl_magic2(cart_fl_magic2),
        .fl_mask(cart_fl_mask), .fl_id(cart_fl_id), .cart_desc(cart_desc), .evt_tgl(cart_evt_tgl), .evt_word(cart_evt_word),
        .fl_ack_tgl(cart_fl_ack_tgl), .inval_tgl(cart_inval_tgl), .rom_sel(cart_rom_sel), .ee_tgl(cart_ee_tgl),
        .ee_addr(cart_ee_addr), .ee_we(cart_ee_we), .ee_wdata(cart_ee_wdata), .ee_rdata(cart_ee_rdata),
        .p6_we(px_we), .p6_re(px_re), .p6_rdata(px_rdata), .ev_vsync(px_ev_vsync), .ev_flip(px_ev_flip),
        .build(usr_access), .ddr_cal(ddr_cal),
        .caps3({6'd0, 1'b1, sid_resid8}), .rs8_cfg(sid_rs8),   // b1: the lens unit's look covers the I/O page (U13)
        .sm_tgl(smp_a_tgl), .sm_addr(smp_a_addr), .sm_we(smp_a_we), .sm_wdata(smp_a_wdata),
        .sm_rs_tgl(smp_a_rs_tgl), .sm_rdata(smp_a_rdata)
    );

    // ---- program both synthesizers -------------------------------------------
    // (U16: the HDMI row the app asked for, vm_mode / vm_rst from mode_req.v in the HDMI section; a System Mode switch's lock loss
    // restarts the sequence as well, with the row of the new family)
    wire scl_low, sda_low, i2c_done;
    wire mc_cdce_v60;
    wire [3:0] vm_mode;
    wire       vm_rst;
    cdce_mode i2c (.clk(clk50), .rst(por_rst | ~clocks_ok | vm_rst), .scl_low(scl_low),
                   .sda_low(sda_low), .done(i2c_done), .mode(vm_mode));
    // After the synthesizers, the keyboard scan takes the buses over. Which of
    // the three buses the keyboard expander sits on differs between board
    // versions (Gideon's bootloader swaps them by board revision), so the scan
    // runs on all three at once: the buses are open drain, the expander answers
    // on its own one and the others stay high.
    wire kb_scl_low, kb_sda_low, sda18_in, sdahd_in, sda33_in;
    wire bus_scl_low = i2c_done ? kb_scl_low : scl_low;
    wire bus_sda_low = i2c_done ? kb_sda_low : sda_low;
    // JTAG register 10 can take the buses over for exploring them by hand
    // (tools/trxdbg.py i2c): bit 31 on, bits 2:0 pull SCL low, bits 5:3 pull
    // SDA low, per bus {3.3 V, HDMI, 1.8 V}. Reading it gives the line levels.
    reg  [31:0] i2c_ovr = 32'd0;            // sys_clk; static levels, no sync needed
    wire        ovr = i2c_ovr[31];
    // The app's master has them while it owns them (app_i2c.v: claimed with
    // scan_enable 0 once the keyboard scan stands between two transactions).
    wire [2:0]  scl_lo = ovr ? i2c_ovr[2:0] : (ai2c_own & i2c_done) ? ai2c_scl_lo :
                         {i2c_done & kb_scl_low, bus_scl_low, bus_scl_low};
    wire [2:0]  sda_lo = ovr ? i2c_ovr[5:3] : (ai2c_own & i2c_done) ? ai2c_sda_lo :
                         {i2c_done & kb_sda_low, bus_sda_low, bus_sda_low};
    wire scl18_in, sclhd_in, scl33_in;
    IOBUF b_scl18 (.IO(I2C18_SCL), .I(1'b0), .T(~scl_lo[0]), .O(scl18_in));
    IOBUF b_sda18 (.IO(I2C18_SDA), .I(1'b0), .T(~sda_lo[0]), .O(sda18_in));
    IOBUF b_sclhd (.IO(HDMI_SCL),  .I(1'b0), .T(~scl_lo[1]), .O(sclhd_in));
    IOBUF b_sdahd (.IO(HDMI_SDA),  .I(1'b0), .T(~sda_lo[1]), .O(sdahd_in));
    IOBUF b_scl33 (.IO(I2C33_SCL), .I(1'b0), .T(~scl_lo[2]), .O(scl33_in));
    IOBUF b_sda33 (.IO(I2C33_SDA), .I(1'b0), .T(~sda_lo[2]), .O(sda33_in));
    wire kb_sda_in = sda18_in & sdahd_in & sda33_in;
    app_i2c ai2c (
        .clk(cpu_clk), .rst(cpu_rst), .we(ai2c_we), .re(ai2c_re), .addr(ai2c_addr),
        .wdata(ai2c_wdata), .rdata(ai2c_rdata), .scan_en(ai2c_scan),
        .i2c_done(i2c_done), .kb_pause(kb_pause), .kb_parked(kb_parked), .own(ai2c_own),
        .scl_lo(ai2c_scl_lo), .sda_lo(ai2c_sda_lo),
        .scl_in({scl33_in, sclhd_in, scl18_in}), .sda_in({sda33_in, sdahd_in, sda18_in}),
        .hold(mc_i2c_hold), .parked(ai2c_parked)
    );

    // System Mode (T40): the app's C64_VIDEOFORMAT picks PAL, NTSC, PAL-60 or
    // NTSC-50; mode_ctl switches the C64 clock (mmcm_sys DRP), the
    // synthesizers and with them the HDMI frame, live
    mode_ctl mctl (
        .clk(clk50), .rst(por_rst), .fmt(video_fmt),
        .halt_req(mc_halt), .halted(c64_halted), .in_reset(core_reset_req),
        .i2c_hold(mc_i2c_hold), .i2c_parked(ai2c_parked), .kb_pause(mc_kb_pause), .kb_parked(kb_parked),
        .i2c_done(i2c_done), .cdce_v60(mc_cdce_v60), .sys_en(mc_sys_en), .reconf(mc_reconf),
        .mmcm_rst(mmcm_drst), .daddr(mmcm_daddr), .di(mmcm_di), .dout(mmcm_do), .den(mmcm_den),
        .dwe(mmcm_dwe), .drdy(mmcm_drdy), .sys_locked(sys_locked), .ref_locked(ref_locked),
        .vmode(vmode), .status(mc_status), .status2(mc_status2)
    );

    // give the synthesizer PLL 50 ms to settle before the transmitters start
    reg [21:0] settle = 0;
    wire gt_rst = ~settle[21];
    always @(posedge clk50)
        if (!i2c_done) settle <= 0;
        else if (!settle[21]) settle <= settle + 1;

    // ---- keyboard, joysticks, RESTORE ------------------------------------------
    wire [63:0] kb_keys;                // I2C expander behind P7 (an original C64 keyboard)
    wire [63:0] bl_keys;                // the C64U keyboard on J8 (bling_keyboard, below)
    wire  [7:0] bl_last, bl_other;
    wire        bl_restore;
    wire [15:0] kb_status;
    wire  [9:0] x_text;                 // from vic_scaler (below)
    c64u_keyboard kb (.clk(clk50), .rst(~i2c_done), .sda_in(kb_sda_in),
                      .scl_low(kb_scl_low), .sda_low(kb_sda_low), .keys(kb_keys),
                      .status(kb_status), .pause(kb_pause | mc_kb_pause), .parked(kb_parked));
    // Into the C64 clock. The key bits change slowly (one column every 0.7 ms),
    // so bits of one scan arriving a clock apart do no harm.
    (* ASYNC_REG = "TRUE" *) reg [63:0] keys_s1, keys_s2;
    // line levels for register 10: bit 6 KBJOY_INT, 5:3 SDA, 2:0 SCL {3.3 V, HDMI, 1.8 V};
    // bit 13 the app owns the buses, bit 12 its scan_enable
    (* ASYNC_REG = "TRUE" *) reg [11:0] i2c_lv_s1, i2c_lv_s;
    always @(posedge sys_clk) begin
        i2c_lv_s1 <= {5'd0, KBJOY_INT, sda33_in, sdahd_in, sda18_in, scl33_in, sclhd_in, scl18_in};
        i2c_lv_s  <= i2c_lv_s1;
    end
    (* ASYNC_REG = "TRUE" *) reg [10:0] pins_s1, pins_s2;
    always @(posedge sys_clk) begin
        keys_s1 <= kb_keys | bl_keys;  keys_s2 <= keys_s1;
        pins_s1 <= ~{KBJOY_RESTORE, KBJOY_2_JOY, KBJOY_1_JOY};
        pins_s2 <= pins_s1;
    end
    // the app's own scans (KEYB_ROW, KEYB_JOY, ultimate_sys.vhd) in the CPU's clock
    (* ASYNC_REG = "TRUE" *) reg [9:0] joys_cpu_s1, joys_cpu_s2;
    always @(posedge cpu_clk) begin
        restore_cpu_s1 <= pins_s2[10] | bl_restore;
        restore_cpu_s2 <= restore_cpu_s1;
        iec_rst_s1 <= {IEC_RESET_I, iec_port_off};
        iec_rst_s2 <= iec_rst_s1;
    end
    always @(posedge cpu_clk) begin
        keys_cpu_s1 <= keys_s2; keys_cpu_s2 <= keys_cpu_s1;
        joys_cpu_s1 <= pins_s2[9:0]; joys_cpu_s2 <= joys_cpu_s1;
    end

    // ---- the C64 ---------------------------------------------------------------
    wire        pix_clk, pix_rst;           // HDMI pixel clock (below)
    wire        audio_sample_en;            // mclk, once per I2S frame (48 kHz)
    wire signed [23:0] audio_l, audio_r;    // mclk
    // Silent while the app has the C64 stopped (its freeze menu) or the machine
    // is paused. The app's mixer gains for the UltiSIDs are applied in the SID's
    // mixer (T25.3, cores/trx_sid/rtl/trx_audio.v), which also decays to 0 on
    // its own when the C64 clock stops; this cut stays for the app's C64_STOP.
    reg         c64_mute = 1'b0;            // sys_clk, set below
    (* ASYNC_REG = "TRUE" *) reg [1:0] pause_mclk = 2'b00;
    always @(posedge mclk) pause_mclk <= {pause_mclk[0], c64_mute};
    // T37: the real SIDs' audio from the codec's ADC, mixed by the app's Socket 1/2 gains, joins here: after the
    // SID mix and the pause gate (the freezer mutes the sockets through the mixer bytes), before I2S, HDMI and UDP
    wire        [7:0] i2s_cnt;
    wire signed [23:0] sk_l, sk_r;
    sock_audio sockaud (
        .mclk(mclk), .cnt(i2s_cnt), .sdo(AUDIO_SDO), .gains(sock_gains), .en(sock_en),
        .out_l(sk_l), .out_r(sk_r)
    );
    wire signed [24:0] sum_l = (pause_mclk[1] ? 25'sd0 : {audio_l[23], audio_l}) + {sk_l[23], sk_l};
    wire signed [24:0] sum_r = (pause_mclk[1] ? 25'sd0 : {audio_r[23], audio_r}) + {sk_r[23], sk_r};
    reg signed [23:0] snd_l, snd_r;
    always @(posedge mclk) begin
        snd_l <= (sum_l[24] == sum_l[23]) ? sum_l[23:0] : sum_l[24] ? 24'sh800000 : 24'sh7FFFFF;
        snd_r <= (sum_r[24] == sum_r[23]) ? sum_r[23:0] : sum_r[24] ? 24'sh800000 : 24'sh7FFFFF;
    end
    wire        mclk_rst;
    wire [3:0] pix_color;
    wire [9:0] pix_x;
    wire [8:0] pix_y;
    wire [4:0] c64_leds;
    // ---- debug over JTAG (tools/trxdbg.py) -------------------------------------
    // 0x000000-0x00FFFF C64 RAM (one byte per access); 0x100000 + n: registers
    //   0 'TRX1', 1 x_text, 2 keyboard status {J8: other codes, last byte;
    //     P7 expander: nacks, rows}, 3/4 keys low/high,
    //   5 {DDR2 calibrated, synthesizers done, RESTORE, joystick 2, joystick 1},
    //   6 RISC-V console: bytes written so far,
    //   7 MDIO by hand: write bit 31 = take MDC/MDIO from the app, bit 0 MDC,
    //     bit 1 MDIO (1 = released); read bit 2 = MDIO pin,
    //   8 DDR2 window (16 MB units, bits 2:0);
    //   9 machine control: bit 0 C64 CPU reset (held while set), bit 1 C64 CPU
    //     pause, bit 2 RISC-V reset (held while set), bit 3 machine pause (the
    //     whole C64 and drive A, c64_core.v), bit 4 the app's C64_STOP pauses
    //     only the CPU (as before T12 B), whatever GATE_CTRL says (the app's
    //     stop is a machine pause only with GATE_CTRL b0, trx_sys.v);
    //     read bit 8 = the machine is paused;
    //   10 I2C by hand (tools/i2cx.py), 11 BLING_KEYB changes recorded;
    //   12 Ethernet frames received {CRC ok, all}, 13 sent {video frames,
    //   MAC frames}, 14 {app DMA bytes, PHY carrier edges}, 15 {audio packets, stream enable};
    //   0x10 USB: read ULPI_CLOCK in kHz, write bit 0 inverts ULPI_RESET, bit 1
    //   lets the app's ULPI reset reach the PHY; 0x11 {DIR edges, ULPI_RESET,
    //   HUB_RESETn, those two bits} (usb_phy.v);
    //   0x20-0x23 drive A (drive_unit.vhd): 0x20 last GCR fetch address,
    //   0x21 {GCR fetches, their answers}, 0x22 the drive CPU's last bus cycle
    //   {0, ATN, DATA, CLK, SYNC, SO, IRQ, read, data, address}, 0x23 {SYNC
    //   edges (12 bits), byte-ready edges (12 bits), last GCR byte};
    //   0x24 serial bus {pins in ATN/CLK/DATA (1 = low), pins out (1 = pull),
    //   C64 out, drive CLK/DATA out ('0' = pull), port off}; write bit 0 = 1
    //   disconnects the board's IEC port (iec_bus.v);
    //   0x25 drive A {half track (7), step phase (2), motor, mode, write_prot_n,
    //   inserted, power, reset, byte ready | last write to VIA 2 port B $1C00 |
    //   highest half track since power-on (7), 0}; 0x26/0x27 the parameter
    //   RAM's second word for half tracks 0 and 34 as the app wrote it
    //   {length - 1 | bit time << 16}; 0x28 {GCR fetches in the last 100 ms, drive
    //   CPU cycles (16 bits, wrapping)};
    //   0x30 System Mode (T40, mode_ctl.v status), 0x31 its status2 (DRP
    //   check value, last switch in 20.48 us units), 0x32 the VIC as measured
    //   {lines per frame (9), dot4x clocks per line (12)}, 0x33 dot4x clocks
    //   per frame (sys_clk counts between two line-0 starts); 0x34 HDMI
    //   {gt_ready, pix_locked, scaler locked, 0, output frames in the last
    //   second (12), pixel clocks per frame / 256 (16)};
    //   (registers answer only at 0x100000-0x1000FF, groups 0-3 of bits 7:4;
    //   the rest of 0x100000-0x100FFF reads 0)
    // 0x101000-0x1013FF: our IO block's page JTAG_PAGE (trx_sys.v, 32-bit
    //   words at byte offset 4 x bits 9:0; only its pages in the C64 clock:
    //   0 from 0x100 (and ID at 0), 1, 5); 0x101400 JTAG_PAGE; the rest of
    //   0x101000-0x1FFFFF reads 0
    // 0x200000-0x200FFF: RISC-V console ring (4 bytes per read, see con_ring.v)
    // 0x300000-0x3003FF: BLING_KEYB edge recorder (above)
    // 0x400000-0x7FFFFF: DDR2, 32-bit words, window + word address bits 21:0
    // 0x800000-0xFFFFFF: DDR2, 16-bit words, window + address bits 22:0
    wire [23:0] dbg_addr;
    wire [31:0] dbg_wdata;
    wire        dbg_re, dbg_we;
    wire  [7:0] ram_rdata;
    reg  [31:0] dbg_reg;
    wire [255:0] rv_hold;                   // rv_dbg.v, below
    (* ASYNC_REG = "TRUE" *) reg [255:0] rv_hold_s1, rv_hold_s2;   // still between snapshots
    reg         dbg_is_reg, dbg_is_ddr, dbg_is_con, dbg_is_edge, dbg_is_trx, dbg_is_ram, dbg_rvalid_fast;
    wire        dbg_trx_sel = dbg_addr[23:12] == 12'h101;

    // BLING_KEYB edge recorder, for working out its protocol: every change of
    // the line is stored as {new level, sys_clk count bits 30:0}, 1024 of them,
    // read at 0x300000 + n; register 11 counts the changes so far.
    (* ASYNC_REG = "TRUE" *) reg [2:0] bk_s = 3'b111;
    reg  [30:0] bk_time = 31'd0;
    reg  [31:0] bk_count = 32'd0;
    reg  [31:0] bk_mem [0:1023];
    reg  [31:0] bk_rdata;
    always @(posedge sys_clk) begin
        bk_s    <= {bk_s[1:0], BLING_KEYB};
        bk_time <= bk_time + 31'd1;
        if (bk_s[2] != bk_s[1]) begin
            if (bk_count < 32'd1024) bk_mem[bk_count[9:0]] <= {bk_s[1], bk_time};
            bk_count <= bk_count + 32'd1;
        end
        bk_rdata <= bk_mem[dbg_addr[9:0]];
    end
    wire        bl_rx_tgl, bl_locked, bl_lock_off;
    (* ASYNC_REG = "TRUE" *) reg [1:0] bl_lock_off_s = 2'b00;
    always @(posedge sys_clk) bl_lock_off_s <= {bl_lock_off_s[0], bl_lock_off};
    bling_keyboard bkb (.clk(sys_clk), .v60(v60), .rx(bk_s[1]), .keys(bl_keys), .restore(bl_restore),
                        .last(bl_last), .other(bl_other),
                        .rx_tgl(bl_rx_tgl), .locked(bl_locked), .lock_off(bl_lock_off_s[1]));
    bling_io bio (.clk(cpu_clk), .rst(cpu_rst), .we(bl_we), .addr(bl_addr), .wdata(bl_wdata),
                  .rdata(bl_rdata), .irq(bl_irq),
                  .rx_tgl(bl_rx_tgl), .rx_byte(bl_last), .locked(bl_locked), .lock_off(bl_lock_off),
                  .bling_led(BLING_LED));
    assign LED_DATA  = 1'b0;            // case LED strip: out (OPT-bling), idle level
    assign LED_CLOCK = 1'b0;
    reg  [31:0] hdmi_meas_s;              // T40: HDMI as measured (below, LEDs section)
    // T40: mode_ctl's status (clk50, static between switches) and the VIC's
    // frame as it runs: raster lines, dot4x clocks per line and per frame
    reg  [31:0] mc_status_s, mc_status2_s;
    reg  [8:0]  vm_lines = 9'd0, vm_py_d = 9'd0;
    reg  [11:0] vm_line_clk = 12'd0, vm_lc = 12'd0;
    reg  [19:0] vm_frame_clk = 20'd0, vm_fc = 20'd0;
    reg  [9:0]  vm_px_d = 10'd0;
    always @(posedge sys_clk) begin
        mc_status_s  <= mc_status;
        mc_status2_s <= mc_status2;
        vm_py_d <= pix_y;
        vm_px_d <= pix_x;
        vm_lc <= vm_lc + 12'd1;
        vm_fc <= vm_fc + 20'd1;
        if (pix_x == 10'd0 && vm_px_d != 10'd0) begin vm_line_clk <= vm_lc; vm_lc <= 12'd1; end
        if (pix_y == 9'd0 && vm_py_d != 9'd0) begin
            vm_lines <= vm_py_d + 9'd1; vm_frame_clk <= vm_fc; vm_fc <= 20'd1;
        end
    end
    reg  [2:0]  ddr_window = 3'd0;
    wire        dbg_ddr_sel = dbg_addr[23] | dbg_addr[22];
    (* ASYNC_REG = "TRUE" *) reg [1:0] calib_s;
    always @(posedge sys_clk) begin
        calib_s <= {calib_s[0], ddr_calib};
        dbg_rvalid_fast <= dbg_re && !dbg_ddr_sel && !dbg_trx_sel;
        if (dbg_re || dbg_we) begin
            dbg_is_reg <= dbg_addr[23:12] == 12'h100;
            dbg_is_trx <= dbg_trx_sel;
            dbg_is_ram <= dbg_addr[23:16] == 8'h00;
            dbg_is_con <= dbg_addr[23:20] == 4'h2;
            dbg_is_edge <= dbg_addr[23:20] == 4'h3;
            dbg_is_ddr <= dbg_ddr_sel;
        end
        if (dbg_we && dbg_addr == 24'h100008) ddr_window <= dbg_wdata[2:0];
        if (dbg_we && dbg_addr == 24'h100009) machine_ctl <= dbg_wdata[4:0];
        if (dbg_we && dbg_addr == 24'h10000A) i2c_ovr <= dbg_wdata;
        if (dbg_we && dbg_addr == 24'h100007) mdio_ovr <= dbg_wdata;
        if (dbg_we && dbg_addr == 24'h100010) usb_ctl <= dbg_wdata[1:0];
        // DDR2 accesses from the debug port, queued for the DDR2 arbiter
        if ((dbg_re || dbg_we) && dbg_ddr_sel) begin
            jtag_ddr_pending <= 1'b1;
            jtag_ddr_wide    <= !dbg_addr[23];
            jtag_ddr_addr    <= dbg_addr[23] ? {ddr_window, dbg_addr[22:0]}
                                             : {ddr_window, dbg_addr[21:0], 1'b0};
            jtag_ddr_we      <= dbg_we;
            jtag_ddr_wdata   <= dbg_wdata;
        end else if (ddr_req && !ddr_for_reu)
            jtag_ddr_pending <= 1'b0;
        case (dbg_addr[3:0])
            4'd0: dbg_reg <= 32'h54525831;
            4'd1: dbg_reg <= {22'd0, x_text};
            4'd2: dbg_reg <= {bl_other, bl_last, kb_status};
            4'd3: dbg_reg <= keys_s2[31:0];
            4'd4: dbg_reg <= keys_s2[63:32];
            4'd5: dbg_reg <= {19'd0, calib_s[1], i2c_done, pins_s2};
            4'd6: dbg_reg <= con_count;
            4'd8: dbg_reg <= {29'd0, ddr_window};
            4'd7: dbg_reg <= {mdio_ovr[31], 28'd0, mdio_in, mdio_ovr[1:0]};  // MDIO by hand
            4'd9: dbg_reg <= {23'd0, c64_halted, 3'd0, machine_ctl};
            4'd10: dbg_reg <= {i2c_ovr[31], 17'd0, ai2c_own, ai2c_scan, i2c_lv_s[11:0]};
            4'd11: dbg_reg <= bk_count;
            4'd12: dbg_reg <= {cnt_rx_ok, cnt_rx};         // Ethernet frames received: CRC ok, all
            4'd13: dbg_reg <= {st_frames, cnt_tx};         // sent: video frames (68 packets), MAC frames
            4'd14: dbg_reg <= {16'd0, cnt_crs};            // PHY carrier edges
            4'd15: dbg_reg <= {as_pkts, 8'd0, stream_ena};
            default: dbg_reg <= 32'd0;
        endcase
        if (dbg_addr[7:4] == 4'h1)
            dbg_reg <= dbg_addr[0] ? {usb_dir_edges, 12'd0, ULPI_RESET, HUB_RESETn, usb_ctl} : usb_khz;
        if (dbg_addr[7:4] == 4'h2)
            dbg_reg <= dbg_addr[3:0] == 4'd4 ? {20'd0, iec_s2, iec_port_off} :
                       dbg_addr[3:0] < 4'd4 ? drive_dbg_s2[dbg_addr[1:0] * 32 +: 32] :
                       dbg_addr[3:0] < 4'd9 ? drive_dbg_s2[(dbg_addr[3:0] - 4'd1) * 32 +: 32] : 32'd0;
        if (dbg_addr[7:4] == 4'h3)
            case (dbg_addr[2:0])
                3'd0: dbg_reg <= mc_status_s;
                3'd1: dbg_reg <= mc_status2_s;
                3'd2: dbg_reg <= {11'd0, vm_lines, vm_line_clk};
                3'd3: dbg_reg <= {12'd0, vm_frame_clk};
                3'd4: dbg_reg <= hdmi_meas_s;
                default: dbg_reg <= 32'd0;
            endcase
        if (dbg_addr[7:4] == 4'h4) dbg_reg <= rv_hold_s2[dbg_addr[2:0] * 32 +: 32];
        // DDR2 calibration (ddr2_ctrl.v): the id, then the result words; they are final
        // once CAL0 bit 29 (done) is set, no synchronizer in front (static from then on)
        if (dbg_addr[7:4] == 4'h5)
            case (dbg_addr[3:0])
                4'd0: dbg_reg <= 32'hDD2C0001;
                4'd1: dbg_reg <= ddr_cal[31:0];
                4'd2: dbg_reg <= ddr_cal[63:32];
                default: dbg_reg <= 32'd0;
            endcase
        if (dbg_addr[11:8] != 4'h0 || dbg_addr[7:4] > 4'h5) dbg_reg <= 32'd0;   // not ours
        if (dbg_we && dbg_addr == 24'h100024) iec_port_off <= dbg_wdata[0];
    end
    // RISC-V tap (rv_dbg.v): a write to 0x100040 takes a snapshot, to 0x100041
    // clears the longest-stall record; the words are read at 0x100040-0x100047
    reg         rv_snap_tgl = 1'b0, rv_clr_tgl = 1'b0;
    always @(posedge sys_clk) begin
        rv_hold_s1 <= rv_hold; rv_hold_s2 <= rv_hold_s1;
        if (dbg_we && dbg_addr == 24'h100040) rv_snap_tgl <= ~rv_snap_tgl;
        if (dbg_we && dbg_addr == 24'h100041) rv_clr_tgl  <= ~rv_clr_tgl;
    end
    rv_dbg rvd (.clk(cpu_clk), .rst(cpu_rst), .tap(rv_tap), .snap_req(rv_snap_tgl),
                .clr_req(rv_clr_tgl), .hold(rv_hold));
`ifdef NO_JTAG_DBG
    // T46 (Alex, 2026-11-13): no JTAG debug master. build.tcl defines NO_JTAG_DBG unless TRX_FEATURES lists jtag.
    // Every debug register, the console and keyboard-edge read-out, the RISC-V tap and the JTAG DDR2 / C64 RAM /
    // trx ports fall away with it; the registers JTAG writes keep their power-up values, which are the ones the
    // machine runs with.
    assign dbg_addr = 24'd0;
    assign dbg_wdata = 32'd0;
    assign dbg_re = 1'b0;
    assign dbg_we = 1'b0;
`else
    jtag_dbg dbg (
        .clk(sys_clk), .addr(dbg_addr), .wdata(dbg_wdata), .re(dbg_re), .we(dbg_we),
        .rdata(dbg_is_ddr ? jtag_ddr_rdata : dbg_is_reg ? dbg_reg :
               dbg_is_con ? con_rdata : dbg_is_edge ? bk_rdata : dbg_is_trx ? trx_j_rdata :
               dbg_is_ram ? {24'd0, ram_rdata} : 32'd0),
        .rvalid(dbg_rvalid_fast || (dbg_is_ddr && jtag_ddr_done) || (dbg_is_trx && trx_j_rvalid))
    );
`endif

    // the RISC-V console, read through the debug port
    con_ring con (
        .wclk(cpu_clk), .we(con_valid), .wdata(con_data),
        .rclk(sys_clk), .raddr(dbg_addr[11:2]), .rdata(con_rdata), .count(con_count)
    );

    // ---- UDP video stream (step D) -------------------------------------------------
    udp_stream vstream (
        .vclk(sys_clk), .pix(pix_color), .px(pix_x), .py(pix_y), .v60(v60),
        .cpu_clk(cpu_clk), .tpl_we(udp_we && udp_addr[7:6] == 2'd0), .tpl_addr(udp_addr[5:0]),
        .tpl_wdata(udp_wdata),
        .eth_clk(clk50), .eth_rst(eth_rst), .enable(stream_ena[0]),
        .tx_req(vs_req), .tx_grant(vs_grant), .tx_data(vs_data), .tx_valid(vs_valid),
        .tx_eof(vs_eof), .tx_ready(vs_ready), .frames_sent(st_frames)
    );
    // audio stream (T13), sharing the stream port with the video
    udp_audio astream (
        .snd_clk(mclk), .snd_en(audio_sample_en), .in_l(snd_l), .in_r(snd_r),
        .cpu_clk(cpu_clk), .tpl_we(udp_we && udp_addr[7:6] == 2'd1), .tpl_addr(udp_addr[5:0]),
        .tpl_wdata(udp_wdata),
        .eth_clk(clk50), .eth_rst(eth_rst), .enable(stream_ena[1]),
        .tx_req(as_req), .tx_grant(as_grant), .tx_data(as_data), .tx_valid(as_valid),
        .tx_eof(as_eof), .tx_ready(as_ready), .pkts_sent(as_pkts)
    );
    stream_mux smux (
        .clk(clk50), .rst(eth_rst),
        .a_req(vs_req), .b_req(as_req), .a_grant(vs_grant), .b_grant(as_grant),
        .a_data(vs_data), .b_data(as_data), .a_valid(vs_valid), .b_valid(as_valid),
        .a_eof(vs_eof), .b_eof(as_eof), .a_ready(vs_ready), .b_ready(as_ready),
        .req(st_req), .grant(own_st), .data(st_tx_data), .valid(st_tx_valid), .eof(st_tx_eof),
        .ready(own_st & x_ready)
    );

    // ---- the app's C64 control (step C), into the C64 clock ------------------------
    // (CART_TYPE is static around the reset and the force that take it)
    (* ASYNC_REG = "TRUE" *) reg [5:0] app_s1 = 6'b000000, app_s2 = 6'b000000;
    (* ASYNC_REG = "TRUE" *) reg [7:0] cart_type_s1 = 8'h00, cart_type_s2 = 8'h00;
    (* ASYNC_REG = "TRUE" *) reg [3:0] reu_s1 = 4'h0, reu_s2 = 4'h0;    // {REU_SIZE[2:0], REU_ENABLE} (T60)
    always @(posedge sys_clk) begin
        app_s1 <= {app_ultimax, 3'b000, app_c64_pause, app_c64_reset};
        app_s2 <= app_s1;
        c64_mute <= c64_halted | app_s2[1];
        cart_type_s1 <= app_cart_type;
        cart_type_s2 <= cart_type_s1;
        reu_s1 <= {app_reu_size, app_reu_en};
        reu_s2 <= reu_s1;
    end
    // The app's C64_STOP pauses the whole machine (T12 B) only with GATE_CTRL
    // b0 (trx_sys.v) and JTAG register 9 bit 4 clear; otherwise it pauses
    // only the CPU, as before T12 B (the reset value, until our firmware
    // draws the freeze menu: T12 P2).
    wire        gate_frz;
    wire        app_pause = gate_frz && !machine_ctl[4];
    wire        dbg_own;                    // the debugger owns run control (trx_dbg.v OWNER)
    wire        dbg_hold, dbg_rel;          // (dbg_stop is declared with the DMA window's wires)
    wire        app_stop = app_s2[1] & !dbg_own;
    wire        c64_halt_req = machine_ctl[3] | (app_stop & app_pause);
    // T40: a System Mode switch halts the machine too, unseen by the DMA window
    (* ASYNC_REG = "TRUE" *) reg [1:0] mc_halt_s = 2'b00;
    (* ASYNC_REG = "TRUE" *) reg [1:0] vmode_s1 = 2'd0, vmode_sys = 2'd0;
    always @(posedge sys_clk) begin
        mc_halt_s <= {mc_halt_s[0], mc_halt};
        vmode_s1  <= vmode;
        vmode_sys <= vmode_s1;
    end
    // the C64 keyboard and joysticks: the physical ones (keys_s2, pins_s2,
    // bl_restore) with the app's (T38); the physical ones stay on their own
    wire [63:0] c64_keys;
    wire  [4:0] c64_joy1, c64_joy2;
    wire        c64_restore, c64_app_reset, c64_freeze;
    app_input ain (
        .clk(sys_clk), .ovl_own(ovl_own_s[1]), .phys_keys(keys_s2),
        .phys_joy1(pins_s2[4:0]), .phys_joy2(pins_s2[9:5]), .phys_restore(pins_s2[10] | bl_restore),
        .app_keys(app_keys), .app_joy1(app_joy1), .app_joy2(app_joy2),
        .app_restore(app_restore), .app_reset(app_reset), .app_freeze(app_freeze),
        .keys(c64_keys), .joy1(c64_joy1), .joy2(c64_joy2), .restore(c64_restore),
        .reset(c64_app_reset), .freeze(c64_freeze)
    );
    wire        c64_reset_req = machine_ctl[0] | app_s2[0] | c64_app_reset;
    // DMA window and lens unit (dma_win.v): a byte goes to the C64 RAM's second port whenever
    // the debug port does not use it in that clock, or with the C64 stopped becomes a C64
    // bus cycle; a byte of the app's ROM apertures (dma_rom) goes to the ROM block through
    // the same port
    wire        win_port_use, win_port_rom, win_port_we;
    wire [15:0] win_port_addr;
    wire  [7:0] win_port_wdata;
    wire        dma_cyc_req, dma_cyc_we, dma_cyc_done;
    wire [15:0] dma_cyc_addr;
    wire  [7:0] dma_cyc_wdata, dma_cyc_rdata;
    wire        core_ultimax;
    wire  [1:0] dma_cyc_force;
    wire        lk_go;
    wire [18:0] lk_req;
    wire [31:0] lk_res;
    wire [11:0] sb_addr;
    wire [31:0] sb_rdata;
    wire        sb_frozen;
    wire  [9:0] cram_addr, win_cram_addr, tx_cram_addr;
    wire        cram_we, win_cram_use;
    wire  [3:0] cram_wdata, cram_rdata;
    wire [15:0] tx_view;
    wire        tx_req, tx_rom;
    wire [15:0] tx_paddr;
    wire [15:0] pk_addr;
    wire  [1:0] pk_cls;
    wire        pk_hit, pk_mem, pk_ext;
    wire        pk_on, pk_cram, pk_open, pk_wait;   // U13: the I/O look
    wire [23:0] pk_maddr;
    wire  [7:0] pk_data;
    wire  [7:0] cap_every;
    wire [15:0] cap_gen, cap_nosnap;
    dma_win win (
        .clk(sys_clk),
        .dma_req_tgl(dma_req_tgl), .dma_addr(dma_addr), .dma_we(dma_we), .dma_wdata(dma_wdata),
        .dma_bus(dma_bus), .dma_rom(dma_rom), .dma_done_tgl(dma_done_tgl), .dma_rdata(dma_rdata_q),
        .dbg_busy(dbg_re || dbg_we), .port_use(win_port_use), .port_addr(win_port_addr),
        .port_rom(win_port_rom), .port_we(win_port_we), .port_wdata(win_port_wdata),
        .port_rdata(ram_rdata),
        .cyc_req(dma_cyc_req), .cyc_addr(dma_cyc_addr), .cyc_we(dma_cyc_we), .cyc_wdata(dma_cyc_wdata),
        .cram_addr(win_cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata),
        .cram_use(win_cram_use),
        .cyc_done(dma_cyc_done), .cyc_rdata(dma_cyc_rdata), .cyc_force(dma_cyc_force),
        .halted(c64_halted), .app_ultimax(app_s2[5]), .core_ultimax(core_ultimax),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .cart_ext(pk_ext),
        .pk_on(pk_on), .pk_cram(pk_cram), .pk_open(pk_open), .pk_wait(pk_wait),
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
        .pkd_tgl(pkd_tgl), .pkd_addr(pkd_addr), .pkd_ack_tgl(pkd_ack_tgl), .pkd_data(pkd_data)
    );
    assign core_reset_req = c64_reset_req;
    // The RAM's second port: the window, then JTAG, then the text layer's reads
    // (c64_text.v), then APPLY, in the clocks the ones before do not use. The
    // colour RAM's second port: the window's aperture in the clocks it uses it,
    // the text layer's in the others.
    wire        tx_go      = tx_req && !win_port_use && !(dbg_re || dbg_we);
    // the engine's APPLY (trx_engine.v, T20 §17.3) last: a byte in a clock none of them uses
    wire        ap_req, ap_gnt;
    wire [15:0] ap_addr;
    wire  [7:0] ap_wdata;
    assign      ap_gnt     = ap_req && !win_port_use && !tx_go && !(dbg_re || dbg_we);
    wire        port_req   = win_port_use || tx_go || ap_gnt || dbg_re || dbg_we;   // c64_mem's second port wanted (T29)
    wire [15:0] port_addr  = win_port_use ? win_port_addr : tx_go ? tx_paddr : ap_gnt ? ap_addr : dbg_addr[15:0];
    wire        port_we    = win_port_use ? win_port_we : ap_gnt || (dbg_we && dbg_addr[23:16] == 8'h00);
    wire [7:0]  port_wdata = win_port_use ? win_port_wdata : ap_gnt ? ap_wdata : dbg_wdata[7:0];
    wire        port_rom   = win_port_use ? win_port_rom : tx_go && tx_rom;
    assign cram_addr = win_cram_use ? win_cram_addr : tx_cram_addr;

    // the engine's side of c64_core and trx_sys (trx_engine below)
    wire        log_on, seq_busy, mk, mk_snap, mk_cap, mk_skip, mk_intg, reu_busy, phi2_tick;
    // debug (trx_dbg.v in c64_core, drv_dbg.v): page 3 / page 0 words from trx_sys
    wire        dr_we, dr_re, dbg_ev, dbg_log_put;
    wire [10:0] dr_addr;
    wire [31:0] dr_wdata, dbg_dr_rdata;
    wire [63:0] dbg_log_rec;
    wire        tr_ib;                      // the trace former's feed (T21 §4.11.1)
    wire [15:0] tr_pc;
    wire  [7:0] tr_op, tr_b1, tr_b2;
    wire [39:0] tr_regs;
    wire  [9:0] mk_x;
    wire  [8:0] mk_line;
    wire        p2_we, p2_re, seq_sb_req, seq_sb_gnt;
    wire  [9:0] p2_addr;
    wire [31:0] p2_wdata, p2_rdata;
    wire [11:0] seq_sb_addr;
    wire        wa_we, wc_we, wk_we;
    wire [15:0] wa_addr;
    wire  [7:0] wa_new, wa_old, wk_new;
    wire  [9:0] wc_addr;
    wire  [3:0] wc_new, wc_old;
    wire [14:0] wk_addr;
    // restore (trx_sys page 0/1 -> c64_core): staged writes, loads, stop position
    wire        sb_we, sb_ldreq, pos_set, pos_arm_in, pos_release, pos_arm, pos_hold;
    wire [11:0] sb_waddr;
    wire [31:0] sb_wdata;
    wire  [4:0] sb_ldmask, sb_ldarm;
    wire  [5:0] pos_cyc;
    wire [19:0] resume_phase, rp_pend;
    wire        rp_set;
    // the app's C64_BUS_INTERNAL/EXTERNAL into the C64 clock (quasi-static: the
    // port takes them only in reset or while paused)
    (* ASYNC_REG = "TRUE" *) reg [7:0] bus_m_s1 = 8'h0F, bus_m_s2 = 8'h0F;
    always @(posedge sys_clk) begin
        bus_m_s1 <= {bus_ext_cpu, bus_int_cpu};
        bus_m_s2 <= bus_m_s1;
    end
    // machine pause: JTAG bit 3, or the app's C64_STOP as above
    c64_core c64 (
        .turbo_en(turbo_en), .turbo_prefer(turbo_prefer), .turbo_upd_tgl(turbo_upd),
        .turbo_stat(turbo_stat), .turbo_w1(turbo_w1), .turbo_w2(turbo_w2), .turbo_ran_clr(turbo_ran_clr), .turbo_on(turbo_on),
        .dbg_req(port_req), .d7ff_ev(d7ff_ev), .d7ff_data(d7ff_data),
        .clk(sys_clk), .gclk(c64_gclk), .gate_ce(c64_ce), .rst(sys_rst),
        .halt_req(c64_halt_req | mc_halt_s[1]), .halted(c64_halted), .vmode(vmode_sys),
        .snd_clk(mclk), .snd_rst(mclk_rst),
        .audio_sample_en(audio_sample_en), .audio_l(audio_l), .audio_r(audio_r),
        .clk2x(sid2x_clk), .sid_cfg_base(sid_base), .sid_cfg_mask(sid_mask), .sid_cfg_split(sid_split),
        .sock_en(sock_en), .sock_base(sock_base), .sock_mask(sock_mask), .stereo_sel(stereo_sel),
        .sid_cfg_8580(sid_waves), .sid_cfg_res(sid_res), .sid_cfg_digi(sid_digi), .sid_cfg_rs8(sid_rs8), .sid_resid8(sid_resid8),
        .sid_cv_clk(cpu_clk), .sid_cv_we(sidcv_we), .sid_cv_addr(sidcv_addr), .sid_cv_wdata(sidcv_wdata),
        .sid_lv_addr(sid_lv_addr), .sid_lv_data(sid_lv_data), .sid_mix_gains(sid_gains),
        .sid_mix_gains2(sid_gains2), .smp_en(smp_en),
        .smp_a_tgl(smp_a_tgl), .smp_a_we(smp_a_we), .smp_a_addr(smp_a_addr), .smp_a_wdata(smp_a_wdata),
        .smp_a_rs_tgl(smp_a_rs_tgl), .smp_a_rdata(smp_a_rdata),
        .smp_f_req_tgl(smp_f_tgl), .smp_f_addr(smp_f_addr), .smp_f_done_tgl(smp_f_done_tgl), .smp_f_data(smp_f_data),
        .dbg_addr(port_addr), .dbg_rom(port_rom), .dbg_we(port_we),
        .dbg_wdata(port_wdata), .dbg_rdata(ram_rdata),
        .reu_cmd_addr(reu_cmd_addr), .reu_cmd_we(reu_cmd_we), .reu_cmd_valid(reu_cmd_valid),
        .reu_cmd_ready(reu_cmd_ready), .reu_wdata(reu_wdata), .reu_wdata_we(reu_wdata_we),
        .reu_rdata(reu_rdata), .reu_rdata_valid(reu_rdata_valid),
        .cpu_reset_req(core_reset_req),
        .cpu_pause_req(machine_ctl[1] | (app_stop & !app_pause) | uci_freeze),
        .reu_en(reu_s2[0]), .reu_size(reu_s2[3:1]),
        .cart_type(cart_type_s2), .cart_kill_tgl(app_cart_kill_tgl),
        .cart_force_tgl(app_cart_force_tgl), .cart_active(cart_active),
        .cart_wclk(ui_clk), .cart_req_tgl(cart_req_tgl), .cart_req_we(cart_req_we),
        .cart_req_addr(cart_req_addr), .cart_req_data(cart_req_data), .cart_req_slot(cart_req_slot),
        .cart_req_cached(cart_req_cached), .cart_ack_tgl(cart_ack_tgl),
        .cart_fill_be(cart_fill_be), .cart_fill_addr(cart_fill_addr), .cart_fill_data(cart_fill_data),
        .cart_fl_enable(cart_fl_enable), .cart_fl_magic1(cart_fl_magic1), .cart_fl_magic2(cart_fl_magic2),
        .cart_fl_mask(cart_fl_mask), .cart_fl_id(cart_fl_id), .cart_desc(cart_desc), .cart_evt_tgl(cart_evt_tgl),
        .cart_evt_word(cart_evt_word), .cart_fl_ack_tgl(cart_fl_ack_tgl), .cart_inval_tgl(cart_inval_tgl),
        .cart_ee_tgl(cart_ee_tgl), .cart_ee_clk(cpu_clk), .cart_ee_addr(cart_ee_addr), .cart_ee_we(cart_ee_we),
        .cart_ee_wdata(cart_ee_wdata), .cart_ee_rdata(cart_ee_rdata),
        .dma_req(dma_cyc_req), .dma_addr(dma_cyc_addr), .dma_we(dma_cyc_we), .dma_wdata(dma_cyc_wdata),
        .dma_done(dma_cyc_done), .dma_rdata(dma_cyc_rdata), .ultimax(core_ultimax),
        .dma_force(dma_cyc_force),
        .pix_color(pix_color), .pix_x(pix_x), .pix_y(pix_y), .leds(c64_leds),
        .keys(c64_keys), .joy1(c64_joy1), .joy2(c64_joy2), .restore(c64_restore), .freeze_btn(c64_freeze),
        .iec_atn_o(c64_atn_o), .iec_clk_o(c64_clk_o), .iec_data_o(c64_data_o),
        .iec_atn_i(c64_atn_i), .iec_clk_i(c64_clk_i), .iec_data_i(c64_data_i),
        .sb_addr(sb_addr), .sb_rdata(sb_rdata), .sb_frozen(sb_frozen),
        .cap_every(cap_every), .cap_gen(cap_gen), .cap_tgl(cap_tgl), .cap_nosnap(cap_nosnap),
        .log_on(log_on && !turbo_on), .cap_block(seq_busy), .mk(mk), .mk_snap(mk_snap), .mk_cap(mk_cap),
        .mk_skip(mk_skip), .mk_intg(mk_intg), .reu_busy(reu_busy), .mk_x(mk_x), .mk_line(mk_line),
        .phi2_tick(phi2_tick),
        .wa_we(wa_we), .wa_addr(wa_addr), .wa_new(wa_new), .wa_old(wa_old),
        .wc_we(wc_we), .wc_addr(wc_addr), .wc_new(wc_new), .wc_old(wc_old),
        .wk_we(wk_we), .wk_addr(wk_addr), .wk_new(wk_new),
        .sb_we(sb_we), .sb_waddr(sb_waddr), .sb_wdata(sb_wdata), .sb_ldreq(sb_ldreq),
        .sb_ldmask(sb_ldmask), .sb_ldarm(sb_ldarm), .pos_set(pos_set), .pos_arm_in(pos_arm_in),
        .pos_cyc(pos_cyc), .pos_release(pos_release), .pos_arm(pos_arm), .pos_hold(pos_hold),
        .resume_phase(resume_phase), .rp_set(rp_set), .rp_pend(rp_pend),
        .cram_addr(cram_addr), .cram_we(cram_we), .cram_wdata(cram_wdata), .cram_rdata(cram_rdata),
        .bus_addr(uci_bus_addr), .io_ev(uci_ev), .io_ev_addr(uci_ev_addr), .io_ev_data(uci_ev_data),
        .io_ev_we(uci_ev_we), .io_ev_io(uci_ev_io), .io_oe(uci_oe), .io_dout(uci_dout),
        .io_irq(uci_c64_irq),
        .dr_we(dr_we), .dr_re(dr_re), .dr_addr(dr_addr), .dr_wdata(dr_wdata), .dr_rdata(dbg_dr_rdata),
        .dbg_own(dbg_own), .dbg_ev(dbg_ev), .dbg_hold(dbg_hold), .dbg_stop(dbg_stop), .dbg_rel(dbg_rel),
        .fw_stop(app_s2[1]), .drv_stop(drv_stop_ev),
        .drv_done(drv_done_ev), .dbg_log_put(dbg_log_put), .dbg_log_rec(dbg_log_rec),
        .tr_ib(tr_ib), .tr_pc(tr_pc), .tr_op(tr_op), .tr_b1(tr_b1), .tr_b2(tr_b2), .tr_regs(tr_regs),
        .slot_int(bus_m_s2[3:0]), .slot_ext(bus_m_s2[7:4]), .slot_wr_en(slot_wr_en),
        .slot_status(slot_status), .slot_cnt0(slot_cnt0), .slot_cnt1(slot_cnt1), .slot_last(slot_last),
        .slot_a(slot_a), .slot_ahi_t(slot_ahi_t), .slot_rw(slot_rw), .slot_d_o(slot_d_o),
        .slot_d_t(slot_d_t), .slot_d_i(SLOT_DATA),
        .slot_adir(SLOT_ADDRDIR), .slot_aoen(SLOT_ADDROEn), .slot_ddir(SLOT_DATADIR), .slot_doen(SLOT_DATAOEn),
        .slot_roml(SLOT_ROMLn), .slot_romh(SLOT_ROMHn), .slot_io1(SLOT_IO1n), .slot_io2(SLOT_IO2n),
        .slot_phi2(SLOT_PHI2), .slot_dotclk(SLOT_DOTCLK), .slot_ba(SLOT_BA), .slot_rst_o(slot_rst_o),
        .slot_sid1(slot_sid1), .slot_sid2(slot_sid2), .slot_sid_a(slot_sid_a),
        .slot_game(SLOT_GAMEn), .slot_exrom(SLOT_EXROMn), .slot_dma(SLOT_DMAn), .slot_irq(SLOT_IRQn),
        .slot_nmi(SLOT_NMIn), .slot_rst_i(SLOT_RST_I),
        .pk_addr(pk_addr), .pk_cls(pk_cls), .pk_hit(pk_hit), .pk_mem(pk_mem), .pk_maddr(pk_maddr),
        .pk_data(pk_data), .pk_ext(pk_ext),
        .pk_on(pk_on), .pk_cram(pk_cram), .pk_open(pk_open), .pk_wait(pk_wait)
    );

    drv_dbg ddbg (
        .clk(sys_clk), .r_we(dr_we), .r_addr(dr_addr), .r_wdata(dr_wdata), .r_rdata(drv_dr_rdata),
        .stop_ev(drv_stop_ev), .done_ev(drv_done_ev),
        .dclk(cpu_clk), .trace_valid(drv_trace_valid), .trace(drv_trace),
        .c64_halted(drv_halt_s[1]), .drive_halt(drv_dbg_halt)
    );

    // the engine (trx_engine.v, page 2): frame markers, the chip block of every
    // capture, the write log; its DDR2 side on mig_arb's engine client
    trx_engine eng (
        .clk(sys_clk),
        .r_we(p2_we), .r_re(p2_re), .r_addr(p2_addr), .r_wdata(p2_wdata), .r_rdata(p2_rdata),
        .log_on(log_on), .seq_busy(seq_busy), .mk(mk), .mk_snap(mk_snap), .mk_cap(mk_cap),
        .mk_skip(mk_skip), .mk_intg(mk_intg), .reu_busy(reu_busy), .mk_x(mk_x), .mk_line(mk_line),
        .phi2_tick(phi2_tick), .cap_gen(cap_gen), .dbg_put(dbg_log_put), .dbg_rec(dbg_log_rec),
        .tr_ib(tr_ib), .tr_pc(tr_pc), .tr_op(tr_op), .tr_b1(tr_b1), .tr_b2(tr_b2), .tr_regs(tr_regs),
        .sb_req(seq_sb_req), .sb_addr(seq_sb_addr), .sb_gnt(seq_sb_gnt), .sb_rdata(sb_rdata),
        // the second ports' writes come from outside the machine (JTAG, the
        // app's window); their old values are the ports' read data
        .wa_we(wa_we), .wa_addr(wa_addr), .wa_new(wa_new), .wa_old(wa_old),
        .wb_we(port_we && !port_rom && !ap_gnt), .wb_addr(port_addr), .wb_new(port_wdata), .wb_old(ram_rdata),
        .wc_we(wc_we), .wc_addr(wc_addr), .wc_new(wc_new), .wc_old(wc_old),
        .wd_we(cram_we), .wd_addr(cram_addr), .wd_new(cram_wdata), .wd_old(cram_rdata),
        .wk_we(wk_we), .wk_addr(wk_addr), .wk_new(wk_new), .cart_rbw(cart_rbw),
        .ap_req(ap_req), .ap_addr(ap_addr), .ap_wdata(ap_wdata), .ap_gnt(ap_gnt),
        .eng_tgl(eng_tgl), .gap_tgl(gap_tgl),
        .ui_clk(ui_clk), .ui_rst(ui_rst || !ddr_calib),
        .e_addr(e_addr), .e_cmd(e_cmd), .e_en(e_en), .e_rdy(e_rdy), .e_wdf_data(e_wdf_data),
        .e_wdf_mask(e_wdf_mask), .e_wdf_wren(e_wdf_wren), .e_rd_data(c_rd_data),
        .e_rd_data_valid(e_rd_valid), .ob_v(ob_v), .ob_addr(ob_addr), .ob_old(ob_old)
    );

    // our IO block's pages in this clock (trx_sys.v): gate and capture
    // control, state bus, lens unit, text layer view; from the app through trx_io, from
    // JTAG through the JTAG page
    reg         c64_halted_q = 1'b0;
    always @(posedge sys_clk) begin
        c64_halted_q <= c64_halted;
        if ((c64_halted && !c64_halted_q) || dbg_ev) stop_tgl <= ~stop_tgl;
    end
    trx_sys tsys (
        .clk(sys_clk), .turbo_stat(turbo_stat), .turbo_w1(turbo_w1), .turbo_w2(turbo_w2), .turbo_ran_clr(turbo_ran_clr),
        .rq_tgl(trx_rq_tgl), .rq_addr(trx_rq_addr), .rq_we(trx_rq_we), .rq_wdata(trx_rq_wdata),
        .rs_tgl(trx_rs_tgl), .rs_rdata(trx_rs_rdata),
        .j_re(dbg_re && dbg_trx_sel), .j_we(dbg_we && dbg_trx_sel), .j_addr(dbg_addr[11:0]),
        .j_wdata(dbg_wdata), .j_rdata(trx_j_rdata), .j_rvalid(trx_j_rvalid),
        .gate_frz(gate_frz), .cap_every(cap_every),
        .slot_wr_en(slot_wr_en), .slot_status(slot_status), .slot_cnt0(slot_cnt0),
        .slot_cnt1(slot_cnt1), .slot_last(slot_last),
        .lk_go(lk_go), .lk_req(lk_req), .lk_res(lk_res),
        .sb_we(sb_we), .sb_waddr(sb_waddr), .sb_wdata(sb_wdata), .sb_ldreq(sb_ldreq),
        .sb_ldmask(sb_ldmask), .sb_ldarm(sb_ldarm), .pos_set(pos_set), .pos_arm_in(pos_arm_in),
        .pos_cyc(pos_cyc), .pos_release(pos_release), .pos_arm(pos_arm), .pos_hold(pos_hold),
        .resume_phase(resume_phase), .rp_set(rp_set), .rp_pend(rp_pend),
        .p2_we(p2_we), .p2_re(p2_re), .p2_addr(p2_addr), .p2_wdata(p2_wdata), .p2_rdata(p2_rdata),
        .dr_we(dr_we), .dr_re(dr_re), .dr_addr(dr_addr), .dr_wdata(dr_wdata),
        .dr_rdata(dbg_dr_rdata | drv_dr_rdata),
        .seq_sb_req(seq_sb_req),
        .seq_sb_addr(seq_sb_addr), .seq_sb_gnt(seq_sb_gnt), .sb_addr(sb_addr), .sb_rdata(sb_rdata),
        .sb_frozen(sb_frozen), .halted(c64_halted), .cap_gen(cap_gen), .cap_nosnap(cap_nosnap),
        .tx_view(tx_view)
    );

    // ---- HDMI with audio (U16) -----------------------------------------------------------
    // The GTPs run on usrclk = pix_clk / 2. An MMCM makes pix_clk = the TMDS clock (x2) from it, phase-locked: 122.7-149.1 MHz over the
    // rows of U16 D2 (tools/hdmi_modes.py). A pixel lasts `gear` pix_clk (1, 2, 3 or 5); every register of the picture chain after
    // hdmi_timing changes only when its ce is high, the encoder too, and tmds_rep.v sends each TMDS bit gear times.
    wire        usrclk, gt_ready;
    wire        pfb, pfb_bufg, pix_mmcm, pix_locked;
    MMCME2_BASE #(
        .CLKIN1_PERIOD(13.194), .DIVCLK_DIVIDE(1), .CLKFBOUT_MULT_F(12.0),  // VCO 6 x the TMDS clock: 736-895 MHz over the rows
        .CLKOUT0_DIVIDE_F(6.0)                                             // = the TMDS clock (period: 1080p50 PAL, the fastest)
    ) mmcm_pix (
        .CLKIN1(usrclk), .CLKFBIN(pfb_bufg), .CLKFBOUT(pfb), .CLKOUT0(pix_mmcm),
        .LOCKED(pix_locked), .RST(~gt_ready), .PWRDWN(1'b0),
        .CLKOUT0B(), .CLKOUT1(), .CLKOUT1B(), .CLKOUT2(), .CLKOUT2B(), .CLKOUT3(),
        .CLKOUT3B(), .CLKOUT4(), .CLKOUT5(), .CLKOUT6(), .CLKFBOUTB()
    );
    BUFG bg_pfb (.I(pfb), .O(pfb_bufg));            // feedback through a BUFG: in phase with usrclk
    BUFG bg_pix (.I(pix_mmcm), .O(pix_clk));
    rst_sync rs_pix (.clk(pix_clk), .arst(~pix_locked), .rst(pix_rst));

    // the mode request and the scale (cpu_clk, the overlay window 0x4F00 / 0x4F01, status 0x4F04-0x4F0A), the synthesizer's row
    // (clk50, cdce_mode above), the timing's mode (pix_clk)
    wire [3:0]  mode_pix, m_mode;
    wire [1:0]  scale_pix;
    wire        running, pce, fstart;
    wire [7:0]  mr_rdata, o_rdata;
    wire        mr_rhit;
    assign ovl_rdata = mr_rhit ? mr_rdata : o_rdata;
    mode_req mreq (
        .clk_sys(cpu_clk), .we(ovl_we), .addr(ovl_addr), .wdata(ovl_wdata), .rdata(mr_rdata), .rhit(mr_rhit),
        .clk50(clk50), .fam_v60(mc_cdce_v60), .vm_mode(vm_mode), .vm_rst(vm_rst),
        .clk_pix(pix_clk), .mode_pix(mode_pix), .fstart(fstart), .scale_pix(scale_pix),
        .gt_ready(gt_ready), .pix_locked(pix_locked), .running(running), .mode_q(m_mode)
    );

    wire        align, t_de, t_hs, t_vs;
    wire [11:0] t_cx, m_hact, m_xact0, m_htot_m1;
    wire [10:0] t_cy, m_vact, m_yact0;
    wire  [3:0] m_gear;
    wire  [6:0] m_vic;
    hdmi_timing mtim (
        .clk(pix_clk), .rst(pix_rst), .mode(mode_pix), .align(align), .valid(), .aligned(),
        .ce(pce), .rgb(), .de(t_de), .hs(t_hs), .vs(t_vs), .cx(t_cx), .cy(t_cy), .running(running),
        .hact(m_hact), .vact(m_vact), .xact0(m_xact0), .yact0(m_yact0), .gear(m_gear), .htot_m1(m_htot_m1), .vtot_m1(),
        .vic(m_vic), .rate(), .sx_e(), .fw_m1(), .sy_e(), .ey_e(), .ph_e(), .mode_q(m_mode)
    );
    assign fstart = pce && t_cx == 12'd0 && t_cy == 11'd0;

    // the VIC picture on the timing's counters, at the scale in use
    wire [23:0] s_rgb, rgb;
    wire        s_de, s_hs, s_vs, de, hs, vs, video_locked;
    wire [11:0] s_cx, cx;
    wire [10:0] s_cy, cy;
    wire [81:0] geom;                               // hm_geom(m_mode, scale_pix): the scaler's and the text layer's geometry
    vic_scaler scaler (
        .vclk(sys_clk), .pix(pix_color), .px(pix_x), .py(pix_y),
        .clk(pix_clk), .rst(pix_rst), .ce(pce),
        .pal_clk(cpu_clk), .pal_we(pal_we), .pal_addr(pal_addr), .pal_wdata(pal_wdata),
        .mode(m_mode), .scale(scale_pix), .t_de(t_de), .t_hs(t_hs), .t_vs(t_vs), .t_cx(t_cx), .t_cy(t_cy),
        .xact0(m_xact0), .yact0(m_yact0), .hact(m_hact), .vact(m_vact), .htot_m1(m_htot_m1), .geom(geom), .align(align),
        .rgb(s_rgb), .de(s_de), .hs(s_hs), .vs(s_vs), .cx(s_cx), .cy(s_cy), .locked(video_locked),
        .blank(c64_halted), .x_text(x_text), .scan(scanlines)
    );

    // the app's menu (U64-II overlay, 0x10140000) on top of the picture, centred on the active area
    wire [23:0] o_rgb;
    wire        o_de, o_hs, o_vs;
    wire [11:0] o_cx;
    wire [10:0] o_cy;
    overlay ovl (
        .clk_sys(cpu_clk), .we(ovl_we), .addr(ovl_addr), .wdata(ovl_wdata), .rdata(o_rdata),
        .clk(pix_clk), .ce(pce), .hact(m_hact), .vact(m_vact), .xact0(m_xact0), .yact0(m_yact0),
        .rgb_in(s_rgb), .de_in(s_de), .hs_in(s_hs), .vs_in(s_vs),
        .cx_in(s_cx), .cy_in(s_cy),
        .rgb(o_rgb), .de(o_de), .hs(o_hs), .vs(o_vs), .cx(o_cx), .cy(o_cy),
        .own_keyboard(ovl_own)
    );

    // the pixel layer (DDR2, RGB565) on top of that, and the still capture
    // (trx_io page 6, mig_arb's video client)
    wire [23:0] p_rgb;
    wire        p_de, p_hs, p_vs;
    wire [11:0] p_cx;
    wire [10:0] p_cy;
    pix_layer pxl (
        .clk_sys(cpu_clk), .we(px_we), .re(px_re), .addr(cw_addr[11:0]), .wdata(cw_wdata),
        .rdata(pxl_rdata), .ev_vsync(px_ev_vsync), .ev_flip(px_ev_flip),
        .ui_clk(ui_clk), .ui_rst(ui_rst || !ddr_calib), .v_addr(v_addr), .v_cmd(v_cmd), .v_en(v_en),
        .v_rdy(v_rdy), .v_wdf_data(v_wdf_data), .v_wdf_mask(v_wdf_mask), .v_wdf_wren(v_wdf_wren),
        .v_rd_data(c_rd_data), .v_rd_data_valid(v_rd_valid), .v_urgent(v_urgent),
        .vclk(sys_clk), .pix(pix_color), .px(pix_x), .py(pix_y), .v60(v60),
        .clk(pix_clk), .ce(pce), .hact(m_hact), .vact(m_vact), .xact0(m_xact0), .yact0(m_yact0),
        .rgb_in(o_rgb), .de_in(o_de), .hs_in(o_hs), .vs_in(o_vs), .cx_in(o_cx), .cy_in(o_cy),
        .rgb(p_rgb), .de(p_de), .hs(p_hs), .vs(p_vs), .cx(p_cx), .cy(p_cy)
    );

    // the freeze menu's C64 text on top: the still the app drew on the pixel
    // layer stays below it (trx_io page 6 from 0x0C0); its area and steps are the scale's (hm_geom)
    c64_text txt (
        .clk_sys(cpu_clk), .we(px_we), .re(px_re), .addr(cw_addr[11:0]), .wdata(cw_wdata),
        .rdata(tx_rdata),
        .cclk(sys_clk), .halted(c64_halted), .vic_view(tx_view),
        .pal_clk(cpu_clk), .pal_we(pal_we), .pal_addr(pal_addr), .pal_wdata(pal_wdata),
        .port_req(tx_req), .port_addr(tx_paddr), .port_rom(tx_rom), .port_grant(tx_go),
        .port_rdata(ram_rdata), .cram_addr(tx_cram_addr), .cram_grant(!win_cram_use),
        .cram_rdata(cram_rdata),
        .clk(pix_clk), .ce(pce), .xact0(m_xact0), .yact0(m_yact0), .xt0(geom[22:11]), .yt0(geom[10:0]),
        .hn(geom[81:76]), .hd(geom[75:70]), .tvn(geom[57:52]), .tvd(geom[51:46]),
        .rgb_in(p_rgb), .de_in(p_de), .hs_in(p_hs), .vs_in(p_vs), .cx_in(p_cx), .cy_in(p_cy),
        .rgb(rgb), .de(de), .hs(hs), .vs(vs), .cx(cx), .cy(cy)
    );

    // the encoder's pixel clock rate for its 48 kHz audio tick and N / CTS: the row's (hm_rate) in the family's first System Mode
    // (PAL, NTSC), hm_rate_alt in its second (NTSC-50, PAL-60; T40). The VIC code and the frame are the timing's.
    (* ASYNC_REG = "TRUE" *) reg [1:0] vm_p1 = 2'd0, vm_p = 2'd0;
    reg  [31:0] hdmi_rate = 32'd148869143;
    reg  [1:0]  hdmi_aspect = 2'd2;
    reg  [11:0] e_fwm1 = 12'd0;
    reg  [10:0] e_ey = 11'd0;
    always @(posedge pix_clk) begin
        vm_p1 <= vmode; vm_p <= vm_p1;
        hdmi_rate   <= (vm_p == 2'd2 || vm_p == 2'd3) ? hm_rate_alt(m_mode) : hm_rate(m_mode);
        hdmi_aspect <= hm_aspect(m_mode);
        e_fwm1      <= m_xact0 + m_hact - 12'd1;
        e_ey        <= m_yact0 + m_vact;
    end
    wire [9:0] tmds_0, tmds_1, tmds_2;
    // data islands: 3 packets a line fit into the shortest blanking (576p: 144 pixels; cx 4-112, the video preamble behind them)
    hdmi #(
        .DVI_OUTPUT(1'b0), .IT_CONTENT(1'b1), .VIDEO_ID_CODE(0),
        .VIDEO_RATE(149_138_494), .RUNTIME(1), .MAX_PACKETS(3),
        .AUDIO_RATE(48_000), .AUDIO_BIT_WIDTH(24),
        .BIT_WIDTH(12), .BIT_HEIGHT(11),
        .VENDOR_NAME(64'h5452583634000000),                        // "TRX64"
        .PRODUCT_DESCRIPTION(128'h43363420556C74696D61746500000000), // "C64 Ultimate"
        .SOURCE_DEVICE_INFORMATION(8'h08)
    ) hdmi_enc (
        .clk_pixel_x5(1'b0), .clk_pixel(pix_clk), .ce(pce | pix_rst), .reset(pix_rst),
        .rgb(rgb), .hsync(hs), .vsync(vs), .cx(cx), .cy(cy), .de(de),
        .vic_code(m_vic), .video_rate(hdmi_rate), .aspect(hdmi_aspect),
        .sx_e(m_xact0), .fw_m1(e_fwm1), .sy_e(m_yact0), .ey_e(e_ey),
        .audio_sample_word_0(hdmi_snd[47:24]), .audio_sample_word_1(hdmi_snd[23:0]),
        .audio_sample_en(hdmi_sample_en),
        .tmds_0(tmds_0), .tmds_1(tmds_1), .tmds_2(tmds_2)
    );

    // ---- analog audio: the same SID samples to the codec over I2S -------------
    wire i2s_bclk, i2s_lrck, i2s_data, i2s_spk;
    i2s_out i2s (
        .mclk(mclk), .frame_en(audio_sample_en), .in_l(snd_l), .in_r(snd_r),
        .spk_en(speaker_en), .spk_gain(mix_spk),
        .bclk(i2s_bclk), .lrck(i2s_lrck), .sdata(i2s_data), .spk(i2s_spk), .cnt(i2s_cnt)
    );

    // the same samples for HDMI: held here, taken over in the pixel clock (the
    // encoder sends them at its own 48 kHz; a pair is repeated or skipped now
    // and then, the two clocks being 0.005 % apart)
    reg [47:0] snd_hold;
    reg        snd_tgl = 1'b0;
    always @(posedge mclk)
        if (audio_sample_en) begin
            snd_hold <= {snd_l, snd_r};
            snd_tgl  <= ~snd_tgl;
        end
    (* ASYNC_REG = "TRUE" *) reg [2:0] snd_tgl_s = 3'b000;
    reg [47:0] hdmi_snd;
    always @(posedge pix_clk) begin
        snd_tgl_s <= {snd_tgl_s[1:0], snd_tgl};
        if (snd_tgl_s[2] != snd_tgl_s[1]) hdmi_snd <= snd_hold;
    end
    wire hdmi_sample_en;
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE")) fwd_mclk (.Q(AUDIO_MCLK), .C(mclk), .CE(1'b1),
        .D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0));
    assign AUDIO_BCLK  = i2s_bclk;
    assign AUDIO_LRCLK = i2s_lrck;
    assign AUDIO_SDI   = i2s_data;
    assign SPEAKER_DATA = i2s_spk;

    // the TMDS bit repeater (gear) and the GTPs: two TMDS clocks per 20-bit word, the older one in the low half (sent first)
    wire [19:0] w0, w1, w2, wc;
    tmds_rep rep (
        .clk(pix_clk), .usrclk(usrclk), .rst(pix_rst), .ce(pce), .gear(m_gear),
        .sym0(tmds_0), .sym1(tmds_1), .sym2(tmds_2), .w0(w0), .w1(w1), .w2(w2), .wc(wc)
    );
    hdmi_gtp gtp (
        .sysclk(clk50), .reset(gt_rst),
        .refclk_p(HDMI_REFCLK_P), .refclk_n(HDMI_REFCLK_N),
        // Lane 2 (TMDS 0, blue) is wired with P and N swapped on the board.
        .tx0(w2), .tx1(w1), .tx2(~w0),
        .tx3(wc),                           // the TMDS clock: one period per pixel (10 x gear line bits)
        .usrclk(usrclk), .ready(gt_ready),
        .txp({HDMI_CLK_P, HDMI_DATA_P[0], HDMI_DATA_P[1], HDMI_DATA_P[2]}),
        .txn({HDMI_CLK_N, HDMI_DATA_N[0], HDMI_DATA_N[1], HDMI_DATA_N[2]})
    );

    // T40: the HDMI output as measured: frames per second (clk50 window) and
    // pix_clk (TMDS clocks) per frame, from the timing's vsync (its leading edge in either polarity is a rising edge once a frame)
    reg        hm_vs_q = 1'b0, hm_tgl = 1'b0;
    reg [23:0] hm_pc = 24'd0, hm_pcf = 24'd0;
    always @(posedge pix_clk) begin
        hm_vs_q <= t_vs;
        hm_pc   <= hm_pc + 24'd1;
        if (s_vs && !hm_vs_q) begin hm_pcf <= hm_pc; hm_pc <= 24'd1; hm_tgl <= ~hm_tgl; end
    end
    (* ASYNC_REG = "TRUE" *) reg [2:0] hm_tgl_s = 3'b000;
    (* ASYNC_REG = "TRUE" *) reg [2:0] hm_lk1 = 3'b000, hm_lk = 3'b000;
    reg [25:0] hm_sec = 26'd0;
    reg [11:0] hm_fc = 12'd0, hm_fps = 12'd0;
    always @(posedge clk50) begin
        hm_tgl_s <= {hm_tgl_s[1:0], hm_tgl};
        hm_lk1 <= {gt_ready, pix_locked, video_locked}; hm_lk <= hm_lk1;
        hm_sec <= (hm_sec == 26'd49_999_999) ? 26'd0 : hm_sec + 26'd1;
        if (hm_sec == 26'd49_999_999) begin hm_fps <= hm_fc; hm_fc <= 12'd0; end
        else if (hm_tgl_s[2] != hm_tgl_s[1]) hm_fc <= hm_fc + 12'd1;
    end
    always @(posedge sys_clk) hdmi_meas_s <= {hm_lk, 1'b0, hm_fps, hm_pcf[23:8]};

    // ---- LEDs ------------------------------------------------------------------
    (* ASYNC_REG = "TRUE" *) reg [1:0] vlock_sync = 2'b00;
    always @(posedge clk50) vlock_sync <= {vlock_sync[0], video_locked};  // pix_clk -> clk50
    wire video_locked_s = vlock_sync[1];
    reg [25:0] blink = 0;
    always @(posedge clk50) blink <= blink + 1;
    assign LED_BOARDn = 1'b0;               // off: active high despite the name (green, on the FPGA module)
    // case LEDs: drive A = the app's 1541 (power always on, activity = its LED),
    // no drive B, no IRQ source yet
    (* ASYNC_REG = "TRUE" *) reg [2:0] cled_s1, cled_s2;
    always @(posedge cpu_clk) begin
        cled_s1 <= {!drive_leds_n[0], gt_ready & video_locked_s, c64_leds[0]};
        cled_s2 <= cled_s1;
    end
    wire led1n, led2n;
    case_leds cled (.clk(cpu_clk), .sel(caseled_sel), .sel_set(caseled_set),
                    .def1(cled_s2[0]), .def2(cled_s2[1]),
                    .a_pwr(1'b1), .b_pwr(1'b0), .a_act(cled_s2[2]), .b_act(1'b0),
                    .usb_act(busy_usb), .any_act(busy_any | cled_s2[2]), .irq_line(1'b0),
                    .led1n(led1n), .led2n(led2n));
    // The case LED pins are active high despite their names (Gideon's XDC):
    // boundary scan of his design shows H19/H17 high with the selector On and
    // low with Off (1541ultimate-tools pinmap notes, 2026-09-30).
    assign LED_CASE1n = !led1n;
    assign LED_CASE2n = !led2n;
    assign LED_CASE3n = 1'b0;
endmodule

// Asynchronous assert, synchronous release.
module rst_sync (
    input  wire clk,
    input  wire arst,
    output wire rst
);
    (* ASYNC_REG = "TRUE" *) reg [2:0] rst_sync = 3'b111;
    always @(posedge clk or posedge arst)
        if (arst) rst_sync <= 3'b111;
        else      rst_sync <= {rst_sync[1:0], 1'b0};
    assign rst = rst_sync[2];
endmodule
