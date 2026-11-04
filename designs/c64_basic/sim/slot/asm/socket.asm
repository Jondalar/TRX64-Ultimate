; SPDX-License-Identifier: GPL-3.0-or-later
; T37 bench: a raster-IRQ handler, installed in RAM at $C000 over the KERNAL's CINV ($0314), that each frame writes
; 25 registers of the SID in socket 1 ($D400) and socket 2 ($D420) with CPU cycles (STA abs,X has its dummy read), reads
; them back into RAM ($C100 / $C120) and counts the rounds at $C0F1.
        * = $C000
irq     inc $c0f0
        ldx #0
w1      txa
        eor #$5a
        sta $d400,x
        inx
        cpx #25
        bne w1
        ldx #0
w2      txa
        eor #$a5
        sta $d420,x
        inx
        cpx #25
        bne w2
        ldx #0
r1      lda $d400,x
        sta $c100,x
        inx
        cpx #25
        bne r1
        ldx #0
r2      lda $d420,x
        sta $c120,x
        inx
        cpx #25
        bne r2
        inc $c0f1
        jmp $ea31
