# The trx64u base library (U18 D5): the operations of the menu (trx64u_ops) and the C64U settings (u64cfg), the C64 descriptor's code (c64core, c64_go:
# MultiCore's c64.cc and c64_go.cc); lib_trx64u.cc is the entry, the record and the core hooks, setting.cc the settings as the base shows them,
# libc_extra.c the C library beyond the SDK's. Built by ../build.sh.
# The image carries no menu record: menu.toml (the menu and its key table) and desc.toml (the descriptor's data, the register windows) are the core's
# description, delivered beside the library and packed as record 6 of trx64u.cor (cor_pack.py pack --desc menu.toml --desc desc.toml). This build
# reads menu.toml only, for setlists.h.
LIB_NAME = trx64u
LIB_VERSION = 1
LIB_NEEDS = base>=3.3
LIB_SRCS = lib_trx64u.cc trx64u_ops.cc u64cfg.cc c64core.cc c64_go.cc setting.cc libc_extra.c
LIB_LIBC = 1
# The named exports of lib_register's export_list (lib_trx64u.cc): the core hooks.
LIB_EXPORTS = corehooks=1
# The budget (T239.3 S13): the packer refuses an image over `image`, the loader a library over any figure. image: payload and bss as linked against SDK
# 0.3.0 (base 3.3, d6f50a0d) on 2026-11-23, 92,080 + 120,708 bytes; 65,564 bytes of the bss are c64core.cc's program buffer, which a queued program job holds until
# the service starts it. heap: no block held while idle, at most 66,044 bytes at once (U18 S13); the counts: lib_register's record (ops 66, states 54).
LIB_BUDGET = image=212788 heap_keep=0 heap_peak=66044 ops=66 states=54 events=0 nodes=0 file_actions=0 settings=0 tabs=0 polls=0
include $(SDK)/mk/lib.mk

# setting.cc's lists of the values of every choice setting, from the core's menu.toml (setlists.py)
setlists.h: menu.toml setlists.py
	python3 setlists.py $(SDK_TOOLS) menu.toml $@
setting.o: setlists.h

clean: clean-setlists
clean-setlists:
	rm -f setlists.h
.PHONY: clean-setlists
