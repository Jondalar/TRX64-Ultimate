# The monitor library (U18 D5): trxmon's engine, the same files as the firmware's trxmon (D4, copied into trxmon/ by
# build.sh), and its platform: monitor.cc (the plat_* calls, the F4 screen) and monsrv.cc (the srv_* calls, the RPC
# server on TCP 4312). Built by ../build.sh, which passes SDK, APPSDK and APPLOADER.
LIB_NAME = monitor
LIB_VERSION = 1
LIB_NEEDS = base>=3.1 ring>=1
LIB_SRCS = monitor.cc monsrv.cc $(sort $(wildcard trxmon/*.c))
LIB_LIBC = 1
include $(SDK)/mk/lib.mk

# trxmon is built against the firmware's application ABI (u2p_app.h with the whole ring table), not the SDK's ABI 2
# shim: one source for both (D4), and the ring table is the ring library's own export (MultiCore's interim rule while
# R4 is open).
TRXMON_CFLAGS = -I$(APPSDK) -I$(APPLOADER) $(LIB_CFLAGS)
trxmon/%.o: trxmon/%.c
	$(CC_RV) $(TRXMON_CFLAGS) -MMD -MP -c $< -o $@
