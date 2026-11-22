# The trx64u base library (U18 D5): the operations of the menu (trx64u_ops) and the C64U settings (u64cfg); lib_trx64u.cc is the entry, the record and
# the core hooks, setting.cc the settings as the base shows them, libc_extra.c the C library beyond the SDK's. Built by ../build.sh.
# The image carries no menu record: menu.toml is the core's description, delivered beside the library and packed by MultiCore's pipeline as record 6 of
# trx64u.cor (cor_pack.py pack --desc). This build reads it only for setlists.h.
LIB_NAME = trx64u
LIB_VERSION = 1
LIB_NEEDS = base>=3.1
LIB_SRCS = lib_trx64u.cc trx64u_ops.cc u64cfg.cc setting.cc libc_extra.c
LIB_LIBC = 1
LIB_INC = $(TRX_INC)
include $(SDK)/mk/lib.mk

# lib.mk gives hidden.h (every declaration hidden) only to the C files. A C++ file that reads a variable of another file (g_api) would then reach it through
# the global offset table, which lib.ld discards and the loader does not fill: the link fails on `.got.plt`.
LIB_CXXFLAGS += -include $(SDK_HIDDEN)

# setting.cc's lists of the values of every choice setting, from the core's menu.toml (setlists.py)
setlists.h: menu.toml setlists.py
	python3 setlists.py $(SDK_TOOLS) menu.toml $@
setting.o: setlists.h

clean: clean-setlists
clean-setlists:
	rm -f setlists.h
.PHONY: clean-setlists
