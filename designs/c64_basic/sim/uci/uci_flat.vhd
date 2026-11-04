-- SPDX-License-Identifier: GPL-3.0-or-later
-- uci_unit with its IO bus records as plain ports, for ghdl synth to Verilog
-- (tb_uci_top.v under Verilator).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

library work;
use work.io_bus_pkg.all;

entity uci_flat is
port (
    io_clock    : in  std_logic;
    io_reset    : in  std_logic;
    io_read     : in  std_logic;
    io_write    : in  std_logic;
    io_addr     : in  std_logic_vector(12 downto 0);
    io_wdata    : in  std_logic_vector(7 downto 0);
    io_rdata    : out std_logic_vector(7 downto 0);
    io_ack      : out std_logic;
    io_irq      : out std_logic;
    c64_clock   : in  std_logic;
    bus_addr    : in  std_logic_vector(15 downto 0);
    ev          : in  std_logic;
    ev_addr     : in  std_logic_vector(15 downto 0);
    ev_data     : in  std_logic_vector(7 downto 0);
    ev_we       : in  std_logic;
    ev_io       : in  std_logic;
    dout        : out std_logic_vector(7 downto 0);
    oe          : out std_logic;
    c64_irq     : out std_logic;
    freeze      : out std_logic );
end entity;

architecture flat of uci_flat is
    signal req  : t_io_req;
    signal resp : t_io_resp;
begin
    req.read    <= io_read;
    req.write   <= io_write;
    req.address <= resize(unsigned(io_addr), 24);
    req.data    <= io_wdata;
    io_rdata    <= resp.data;
    io_ack      <= resp.ack;

    i_uci: entity work.uci_unit
    port map (
        io_clock => io_clock, io_reset => io_reset, io_req => req, io_resp => resp,
        io_irq => io_irq, c64_clock => c64_clock, bus_addr => bus_addr, ev => ev,
        ev_addr => ev_addr, ev_data => ev_data, ev_we => ev_we, ev_io => ev_io,
        dout => dout, oe => oe, c64_irq => c64_irq, freeze => freeze );
end architecture;
