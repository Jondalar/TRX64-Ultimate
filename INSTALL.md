# Installing TRX64 Ultimate

For the C64 Ultimate and the Ultimate 64 Elite II (Artix-7 XC7A50T). An update package is one file,
`trx64_update_<commit>_<YYYYMMDD>.cfw` (or the same as `.ue2`): the TRX64 Ultimate Updater with the bitstream and the app.
It contains no ROMs; the ROMs already in the machine's flash (from 0x400000) are not touched.

Update on mains power. Before you start, unplug a JTAG cable (USB Blaster) and switch the machine off and on once.

## From the stock firmware (Gideon's or Commodore's)

1. Copy the `.cfw` to a USB stick. Leave about 8 MB free on the stick or an SD card for the backup.
2. Power on, open the menu, insert the stick.
3. File browser → the `.cfw` → **Run Update**. The TRX64 Ultimate Updater appears.
4. Check installed vs. package and answer **YES**. Do not switch off or pull the stick (about a minute).
   After "Update OK" the machine switches itself off after 5 s.
5. Switch on. TRX64 Ultimate boots.

## Update from TRX64 Ultimate

The same steps with the `.cfw` (or the `.ue2`).

## Backup

Before it writes anything, the updater saves the current firmware on the USB stick or SD card. Without a medium with
enough free space it asks for one and does not update. It writes two files with the same number `nnn`
(`TRXBK_YYYYMMDD_nnn` when the date is known, otherwise `TRXBKnnn`):

- **`TRXBK…nnn.CFW`**: an installable restore package with the previous bitstream and app (flash 0x000000-0x3FFFFF)
  and the settings (0xFE8000-0xFFFFFF).
- **`TRXBK…nnn.BIN`**: the raw copy, with a checksum, read back after writing. It can only be put back over JTAG; it is
  the last way back when the bitstream no longer starts.

The ROMs (flash from 0x400000) are not in the backup; the update does not change them.

Keep both files and do not pass them on: they hold your network password in clear text.

## Going back

- **Restore the backup:** File browser → `TRXBK…nnn.CFW` → **Run Update**. The title shows "Restore backup …", then
  **YES**/**NO**, then "Restore your settings from the backup?" (default **NO**: your current settings stay). This works
  as long as the machine still boots to the file browser, and it also brings back Gideon's or Commodore's firmware.
- **Stock firmware:** run Gideon's or Commodore's own `.cfw`/`.ue2` from the TRX64 Ultimate menu the same way.
- **Bitstream dead:** write the `.BIN` back over JTAG.
