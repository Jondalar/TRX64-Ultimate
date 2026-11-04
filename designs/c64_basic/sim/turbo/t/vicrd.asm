; SPDX-License-Identifier: GPL-3.0-or-later
; VIC register reads: a pattern is written to every register of $D000-$D03F (the side effect free ones) and
; read back at once (table $C300+r), then all registers are read again in a row (table $C340+r). run.py compares
; the tables of a run at 1 MHz with runs in turbo. $C100 = 1 when done.
*=$C000
        sei
        lda #0
        sta $C100
        ; sprites off during the test would hide nothing: the registers are plain flops
        ldx #0
w1      cpx #$11
        beq nx
        cpx #$12
        beq nx
        cpx #$13
        beq nx
        cpx #$14
        beq nx
        cpx #$19
        beq nx
        cpx #$1E
        beq nx
        cpx #$1F
        beq nx
        cpx #$30
        bcs nx             ; $D030-$D03F: the turbo registers' page, not under test here
        txa
        asl
        asl
        clc
        adc #$25
        eor #$5C
        sta $D000,x
        lda $D000,x
        sta $C300,x
nx      inx
        cpx #$40
        bne w1
        ldx #0
w2      cpx #$11
        beq nx2
        cpx #$12
        beq nx2
        cpx #$13
        beq nx2
        cpx #$14
        beq nx2
        cpx #$19
        beq nx2
        cpx #$1E
        beq nx2
        cpx #$1F
        beq nx2
        cpx #$30
        bcs nx2
        lda $D000,x
        sta $C340,x
nx2     inx
        cpx #$40
        bne w2
        lda #1
        sta $C100
        cli
        rts
