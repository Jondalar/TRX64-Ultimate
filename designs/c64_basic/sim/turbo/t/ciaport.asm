; SPDX-License-Identifier: GPL-3.0-or-later
; CIA 2 port B written and read back at once with DDRB = $FF: the read returns the pins as the CIA sampled them at PHI2's
; rising edge after the write took effect, i.e. what was written. Same for CIA 1 port A / B (DDRA = $FF, DDRB = 0 are the keyboard
; matrix: the pins carry our outputs on A). $C101 counts errors, $C102/$C103 the passes, $C100 = 1 when done.
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
        sta $C102
        sta $C103
        lda #$FF
        sta $DD03          ; CIA 2 DDRB: all outputs
        lda #$5A
        sta $FB
loop    lda $FB
        asl
        bcc n1
        eor #$1D
n1      sta $FB
        sta $DD01          ; write the port
        lda $DD01          ; read it at once
        cmp $FB
        beq ok1
        inc $C101
ok1     lda $FB
        eor #$FF
        sta $DD01
        lda $DD01
        eor #$FF
        cmp $FB
        beq ok2
        inc $C101
ok2     inc $C102
        bne l2
        inc $C103
l2      lda $C103
        beq loop
        lda $C102
        cmp #<400
        bne loop
        lda #1
        sta $C100
        lda #0
        sta $DD03
        cli
        rts
