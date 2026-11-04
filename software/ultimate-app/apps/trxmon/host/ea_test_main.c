/* Host test of trx_ea.h (the effective address of the stores of a trace record, T21 4.11): every one of the
 * 256 opcodes against lists written down from the opcode map, then the addresses and values. */
#include <stdio.h>
#include <string.h>
#include "trx_ea.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

typedef struct { uint8_t op; int mode; } want_t;

/* the memory-writing opcodes and their modes, from the opcode map (6502 and the stable undocumented ones) */
static const want_t W[] = {
    /* STA */ { 0x85, EAM_ZP }, { 0x95, EAM_ZPX }, { 0x8D, EAM_ABS }, { 0x9D, EAM_ABSX }, { 0x99, EAM_ABSY },
    { 0x81, EAM_INDX }, { 0x91, EAM_INDY },
    /* STX */ { 0x86, EAM_ZP }, { 0x96, EAM_ZPY }, { 0x8E, EAM_ABS },
    /* STY */ { 0x84, EAM_ZP }, { 0x94, EAM_ZPX }, { 0x8C, EAM_ABS },
    /* INC DEC */ { 0xE6, EAM_ZP }, { 0xF6, EAM_ZPX }, { 0xEE, EAM_ABS }, { 0xFE, EAM_ABSX },
    { 0xC6, EAM_ZP }, { 0xD6, EAM_ZPX }, { 0xCE, EAM_ABS }, { 0xDE, EAM_ABSX },
    /* ASL LSR ROL ROR */ { 0x06, EAM_ZP }, { 0x16, EAM_ZPX }, { 0x0E, EAM_ABS }, { 0x1E, EAM_ABSX },
    { 0x46, EAM_ZP }, { 0x56, EAM_ZPX }, { 0x4E, EAM_ABS }, { 0x5E, EAM_ABSX },
    { 0x26, EAM_ZP }, { 0x36, EAM_ZPX }, { 0x2E, EAM_ABS }, { 0x3E, EAM_ABSX },
    { 0x66, EAM_ZP }, { 0x76, EAM_ZPX }, { 0x6E, EAM_ABS }, { 0x7E, EAM_ABSX },
    /* SLO */ { 0x07, EAM_ZP }, { 0x17, EAM_ZPX }, { 0x0F, EAM_ABS }, { 0x1F, EAM_ABSX }, { 0x1B, EAM_ABSY }, { 0x03, EAM_INDX }, { 0x13, EAM_INDY },
    /* RLA */ { 0x27, EAM_ZP }, { 0x37, EAM_ZPX }, { 0x2F, EAM_ABS }, { 0x3F, EAM_ABSX }, { 0x3B, EAM_ABSY }, { 0x23, EAM_INDX }, { 0x33, EAM_INDY },
    /* SRE */ { 0x47, EAM_ZP }, { 0x57, EAM_ZPX }, { 0x4F, EAM_ABS }, { 0x5F, EAM_ABSX }, { 0x5B, EAM_ABSY }, { 0x43, EAM_INDX }, { 0x53, EAM_INDY },
    /* RRA */ { 0x67, EAM_ZP }, { 0x77, EAM_ZPX }, { 0x6F, EAM_ABS }, { 0x7F, EAM_ABSX }, { 0x7B, EAM_ABSY }, { 0x63, EAM_INDX }, { 0x73, EAM_INDY },
    /* DCP */ { 0xC7, EAM_ZP }, { 0xD7, EAM_ZPX }, { 0xCF, EAM_ABS }, { 0xDF, EAM_ABSX }, { 0xDB, EAM_ABSY }, { 0xC3, EAM_INDX }, { 0xD3, EAM_INDY },
    /* ISC */ { 0xE7, EAM_ZP }, { 0xF7, EAM_ZPX }, { 0xEF, EAM_ABS }, { 0xFF, EAM_ABSX }, { 0xFB, EAM_ABSY }, { 0xE3, EAM_INDX }, { 0xF3, EAM_INDY },
    /* SAX */ { 0x87, EAM_ZP }, { 0x97, EAM_ZPY }, { 0x8F, EAM_ABS }, { 0x83, EAM_INDX },
    /* SHA SHX SHY TAS */ { 0x9F, EAM_ABSY }, { 0x93, EAM_INDY }, { 0x9E, EAM_ABSY }, { 0x9C, EAM_ABSX }, { 0x9B, EAM_ABSY },
};

