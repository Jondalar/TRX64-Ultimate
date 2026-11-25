TREX CRT on ABI 3 (T104.3 D0, D9.1). PROVENANCE: a copy of `apps/trex_crt` of TRX64U-MultiCore (branch `t239.3`, commit `8ff28ba9`, the commit SDK 0.4.1 is packed from),
MultiCore's port onto ABI 3 of our firmware's cartridge tools: `src/cart_base.*`, `cart_dumper.*`, `cart_writer.*`, `cart_inspector.*`, `cart_screen.*`, `eapi_drivers.*` and
`ui_shim.*` come from `TREX/1541ultimate:apps/trex_crt/src` (the TRX64 line's cartridge tools, GPL-3.0; fork commit `aa2ef5dc`). Changed by that port: the 40 x 24
character screen is gone - `cart_screen.c` publishes what the tools drew as the strings of `trex_ui.h` (state values `cart.*` in `glue.cc`), which the screens of `app.toml`
bind to, and the window calls of `ui_shim.c` draw nothing; `main.c` and the key loop are `trex_main.c` (a task that takes the commands of the screens' operations and runs the
tool's own poll function); the three tools share one session on the port; the write has a confirmation screen, the dump and the verify can be stopped (R/S) and a write or
erase locks R/S with its reason; the file of the Flash screen and the destination of the Dump screen are chosen on the screen; the Inspector shows 16 lines of 16 bytes and
also takes PAGE UP, PAGE DOWN and HOME; `cart_check_dma_access()` always grants access (it did before where the ID block reads TRX2; the refusal on Gideon's core, an external
cartridge with Cartridge Preference Auto or External, is gone); `ui_snprintf` is the SDK's `u2p_snprintf`, which also skips an `l`. `eapi_drivers.c` carries the EAPI notice
of its sources. Against MultiCore's copy, only the Makefile's include (the SDK's `mk/app.mk`), this README and the paths that comments name are changed here.
