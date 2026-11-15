#
# Host build of the U16 HDMI mode request, the freeze still's geometry (software/io/c64/trx_hdmi.h) and the boot splash's
# place and picture in every row (software/io/c64/trx_splash.h, with the splash asset software/lvgl_port/assets/trx_splash.c).
#
#   make          build
#   make test     build and run
#
PRJ     = trxhdmitest
RESULT  = result
OUTPUT  = output

PATH_SW = ../../../../software

VPATH   = $(PATH_SW)/io/c64 $(PATH_SW)/test/trx_hdmi $(PATH_SW)/lvgl_port/assets

CPP     = g++
CC      = gcc
CPPOPTIONS = -std=c++11 -g -O0 -Wall -Wextra -Wno-unused-parameter \
             $(addprefix -I, $(VPATH))
COPTIONS = -std=c99 -g -O0 -Wall -Wextra

SRCS_CC = trx_hdmi_test.cc
SRCS_C  = trx_splash.c
OBJS    = $(addprefix $(OUTPUT)/, $(SRCS_CC:%.cc=%.o) $(SRCS_C:%.c=%.o))

.PHONY: all test clean

all: $(RESULT)/$(PRJ)

$(OUTPUT)/%.o: %.cc trx_hdmi.h trx_hdmi_modes.h trx_splash.h
	@mkdir -p $(OUTPUT)
	$(CPP) $(CPPOPTIONS) -c -o $@ $<

$(OUTPUT)/%.o: %.c
	@mkdir -p $(OUTPUT)
	$(CC) $(COPTIONS) -c -o $@ $<

$(RESULT)/$(PRJ): $(OBJS)
	@mkdir -p $(RESULT)
	$(CPP) -o $@ $(OBJS)

test: all
	@$(RESULT)/$(PRJ)

clean:
	@rm -rf $(OUTPUT) $(RESULT)
