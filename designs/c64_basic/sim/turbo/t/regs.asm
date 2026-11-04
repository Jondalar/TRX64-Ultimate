; SPDX-License-Identifier: GPL-3.0-or-later
; turbo registers: the tb pokes a script at $C200: pairs {opcode kind, value}:
;   sequence of steps: each step reads or writes one address and stores to results
; Simple form: read a fixed list of addresses into $C100.., after the tb wrote $C0F0 = step id
*=$C000
        sei
; results
        ldx #0
rd      lda tab_lo,x
        sta $FB
        lda tab_hi,x
        sta $FC
        ldy #0
        lda ($FB),y
        sta $C100,x
        inx
        cpx #tab_lo_end-tab_lo
        bne rd
        lda #1
        sta $C0FF          ; done flag
        cli
        rts
tab_lo  .byte <$D030,<$D031,<$D0BC,<$D070,<$D071,<$D0B0,<$D0B1,<$D0F0,<$D0F1,<$D03C,<$D07C,<$D0FC,<$D0BD,<$D07A,<$D07B,<$D0FA
tab_lo_end
tab_hi  .byte >$D030,>$D031,>$D0BC,>$D070,>$D071,>$D0B0,>$D0B1,>$D0F0,>$D0F1,>$D03C,>$D07C,>$D0FC,>$D0BD,>$D07A,>$D07B,>$D0FA
