# apps/unzip — extracts ZIP files from the file browser.
#
# miniz (MIT) reads the archive; see src/miniz. Lives as an app so it costs
# the firmware no flash.
#
# Build with build.sh in this directory (container), or directly when
# nios2-elf-gcc and riscv32-unknown-elf-gcc are on $PATH.

APP       = unzip
MANIFEST  = manifest.json
SDK       = ../../software/appsdk

INC       = -Isrc -Isrc/miniz

SRCS      = src/unzip.c src/libc.c src/miniz/miniz.c src/miniz/miniz_tinfl.c src/miniz/miniz_zip.c

include $(SDK)/app.mk
