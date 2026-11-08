// Drive A's own state: its RAM, its 6502's registers and both 6522s
// (TRX64-Ultimate docs/specs/T20-delta-ring.md §9.3 "drive1541", T66 part 8).
//
// Way B, the one Alex chose on 2026-10-19: no gateware. The drive's RAM is in
// DDR2 and the app reads and writes it there; the VIA registers and the CPU's
// registers are not on any bus the app can reach, so a stub of about 130 bytes
// runs on the drive's own CPU, writes them into memory the app can read, and
// puts the drive back where it was.
//
// Where the stub lives: the free part of the stack page, below the drive's own
// stack pointer. Those bytes are dead for the program that is running (they
// are what it has already popped), so nothing has to be borrowed and nothing
// has to be put back. A drive whose stack pointer is too low for the stub is
// refused instead.
//
// How the stub is entered: the drive is halted at an instruction boundary, so
// the opcode at DPC is already in the CPU. A JMP therefore goes behind that
// instruction, not over it, and only after an instruction that has exactly one
// successor (no branch, jump, call or return) - the drive is stepped until it
// stands in front of one. The stub puts those three bytes back itself before
// it hands the drive on.
//
// What this cannot do, and says so (the limits Alex accepted with B):
//   - reading a 6522 clears interrupt flags (T1C-L, T2C-L, ORB, SR), and no
//     write can set them again. The IFR is read first, so the file has the
//     true value; the drive that runs on has lost those flags.
//   - the timers run during the stub's own cycles.
//   - the write-only T2 latch low is not readable.
//   - an interrupt taken between the JMP and the stub's SEI runs first.
#ifndef TRX_DRIVE_H
#define TRX_DRIVE_H

#include <stdint.h>

#define TRX_DRIVE_RAM 2048          // $0000-$07FF, mirrored up to $1800

struct TrxDriveState {
    bool     regs;                  // the stub ran: cpu and via* are there
    uint8_t  a, x, y, sp, p;
    uint16_t pc;
    uint8_t  via1[16], via2[16];    // $1800-$180F and $1C00-$1C0F as read
    uint32_t cycles;                // the drive's cycle counter when it was read
    uint8_t  ram[TRX_DRIVE_RAM];
};

#if (U64 == 2) && !defined(RECOVERYAPP)

// Is there a drive whose state can be read at all (the bitstream has the debug
// page and drive A is powered)?
bool trx_drive_present(void);

// RAM always, the registers when with_regs and the stub could be entered.
// NULL, or what went wrong; st->regs says whether the registers came.
const char *trx_drive_read(TrxDriveState *st, bool with_regs);

// Puts a state back: the RAM first, then - when st->regs - the registers and
// the VIAs through a stub that ends in an RTI to the saved PC.
const char *trx_drive_write(const TrxDriveState *st);

#else

static inline bool trx_drive_present(void) { return false; }
static inline const char *trx_drive_read(TrxDriveState *, bool) { return "No drive debug in this build"; }
static inline const char *trx_drive_write(const TrxDriveState *) { return "No drive debug in this build"; }

#endif

#endif
