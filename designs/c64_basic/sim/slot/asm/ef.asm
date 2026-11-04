; SPDX-License-Identifier: GPL-3.0-or-later
; T16 bench: an EasyFlash-like cart, ROMH bank 0 at $E000 (it boots in
; ultimax). IO2 RAM, a flash command sequence on ROML and ROMH, VIC fetches
; from ROMH ($D018: screen $3C00, chars $3000), then from RAM: bank 3 (IO1
; register read back, ROML in ultimax), bank 1, 16K, ROML/ROMH reads, done
; marker, a counter.
;   $0200/01 IO2 RAM read back, $0202 $DE00 read back, $0203 ROML bank 3
;   $8010, $0204 ROML bank 1 $8020 (16K), $0205 ROMH bank 1 $A030 (16K),
;   $0206 $AA done, $0207 loop counter, $0208 boots
        * = $e000
reset   sei
        ldx #$ff
        txs
        cld
        inc $0208
        lda #$5a
        sta $df00
        lda #$a5
        sta $df01
        lda $df00
        sta $0200
        lda $df01
        sta $0201
        lda #$aa
        sta $8555
        lda #$55
        sta $82aa
        lda #$a0
        sta $8555
        lda #$42
        sta $8123
        lda #$aa
        sta $e555
        lda #$fc
        sta $d018
        lda #$1b
        sta $d011
        ldy #$30
        ldx #0
-       dex
        bne -
        dey
        bne -
        ldx #0
-       lda stage2,x
        sta $0300,x
        inx
        cpx #stage2_end - stage2
        bne -
        jmp $0300
stage2  .logical $0300
        lda #$03
        sta $de00
        lda $de00
        sta $0202
        lda $8010
        sta $0203
        lda #$01
        sta $de00
        lda #$07
        sta $de02
        lda $8020
        sta $0204
        lda $a030
        sta $0205
        lda #$aa
        sta $0206
-       inc $0207
        jmp -
        .endlogical
stage2_end
        .fill $fffa - *, $ea
        .word reset, reset, reset
