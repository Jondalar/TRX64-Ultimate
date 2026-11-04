# SPEC-02 Path picker with memory

| | |
|---|---|
| Status | Done (branch `feature/path-picker`) |
| Boards | U64, C64U |
| TODO | T1 |
| Code | `pick_path()`, `software/userinterface/tree_browser.cc:821` |

## Purpose

Picking a destination directory today starts at `/` every time: `pick_path()`
builds a fresh `BrowsableRoot` and has no start path (`tree_browser.cc:827-833`).
Our own picker before 3.15 started in the caller's path and switched to a
text box when the user typed (`3a4c8b43`, `8a23e350`); upstream removed it in
`88fe1ef3`. This restores that for us, in one place, so every caller gets it.

## Callers (all get the change)

| Caller | Where |
|---|---|
| Copy to… | `user_file_interaction.cc:303` |
| Move to… | `user_file_interaction.cc:330` |
| IEC path | `iec_ui.cc:78` |
| Apps: `ui_path_input` (dumper save location) | `app_api_impl.cc:686` |

## Requirements

### Where it starts

- **R1** `pick_path()` takes an optional start path.
- **R2** Start directory, first that exists:
  1. the path last picked in this picker (any caller),
  2. the start path the caller passes (e.g. the browser's current directory),
  3. `/`.
- **R3** A remembered path that no longer exists (stick removed, directory
  deleted) falls back to its nearest existing parent, then to `/`. No error popup.
- **R4** The remembered path lives in RAM and is lost at power-off (Q1).
- **R5** A cancelled pick does not change the remembered path.

### Keys inside the picker

| Key | Action | Today |
|---|---|---|
| ↑ ↓ PgUp PgDn | move | same |
| → / RETURN | enter directory; on `<< Select Current Dir >>` pick it | same |
| ← | up one level; at `/` leave the picker | same |
| **HOME** | go to the home directory (`CFG_USERIF_HOME_DIR`), or `/` when none is set | new |
| **SHIFT-HOME (CLR)** | go to `/` | new |
| **/** | open a text box with the current path filled in (Q2) | new |
| letters, digits | quick seek to an entry starting with them | same (`seek_char`, `tree_browser.cc:380`) |
| RUN/STOP, F8 | cancel | same |

- **R6** The text box (key `/`) accepts an absolute path. RETURN on an existing
  directory moves the picker there; on a path that does not exist, a popup says
  "Not found: <path>" and names the nearest existing parent, and the picker moves
  there. It does not create directories.
- **R7** Quick seek stays as it is; typing letters does not open the text box
  (that is why the text box has its own key).

### Display

- **R8** The picker's path row shows the current path while navigating (it is the
  path that `<< Select Current Dir >>` would pick). The existing path row already
  does this; nothing to add.

## Non-goals

- File pickers (`PICK_SAVE`, "Create New File"): same idea, separate step once
  this is done.
- Remembering per caller (one memory for all callers, Q3).
- Upstream PR (convenience for us; our change, our merge cost).

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | First pick starts at `/`; after picking `/USB0/games`, the next pick (any caller) starts in `/USB0/games` | emulator e2e (dumper save location, `tests/e2e/cart/dumper_test.py` flow) |
| A2 | Remembered path on a removed stick: picker opens at `/` without a popup | emulator (USB dir swapped) |
| A3 | HOME goes to the home directory; SHIFT-HOME to `/` | emulator |
| A4 | `/` opens the text box with the current path; RETURN on an existing path moves there; on a missing one shows the parent and the message | emulator |
| A5 | Cancel keeps the remembered path | emulator |
| A6 | Copy/Move/IEC still work in both interfaces | existing e2e + manual on both boards |

## Decisions taken (defaults of the draft)

- **Q1** RAM only; after power-off the picker starts from the caller's path again.
- **Q2** `/` opens the text box: no FAT name starts with it, so quick seek keeps
  every letter.
- **Q3** One memory for all callers.

## Implementation

`pick_path(ui, path, start)` in `tree_browser.cc`; callers pass the browser's
directory (Copy/Move) or the app's buffer (`ui_path_input`, empty for "where the
last pick went"). Test: `tests/e2e/filemanager/path_picker_test.py`, 8 checks per
interface.
