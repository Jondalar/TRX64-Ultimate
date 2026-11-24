# The ring library (U18 D5): the Timewarp ring, its files and its operations, and the picture under the Timewarp page (U18 T-3: twstill.cc, the bar
# twbar.cc). It exports three tables through SvcLib: "ring", the firmware's u2p_ring_api_t, for the monitor and trex_crt, "ringsteps", the dumps in steps
# (ring_steps.h), for the monitor (ring_api.cc), and "ringstill", the page's coming and going (ring_still.h), for the trx64u library's core hook menu_video
# (twstill.cc). Built by ../build.sh, which passes APPLOADER (the firmware's app_loader, for app_api.h).
LIB_NAME = ring
LIB_VERSION = 1
LIB_NEEDS = base>=3.3 trx64u>=1
LIB_SRCS = lib_ring.cc ring.cc ringfile.cc trxring.cc trx_c64re_nodes.cc trx_sha256.c trx_inflate.c ringops.cc ring_api.cc ringsys.cc ringmachine.cc \
           twstill.cc twbar.cc
LIB_INC = -I$(APPLOADER)
LIB_LIBC = 1
# The named exports of lib_register's export_list (lib_ring.cc).
LIB_EXPORTS = ring=1 ringsteps=1 ringstill=1
# The budget (T239.3 S13): the packer refuses an image over `image`, the loader a library over any figure. image: payload and bss as linked against SDK
# 0.4.3 (base 3.3, e45fbb97) and 0.4.4 (9cd216eb) with the Timewarp still (U18 T-3) on 2026-11-24, 136,160 + 16,852 bytes; heap (U18 S13): 1,486,893 bytes held while idle (the
# timeline 1,146,880 of it); while the Timewarp page shows, its still on top, 1,036,800 bytes (960 x 540 RGB565, twstill.cc; freed when the page goes); the
# peak is that plus the most one operation holds on top. Measured from the allocations: the .trxring save (save_native) with cartridge RAM and a full
# timeline 1,444,975 bytes (the timeline copied, 1,146,880, the memories, the walk with its cartridge RAM, the 32 KB buffer); a .c64rering dump of 64
# entries 107,152 (one entry's walk and the 16 KB block: it streams); a frame .c64re 287,648; a format 1 load (it streams, ringfile.cc f1_read) of a
# 64-entry ring 107,356, 126,793 for one with Huffman blocks (the 32 KB window), largest block 65,536 (the RAM); an entry's cartFlash image is held
# until the apply and its cartState.cartRam takes 65,536 more (273,387 measured with a 100,000-byte image and cartRam), so the load passes the
# .trxring save only with an image above 1,272,083 bytes. The monitor's RPC (its poll runs with the menu open too) can save a .trxring while the Timewarp
# page shows, so heap_peak is the idle heap, the page's still and the .trxring save: 1,486,893 + 1,036,800 + 1,444,975. It is a ceiling, the base
# reserves nothing for it (api.md 8.4.3, 8.4.4). The counts: lib_register's record (ops 20, states 16, the poll ring.poll).
LIB_BUDGET = image=153012 heap_keep=1486893 heap_peak=3968668 ops=20 states=16 events=0 nodes=0 file_actions=0 settings=0 tabs=0 polls=1
include $(SDK)/mk/lib.mk

# The driver table (api.md 6.17, MultiCore T239.3 D31): DRV_<block>_<property>_* of c64_basic's TOML A, made by the SDK's descrec.py with the shared
# drivers' maps of the SDK (GATEWARE: ../build.sh passes the folder of the TOML A). descrec.py writes a file only when its text changes; every object
# of the library waits for it.
GATEWARE ?= ../../gateware
DRV_TOOLS ?= $(SDK_TOOLS)
DRV_MAPS ?= $(SDK)/maps
drvtab.h: $(GATEWARE)/c64_basic.toml
	python3 $(DRV_TOOLS)/descrec.py drivers $< . --maps $(DRV_MAPS)
$(LIB_OBJS): drvtab.h

# The bar's font (twbar.cc): ASCII 32 to 126 of the 8 x 8 font of c64_basic's overlay (Gideon's character generator, tools/font2mem.py, PROVENANCE.md),
# as the C table twfont.h. ../build.sh builds in build/ring/, four levels below the repository's root; FONT8 names another file.
FONT8 ?= ../../../../designs/c64_basic/mem/ovl_font8.mem
twfont.h: $(FONT8) twfont.py
	python3 twfont.py $< $@
twbar.o: twfont.h

clean: clean-drvtab
clean-drvtab:
	rm -f drvtab.h drvtab.c twfont.h
.PHONY: clean-drvtab
