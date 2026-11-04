; SPDX-License-Identifier: GPL-3.0-or-later
; T16 bench: an 8K cart (EXROM low) that the KERNAL starts through CBM80.
; Copies ROML $9F00-$9FFF to $C000, then $AA to $C100. With BIG = 1 (16K)
; also ROMH $BF00-$BFFF to $C200.
        * = $8000
        .word start, start
        .byte $c3, $c2, $cd, $38, $30
start   sei
        ldx #0
-       lda $9f00,x
        sta $c000,x
.if BIG
        lda $bf00,x
        sta $c200,x
.endif
        inx
        bne -
        inc $c101
        lda #$aa
        sta $c100
-       jmp -
        .fill $8000 + $1f00 - *, $ea
