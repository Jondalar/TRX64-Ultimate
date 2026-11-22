# The trx64u base library (U18 D5): the menu (menu.toml), its operations (trx64u_ops) and the C64U settings (u64cfg); lib_trx64u.cc is the entry and the
# record, setting.cc the settings as the base shows them, libc_extra.c the C library beyond the SDK's. Built by ../build.sh.
LIB_NAME = trx64u
LIB_VERSION = 1
LIB_NEEDS = base>=3.1
LIB_SRCS = lib_trx64u.cc trx64u_ops.cc u64cfg.cc setting.cc libc_extra.c
LIB_MENU = menu.toml
LIB_LIBC = 1
LIB_INC = $(TRX_INC)
include $(SDK)/mk/lib.mk

# setting.cc's lists of the values of every choice setting, from the menu.toml the image carries (setlists.py)
setlists.h: menu.toml setlists.py
	python3 setlists.py $(SDK_TOOLS) menu.toml $@
setting.o: setlists.h

clean: clean-setlists
clean-setlists:
	rm -f setlists.h
.PHONY: clean-setlists
