# The trx64u base library (U18 D5): the operations of the menu (trx64u_ops) and the C64U settings (u64cfg), the C64 descriptor's code (c64core, c64_go:
# MultiCore's c64.cc and c64_go.cc), the `.cfw` run (cfwrun: MultiCore's update.cc, T239.3 P2-5); lib_trx64u.cc is the entry, the record and the core hooks, setting.cc the settings as the base shows them,
# libc_extra.c the C library beyond the SDK's. Built by ../build.sh.
# The image carries no menu record: menu.toml (the menu and its key table) and desc.toml (the descriptor's data, the four register windows still open)
# are the core's description, delivered beside the library and packed as record 6 of trx64u.cor (cor_pack.py pack --desc menu.toml --desc desc.toml).
# This build reads menu.toml for setlists.h, and c64_basic's TOML A (../gateware/c64_basic.toml) for the driver table drvtab.h.
LIB_NAME = trx64u
LIB_VERSION = 1
LIB_NEEDS = base>=3.3
LIB_SRCS = lib_trx64u.cc trx64u_ops.cc u64cfg.cc c64core.cc c64_go.cc cfwrun.cc setting.cc libc_extra.c
LIB_LIBC = 1
# The named exports of lib_register's export_list (lib_trx64u.cc): the core hooks.
LIB_EXPORTS = corehooks=1
# The budget (T239.3 S13): the packer refuses an image over `image`, the loader a library over any figure. image: the ceiling of payload and bss as linked
# against SDK 0.3.0 (base 3.3, e4f99648) on 2026-11-23, 92,152 + 120,708 bytes; on SDK 0.4.1 with the `.cfw` run (2026-11-24) 89,252 + 120,676; on SDK
# 0.4.3 to 0.4.5 with the Timewarp still's hook and palette source (U18 T-3, 2026-11-24) 89,760 + 120,684. 65,564 bytes of
# the bss are c64core.cc's program buffer, which a queued program job holds until the service starts it. heap: no block held while idle, at most 66,044 bytes
# at once (U18 S13; the `.cfw` run's two 32 KB halves are one block of 65,536); the counts: lib_register's record (ops 67, states 54).
LIB_BUDGET = image=212860 heap_keep=0 heap_peak=66044 ops=67 states=54 events=0 nodes=0 file_actions=0 settings=0 tabs=0 polls=0
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

# lib_trx64u.cc reads the ring library's ring_still.h (the table of its export "ringstill"): build.sh builds each library in build/<name>/, two levels below
# software/trx64u/, where the ring library's sources are (as the monitor's Makefile reads ring_steps.h).
RING_SRC = $(abspath $(CURDIR)/../../ring)
lib_trx64u.o: LIB_INC += -I$(RING_SRC)

# setting.cc's lists of the values of every choice setting, from the core's menu.toml (setlists.py)
setlists.h: menu.toml setlists.py
	python3 setlists.py $(SDK_TOOLS) menu.toml $@
setting.o: setlists.h

clean: clean-setlists
clean-setlists:
	rm -f setlists.h drvtab.h drvtab.c
.PHONY: clean-setlists
