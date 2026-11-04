; SPDX-License-Identifier: GPL-3.0-or-later
; T16 bench: an 8K cart (EXROM low) started by the KERNAL (CBM80). Copies a stage to RAM, runs it there:
; CPU port $00 = $2F, $01 = $35 (LORAM 1, HIRAM 0: ROML hidden), marker $AA at $C100, then a counter at $C101 forever.
        * = $8000
        .word start, start
        .byte $c3, $c2, $cd, $38, $30
start   sei
        ldx #0
-       lda stage,x
        sta $0300,x
        inx
        cpx #stage_end - stage
        bne -
        jmp $0300
stage   .logical $0300
        lda #$2f
        sta $00
        lda #$35
        sta $01
        lda #$aa
        sta $c100
-       inc $c101
        jmp -
        .endlogical
stage_end
        .fill $8000 + $1f00 - *, $ea
