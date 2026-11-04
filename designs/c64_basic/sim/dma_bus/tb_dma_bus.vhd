-- SPDX-License-Identifier: GPL-3.0-or-later
--------------------------------------------------------------------------------
-- tb_dma_bus (T38/T39): ultimate_sys with rvlite_bfm_input in place of the
-- RISC-V. The BFM writes MATRIX_KEYB, the joystick outputs and C64_MODE, and
-- checks the app_* ports through tb_dma_bus_pkg; it drives the reset sense and
-- checks C64_CLOCK_DETECT bit 4 and ITU interrupt bit 7. Port map generated
-- from the entity (inputs idle, outputs open).
--------------------------------------------------------------------------------
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.tb_dma_bus_pkg.all;

entity tb_dma_bus is
end entity;

architecture tb of tb_dma_bus is
    signal clock    : std_logic := '0';
    signal reset    : std_logic := '1';
    signal finished : boolean := false;
    signal dma_req_tgl, dma_we, dma_bus, dma_rom : std_logic;
    signal dma_addr : std_logic_vector(15 downto 0);
    signal dma_done_tgl : std_logic := '0';
    signal req_s : std_logic := '0';
begin
    clock <= not clock after 5 ns when not finished;
    reset <= '0' after 100 ns;

    dut: entity work.ultimate_sys
    port map (
        clock => clock,
        reset => reset,
        mem_request => open,
        mem_tag => open,
        mem_read_writen => open,
        mem_address => open,
        mem_byte_en => open,
        mem_wdata => open,
        mem_rdata => (others => '0'),
        mem_rack => '0',
        mem_rack_tag => (others => '0'),
        mem_dack_tag => (others => '0'),
        ddr_ready => '1',
        uart_txd => open,
        uart_rxd => '1',
        con_valid => open,
        con_data => open,
        eth_clock => clock,
        eth_reset => reset,
        eth_tx_data => open,
        eth_tx_eof => open,
        eth_tx_valid => open,
        eth_tx_ready => '0',
        eth_rx_data => (others => '0'),
        eth_rx_sof => '0',
        eth_rx_eof => '0',
        eth_rx_valid => '0',
        mdc => open,
        mdio_i => '1',
        mdio_o => open,
        wifi_txd => open,
        wifi_rxd => '1',
        wifi_rts => open,
        wifi_cts => '0',
        sd_ssn => open,
        sd_clk => open,
        sd_mosi => open,
        sd_miso => '1',
        sd_detectn => '1',
        flash_csn => open,
        flash_sck => open,
        flash_mosi => open,
        flash_miso => '1',
        keys => (others => '0'),
        joys => (others => '0'),
        app_keys => open,
        app_reset => open,
        app_restore => open,
        app_freeze => open,
        app_joy1 => open,
        app_joy2 => open,
        c64_reset_sense => '0',
        hdmi_hpd => '1',
        hdmi_ddc_en => open,
        c64_reset_req => open,
        c64_pause_req => open,
        c64_ultimax_req => open,
        cart_type => open,
        cart_kill_tgl => open,
        cart_force_tgl => open,
        cart_active => '0',
        dma_req_tgl => dma_req_tgl,
        dma_addr => dma_addr,
        dma_we => dma_we,
        dma_wdata => open,
        dma_bus => dma_bus,
        dma_rom => dma_rom,
        dma_done_tgl => dma_done_tgl,
        dma_rdata => (others => '0'),
        stream_ena => open,
        udp_we => open,
        udp_addr => open,
        udp_wdata => open,
        i2c_we => open,
        i2c_re => open,
        i2c_addr => open,
        i2c_wdata => open,
        i2c_rdata => (others => '0'),
        ovl_we => open,
        ovl_addr => open,
        ovl_wdata => open,
        pal_we => open,
        pal_addr => open,
        pal_wdata => open,
        ovl_rdata => (others => '0'),
        bl_we => open,
        bl_addr => open,
        bl_wdata => open,
        bl_rdata => (others => '0'),
        bl_irq => '0',
        cw_we => open,
        cw_re => open,
        cw_addr => open,
        cw_wdata => open,
        cw_rdata => (others => '0'),
        cw_ack => '0',
        cw_irq => '0',
        c64_clock => clock,
        uci_bus_addr => (others => '0'),
        uci_ev => '0',
        uci_ev_addr => (others => '0'),
        uci_ev_data => (others => '0'),
        uci_ev_we => '0',
        uci_ev_io => '0',
        uci_dout => open,
        uci_oe => open,
        uci_c64_irq => open,
        uci_freeze => open,
        caseled_sel => open,
        caseled_set => open,
        busy_any => open,
        busy_usb => open,
        speaker_en => open,
        mix_l => open,
        mix_r => open,
        mix_spk => open,
        speaker_vol => open,
        sid_base => open,
        sid_mask => open,
        sid_split => open,
        sid_waves => open,
        sid_res => open,
        sid_digi => open,
        sidcv_we => open,
        sidcv_addr => open,
        sidcv_wdata => open,
        sid_lv_addr => open,
        sid_lv_data => (others => '0'),
        sid_gains => open,
        sock_base => sk_base,
        sock_mask => sk_mask,
        sock_en => sk_en,
        stereo_sel => sk_sel,
        sock_gains => sk_gains,
        ulpi_clock => clock,
        ulpi_nxt => '0',
        ulpi_dir => '0',
        ulpi_stp => open,
        ulpi_data_i => (others => '0'),
        ulpi_data_o => open,
        ulpi_data_t => open,
        ulpi_reset_req => open,
        hub_reset_n => open,
        iec_atn_i => '1',
        iec_clk_i => '1',
        iec_data_i => '1',
        iec_clk_o => open,
        iec_data_o => open,
        drive_leds_n => open,
        drive_audio => open,
        drive_dbg => open,
        drive_halt => '0',
        drive_trace => open,
        drive_trace_valid => open,
        drv_mem_tgl => open,
        drv_mem_addr => open,
        drv_mem_we => open,
        drv_mem_wdata => open,
        drv_mem_be => open,
        drv_mem_done => '0',
        drv_mem_rdata => (others => '0') );

    -- the C64 side: every request is answered at once; what the window asked for is kept for the BFM
    process(clock)
    begin
        if rising_edge(clock) then
            req_s <= dma_req_tgl;
            dma_done_tgl <= dma_req_tgl;
            if dma_req_tgl /= req_s and not is_x(dma_req_tgl) then
                pk_cnt <= pk_cnt + 1;
                pk_bus <= dma_bus; pk_rom <= dma_rom; pk_we <= dma_we; pk_addr <= dma_addr;
            end if;
        end if;
    end process;

    process
    begin
        wait until bfm_step = 99 or now > 1 ms for 1 ms;
        if bfm_step = 99 and bfm_errors = 0 then
            report "PASS (" & time'image(now) & ")";
        else
            report "FAIL: bfm " & integer'image(bfm_errors) & ", step " & integer'image(bfm_step) severity failure;
        end if;
        finished <= true;
        wait;
    end process;
end architecture;
