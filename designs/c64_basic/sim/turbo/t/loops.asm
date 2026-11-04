; SPDX-License-Identifier: GPL-3.0-or-later
; INC zp / JMP loop counting, 16 bit in $C100/$C101 (the loop runs forever)
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
loop    inc $C100
        bne loop
        inc $C101
        jmp loop
