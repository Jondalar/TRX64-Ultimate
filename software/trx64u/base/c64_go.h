// "Start at this address" for the C64 (T104.2 D6): the CPU's program counter set directly, so a program that was loaded over the zero page, the stack or
// the keyboard buffer - which a typed SYS needs intact - runs all the same. The debug block stops the machine on an instruction boundary, the CPU's
// flip-flops are staged through the state bus with PC and the fetched opcode replaced, and they load at the next edge the gate lets through (the monitor's
// path, trxmon hw_trx.c hw_cpu_write, T21 5.1.1). It needs CAPS2 b2; without it the caller types SYS. Moved here from MultiCore's coreapp/cores/c64_go.cc
// (t239.3 beee9b09) with the C64 descriptor; every access goes through the io group (core window, RAM window, ROM aperture) and needs the core lock.
#ifndef TRX64U_C64_GO_H
#define TRX64U_C64_GO_H

#include <stdint.h>
#include <stdbool.h>

// The bitstream has the CPU write path and the pages it is made of (CAPS b1 state bus, b3 debug, CAPS2 b2). False without the io group.
bool c64_go_available(void);

// The place hook of c64_go: called with the machine stopped on an instruction boundary and the debugger holding it, to write the program into RAM. 0 or a
// reason in msg.
typedef int (*c64_go_place_t)(void *user, char *msg, int msglen);

// Stops the machine on the next instruction boundary, calls `place` (null: nothing to place), points the CPU at `pc` and runs it. A failure on the way lets
// the machine go again and hands the debugger back: C64_STOP works as before. Returns 0, or a reason.
int c64_go(uint16_t pc, c64_go_place_t place, void *user, char *msg, int msglen);

#endif
