; SPDX-License-Identifier: GPL-3.0-or-later
; speed changes at random while RAM, CIA and VIC register accesses are checked.
; $C100 errors, $C102/3 iterations, $C104 last failure kind
*=$C000
        sei
        lda #0
        sta $C100
        sta $C102
        sta $C103
        lda #$A7
        sta $FB                 ; lfsr
loop    ; next random
        lda $FB
        asl
        bcc nox
        eor #$1D
nox     sta $FB
        and #$0F
        tax
        lda $FB
        and #$80
        sta $FC                 ; badline bit as $D031 bit 7
        txa
        ora $FC
        sta $D031               ; the speed changes here
        lda $FB
        sta $FD
        ; RAM: 32 bytes
        ldy #31
w1      lda $FD
        clc
        adc $FB
        sta $4000,y
        dey
        bpl w1
        ldy #31
        lda $FD
        clc
        adc $FB
        sta $FE
r1      lda $4000,y
        cmp $FE
        beq r1ok
        lda #1
        sta $C104
        inc $C100
r1ok    dey
        bpl r1
        ; CIA2 DDRB
        lda $FB
        sta $DD03
        lda $DD03
        cmp $FB
        beq c1ok
        lda #2
        sta $C104
        inc $C100
c1ok    ; VIC sprite colour register (4 bits)
        lda $FB
        sta $D027
        lda $D027
        eor $FB
        and #$0F
        beq v1ok
        lda #3
        sta $C104
        inc $C100
v1ok    ; stack and zero page
        lda $FB
        pha
        pla
        cmp $FB
        beq s1ok
        lda #4
        sta $C104
        inc $C100
s1ok    inc $C102
        bne j1
        inc $C103
j1      jmp loop
