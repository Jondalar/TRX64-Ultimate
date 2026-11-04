# apps/softboot — starts the firmware inside an update image from RAM.
#
# Build with build.sh in this directory (container), or directly when
# nios2-elf-gcc and riscv32-unknown-elf-gcc are on $PATH.

APP       = softboot
MANIFEST  = manifest.json
SDK       = ../../software/appsdk

INC       = -Isrc

SRCS      = src/softboot.c src/libc.c

include $(SDK)/app.mk
