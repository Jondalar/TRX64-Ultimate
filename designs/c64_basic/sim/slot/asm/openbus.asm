; SPDX-License-Identifier: GPL-3.0-or-later
; T74 bench: an 8K cart (EXROM low, started through CBM80) with no IO1/IO2 of its own. The VIC's fetches are known
; ($3FFF idle $5C, $3F00-$3FFE refresh $11, sprite pointers $07F8-$07FF $22; DEN off, no sprites), the CPU writes $A5 to
; $DF80 / $DE80 and reads it back: the byte must be the VIC's, not the $A5 left on the port's bus. 256 reads each
; to $C200 (IO2) and $C300 (IO1), $AA to $C100 when done.
        * = $8000
        .word start, start
        .byte $c3, $c2, $cd, $38, $30
start   sei
        lda #$15
        sta $d018               ; the screen at $0400: the sprite pointer reads are $07F8-$07FF (CBM80 starts before IOINIT)
        lda #$0b
        sta $d011
        lda #0
        sta $d015
        ldx #0
-       lda #$11
        sta $3f00,x
        inx
        bne -
        lda #$5c
        sta $3fff
        ldx #7
-       lda #$22
        sta $07f8,x
        dex
        bpl -
        ldy #40
-       dex
        bne -
        dey
        bne -
        ldx #0
io2     lda #$a5
        sta $df80
        lda $df80
        sta $c200,x
        inx
        bne io2
io1     lda #$a5
        sta $de80
        lda $de80
        sta $c300,x
        inx
        bne io1
        lda #$aa
        sta $c100
-       jmp -
        .fill $8000 + $1f00 - *, $ea
