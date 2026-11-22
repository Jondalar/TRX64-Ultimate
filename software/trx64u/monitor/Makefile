# The monitor library (U18 D5): trxmon's engine, the same files as the firmware's trxmon (D4, copied into trxmon/ by build.sh), and its platform:
# lib_monitor.cc (the entry, the operations, the plat_* calls), monitor.cc (the session, trxmon's trx table, the F4 screen) and monsrv.cc (the srv_* calls,
# the RPC server on TCP 4312). Built by ../build.sh, which passes SDK, APPSDK and APPLOADER.
LIB_NAME = monitor
LIB_VERSION = 1
LIB_NEEDS = base>=3.1 ring>=1
LIB_SRCS = lib_monitor.cc monitor.cc monsrv.cc $(sort $(wildcard trxmon/*.c))
LIB_LIBC = 1
LIB_INC = $(TRX_INC)
include $(SDK)/mk/lib.mk

# lib.mk gives hidden.h (every declaration hidden) only to the C files. A C++ file that reads a variable of another file (g_api, trxmon's M) would then reach
# it through the global offset table, which lib.ld discards and the loader does not fill: the link fails on `.got.plt`.
LIB_CXXFLAGS += -include $(SDK_HIDDEN)

# trxmon is built against the firmware's application ABI (u2p_app.h with the whole ring table), not the SDK's ABI 2 shim: one source for both (D4), and the
# ring table is the ring library's own export (MultiCore's interim rule while R4 was open, the rule since).
TRXMON_CFLAGS = -I$(APPSDK) -I$(APPLOADER) $(LIB_CFLAGS)
trxmon/%.o: trxmon/%.c
	$(CC_RV) $(TRXMON_CFLAGS) -MMD -MP -c $< -o $@

# monitor.cc builds trxmon's trx table and reads the same ABI; it includes no header of the SDK (svctab.h defines u2p_video_t and the ring records again).
MONITOR_CXXFLAGS = -I$(APPSDK) -I$(APPLOADER) $(LIB_CXXFLAGS)
monitor.o: monitor.cc
	$(CXX_RV) $(MONITOR_CXXFLAGS) -MMD -MP -c $< -o $@
