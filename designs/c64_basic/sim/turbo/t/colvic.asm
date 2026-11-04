; SPDX-License-Identifier: GPL-3.0-or-later
; colour RAM rewritten for ever with the same values (colour index = low nibble of the offset): the picture must not
; change while the CPU writes (the VIC reads the same block RAM). Screen RAM is filled with a solid character
; and the border is black, so every pixel of the text area shows its colour RAM entry.
*=$C000
        sei
        ldx #0
        lda #$A0           ; reverse space: solid block
s0      sta $0400,x
        sta $0500,x
        sta $0600,x
        sta $0700,x
        inx
        bne s0
        lda #0
        sta $D020
        sta $D021
loop    ldx #0
l1      txa
        and #$0F
        sta $D800,x
        sta $D900,x
        sta $DA00,x
        sta $DB00,x
        inx
        bne l1
        jmp loop
