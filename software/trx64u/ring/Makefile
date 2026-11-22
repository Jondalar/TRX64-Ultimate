# The ring library (U18 D5): the Timewarp ring, its files and its operations. It exports two tables (ring_api.cc) through SvcLib: "ring", the firmware's
# u2p_ring_api_t, for the monitor and trex_crt, and "ringsteps", the dumps in steps (ring_steps.h), for the monitor. Built by ../build.sh, which passes
# APPLOADER (the firmware's app_loader, for app_api.h).
LIB_NAME = ring
LIB_VERSION = 1
LIB_NEEDS = base>=3.2 trx64u>=1
LIB_SRCS = lib_ring.cc ring.cc ringfile.cc trxring.cc trx_c64re_nodes.cc trx_sha256.c trx_inflate.c ringops.cc ring_api.cc ringsys.cc ringmachine.cc
LIB_INC = -I$(APPLOADER) $(TRX_INC)
LIB_LIBC = 1
# The named exports of lib_register's export_list (lib_ring.cc).
LIB_EXPORTS = ring=1 ringsteps=1
# The budget (T239.3 S13): the packer refuses an image over `image`, the loader a library over any figure. image: payload and bss as packed on 2026-11-23,
# the base 3.3 accessors stubbed (56 bytes); heap (U18 S13): 1,486,893 bytes held while idle (the timeline 1,146,880 of it), the peak that plus the
# 4,349,536 bytes of a 64-entry .c64rering dump; the counts: lib_register's record.
LIB_BUDGET = image=135816 heap_keep=1486893 heap_peak=5836429 ops=20 states=16 events=0 nodes=0 file_actions=0 settings=0 tabs=0 polls=1
include $(SDK)/mk/lib.mk
