; SPDX-License-Identifier: GPL-3.0-or-later
; Colour RAM from the CPU: fill $D800-$DBE7 with a pattern, read it back, scroll it one position with LDA/STA, read
; modify write it (INC abs,x and ASL abs,x on nibbles) and verify every step against a model computed in
; the program. $C101 counts errors, $C102 gets the upper nibble of a read ($D800 as the CPU sees it), $C100 = 1 when done.
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
        ; 1. fill: colour(i) = (i*7+3) & 15, 1000 bytes in four pages
        ldx #0
        lda #3
        sta $FB
f1      lda $FB
        sta $D800,x
        sta $D900,x
        sta $DA00,x
        clc
        adc #7
        and #$0F
        sta $FB
        inx
        bne f1
        ; (the page $DB is only 232 bytes: pattern continues)
        ldx #0
f2      lda $FB
        sta $DB00,x
        clc
        adc #7
        and #$0F
        sta $FB
        inx
        cpx #$E8
        bne f2
        ; the upper nibble the CPU sees
        lda $D800
        and #$F0
        sta $C102
        ; 2. verify the fill: page 0..2 got the same value per x (the pattern restarts per page: pages share $FB flow, so
        ;    each page holds the value written first in the loop; verify in the same order)
        ldx #0
        lda #3
        sta $FB
v1      lda $D800,x
        and #$0F
        cmp $FB
        beq v1a
        inc $C101
v1a     lda $D900,x
        and #$0F
        cmp $FB
        beq v1b
        inc $C101
v1b     lda $DA00,x
        and #$0F
        cmp $FB
        beq v1c
        inc $C101
v1c     lda $FB
        clc
        adc #7
        and #$0F
        sta $FB
        inx
        bne v1
        ; 3. scroll page $D8 down by one with LDA abs,x / STA abs,x (the scroller idiom), 255 times
        ldx #0
s1      lda $D801,x
        sta $D800,x
        inx
        cpx #$FF
        bne s1
        ; verify: D800+x = ((x+1)*7+3)&15 for x < 255
        ldx #0
        lda #10            ; (1*7+3)
        sta $FB
v2      lda $D800,x
        and #$0F
        cmp $FB
        beq v2a
        inc $C101
v2a     lda $FB
        clc
        adc #7
        and #$0F
        sta $FB
        inx
        cpx #$FF
        bne v2
        ; 4. read modify write on the nibbles: INC abs,x and ASL abs,x on page $D9; the model is in zero page
        ldx #0
r1      lda $D900,x
        and #$0F
        sta $FC            ; model: old value
        inc $D900,x        ; value+1 (the upper nibble is not stored)
        lda $D900,x
        and #$0F
        sta $FD
        lda $FC
        clc
        adc #1
        and #$0F
        cmp $FD
        beq r1a
        inc $C101
r1a     asl $D900,x
        lda $D900,x
        and #$0F
        sta $FD
        lda $FC
        clc
        adc #1
        asl
        and #$0F
        cmp $FD
        beq r1b
        inc $C101
r1b     inx
        bne r1
        lda #1
        sta $C100
        cli
        rts
