# The ring library (U18 D5): the Timewarp ring, its files and its operations; it exports the ring table (the firmware's u2p_ring_api_t,
# ring_api.cc) through SvcLib for the monitor and trex_crt. Built by ../build.sh, which passes APPLOADER (the firmware's app_loader, for app_api.h).
LIB_NAME = ring
LIB_VERSION = 1
LIB_NEEDS = base>=3.1 trx64u>=1
LIB_SRCS = lib_ring.cc ring.cc ringfile.cc trxring.cc trx_c64re_nodes.cc trx_sha256.c trx_inflate.c ringops.cc ring_api.cc ringsys.cc
LIB_INC = -I$(APPLOADER) $(TRX_INC)
LIB_LIBC = 1
include $(SDK)/mk/lib.mk
