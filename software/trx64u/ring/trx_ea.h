// PROVENANCE: the firmware fork TREX/1541ultimate:software/io/c64/trx_ea.h, copied for the ring library (U18 D6: this copy is the master).
/*
 * trx_ea.h - what an instruction of the trace writes, from the trace record alone (T21 4.11).
 *
 * The trace record holds the opcode, its operand bytes and A X Y as they were BEFORE the
 * instruction. That is enough for the effective address of every store and read-modify-write
 * instruction with an absolute or zero page mode (the I/O writes of whowrote, which the RAM
 * write log, stream 0, does not carry). The modes through a zero page pointer, (zp,X) and
 * (zp),Y, need the pointer bytes of that moment: not in the record, reported as unresolved.
 *
 * Plain C, no includes beyond <stdint.h>: the firmware (trx_ring.cc) and the host tests of trxmon
 * use the same code.
 */
#ifndef TRX_EA_H
#define TRX_EA_H

#include <stdint.h>

enum {
    TRX_EA_NONE = 0,       /* writes nothing outside the stack (or loads, branches, ...) */
    TRX_EA_KNOWN = 1,      /* ea is the address written */
    TRX_EA_UNRESOLVED = 2  /* writes, but the address needs memory the record does not hold */
};

typedef struct
{
    uint16_t ea;
    uint8_t new_known; /* new_value is the byte written (a plain store); a read-modify-write has no
                          value the record gives */
    uint8_t new_value;
} trx_store_t;

/* addressing modes of the memory-writing opcodes */
enum { EAM_ZP, EAM_ZPX, EAM_ZPY, EAM_ABS, EAM_ABSX, EAM_ABSY, EAM_INDX, EAM_INDY, EAM_NONE };

/* cc=01 (STA), cc=00, cc=10, cc=11 by the bbb field of the opcode aaabbbcc: the mode, or EAM_NONE */
static inline int trx_ea_mode(uint8_t op)
{
    int aaa = op >> 5, bbb = (op >> 2) & 7, cc = op & 3;
    switch (cc) {
    case 1: /* ORA AND EOR ADC STA LDA CMP SBC: only STA ($81 85 8D 91 95 99 9D) writes */
        if (aaa != 4) {
            return EAM_NONE;
        }
        switch (bbb) {
        case 0: return EAM_INDX;
        case 1: return EAM_ZP;
        case 3: return EAM_ABS;
        case 4: return EAM_INDY;
        case 5: return EAM_ZPX;
        case 6: return EAM_ABSY;
        case 7: return EAM_ABSX;
        default: return EAM_NONE; /* $89 is a NOP #imm */
        }
    case 2: /* ASL ROL LSR ROR STX LDX DEC INC */
        if (aaa == 5) {
            return EAM_NONE; /* LDX */
        }
        if (aaa == 4) { /* STX $86 $8E $96, SHX $9E */
            switch (bbb) {
            case 1: return EAM_ZP;
            case 3: return EAM_ABS;
            case 5: return EAM_ZPY;
            case 7: return EAM_ABSY;
            default: return EAM_NONE; /* TXA, TXS, $82 */
            }
        }
        switch (bbb) { /* RMW: zp, abs, zp,X, abs,X; the others are the accumulator forms, NOPs, JAMs */
        case 1: return EAM_ZP;
        case 3: return EAM_ABS;
        case 5: return EAM_ZPX;
        case 7: return EAM_ABSX;
        default: return EAM_NONE;
        }
    case 0: /* BIT JMP JMP() STY LDY CPY CPX: only STY ($84 8C 94) and SHY ($9C) write */
        if (aaa != 4) {
            return EAM_NONE;
        }
        switch (bbb) {
        case 1: return EAM_ZP;
        case 3: return EAM_ABS;
        case 5: return EAM_ZPX;
        case 7: return EAM_ABSX;
        default: return EAM_NONE; /* DEY, TYA, branches, NOP #imm */
        }
    default: /* cc=3, the combined opcodes: SLO RLA SRE RRA SAX LAX DCP ISC */
        if (aaa == 5) {
            return EAM_NONE; /* LAX loads */
        }
        if (bbb == 2) {
            return EAM_NONE; /* the immediate ones: ANC ALR ARR XAA SBX SBC */
        }
        if (aaa == 4) { /* SAX $83 87 8F 97, SHA $93 9F, TAS $9B */
            switch (bbb) {
            case 0: return EAM_INDX;
            case 1: return EAM_ZP;
            case 3: return EAM_ABS;
            case 4: return EAM_INDY;
            case 5: return EAM_ZPY;
            default: return EAM_ABSY; /* 6: TAS $9B, 7: SHA $9F */
            }
        }
        switch (bbb) {
        case 0: return EAM_INDX;
        case 1: return EAM_ZP;
        case 3: return EAM_ABS;
        case 4: return EAM_INDY;
        case 5: return EAM_ZPX;
        case 6: return EAM_ABSY;
        default: return EAM_ABSX;
        }
    }
}

