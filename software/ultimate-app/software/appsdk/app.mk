# app.mk — build rules for a .u2a app with a Nios II (U64) and a RISC-V
# (U64-II, C64U) variant.
#
# Set these in the app Makefile, then include this file:
#   APP       output name, builds $(APP).u2a
#   SDK       path of this SDK directory
#   SRCS      C sources (objects are built in the current directory)
#   MANIFEST  optional manifest JSON
#   INC       optional extra -I flags
#
# Toolchains: nios2-elf-gcc (Quartus 18.1 Nios II EDS) and
# riscv32-unknown-elf-gcc, both in the build container.

NIOS2_CROSS  ?= nios2-elf-
RISCV_CROSS  ?= riscv32-unknown-elf-

NIOS2_CC      = $(NIOS2_CROSS)gcc
RISCV_CC      = $(RISCV_CROSS)gcc
RISCV_OBJDUMP = $(RISCV_CROSS)objdump

APP_INC       = -I$(SDK)/include $(INC)
LIBS         ?= -lgcc

# Nios II: -G0 keeps data out of the gp-relative small data area.
NIOS2_CFLAGS  = -Os -g -ffreestanding -nostdlib -fno-exceptions -Wall $(APP_INC) \
                -mno-hw-div -mno-hw-mul -mno-hw-mulx -G0
NIOS2_LDFLAGS = -nostdlib -T$(SDK)/linker/app_nios2.lds -e _app_start

# RISC-V: gp belongs to the firmware, so no small data and no linker relaxation.
RISCV_ARCH    = -march=rv32i -mabi=ilp32
RISCV_CFLAGS  = $(RISCV_ARCH) -mno-relax -msmall-data-limit=0 -ffreestanding -fno-builtin -nostdlib \
                -Os -ffunction-sections -fdata-sections -Wall $(APP_INC)
RISCV_LDFLAGS = $(RISCV_ARCH) -nostdlib -T$(SDK)/linker/app_riscv.lds -e _app_start \
                -Wl,--no-relax -Wl,--gc-sections

APP_OBJS      = $(basename $(notdir $(SRCS)))
NIOS2_OBJS    = app_crt0_nios2.o $(addsuffix .nios2.o,$(APP_OBJS))
RISCV_OBJS    = app_crt0_riscv.o $(addsuffix .riscv.o,$(APP_OBJS))

vpath %.c $(sort $(dir $(SRCS)))

all: $(APP).u2a

%.nios2.o: %.c
	$(NIOS2_CC) $(NIOS2_CFLAGS) -c -o $@ $<

%.riscv.o: %.c
	$(RISCV_CC) $(RISCV_CFLAGS) -c -o $@ $<

app_crt0_nios2.o: $(SDK)/crt0/app_crt0_nios2.S
	$(NIOS2_CC) $(NIOS2_CFLAGS) -c -o $@ $<

app_crt0_riscv.o: $(SDK)/crt0/app_crt0_riscv.S
	$(RISCV_CC) $(RISCV_CFLAGS) -c -o $@ $<

$(APP)_nios2.elf: $(NIOS2_OBJS) $(SDK)/linker/app_nios2.lds
	$(NIOS2_CC) $(NIOS2_LDFLAGS) -o $@ $(NIOS2_OBJS) $(LIBS)

# An app that touches gp would corrupt the firmware's global data: fail the build.
$(APP)_riscv.elf: $(RISCV_OBJS) $(SDK)/linker/app_riscv.lds
	$(RISCV_CC) $(RISCV_LDFLAGS) -o $@ $(RISCV_OBJS) $(LIBS)
	$(RISCV_OBJDUMP) -d $@ > $@.dis
	@if sed 's/<[^>]*>//g' $@.dis | grep -E '(^|[[:space:],(])gp([[:space:],)]|$$)'; then \
	    echo "error: $@ uses gp, which belongs to the firmware" >&2; rm -f $@.dis; exit 1; \
	fi
	@rm -f $@.dis

$(APP).u2a: $(APP)_nios2.elf $(APP)_riscv.elf $(MANIFEST) $(SDK)/tools/mk_u2a.py
	python3 $(SDK)/tools/mk_u2a.py \
	    --variant nios2=$(APP)_nios2.elf \
	    --variant riscv32=$(APP)_riscv.elf \
	    $(if $(MANIFEST),--manifest $(MANIFEST)) \
	    --name "$(APP)" \
	    --out $@

clean:
	rm -f $(NIOS2_OBJS) $(RISCV_OBJS) $(APP)_nios2.elf $(APP)_riscv.elf $(APP)_riscv.elf.dis $(APP).u2a

.DELETE_ON_ERROR:
.PHONY: all clean
