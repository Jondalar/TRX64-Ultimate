-- SPDX-License-Identifier: GPL-3.0-or-later
-- tb_dma_bus: the signals the BFM and the testbench share
library ieee;
use ieee.std_logic_1164.all;
package tb_dma_bus_pkg is
    signal bfm_step   : natural := 0;
    signal bfm_errors : natural := 0;
    signal pk_cnt     : natural := 0;
    signal pk_bus, pk_rom, pk_we : std_logic;
    signal pk_addr    : std_logic_vector(15 downto 0);
    -- T37: the socket registers as ultimate_sys puts them out
    signal sk_base, sk_mask : std_logic_vector(15 downto 0);
    signal sk_en      : std_logic_vector(1 downto 0);
    signal sk_sel     : std_logic_vector(2 downto 0);
    signal sk_gains   : std_logic_vector(31 downto 0);
    -- D37: the words the ICAP loader hands to the ICAPE2 (icap_we pulses), the first and the last
    signal ic_cnt     : natural := 0;
    signal ic_first, ic_last : std_logic_vector(31 downto 0) := (others => '0');
end package;
