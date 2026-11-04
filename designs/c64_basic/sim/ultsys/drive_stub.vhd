-- SPDX-License-Identifier: GPL-3.0-or-later
-- sim/ultsys: the 1541 (CPU, VIAs, GCR) is not part of the app's D-page window; a stand-in keeps the
-- bench's simulation speed up. Entity as in designs/c64_basic/drive_unit.vhd.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.io_bus_pkg.all;
use work.mem_bus_pkg.all;

entity drive_unit is
generic (
    g_audio         : boolean := true );
port (
    clock           : in  std_logic;
    reset           : in  std_logic;
    tick_16MHz      : in  std_logic;
    tick_4MHz       : in  std_logic;
    tick_1kHz       : in  std_logic;

    io_req          : in  t_io_req;
    io_resp         : out t_io_resp;

    dp_req_tgl      : out std_logic;        -- DDR2 port, see drive_mem
    dp_addr         : out std_logic_vector(25 downto 0);
    dp_we           : out std_logic;
    dp_wdata        : out std_logic_vector(31 downto 0);
    dp_be           : out std_logic_vector(3 downto 0);
    dp_done_tgl     : in  std_logic;
    dp_rdata        : in  std_logic_vector(127 downto 0);
    snoop_req       : in  t_mem_req_32;     -- the system bus after the arbiter
    snoop_resp      : in  t_mem_resp_32;

    drive_stop      : in  std_logic;        -- C64 stopped (the app's freeze)
    halt            : in  std_logic := '0'; -- machine pause: no ticks, so the CPU,
                                            -- VIAs, head and GCR stream stand still
    c64_reset_n     : in  std_logic;

    iec_reset_n     : in  std_logic := '1';
    iec_atn_i       : in  std_logic;
    iec_clk_i       : in  std_logic;
    iec_data_i      : in  std_logic;
    iec_clk_o       : out std_logic;
    iec_data_o      : out std_logic;

    act_led_n       : out std_logic;
    motor_led_n     : out std_logic;
    dirty_led_n     : out std_logic;
    audio_sample    : out signed(12 downto 0);

    -- for the JTAG debug map: last GCR fetch address; GCR fetches and their
    -- answers; the drive CPU's last bus cycle {0, ATN, DATA, CLK, SYNC, SO,
    -- IRQ, read, data, address}; SYNC and byte-ready (SO) edges, last GCR byte;
    -- {c1541_drive's dbg_status: half track, step, motor, mode, write_prot_n,
    -- inserted, power, reset, byte ready; last write to VIA 2 port B ($1C00);
    -- highest half track since reset}; parameter RAM words as the app wrote
    -- them: half track 0 {length - 1 | bit time << 16}, half track 34 the same;
    -- {GCR fetches in the last 100 ms, drive CPU cycles (wraps)}
    dbg             : out std_logic_vector(255 downto 0);
    -- the drive CPU's cycles as they complete (cpu_part_1541's debug stream,
    -- one clock after the cycle): {opcode fetch (patch 0004), ATN, DATA, CLK,
    -- disk SYNC, SO, IRQ, read, data, address}, for drv_dbg.v
    cpu_trace       : out std_logic_vector(31 downto 0);
    cpu_trace_valid : out std_logic );
end entity;

architecture stub of drive_unit is
begin
    io_resp         <= c_io_resp_init;
    dp_req_tgl      <= '0';
    dp_addr         <= (others => '0');
    dp_we           <= '0';
    dp_wdata        <= (others => '0');
    dp_be           <= (others => '0');
    iec_clk_o       <= '1';
    iec_data_o      <= '1';
    act_led_n       <= '1';
    motor_led_n     <= '1';
    dirty_led_n     <= '1';
    audio_sample    <= (others => '0');
    dbg             <= (others => '0');
    cpu_trace       <= (others => '0');
    cpu_trace_valid <= '0';
end architecture;
