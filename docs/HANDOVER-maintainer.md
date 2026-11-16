# Handover: maintainer of TRX64 Ultimate

From the session that worked on TRX64 Ultimate out of `~/Development/1541ultimate` (2026-10-25), to the session that starts in this
repository. Not exported to GitHub (it speaks of MultiCore; T68 export rules).

## 1. Your role ([ADR-0058](adr/0058-trx64-ultimate-has-its-own-maintainer.md))

Alex 2026-10-25 (translated): "The new session there shall become the maintainer of TRX64 Ultimate, without ifs and buts. Not bound to
Gideon's 1541, and its firmware is only 'loaded' in MultiCore. It is also an equal developer of the LVGL UI and the app."

- You own this repository: TODO, specs, ADRs, gateware, the app submodule, releases. Alex sets order and scope; you keep status.
- The 1541ultimate fork is not your master. The app lives here as `software/ultimate-app` and is yours to change.
- MultiCore (`TREX/TRX64U-MultiCore`, session "TRX64-MC") only loads TRX64 Ultimate's firmware (full bitstream via IPROG, MC T115).
- What the two sides owe each other is written down: [docs/contracts/trx64u-firmware-handover.md](contracts/trx64u-firmware-handover.md)
  (agreed 2026-10-30, the same text in both repositories). It covers who builds what, how a state moves over by pins, the hand-over
  block at `0x03EFF000`, where the settings go, foreign `.cfw` conversion, and what either side may change alone.
- On the LVGL UI and the generic core app (MC T104, T104.3) you and TRX64-MC are equals. You build here, isolated; MC gets a hand-over
  when it is round. Never push into MultiCore's CI while its host rework runs (it has one builder; run 963 of 2026-10-25 blocked it).

## 2. How to build (Alex 2026-10-25)

"Du solltest es lokal compilen, mit dem Emulator testen, bis eine vollwertige App da ist, wie fürs board und als letzter Schritt geht es
aufs Board." (Compile locally, test with the emulator until a complete app is there, as for the board; the board is the last step.)

- Compile locally in the build container. The image `u64-build-env` is NOT on the Mac at the moment; build it with
  `container build --arch amd64 --tag u64-build-env <1541ultimate>/build/` (amd64, it runs under Rosetta; an arm64 build gives a toolchain
  that does not run). Builds only in containers, never on the NAS file system.
