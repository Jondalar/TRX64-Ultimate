; SPDX-License-Identifier: GPL-3.0-or-later
; eight colours into $D020 eight CPU cycles apart on raster line $64 of every frame (display off,
; the whole line is border), then the base colour 0 again
*=$C000
        sei
        lda #$0B
        sta $D011
        lda #0
        sta $D020
frame   lda $D012
        cmp #$64
        bne frame
        lda $D011
        bpl chk
        jmp frame
chk     ldx #20
dl      dex
        bne dl
        lda #1
        sta $D020
        nop
        lda #2
        sta $D020
        nop
        lda #3
        sta $D020
        nop
        lda #4
        sta $D020
        nop
        lda #5
        sta $D020
        nop
        lda #6
        sta $D020
        nop
        lda #7
        sta $D020
        nop
        lda #8
        sta $D020
        lda #0
        sta $D020
        lda $D012
        cmp #$64
        beq *-5
        jmp frame
