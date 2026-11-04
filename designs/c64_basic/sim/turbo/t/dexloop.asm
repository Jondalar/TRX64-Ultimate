; SPDX-License-Identifier: GPL-3.0-or-later
; DEX / BNE busy loop (the board's turbomeas loop), outer count 16 bit in $C100/$C101.
; One outer pass: LDX #0 (2) + 256 x DEX (2) + 255 taken BNE (3) + 1 not taken (2) + INC zp (5) +
; BNE (3) + JMP (3) = 1292 cycles (+ 6 when the low byte wraps).
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
outer   ldx #0
xl      dex
        bne xl
        inc $C100
        bne outer
        inc $C101
        jmp outer