int main(void)
{
    /* the classifier against the list, for all 256 opcodes */
    for (int op = 0; op < 256; op++) {
        int want = EAM_NONE;
        for (size_t i = 0; i < sizeof(W) / sizeof(W[0]); i++) {
            if (W[i].op == op) {
                want = W[i].mode;
            }
        }
        if (trx_ea_mode((uint8_t)op) != want) {
            printf("opcode %02x: mode %d, want %d\n", op, trx_ea_mode((uint8_t)op), want);
        }
        CHECK(trx_ea_mode((uint8_t)op) == want);
    }
    trx_store_t s;
    /* STA $D020 */
    CHECK(trx_store_ea(0x8D, 0x20, 0xD0, 5, 1, 2, &s) == TRX_EA_KNOWN && s.ea == 0xD020 && s.new_known && s.new_value == 5);
    /* STA $D000,X with X = $20; STA $D000,Y with Y = 2 */
    CHECK(trx_store_ea(0x9D, 0x00, 0xD0, 7, 0x20, 2, &s) == TRX_EA_KNOWN && s.ea == 0xD020 && s.new_value == 7);
    CHECK(trx_store_ea(0x99, 0x00, 0xD0, 7, 0x20, 2, &s) == TRX_EA_KNOWN && s.ea == 0xD002);
    /* the index wraps the 64 K, a zero page index the page */
    CHECK(trx_store_ea(0x9D, 0xF0, 0xFF, 7, 0x20, 0, &s) == TRX_EA_KNOWN && s.ea == 0x0010);
    CHECK(trx_store_ea(0x95, 0xF0, 0x00, 7, 0x20, 0, &s) == TRX_EA_KNOWN && s.ea == 0x0010);
    CHECK(trx_store_ea(0x96, 0xF0, 0x00, 7, 0, 0x20, &s) == TRX_EA_KNOWN && s.ea == 0x0010);
    /* STX, STY */
    CHECK(trx_store_ea(0x8E, 0x21, 0xD0, 1, 2, 3, &s) == TRX_EA_KNOWN && s.ea == 0xD021 && s.new_value == 2);
    CHECK(trx_store_ea(0x8C, 0x21, 0xD0, 1, 2, 3, &s) == TRX_EA_KNOWN && s.new_value == 3);
    /* read-modify-write: the address, no value */
    CHECK(trx_store_ea(0xEE, 0x20, 0xD0, 1, 2, 3, &s) == TRX_EA_KNOWN && s.ea == 0xD020 && !s.new_known);
    CHECK(trx_store_ea(0x1E, 0x00, 0xD0, 1, 0x20, 3, &s) == TRX_EA_KNOWN && s.ea == 0xD020 && !s.new_known);
    CHECK(trx_store_ea(0x1B, 0x00, 0xD0, 1, 2, 0x20, &s) == TRX_EA_KNOWN && s.ea == 0xD020 && !s.new_known); /* SLO abs,Y */
    /* SAX: A and X */
    CHECK(trx_store_ea(0x8F, 0x20, 0xD0, 0xF0, 0x3C, 0, &s) == TRX_EA_KNOWN && s.new_value == 0x30);
    /* through a zero page pointer: unresolved */
    CHECK(trx_store_ea(0x91, 0xFB, 0, 1, 2, 3, &s) == TRX_EA_UNRESOLVED);
    CHECK(trx_store_ea(0x81, 0xFB, 0, 1, 2, 3, &s) == TRX_EA_UNRESOLVED);
    CHECK(trx_store_ea(0x13, 0xFB, 0, 1, 2, 3, &s) == TRX_EA_UNRESOLVED);
    /* SH*: the value is ANDed with the high byte + 1; over a page crossing the address is not the plain sum */
    CHECK(trx_store_ea(0x9C, 0x00, 0xD0, 1, 2, 0xFF, &s) == TRX_EA_KNOWN && s.ea == 0xD002 && s.new_value == (0xFF & 0xD1));
    CHECK(trx_store_ea(0x9C, 0xFF, 0xD0, 1, 2, 0xFF, &s) == TRX_EA_UNRESOLVED);   /* SHY $D0FF,X with X = 2 crosses */
    CHECK(trx_store_ea(0x9E, 0xFF, 0xD0, 1, 2, 0xFF, &s) == TRX_EA_UNRESOLVED);   /* SHX $D0FF,Y with Y = FF crosses */
    CHECK(trx_store_ea(0x9E, 0x00, 0xD0, 1, 0xFF, 0x20, &s) == TRX_EA_KNOWN && s.ea == 0xD020 && s.new_value == (0xFF & 0xD1));
    /* not writers */
    CHECK(trx_store_ea(0xAD, 0x20, 0xD0, 1, 2, 3, &s) == TRX_EA_NONE);   /* LDA */
    CHECK(trx_store_ea(0x48, 0, 0, 1, 2, 3, &s) == TRX_EA_NONE);          /* PHA: the stack is RAM, in stream 0 */
    CHECK(trx_store_ea(0x20, 0x20, 0xD0, 1, 2, 3, &s) == TRX_EA_NONE);   /* JSR */
    CHECK(trx_store_ea(0x4C, 0x20, 0xD0, 1, 2, 3, &s) == TRX_EA_NONE);   /* JMP */
    printf("trx_ea_test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
