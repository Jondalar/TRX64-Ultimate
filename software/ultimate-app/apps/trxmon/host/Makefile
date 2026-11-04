# Host unit tests for trxmon (no board, no cross compiler): make test
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -DTRXMON_BUILD=\"hosttest\"
SRC      = ../../../software/io/c64/trx_lens.c ../src/rpc.c ../src/mon.c ../src/mach.c ../src/cond.c ../src/ring.c ../src/trace.c ../src/dis6502.c ../src/asm6502.c ../src/spans.c ../src/ui.c \
           ../src/mjson.c ../src/ws.c ../src/mlib.c
OBJS     = test_main.c fake_hw.c $(SRC)

trxmon_test: $(OBJS) ../src/*.h fake_hw.h
	$(CC) $(CFLAGS) -I../src -I../../../software/io/c64 -o $@ $(OBJS)

# trxmon's own app_main over POSIX sockets and the fake debug block
trxmon_server: server_main.c fake_hw.c ../src/main.c $(SRC) ../src/*.h fake_hw.h
	$(CC) $(CFLAGS) -I../src -I../../../software/io/c64 -I../../../software/appsdk/include -o $@ server_main.c fake_hw.c ../src/main.c $(SRC)

# the screen session: app_main with a scripted keyboard, a text window, and the detached task as a thread
trxmon_ui_test: ui_test_main.c fake_hw.c ../src/main.c $(SRC) ../src/*.h fake_hw.h
	$(CC) $(CFLAGS) -pthread -I../src -I../../../software/io/c64 -I../../../software/appsdk/include -o $@ ui_test_main.c fake_hw.c ../src/main.c $(SRC)

# the one-connection rule over real sockets: app_main (src/main.c) against a WebSocket / plain TCP client
trxmon_net_test: net_test_main.c server_main.c fake_hw.c ../src/main.c $(SRC) ../src/*.h fake_hw.h
	$(CC) $(CFLAGS) -DKEEPALIVE_IDLE_MS=1500 -DKEEPALIVE_DEAD_MS=3000 -pthread -I../src -I../../../software/io/c64 -I../../../software/appsdk/include -o $@ net_test_main.c fake_hw.c ../src/main.c $(SRC)

# the effective address of the stores in a trace record (software/io/c64/trx_ea.h)
trx_ea_test: ea_test_main.c ../../../software/io/c64/trx_ea.h
	$(CC) $(CFLAGS) -I../../../software/io/c64 -o $@ ea_test_main.c

test: trxmon_test trxmon_server trxmon_ui_test trxmon_net_test trx_ea_test
	./trx_ea_test
	./trxmon_test
	./trxmon_ui_test
	./trxmon_net_test

clean:
	rm -rf trx_ea_test trx_ea_test.dSYM trxmon_net_test trxmon_net_test.dSYM trxmon_test trxmon_test.dSYM trxmon_server trxmon_server.dSYM trxmon_ui_test trxmon_ui_test.dSYM

.PHONY: test clean
