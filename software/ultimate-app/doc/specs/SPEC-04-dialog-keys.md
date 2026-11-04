# SPEC-04 Dialog and settings keys

| | |
|---|---|
| Status | Done (branch `feature/spiffy`) |
| Boards | U64, C64U |
| TODO | S5, S6 |
| Source | Spiffy `72c04833`, `16a75020`, `fd499929` |

## Requirements

- **R1** ChoiceBox (`ui_elements.cc:530-590`): ← and joystick left cancel,
  → and joystick right confirm the highlighted choice. Existing keys unchanged.
- **R2** `UserInterface::choice` calls `deinit()` on the box before deleting it
  (`userinterface.cc:915` deletes without it), so the screen under the box is
  restored and nothing leaks.
- **R3** Settings (`config_menu.cc:283-285`): → on an item opens it for editing
  like RETURN does today, not only on a group.

## Acceptance

| # | Check | Where |
|---|---|---|
| A1 | A choice dialog closes on ← (cancel) and on → (confirm), with the screen as before | emulator, both UI modes |
| A2 | Heap free is the same before and after 50 choice dialogs (`GET /v1/machine:heap`) | emulator with `--net` or hardware |
| A3 | → on a settings item opens it | emulator |
