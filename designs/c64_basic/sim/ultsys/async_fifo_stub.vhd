-- SPDX-License-Identifier: GPL-3.0-or-later
-- sim/ultsys: async_fifo_ft does not go through ghdl synth ("clock-level is not a constant").
-- The bench uses neither the Ethernet nor the USB host it serves: an empty fifo.
library ieee;
    use ieee.std_logic_1164.all;
    use ieee.numeric_std.all;

entity async_fifo_ft is
    generic(
        g_fast       : boolean := true;
        g_data_width : integer := 36;
        g_depth_bits : integer := 9
    );
    port (
        wr_clock       : in  std_logic;
        wr_reset       : in  std_logic;
        wr_en          : in  std_logic;
        wr_din         : in  std_logic_vector(g_data_width-1 downto 0);
        wr_full        : out std_logic;
        rd_clock        : in  std_logic;
        rd_reset        : in  std_logic;
        rd_next         : in  std_logic;
        rd_dout         : out std_logic_vector(g_data_width-1 downto 0);
        rd_valid        : out std_logic;
        rd_count        : out unsigned(g_depth_bits downto 0) );
end entity;

architecture stub of async_fifo_ft is
begin
    wr_full  <= '0';
    rd_dout  <= (others => '0');
    rd_valid <= '0';
    rd_count <= (others => '0');
end architecture;
