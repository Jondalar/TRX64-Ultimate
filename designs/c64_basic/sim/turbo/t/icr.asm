; SPDX-License-Identifier: GPL-3.0-or-later
; CIA 1 ICR read side effect at 64 phases against the PHI2 cycle: timer A one-shot (latch 3) sets the
; ICR flag with the mask off; a read at a phase (d NOPs after the last CIA read) must return 01 and the
; read right after it 00 (the read clears). $C200+d counts mismatches, $C100 = 1 when done.
; $C0F0 = 1: the same with a write of the timer start ($DC0E) as the access under test: the flag must
; appear (read 01) after a start written at the phase.
*=$C000
        sei
        lda #0
        sta $C100
        ldy #0
dloop   sty $C110
        sec
        lda #<sled_end
        sbc $C110
        sta jt1+1
        lda #>sled_end
        sbc #0
        sta jt1+2
        lda #0
        sta $C111
        ldx #24
rep     lda #$7F
        sta $DC0D          ; mask off
        lda $DC0D          ; clear
        lda #3
        sta $DC04
        lda #0
        sta $DC05
        lda #$19           ; start, one shot, force load
        sta $DC0E
        ; wait: a dozen CIA 2 reads (>= 12 PHI2 cycles)
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
        lda $DD04
jt1     jsr $0000
        lda $DC0D          ; read under test
        sta $C120
        lda $DD04
        lda $DD04
        lda $DC0D          ; must be cleared now
        sta $C121
        lda $C120
        cmp #1
        bne bad
        lda $C121
        beq ok1
bad     inc $C111
ok1     dex
        bne rep
        lda $C111
        sta $C200,y
        iny
        cpy #64
        beq done
        jmp dloop
done
        lda #1
        sta $C100
        cli
        rts
        .fill 64,$EA
sled_end rts
