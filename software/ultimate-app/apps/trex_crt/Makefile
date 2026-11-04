# apps/trex_crt — standalone TREX CRT Tool app bundle.
#
# Builds trex_crt.u2a with a Nios II (U64) and a RISC-V (U64-II, C64U) variant.
#
# Layout:
#   src/          - app entry point, UI shim and the ported cartridge tools
#   manifest.json - file types and task menu entries of the app
#   The SDK (headers, u2p_ui.c, startup stubs, linker scripts, packer and
#   build rules) is software/appsdk, shared by every app.
#
# Build requirements:
#   - nios2-elf-gcc (from Quartus 18.1 Nios II EDS) on $PATH
#   - riscv32-unknown-elf-gcc on $PATH
#   - python3
# build.sh in this dir builds it in the container through build/build_app.sh.

APP       = trex_crt
MANIFEST  = manifest.json
SDK       = ../../software/appsdk

INC       = -I.

SRC_C     = src/main.c \
            src/ui_shim.c \
            src/cart_base.c \
            src/cart_screen.c \
            src/cart_dumper.c \
            src/cart_writer.c \
            src/cart_inspector.c \
            src/eapi_drivers.c \
            $(SDK)/lib/u2p_ui.c

SRCS      = $(SRC_C)

include $(SDK)/app.mk
