# The monitor library (U18 D5): trxmon's engine (../trxmon, copied into trxmon/ by build.sh; moved from the firmware fork by U20 A2, the only copy
# this tree builds, no longer one source with the firmware's), and its platform:
# lib_monitor.cc (the entry, the operations, the plat_* calls, the export "textgrid" of the F4 screen), monitor.cc (the session, trxmon's trx table, the
# F4 screen's rows) and monsrv.cc (the srv_* calls, the RPC server on TCP 4312). Built by ../build.sh, which passes SDK and FWABI (../fwabi).
# `ring>=1` is met by the export "ring" of whichever library provides it (the base resolves a need on export names first); the ring library also exports
# "ringsteps", which monitor.cc takes when it is there.
LIB_NAME = monitor
LIB_VERSION = 1
LIB_NEEDS = base>=3.3 ring>=1
LIB_SRCS = lib_monitor.cc monitor.cc monsrv.cc $(sort $(wildcard trxmon/*.c))
LIB_LIBC = 1
# The named exports of lib_register's export_list (lib_monitor.cc): the F4 screen.
LIB_EXPORTS = textgrid=1
# The budget (T239.3 S13): the packer refuses an image over `image`, the loader a library over any figure. image: payload and bss as linked against SDK
# 0.3.0 (base 3.3) on 2026-11-23, 209,004 + 473,264 bytes; heap (U18 S13): no block held while idle, at most 50,240 bytes at once (one RPC client while
# gated); the counts: lib_register's record (ops 4, states 1, the poll monitor.serve).
LIB_BUDGET = image=682268 heap_keep=0 heap_peak=50240 ops=4 states=1 events=0 nodes=0 file_actions=0 settings=0 tabs=0 polls=1
include $(SDK)/mk/lib.mk

# trxmon is built against the firmware's application ABI (FWABI: ../fwabi's u2p_app.h with the whole ring table), not the SDK's ABI 2 shim of the same
# name (FWABI comes first), and the ring table is the ring library's own export (MultiCore's interim rule while R4 was open, the rule since).
# TRXMON_BUILD names the build in ping's version.
FWABI ?= $(abspath $(CURDIR)/../../fwabi)
TRXMON_CFLAGS = -I$(FWABI) $(LIB_CFLAGS) -DTRXMON_BUILD=\"in-trx64u\"
trxmon/%.o: trxmon/%.c
	$(CC_RV) $(TRXMON_CFLAGS) -MMD -MP -c $< -o $@

# monitor.cc builds trxmon's trx table and reads the same ABI; it includes no header of the SDK (svctab.h defines u2p_video_t and the ring records again).
# It also reads the ring library's ring_steps.h (the table of its export "ringsteps"): build.sh builds each library in build/<name>/, two levels below
# software/trx64u/, where the ring library's sources are.
RING_SRC = $(abspath $(CURDIR)/../../ring)
MONITOR_CXXFLAGS = -I$(FWABI) -I$(RING_SRC) $(LIB_CXXFLAGS)
monitor.o: monitor.cc
	$(CXX_RV) $(MONITOR_CXXFLAGS) -MMD -MP -c $< -o $@
