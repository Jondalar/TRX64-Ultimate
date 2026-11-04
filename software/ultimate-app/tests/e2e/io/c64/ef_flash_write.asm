; EasyFlash write stimulus for cartridge_autosave_test.py.
;
; Runs from RAM at $0800 with the cartridge in Ultimax mode, so ROML stays at
; $8000-$9FFF while it writes. The Ultimate does not emulate the flash chip's
; command sequences: a write of $65 to $DE09 arms exactly one write into the
; ROM window (all_carts_v5.vhd:466-471, :723), which is what the firmware's own
; EAPI does and what this routine does for every byte.
;
; The harness hands it work through RAM and reads the acknowledgement back over
; machine:readmem, so one running cartridge serves every scenario in the suite:
;
;   $0402   value to program; 0 when there is nothing to do
;   $0403   offset of that byte in the marker page at $9F00
;   $0404   incremented after each write, so the harness can wait for one
;   $0405   $01 once the routine is up
;   $0406   a bank; when set, the sector at that bank's ROML is erased through
;           the UCI, the way the firmware's EAPI erases (eapi.tas,
;           EAPIEraseSectorUCI), and $0404 is incremented as for a write

COMMAND    = $0402
OFFSET     = $0403
ACK        = $0404
RUNNING    = $0405
ERASE      = $0406
MARKERS    = $9f00

EF_BANK    = $de00
EF_MODE    = $de02
EF_ARM     = $de09
EF_ARM_KEY = $65
EF_ULTIMAX = $05                ; mode_bits %101: EXROM high, GAME low, EF enabled

UCI_CONTROL = $de1c             ; read: bits 5-4 the state (1x: data phase), bit 6 status there
UCI_COMMAND = $de1d
UCI_TARGET  = $04               ; the control target
UCI_EF      = $20               ; CTRL_CMD_EASYFLASH
UCI_PUSH    = $01
UCI_ACK     = $02
UCI_CLEAR   = $04               ; clear an earlier error

        * = $0800

start   sei
        lda #EF_ULTIMAX
        sta EF_MODE
        lda #$00
        sta EF_BANK             ; every write in this suite goes to bank 0
        sta COMMAND
        sta ACK
        sta ERASE
        lda #$01
        sta RUNNING

wait    lda ERASE
        bne sector
        lda COMMAND
        beq wait

        ldx OFFSET
        tay                     ; the value to program, kept across the arming write
        lda #EF_ARM_KEY
        sta EF_ARM
        tya
        sta MARKERS,x

        inc ACK
        lda #$00
        sta COMMAND
        beq wait                ; always taken

sector  tax                     ; the bank
        lda #UCI_CLEAR
        sta UCI_CONTROL
idle1   lda UCI_CONTROL
        and #$35                ; as EAPI's ultimateWait4idle
        bne idle1
        lda #UCI_TARGET
        sta UCI_COMMAND
        lda #UCI_EF
        sta UCI_COMMAND
        lda #$00                ; subcommand 0: erase a sector
        sta UCI_COMMAND
        stx UCI_COMMAND
        lda #$80                ; ROML
        sta UCI_COMMAND
        lda #UCI_PUSH
        sta UCI_CONTROL
reply   lda UCI_CONTROL
        and #$20                ; as EAPI's ultimateWait4data: until the reply is there
        beq reply
        lda #UCI_ACK
        sta UCI_CONTROL
idle2   lda UCI_CONTROL
        and #$35
        bne idle2

        inc ACK
        lda #$00
        sta ERASE
        beq wait                ; always taken
