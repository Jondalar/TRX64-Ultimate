#
# Host build of the U16 HDMI mode request and the freeze still's geometry (software/io/c64/trx_hdmi.h).
#
#   make          build
#   make test     build and run
#
PRJ     = trxhdmitest
RESULT  = result
OUTPUT  = output

PATH_SW = ../../../../software

VPATH   = $(PATH_SW)/io/c64 $(PATH_SW)/test/trx_hdmi

CPP     = g++
CPPOPTIONS = -std=c++11 -g -O0 -Wall -Wextra -Wno-unused-parameter \
             $(addprefix -I, $(VPATH))

SRCS_CC = trx_hdmi_test.cc
OBJS    = $(addprefix $(OUTPUT)/, $(SRCS_CC:%.cc=%.o))

.PHONY: all test clean

all: $(RESULT)/$(PRJ)

$(OUTPUT)/%.o: %.cc trx_hdmi.h trx_hdmi_modes.h
	@mkdir -p $(OUTPUT)
	$(CPP) $(CPPOPTIONS) -c -o $@ $<

$(RESULT)/$(PRJ): $(OBJS)
	@mkdir -p $(RESULT)
	$(CPP) -o $@ $(OBJS)

test: all
	@$(RESULT)/$(PRJ)

clean:
	@rm -rf $(OUTPUT) $(RESULT)
