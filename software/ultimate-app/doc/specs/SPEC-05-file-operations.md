# SPEC-05 Smart mount and long extensions

| | |
|---|---|
| Status | Done (branch `feature/spiffy`) |
| Boards | U64, C64U |
| TODO | S3, S4 |
| Source | Spiffy `feb83add`, `b53fb820` |

## Requirements

### Smart mount (disk swaps)

- **R1** The context menu of a D64/G64 (and D71/G71/D81 where the same menu is built)
  puts **"Mount Disk" first** when the file is in the directory the last mount or
  run came from; otherwise "Run Disk" first as today.
- **R2** The check is at runtime and independent of the interface: Spiffy keys it off
  `#if COMMODORE`; ours picks the order with `ui_is_commodore()`
  (`filetype_d64.cc:77-86`) and keeps doing so for the first open.
- **R3** The "last directory" is kept in RAM and set by Mount and Run.

### Long extensions

- **R4** Copying a file keeps its extension when the destination file system can
  hold it (`index.html` stays `index.html`); the extension is only rewritten when
  it actually differs from the source's (`FileManager::fcopy`,
  `filemanager.cc:1373-1379`).

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | Two-disk game: Run disk 1, open disk 2's menu in the same directory: "Mount Disk" is first | emulator, both UI modes |
| A2 | Different directory: order as today | emulator |
| A3 | Copy `index.html` USB0 → Temp keeps the name | emulator e2e |
