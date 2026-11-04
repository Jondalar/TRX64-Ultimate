-- SPDX-License-Identifier: GPL-3.0-or-later
-- sim/ultsys: the USB host (a 6502-like core with its program and sequencer) does not take part in
-- the app's D-page window; as a stand-in it keeps the bench's simulation speed up.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.io_bus_pkg.all;
use work.mem_bus_pkg.all;

entity usb_host_nano is
    generic (
        g_big_endian   : boolean := false;
        g_incl_debug   : boolean := false;
        g_tag          : std_logic_vector(7 downto 0) := X"04";
        g_simulation   : boolean := false );
	port  (
        clock       : in  std_logic;
        reset       : in  std_logic;
        ulpi_nxt    : in    std_logic;
        ulpi_dir    : in    std_logic;
        ulpi_stp    : out   std_logic;
        ulpi_data_i : in    std_logic_vector(7 downto 0);
        ulpi_data_o : out   std_logic_vector(7 downto 0);
        ulpi_data_t : out   std_logic;
        debug_data  : out   std_logic_vector(31 downto 0);
        debug_valid : out   std_logic;
        error_pulse : out   std_logic;
        sys_clock   : in  std_logic;
        sys_reset   : in  std_logic;
        sys_mem_req : out t_mem_req_32;
        sys_mem_resp: in  t_mem_resp_32;
        sys_io_req  : in  t_io_req;
        sys_io_resp : out t_io_resp;
        sys_irq     : out std_logic );
end entity;

architecture stub of usb_host_nano is
begin
    ulpi_stp    <= '0';
    ulpi_data_o <= (others => '0');
    ulpi_data_t <= '1';
    debug_data  <= (others => '0');
    debug_valid <= '0';
    error_pulse <= '0';
    sys_mem_req <= c_mem_req_32_init;
    sys_io_resp <= c_io_resp_init;
    sys_irq     <= '0';
end architecture;
