CXX ?= g++
BUILD := build
SRC := ../../..
CXXFLAGS := -std=c++14 -g -Wall -Wno-format -Wno-unused-variable -I.. -I$(BUILD) -I. \
	-I$(SRC)/components -I$(SRC)/infra -I$(SRC)/system -I$(SRC)/filemanager -include c64_crt_host.h

all: reu $(BUILD)/c64CrtTest $(BUILD)/trxFlashGeomTest
	./$(BUILD)/trxFlashGeomTest
	./$(BUILD)/c64CrtTest > $(BUILD)/loader.log

reu: $(BUILD)/trxRingReuTest
	./$(BUILD)/trxRingReuTest

# A08: the ring follows the REU enable; the decision is a header with no firmware in it.
$(BUILD)/trxRingReuTest: trx_ring_reu_test.cc ../trx_ring_reu.h
	mkdir -p $(BUILD)
	$(CXX) -std=c++14 -g -Wall -o $@ trx_ring_reu_test.cc

# The cartridge type constants and cart_def, taken from c64.h itself.
$(BUILD)/cart_defs.h: ../c64.h
	mkdir -p $(BUILD)
	sed -n -E -e '/^#define (CART_|VARIANT_)/p' -e '/^typedef struct _cart/,/^} cart_def;/p' $< > $@

$(BUILD)/c64CrtTest: c64_crt_test.cc c64_crt_host.h ../c64_crt.cc ../c64_crt.h $(BUILD)/cart_defs.h
	$(CXX) $(CXXFLAGS) c64_crt_test.cc ../c64_crt.cc $(SRC)/components/mystring.cc -o $@

$(BUILD)/trxFlashGeomTest: trx_flash_geom_test.cc ../trx_flash_geom.h ../trx_cart_desc.h
	mkdir -p $(BUILD)
	$(CXX) -std=c++14 -g -Wall -I.. trx_flash_geom_test.cc -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all reu clean
