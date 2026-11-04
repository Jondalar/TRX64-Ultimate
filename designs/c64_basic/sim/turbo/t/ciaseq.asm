; SPDX-License-Identifier: GPL-3.0-or-later
; CIA access ordering: writes, read-modify-writes (read, dummy write, write) and reads of the DDRBs of both CIAs in
; a pseudo random sequence, compared with a model in zero page. $C101 counts errors, $C102/$C103 the passes,
; $C100 = 1 when 300 passes are done.
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
        sta $C102
        sta $C103
        lda #$5A
        sta $FB
loop    lda $FB
        asl
        bcc n1
        eor #$1D
n1      sta $FB
        sta $DD03          ; write
        sta $FC
        inc $DD03          ; RMW
        inc $FC
        lda $DD03
        cmp $FC
        bne bad
        asl $DD03
        asl $FC
        lda $DD03
        cmp $FC
        bne bad
        lsr $DD03
        lsr $FC
        lda $DD03
        cmp $FC
        bne bad
        lda $DD03          ; across the chips
        sta $DC03
        lda $DC03
        cmp $FC
        bne bad
        dec $DC03
        dec $FC
        lda $DC03
        cmp $FC
        bne bad
        ldx $DC03          ; read, write, read, back to back
        stx $DD03
        ldy $DD03
        cpy $FC
        bne bad
        jmp ok
bad     inc $C101
ok      inc $C102
        bne l2
        inc $C103
l2      lda $C103
        beq loop
        lda $C102
        cmp #<300
        bne loop
        lda #1
        sta $C100
        lda #0
        sta $DD03
        sta $DC03
        cli
        rts
