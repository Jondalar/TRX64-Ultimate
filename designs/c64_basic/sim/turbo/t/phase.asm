; SPDX-License-Identifier: GPL-3.0-or-later
; CIA2 DDRB ($DD03) write/read back at 64 different phases against the PHI2 cycle:
; the delay before the access is d NOPs (d = 0..63); $C200+d counts the mismatches.
; $C100 = 1 when done.
*=$C000
        sei
        lda #0
        sta $C100
        ldy #0
dloop   sty $C110
        ; patch the JSR target: sled_end - d
        sec
        lda #<sled_end
        sbc $C110
        sta jt1+1
        sta jt2+1
        sta jt3+1
        sta jt4+1
        lda #>sled_end
        sbc #0
        sta jt1+2
        sta jt2+2
        sta jt3+2
        sta jt4+2
        lda #0
        sta $C111          ; mismatches for this d
        ldx #24
rep     lda #$A5
jt1     jsr $0000
        sta $DD03
jt2     jsr $0000
        lda $DD03
        cmp #$A5
        beq ok1
        sta $C120
        lda #$A5
        sta $C121
        sty $C122
        inc $C111
ok1     lda #$5A
jt3     jsr $0000
        sta $DD03
jt4     jsr $0000
        lda $DD03
        cmp #$5A
        beq ok2
        sta $C120
        lda #$5A
        sta $C121
        sty $C122
        inc $C111
ok2     dex
        bne rep
        lda $C111
        sta $C200,y
        iny
        cpy #64
        bne dloop
        lda #0
        sta $DD03
        lda #1
        sta $C100
        cli
        rts
        .fill 64,$EA
sled_end rts