- Test in ue2emu (release 0.6.0, S43: `brew upgrade ue2emu`, or `~/Development/u64-emulator/target/release/ue2emu`):
  `ue2emu run --board c64u --caps 25E40202 --firmware <elf or .ue2> --roms <dir> --flash <flash.bin> --c64-roms <dir>
  --cart-slot <crt>[,rw|,save=OUT] --usb-dir <dir> --net user --web-port ... --control 127.0.0.1:<port>`. trx-io is on by itself.
  S43 models CAPS3 = 0x01 (RS8 filter bypass only), drive A as 1541 only, the slot block (page 7). The MCP `emu_button` fails with
  trx-io; drive the control port with your own client (the old one: `emu.py`, commands `button`, `key NAME MS`, `type`, `screen`,
  `png PATH`, `expect-console "text" ms`, `quit`; it lived in the old session's scratchpad and is gone, rewrite it).
- "Built" means: ran in the emulator end to end. Native benches are gates, not the proof.
- Board: only after asking Alex, every access (REST, Telnet, FTP, JTAG, even reading). Alex power-cycles, never you.

## 3. State of the repository

- `main` carries the T135 installer since 2026-10-30. Release in use: b151154b (app 234a0abb, capability word 0x25E40202, `top.v:601`).
- The app submodule is pinned at 558a8ed5 on `trx64/main` (2026-10-30, [ADR-0059](adr/0059-t135-installer-and-our-own-application-version.md)):
  the T135/T132 installer from MultiCore's `t135-installer` (32e5fd5f, fast-forwarded onto `trx64/main`) plus
  `APPL_VERSION_NUMBER "0.2.0"` in place of Gideon's `3.15`. Before that it was 234a0abb; commit 00f370c (U3, 2026-10-25) had moved the
  gitlink to 260aeadf (a MultiCore branch, `slim-core-app`) by accident. The working tree in `software/ultimate-app` is checked out at
  `trx64/main` and has an untracked `u64ii/`.
- Worktrees beside it: `TRX64-Ultimate-wt-integ` (integ-m1), `TRX64-Ultimate-wt-t37` (t37-sockets); not from the last session.
- Remotes: `origin` = Gitea `TREX/TRX64-Ultimate`, `mc` = Gitea MultiCore. GitHub `Jondalar/TRX64-Ultimate` (private) holds
  exports of the tree plus the wiki (pages User-Manual, Manual-01..10 from `docs/manual`, Home, Architecture, Capabilities, Resources from `docs/wiki`); never push to GitHub without asking Alex. The export is
  `tools/gh_export.py` (rules and checks in T68): since 2026-11-13 everything goes out except Commodore's ROMs, Gideon's
  C64U constraints and the third-party test files, and each host commit is an export commit of its own
  (`--replay SINCE`); `--check` first, push only when Alex said so.

## 4. Open work (TODO.md is the list; details in the specs)

- **U3 / MC T104.3, the generic core app replaces Gideon's app** ([U3](specs/U3-generic-app-replaces-gideons-app.md), ADR-0057):
  - Decided by Alex 2026-10-25: apps are self-contained `.u2a` containers that hook into the declarative menus like libraries; the
    cartridge tools bring their own modern LVGL UI in a container the base provides; `trxmon` keeps the 80-column terminal.
  - Built so far, on MultiCore's Gitea branch `t104-3-app-ui` (spec `docs/specs/T104.3-applications.md` there), proven on native benches
    only, never compiled for RISC-V, never run in ue2emu:
    - fe1340bf: `LibLoader::run`, `app_main`, svc `sys`/`mem`/`ops`, one heap account per app;
    - 65f27818: svc `fs` (with the `/flash` rule), `CoreFs::mkdir/rename`, `api->ui` (`appui.cc`: window, keys, `on_close`, six dialogs);
    - d406adcf: `api->job` and `ui->post` (`appjob.cc`, progress strip, stop-and-join on close and app end);
    - 448932fa: Alex's build rule in the spec.
  - Next: bring that code here (isolated branch), the coreapp RISC-V build, then the coreapp on the monolith in ue2emu (U3 order step 1,
    "emulator first"), then step 4 of T104.3 (menu hooks, the board's `AppUiPort` in `run.cc`), then `unzip`, `softboot`, `trex_crt`
    (new UI), `trxmon` on ABI 3 (S7 in ue2emu), board (S8) last.
  - Open for Alex, one question at a time: T104.3 **Q2** (LVGL access: import table in the `.u2a`, recommended, or a function table),
    Q1 (may a container stay open while the machine runs), Q3 (trust model); the ten U3 decisions (§5 of U3, first: update/install path
    and rollback gate).
  - MultiCore asked (2026-10-25): no push to `TREX/TRX64U-MultiCore` until TRX64-MC says main is new (after t115-soc); then rebase
    `t104-3-app-ui` onto the new main and push only with an empty CI queue.
- **U2 user manual** (`docs/manual/`, on GitHub too): board pictures open (only with Alex's OK); four findings to fix: the Timewarp popup
  overflows the 40-column box in the Overlay menu; the reSID8 page is listed as "UltiSID Configuration"; the Commodore menu shows
  MODEMS/PRINTERS/DRIVE B which open nothing; `trxmon rewind` still prints F9-F12.
- **U1** trex_crt Erase Cartridge + staging (branch `u1-crt-erase`, scope decided, not built). The other open rows: see TODO.md (no milestones since 2026-11-07, ADR-0066).
- MultiCore host splash is slow in `probe_all` -> `cfw_probe` (walks whole bitstreams of Gideon's/Commodore's .cfw); MultiCore's, told
  to Alex, not acted on.

## 5. Rules Alex holds you to (short; the full set is in `~/.claude/CLAUDE.md` and the memory)

- Chat in German; code, commits, specs, docs in English, quotes translated.
- Specs first; built means complete for the user (no slices); every commit keeps TODO, spec and history consistent in the same commit.
- Decisions: one question at a time; record each at once (ADR here). Not eager: no "shall I...?" endings; short answers.
- Gitea pushes allowed (no force-push); GitHub only after asking. Small commits; stage by name.
- No personal remarks about Gideon or Chris: names only as authorship facts.
- Mac: at most 2 heavy processes, `nice`; agents use sonnet; QC every agent result against code before passing it on.
- Browser: Safari only (safari-mcp), never Chrome.
- Read, compare, plan before a run; the machine is the evidence, comments are third choice.
