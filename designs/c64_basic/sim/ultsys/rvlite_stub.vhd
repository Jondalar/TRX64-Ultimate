-- SPDX-License-Identifier: GPL-3.0-or-later
-- sim/ultsys: the RISC-V's place in ultimate_sys for the bench. Only the entity
-- matters: the generated Verilog module of it is replaced by rvlite_bfm.v (build.sh).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
library work;
    use work.mem_bus_pkg.all;
    use work.io_bus_pkg.all;

entity rvlite_wrapper is
generic (
    g_mult          : boolean := true;
    g_gprf_ram      : string := "auto";
    g_start_addr    : std_logic_vector(31 downto 0) := X"80000000";
    g_bootrom       : boolean := true;
    g_icache        : boolean := true;
    g_tag_i         : std_logic_vector(7 downto 0) := X"20";
    g_tag_d         : std_logic_vector(7 downto 0) := X"21" );
port (
    clock           : in    std_logic;
    reset           : in    std_logic;
    irq_i           : in    std_logic := '0';
    io_req          : out   t_io_req;
    io_resp         : in    t_io_resp;
    io_busy         : out   std_logic;
    mem_req         : out   t_mem_req_32;
    mem_resp        : in    t_mem_resp_32;
    dbg_pc          : out   std_logic_vector(31 downto 0) );
end entity;

architecture stub of rvlite_wrapper is
begin
    io_req   <= c_io_req_init;
    io_busy  <= '0';
    mem_req  <= c_mem_req_32_init;
    dbg_pc   <= (others => '0');
end architecture;
