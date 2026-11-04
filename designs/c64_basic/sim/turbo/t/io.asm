; SPDX-License-Identifier: GPL-3.0-or-later
; one slow I/O access per iteration; counter in $C100/$C101. $C0F0 selects: 0 CIA read, 1 SID write, 2 VIC colour write
*=$C000
        sei
        lda #0
        sta $C100
        sta $C101
        lda $C0F0
        beq cia
        cmp #1
        beq sid
vic     inc $C100
        bne vic1
        inc $C101
vic1    sta $D020
        jmp vic
cia     inc $C100
        bne cia1
        inc $C101
cia1    lda $DC04
        jmp cia
sid     inc $C100
        bne sid1
        inc $C101
sid1    sta $D418
        jmp sid
