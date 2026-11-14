; SPDX-License-Identifier: GPL-3.0-or-later
; T25.6 top-level check: what the app's MOD cartridge and Xander's audio_detect do, then two voices and an IRQ.
; Entered through the reset vector (the bench pokes the KERNAL ROM block), IRQ vector -> irq. Results: $C800 detection (1 found), $C801 IRQ count, $C802 version byte.
        * = $C000
        sei
        cld
        ldx #$FF
        txs
        lda #$0F
        sta $D418
        lda #$09
        sta $D405
        lda #$F0
        sta $D406
        lda #$25
        sta $D400
        lda #$1C
        sta $D401
        lda #$11
        sta $D404
        lda #0
        sta $DF20
        sta $DF40
        sta $DF60
        sta $DF80
        sta $DFA0
        sta $DFC0
        sta $DFE0
        lda #$FF
        sta $DF3F
        ldx #0
d1:     lda $DF20
        bne fail
        dex
        bne d1
        lda #0
        sta $DF21
        sta $DF24
        sta $DF25
        sta $DF26
        sta $DF27
        sta $DF2E
        sta $DF29
        lda #1
        sta $DF2F
        sta $DF2A
        lda #5
        sta $DF20
        ldx #$80
d2:     lda $DF20
        bne d3
        dex
        bne d2
        ldx #0
d3:     lda $DF20
        cmp #1
        bne fail
        dex
        bne d3
        lda #$FF
        sta $DF3F
        lda $DF21
        sta $C802
        lda #1
        sta $C800
        jmp play
fail:   lda #0
        sta $C800
        cli
        jmp *
play:   lda #0
        sta $DF20
        ; voice 0, left: sine 64 samples at REU $8000 (DDR2 $01008000), rate 250, loop
        lda #63
        sta $DF21
        lda #15
        sta $DF22
        lda #1
        sta $DF24
        lda #0
        sta $DF25
        lda #$80
        sta $DF26
        lda #0
        sta $DF27
        lda #0
        sta $DF29
        lda #$08
        sta $DF2A
        lda #0
        sta $DF2B
        lda #0
        sta $DF2E
        lda #250
        sta $DF2F
        lda #3
        sta $DF20
        lda #1
        sta $C804
        ; voice 1, right: sine 32 samples at $9000, rate 250
        lda #0
        sta $DF40
        lda #63
        sta $DF41
        lda #0
        sta $DF42
        lda #1
        sta $DF44
        lda #0
        sta $DF45
        lda #$90
        sta $DF46
        lda #0
        sta $DF47
        lda #0
        sta $DF49
        lda #$08
        sta $DF4A
        lda #0
        sta $DF4B
        lda #0
        sta $DF4E
        lda #250
        sta $DF4F
        lda #3
        sta $DF40
        lda #2
        sta $C804
        ; voice 2: 16 samples once with IRQ
        lda #0
        sta $DF60
        lda #0
        sta $DF61
        lda #1
        sta $DF64
        lda #0
        sta $DF65
        lda #$A0
        sta $DF66
        lda #0
        sta $DF67
        lda #0
        sta $DF69
        lda #0
        sta $DF6A
        lda #16
        sta $DF6B
        lda #0
        sta $DF6E
        lda #100
        sta $DF6F
        lda #5
        sta $DF60
        lda #3
        sta $C804
        cli
        jmp *
        * = $C200
irq:    pha
        lda $DF60               ; the status is the mask of all voices (ADR-0069): voice 2 is bit 2
        and #4
        beq irqe
        inc $C801
        lda #$FF
        sta $DF7F
irqe:   pla
        rti
