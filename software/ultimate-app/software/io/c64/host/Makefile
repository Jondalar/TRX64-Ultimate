# Host tests for the .c64re container pieces (T66 part 9): no board, no cross
# compiler, no hardware access. What is tested here is everything in the load
# path that is pure - the inflate (trx_inflate.c) and the chip block built from
# a file's own nodes (trx_c64re_nodes.cc). The rest of trx_c64re.cc needs the
# file system and the state bus and is tested on the board.
#
# make test also runs stub_test.py: the two 6502 stubs of T66 part 8 in a small
# interpreter, the only place they can be tried without a drive.
#
# zlib (the system's) writes the compressed input: the decoder is checked
# against the real compressor at every level and strategy, not against a stored
# vector.
CXX     ?= c++
CXXFLAGS ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fsanitize=address,undefined
INC      = -I.. -I../../../api
SRC      = ../trx_inflate.c ../trx_c64re_nodes.cc ../trx_sha256.c

c64re_test: test_main.cc $(SRC) ../trx_inflate.h ../trx_c64re_nodes.h ../trx_json.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ test_main.cc $(SRC) -lz

test: c64re_test
	./c64re_test
	python3 -I stub_test.py

clean:
	rm -rf c64re_test c64re_test.dSYM

.PHONY: test clean
