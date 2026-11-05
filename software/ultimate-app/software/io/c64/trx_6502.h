// The 6502's opcode lengths, and which opcodes go on at the next address.
// Shared: the instruction trace reads it to find the operand bytes
// (trx_ring.cc), and the drive stub needs it to put a JMP behind an
// instruction instead of over it (trx_drive.cc, T66 part 8).
#ifndef TRX_6502_H
#define TRX_6502_H

#include <stdint.h>

// Bytes of each opcode, undocumented ones included (a JAM is 1).
static const uint8_t trx_op_len[256] = {
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    3, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
};

// True when the instruction at pc is followed by the one at pc + length: no
// branch, jump, call, return or interrupt, and not one of the opcodes that
// stop the CPU (the JAMs, where no further boundary ever comes).
static inline bool trx_op_falls_through(uint8_t op)
{
    switch (op) {
    case 0x00:                                              // BRK
    case 0x20:                                              // JSR
    case 0x40:                                              // RTI
    case 0x4C: case 0x6C:                                   // JMP
    case 0x60:                                              // RTS
    case 0x10: case 0x30: case 0x50: case 0x70:             // the branches
    case 0x90: case 0xB0: case 0xD0: case 0xF0:
    case 0x02: case 0x12: case 0x22: case 0x32:             // the JAMs
    case 0x42: case 0x52: case 0x62: case 0x72:
    case 0x92: case 0xB2: case 0xD2: case 0xF2:
        return false;
    default:
        return true;
    }
}

#endif
