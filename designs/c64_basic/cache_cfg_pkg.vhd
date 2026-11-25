-- SPDX-License-Identifier: GPL-3.0-or-later
-- PROVENANCE: TRX64U-MultiCore host/cache_cfg_pkg.vhd at e4f99648 (T133, the host RISC-V's caches), taken for U19 unchanged but for comments.
-- The sizes of the app RISC-V's caches as constants of the build (U19). The wrapper (rvlite_wrapper_dc.vhd) takes its generic defaults from here;
-- build.tcl lists this file.
package cache_cfg_pkg is
    constant c_dcache_on   : boolean := true;
    constant c_dcache_bits : natural := 13;     -- bytes = 2**bits
    constant c_icache_bits : natural := 13;
    constant c_line_bits   : natural := 2;      -- log2 of the words per line (2 = 16 bytes filled from the DDR2 read's 128 bits, 0 = one word)
end package;
