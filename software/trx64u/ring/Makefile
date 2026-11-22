# The ring library (U18 D5): the Timewarp ring, its files and its operations; it exports the ring table (the
# firmware's u2p_ring_api_t layout) through SvcLib for the monitor and trex_crt. Built by ../build.sh.
LIB_NAME = ring
LIB_VERSION = 1
LIB_NEEDS = base>=3.1 trx64u>=1
LIB_SRCS = ring.cc ringfile.cc ringops.cc
LIB_LIBC = 1
include $(SDK)/mk/lib.mk
