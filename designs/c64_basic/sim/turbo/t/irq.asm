; SPDX-License-Identifier: GPL-3.0-or-later
; raster IRQ (line $F0) and CIA1 timer A IRQ (latch 2000) counted in $C100 (VIC) and $C102 (CIA);
; the main loop counts in $C104/105 so that the test sees it run
*=$C000
        sei
        lda #0
        sta $C100
        sta $C102
        sta $C104
        sta $C105
        lda #<irq
        sta $0314
        lda #>irq
        sta $0315
        lda #$7F
        sta $DC0D
        lda $DC0D
        lda #<2000
        sta $DC04
        lda #>2000
        sta $DC05
        lda #$81
        sta $DC0D
        lda #$11
        sta $DC0E
        lda #$F0
        sta $D012
        lda $D011
        and #$7F
        sta $D011
        lda #1
        sta $D01A
        sta $D019
        cli
main    inc $C104
        bne main
        inc $C105
        jmp main
irq     lda $D019
        and #1
        beq cia
        sta $D019
        inc $C100
cia     lda $DC0D
        and #1
        beq out
        inc $C102
out     jmp $EA81
