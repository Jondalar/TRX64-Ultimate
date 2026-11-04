#
# Host build of the reSID8 filter mapping (software/u64/resid8_filter.h).
#
#   make          build
#   make test     build and run
#
PRJ     = resid8test
RESULT  = result
OUTPUT  = output

PATH_SW = ../../../../software

VPATH   = $(PATH_SW)/u64 $(PATH_SW)/test/resid8

CPP     = g++
CPPOPTIONS = -std=c++11 -g -O0 -Wall -Wextra -Wno-unused-parameter \
             $(addprefix -I, $(VPATH))

SRCS_CC = resid8_filter_test.cc
OBJS    = $(addprefix $(OUTPUT)/, $(SRCS_CC:%.cc=%.o))

.PHONY: all test clean

all: $(RESULT)/$(PRJ)

$(OUTPUT)/%.o: %.cc resid8_filter.h
	@mkdir -p $(OUTPUT)
	$(CPP) $(CPPOPTIONS) -c -o $@ $<

$(RESULT)/$(PRJ): $(OBJS)
	@mkdir -p $(RESULT)
	$(CPP) -o $@ $(OBJS)

test: all
	@$(RESULT)/$(PRJ)

clean:
	@rm -rf $(OUTPUT) $(RESULT)
