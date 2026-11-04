; T74: a CPU read of an address nobody answers returns the VIC's phi1 byte. DEN off: the VIC only does idle
; accesses ($3FFF), the 5 refresh reads of a line ($3F00+n) and the 8 sprite pointer reads ($07F8+n). 256 reads of TARGET go to $C200 (PRE: write $A5 first).
*=$C000
        sei
        lda #$0B
        sta $D011               ; DEN off
        lda #$00
        sta $D015
        ldx #0
fill    lda #$11
        sta $3F00,x
        inx
        bne fill
        lda #$5C
        sta $3FFF
        ldx #7
ptr     lda #$22
        sta $07F8,x
        dex
        bpl ptr
        ldy #3                  ; DEN is taken at line $30 of a frame: wait for three frames
frame   lda $D012
        cmp #$F0
        bne frame
f2      lda $D012
        cmp #$F0
        beq f2
        dey
        bne frame
        lda #0
        sta $C0FF
        ldx #0
loop
.if PRE
        lda #$A5
        sta TARGET
.endif
        lda TARGET
        sta $C200,x
        inx
        bne loop
        lda #1
        sta $C0FF
        jmp *
