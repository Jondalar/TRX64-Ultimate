# Build restcheck.u2a, the app the app-rest suite starts over REST.
#
# Requires nios2-elf-gcc and riscv32-unknown-elf-gcc in $PATH, or run
# build/build_app.sh restcheck, which runs this Makefile in the build container.

APP       = restcheck
SDK       = ../..
SRCS      = restcheck.c
MANIFEST  = manifest.json

include $(SDK)/app.mk
