; SPDX-License-Identifier: GPL-3.0-or-later
; An 8 KiB autostart cartridge that says it is there, for tests/bdd.
;
; The scenarios need a cartridge in the expansion port whose picture this
; repository may carry, so this is one: the CBM80 signature at $8004 makes the
; KERNAL jump into it at reset, and it writes one line on the screen and stays.
; No game data, no ROM: it is assembled here (tools/64tass.sh) and packed with
; tests/cart/bin2crt.py.
;
;   tools/64tass.sh -q -o port_probe.bin --nostart tests/bdd/data/port_probe.asm
;   tests/cart/bin2crt.py port_probe.bin port_probe.crt 0 --exrom 0 --game 1

                * = $8000

cold            .word coldstart
warm            .word coldstart
                .byte $C3, $C2, $CD, $38, $30   ; "CBM80"

coldstart       sei
                ldx #$ff
                txs
                cld
                stx $8004                       ; a write the ROM ignores: the
                                                ; KERNAL's own RAM test wants it
                jsr $fda3                       ; IOINIT
                jsr $fd50                       ; RAMTAS
                jsr $fd15                       ; RESTOR
                jsr $ff5b                       ; CINT
                cli
                lda #147                        ; clear the screen
                jsr $ffd2
                ldx #0
print           lda text,x
                beq hold
                jsr $ffd2
                inx
                bne print
hold            jmp hold

text            .text "TRX64 PORT PROBE"
                .byte 0

                .fill $a000 - *, $ff