/* The write of one recorded instruction. op, b1, b2 and A X Y are the record's. */
static inline int trx_store_ea(uint8_t op, uint8_t b1, uint8_t b2, uint8_t a, uint8_t x, uint8_t y, trx_store_t *o)
{
    int mode = trx_ea_mode(op);
    if (mode == EAM_NONE) {
        return TRX_EA_NONE;
    }
    int cc = op & 3, aaa = op >> 5;
    o->new_known = 0;
    o->new_value = 0;
    o->ea = 0;
    if (mode == EAM_INDX || mode == EAM_INDY) {
        return TRX_EA_UNRESOLVED;
    }
    uint16_t base = (mode >= EAM_ABS) ? (uint16_t)(b1 | (b2 << 8)) : b1;
    uint16_t idx = 0;
    if (mode == EAM_ZPX || mode == EAM_ABSX) {
        idx = x;
    } else if (mode == EAM_ZPY || mode == EAM_ABSY) {
        idx = y;
    }
    int cross = 0;
    if (mode >= EAM_ABS) {
        o->ea = (uint16_t)(base + idx);
        cross = (uint8_t)(base >> 8) != (uint8_t)(o->ea >> 8);
    } else {
        o->ea = (uint8_t)(base + idx);
    }
    /* the plain stores know their byte; the read-modify-writes do not (the old byte is not in the record) */
    uint8_t hp1 = (uint8_t)((base >> 8) + 1); /* the SH* family ANDs the high byte of the BASE address + 1 */
    if (cc == 1) {                               /* STA */
        o->new_known = 1;
        o->new_value = a;
    } else if (cc == 0) {
        if (op == 0x9C) { /* SHY abs,X */
            o->new_known = 1;
            o->new_value = (uint8_t)(y & hp1);
            if (cross) {
                return TRX_EA_UNRESOLVED; /* the high byte of the address is the value on a page crossing */
            }
        } else { /* STY */
            o->new_known = 1;
            o->new_value = y;
        }
    } else if (cc == 2) {
        if (aaa == 4) {
            if (op == 0x9E) { /* SHX abs,Y */
                o->new_known = 1;
                o->new_value = (uint8_t)(x & hp1);
                if (cross) {
                    return TRX_EA_UNRESOLVED;
                }
            } else { /* STX */
                o->new_known = 1;
                o->new_value = x;
            }
        } /* else INC DEC ASL LSR ROL ROR: read-modify-write */
    } else {
        if (aaa == 4) {
            if (op == 0x9F || op == 0x9B) { /* SHA abs,Y, TAS */
                o->new_known = 1;
                o->new_value = (uint8_t)(a & x & hp1);
                if (cross) {
                    return TRX_EA_UNRESOLVED;
                }
            } else { /* SAX */
                o->new_known = 1;
                o->new_value = (uint8_t)(a & x);
            }
        } /* else SLO RLA SRE RRA DCP ISC: read-modify-write */
    }
    return TRX_EA_KNOWN;
}

#endif
