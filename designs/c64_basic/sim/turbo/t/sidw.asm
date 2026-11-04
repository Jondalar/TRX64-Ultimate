; SPDX-License-Identifier: GPL-3.0-or-later
; SID writes and reads against the engine's behaviour, using voice 3 (OSC3 $D41B, ENV3 $D41C):
;  1. order: gate on, off, on, off, on (written back to back): the envelope must end up high; the same ending off: low.
;  2. a burst of 40 writes to voice 1 and 2 and the filter, then the gate on: ENV3 rises (reads are held for the writes)
;  3. read after write: AD/SR then gate on, wait, read ENV3 = $FF (sustain F); gate off, wait, ENV3 = 0.
; $C101 counts errors, $C100 = 1 when done. Waits are CIA 2 reads (a PHI2 cycle each, whatever the speed).
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
        sta $D418
        ; voice 3: attack 0 (2 ms), decay 0, sustain 15, release 0 (6 ms): fast
        lda #$00
        sta $D413
        lda #$F0
        sta $D414
        lda #$00
        sta $D412
        jsr wait
        ; 1a. on off on off on
        lda #$11
        sta $D412
        lda #$10
        sta $D412
        lda #$11
        sta $D412
        lda #$10
        sta $D412
        lda #$11
        sta $D412
        jsr wait
        lda $D41C
        cmp #$FF
        beq o1
        inc $C101
        lda $D41C
        sta $C110
o1      ; 1b. ... off at the end
        lda #$11
        sta $D412
        lda #$10
        sta $D412
        lda #$11
        sta $D412
        lda #$10
        sta $D412
        jsr wait
        lda $D41C
        beq o2
        inc $C101
        lda $D41C
        sta $C111
o2      ; 2. burst of 40 writes to voice 1/2 and the filter, then the gate
        ldx #0
b1      txa
        sta $D400,x        ; voice 1 and 2 registers $D400-$D406, $D407-$D40D (the loop covers 0..13 and again)
        inx
        cpx #14
        bne b1
        ldx #0
b2      txa
        and #$07
        sta $D415,x        ; filter cutoff/resonance/mode registers $D415-$D417
        inx
        cpx #3
        bne b2
        ldx #0
b3      txa
        sta $D400,x
        inx
        cpx #14
        bne b3
        lda #$11
        sta $D412
        jsr wait
        lda $D41C
        cmp #$FF
        beq o3
        inc $C101
        lda $D41C
        sta $C112
o3      ; 3. read after write at once
        lda #$10
        sta $D412
        jsr wait
        lda #$11
        sta $D412
        lda $D41C          ; right after the write: the gate is on, the envelope may still be low, but the read must come after
        lda #$10
        sta $D412
        jsr wait
        lda $D41C
        beq o4
        inc $C101
o4      lda #1
        sta $C100
        lda #0
        sta $D418
        cli
        rts
wait    ldx #28
w0      ldy #0
w1      lda $DD04
        dey
        bne w1
        dex
        bne w0
        rts
