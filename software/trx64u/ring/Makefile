# The ring library (U18 D5): the Timewarp ring, its files and its operations. It exports two tables (ring_api.cc) through SvcLib: "ring", the firmware's
# u2p_ring_api_t, for the monitor and trex_crt, and "ringsteps", the dumps in steps (ring_steps.h), for the monitor. Built by ../build.sh, which passes
# APPLOADER (the firmware's app_loader, for app_api.h).
LIB_NAME = ring
LIB_VERSION = 1
LIB_NEEDS = base>=3.3 trx64u>=1
LIB_SRCS = lib_ring.cc ring.cc ringfile.cc trxring.cc trx_c64re_nodes.cc trx_sha256.c trx_inflate.c ringops.cc ring_api.cc ringsys.cc ringmachine.cc
LIB_INC = -I$(APPLOADER)
LIB_LIBC = 1
# The named exports of lib_register's export_list (lib_ring.cc).
LIB_EXPORTS = ring=1 ringsteps=1
# The budget (T239.3 S13): the packer refuses an image over `image`, the loader a library over any figure. image: payload and bss as linked against SDK
# 0.3.0 (base 3.3, e4f99648) on 2026-11-23, 131,576 + 16,784 bytes; heap (U18 S13): 1,486,893 bytes held while idle (the timeline 1,146,880 of it); the peak is
# that plus the most one operation holds on top. Measured from the allocations: the .trxring save (save_native) with cartridge RAM and a full
# timeline 1,444,975 bytes (the timeline copied, 1,146,880, the memories, the walk with its cartridge RAM, the 32 KB buffer); a .c64rering dump of 64
# entries 107,152 (one entry's walk and the 16 KB block: it streams); a frame .c64re 287,648; a format 1 load (it streams, ringfile.cc f1_read) of a
# 64-entry ring 107,356, 126,793 for one with Huffman blocks (the 32 KB window), largest block 65,536 (the RAM); an entry's cartFlash image is held
# until the apply and its cartState.cartRam takes 65,536 more (273,387 measured with a 100,000-byte image and cartRam), so the load passes the
# .trxring save only with an image above 1,272,083 bytes. heap_peak is the idle heap plus the .trxring save. It is a ceiling, the base reserves
# nothing for it (api.md 8.4.3, 8.4.4). The counts: lib_register's record (ops 20, states 16, the poll ring.poll).
LIB_BUDGET = image=148360 heap_keep=1486893 heap_peak=2931868 ops=20 states=16 events=0 nodes=0 file_actions=0 settings=0 tabs=0 polls=1
include $(SDK)/mk/lib.mk
