-- SPDX-License-Identifier: GPL-3.0-or-later
-- drive_unit for the Verilator testbench (ghdl synth makes Verilog of it):
-- flat ports, the tick divider as in ultimate_sys, the system memory bus
-- with one master (the testbench in the role of the RISC-V) that drive_unit
-- snoops, and drive_unit's own DDR2 port. tb_drive_top puts cpu_mem_bridge
-- and mig_arb behind them; the testbench is the IO bus.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.io_bus_pkg.all;
use work.mem_bus_pkg.all;

entity drive_flat is
port (
    clock           : in  std_logic;
    reset           : in  std_logic;

    io_read         : in  std_logic;
    io_write        : in  std_logic;
    io_addr         : in  std_logic_vector(15 downto 0);
    io_wdata        : in  std_logic_vector(7 downto 0);
    io_rdata        : out std_logic_vector(7 downto 0);
    io_ack          : out std_logic;

    host_request    : in  std_logic;            -- tag 0x21, like the RISC-V's data port
    host_rwn        : in  std_logic;
    host_address    : in  std_logic_vector(25 downto 0);
    host_byte_en    : in  std_logic_vector(3 downto 0);
    host_wdata      : in  std_logic_vector(31 downto 0);

    mem_request     : out std_logic;
    mem_tag         : out std_logic_vector(7 downto 0);
    mem_rwn         : out std_logic;
    mem_address     : out std_logic_vector(25 downto 0);
    mem_byte_en     : out std_logic_vector(3 downto 0);
    mem_wdata       : out std_logic_vector(31 downto 0);
    mem_rdata       : in  std_logic_vector(31 downto 0);
    mem_rack        : in  std_logic;
    mem_rack_tag    : in  std_logic_vector(7 downto 0);
    mem_dack_tag    : in  std_logic_vector(7 downto 0);

    drive_stop      : in  std_logic;
    drive_halt      : in  std_logic;            -- machine pause (drive_unit's halt)
    c64_reset_n     : in  std_logic;
    iec_atn_i       : in  std_logic;
    iec_clk_i       : in  std_logic;
    iec_data_i      : in  std_logic;
    iec_clk_o       : out std_logic;
    iec_data_o      : out std_logic;
    leds_n          : out std_logic_vector(2 downto 0);
    drive_dbg       : out std_logic_vector(255 downto 0);   -- drive_unit's dbg
    cpu_trace       : out std_logic_vector(31 downto 0);    -- drive_unit's cpu_trace
    cpu_trace_valid : out std_logic;

    dp_req_tgl      : out std_logic;
    dp_addr         : out std_logic_vector(25 downto 0);
    dp_we           : out std_logic;
    dp_wdata        : out std_logic_vector(31 downto 0);
    dp_be           : out std_logic_vector(3 downto 0);
    dp_done_tgl     : in  std_logic;
    dp_rdata        : in  std_logic_vector(127 downto 0) );
end entity;

architecture rtl of drive_flat is
    signal tick_16MHz, tick_4MHz, tick_1kHz : std_logic;
    signal io_req       : t_io_req;
    signal io_resp      : t_io_resp;
    signal mem_req      : t_mem_req_32;
    signal mem_resp     : t_mem_resp_32;
begin
    i_ticks: entity work.fractional_div
    generic map (
        g_numerator   => 4,
        g_denominator => 25 )
    port map (
        clock         => clock,
        tick          => tick_16MHz,
        tick_by_4     => tick_4MHz,
        tick_by_16    => open,
        one_16000     => tick_1kHz );

    io_req.read    <= io_read;
    io_req.write   <= io_write;
    io_req.address <= X"00" & unsigned(io_addr);
    io_req.data    <= io_wdata;
    io_rdata       <= io_resp.data;
    io_ack         <= io_resp.ack;

    mem_req.request     <= host_request;
    mem_req.tag         <= X"21";
    mem_req.read_writen <= host_rwn;
    mem_req.address     <= unsigned(host_address);
    mem_req.byte_en     <= host_byte_en;
    mem_req.data        <= host_wdata;

    mem_request  <= mem_req.request;
    mem_tag      <= mem_req.tag;
    mem_rwn      <= mem_req.read_writen;
    mem_address  <= std_logic_vector(mem_req.address);
    mem_byte_en  <= mem_req.byte_en;
    mem_wdata    <= mem_req.data;
    mem_resp.data     <= mem_rdata;
    mem_resp.rack     <= mem_rack;
    mem_resp.rack_tag <= mem_rack_tag;
    mem_resp.dack_tag <= mem_dack_tag;

    i_drive: entity work.drive_unit
    port map (
        clock        => clock,
        reset        => reset,
        tick_16MHz   => tick_16MHz,
        tick_4MHz    => tick_4MHz,
        tick_1kHz    => tick_1kHz,
        io_req       => io_req,
        io_resp      => io_resp,
        dp_req_tgl   => dp_req_tgl,
        dp_addr      => dp_addr,
        dp_we        => dp_we,
        dp_wdata     => dp_wdata,
        dp_be        => dp_be,
        dp_done_tgl  => dp_done_tgl,
        dp_rdata     => dp_rdata,
        snoop_req    => mem_req,
        snoop_resp   => mem_resp,
        drive_stop   => drive_stop,
        halt         => drive_halt,
        c64_reset_n  => c64_reset_n,
        iec_atn_i    => iec_atn_i,
        iec_clk_i    => iec_clk_i,
        iec_data_i   => iec_data_i,
        iec_clk_o    => iec_clk_o,
        iec_data_o   => iec_data_o,
        act_led_n    => leds_n(0),
        motor_led_n  => leds_n(1),
        dirty_led_n  => leds_n(2),
        audio_sample => open,
        dbg          => drive_dbg,
        cpu_trace    => cpu_trace,
        cpu_trace_valid => cpu_trace_valid );
end architecture;
