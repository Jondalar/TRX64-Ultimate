# apps/c64re — writes the machine as a .c64re snapshot.
#
# The container the TRX64 runtime reads: magic, a sha256 over a gzip'd JSON
# document carrying RAM, colour RAM, the VIC registers and the media that are
# in the machine. Lives as an app so it costs the firmware no flash.
#
# Build with build.sh in this directory (container), or directly when
# nios2-elf-gcc and riscv32-unknown-elf-gcc are on $PATH.

APP       = c64re
MANIFEST  = manifest.json
SDK       = ../../software/appsdk

INC       = -I.

SRC_C     = src/c64re.c

SRCS      = $(SRC_C)

include $(SDK)/app.mk
