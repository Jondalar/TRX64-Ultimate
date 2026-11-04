; SPDX-License-Identifier: GPL-3.0-or-later
; write the value at $C0F1 to the address at $C0F2/$C0F3, then set the flag $C0FE
*=$C000
        sei
        lda $C0F1
        ldx $C0F2
        stx $FB
        ldx $C0F3
        stx $FC
        ldy #0
        sta ($FB),y
        lda #1
        sta $C0FE
        cli
        rts
