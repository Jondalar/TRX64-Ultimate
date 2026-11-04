# TREX specs

One spec per feature cut. Each has an id, the boards it concerns, testable
requirements, and the decisions still open. The backlog they come from is
`doc/TREX_TODO.md`; the architecture behind SPEC-01 is
`doc/TREX_Launcher_Architecture.html`.

Boards: **U64** = U64 MK1 (Nios II), **C64U** = U64-II / C64U (RISC-V).
Status: Draft (open decisions) → Agreed → Done.

| Spec | Title | TODO items | Boards | Status |
|---|---|---|---|---|
| [SPEC-01](SPEC-01-boot-selector.md) | Boot selector | T2 | both | Draft |
| [SPEC-02](SPEC-02-path-picker.md) | Path picker with memory | T1 | both | Done |
| [SPEC-03](SPEC-03-menu-hotkeys.md) | Menu hotkeys | S1, S2 | both | Done |
| [SPEC-04](SPEC-04-dialog-keys.md) | Dialog and settings keys | S5, S6 | both | Done |
| [SPEC-05](SPEC-05-file-operations.md) | Smart mount, long extensions | S3, S4 | both | Done |
| [SPEC-06](SPEC-06-robustness.md) | Heap figures, leaks, flash protection | S7, S8, S9 | both | Done |
| [SPEC-07](SPEC-07-menu-joystick-port2.md) | Menu control from joystick port 2 | S10 | C64U | Done (freeze menu); overlay untested |
| [SPEC-08](SPEC-08-config-files.md) | Assembly64 servers and branding from /Flash | S11, S12 | both | Won't do |
| [SPEC-09](SPEC-09-web-screen.md) | Web UI screen view, streams, readmem | P1, P2 | both | Done |

Rules for all of them:

- Everything runs on both boards unless the spec says otherwise, under `#if U64`;
  U2 / U2+ builds are left as they are.
- A key or setting that already exists keeps its meaning. A spec that needs a
  new key names the conflicts it checked.
- Acceptance is an e2e check in the emulator where the emulator can show it
  (`tests/e2e/`), and a named hardware check where it cannot.
- Code from the Spiffy / prkl forks is GPLv3 like ours; the commit it comes
  from is named in the spec and in the commit message.
