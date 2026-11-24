# U20 — GitHub becomes TRX64-Ultimate's development home; GitHub's runners build everything but Vivado, our builder builds the bitstream

**Status:** SPECIFIED (2026-11-24): Alex's decisions D1-D16, every §4 question answered; build plan §5. Nothing built.
**Repo:** TRX64-Ultimate (CI, tools, the firmware submodule's replacement); the builder (a private Gitea repo); GitHub settings (Alex).
**Number:** U20 (registry: the TODO table in `TODO.md`).
**Origin:** Alex, 2026-11-24, after reading MultiCore's pipeline: *"Stellen wir den Build um auf GitHub. Der builder baut nur noch den
bitstream."* The model is TRX64U-MultiCore's T239.1 (GitHub home, done and in use since release 0.2.1) and T239.5 (the matrix of lanes
and steps; its D2: "this pipeline setup can then basically also be 'copied' by the FW session").

---

## 1. Why

- Our CI runs on Gitea on the builder: lint, app, bitstream, package, sims, update, cor (`.gitea/workflows/pipeline.yml`). GitHub
  `Jondalar/TRX64-Ultimate` (public) only receives filtered exports (`tools/gh_export.py --replay`, one export commit per Gitea commit,
  no ROMs, the submodule flattened).
- MultiCore moved its home to GitHub (T239.1): pull requests and CI there, GitHub's hosted runners for every job but Vivado, the builder
  pulls the Vivado work from GitHub and publishes the bitstreams as releases. One pipeline shape for MultiCore and the core repositories.
- The firmware submodule `software/ultimate-app` (our fork of Gideon Zweijtzer's 1541ultimate, `TREX/1541ultimate` branch `trx64/main`,
  0103c585) lives on Gitea only, which GitHub's runners cannot reach. With MultiCore's SDK the application side no longer needs it.

## 2. Decisions (Alex, 2026-11-24, translated where German)

- **D1:** "Let us replicate MultiCore's pattern. GitHub does everything except Vivado; that we do locally here." Vivado stays on our
  builder; the builder carries no GitHub runner (MultiCore T239.1 D11). Ephemeral runners on rented servers were floated and not chosen.
- **D2:** "GitHub = home." Feature branches, pull requests and CI on GitHub; Gitea becomes the read-only archive (MultiCore T239.1 D8).
- **D3:** the submodule goes, now: "We copy and give him the credits. Decouple now, please." What the bitstream and `trx64u` still need is
  copied into the tree with provenance and file-header notes, as MultiCore's `vendor/gideon` (`VENDOR.md` with the source commit,
  `LICENSING.md` rows, `PROVENANCE.md` per copied file). Our own code that only lives in the fork (`apps/trxmon`, `software/io/c64/trx_lens.*`,
  `software/appsdk`, `software/app_loader`, all added by Alex 2026-04/10) moves into the tree as ours.
- **D4:** `trx64g` leaves this repository's pipeline: "Leave trx64g out." It ends once `trx64u` works well (Alex). Everything else:
  "We do it exactly like MultiCore."
- **D5:** Gideon's gateware (c64_basic's ~110 VHDL files: `rvlite`, `usb2` + `nano_cpu`, `1541` + `6502n`, `command_interface`, `itu`,
  `rmii`, `uart_lite`, `spi`, `sigma_delta_dac`, busses, FIFOs, video, memory; list in `tools/vendor_gideon.sh`) is copied "with
  provenance and header notes, as MultiCore does".
- **D6:** the SID and music player goes, all of it: "the whole music and SID player stuff. All." Gone: `sidcrt.bin`, `muscrt.bin`,
  `musplayer.bin` and 64tass (`pack_cor.sh:108-114`), the `.sid`/`.mus` playback of `base/trx64u_ops.cc` (from :1140, T228 R21 item 7),
  the `sidplayer/` files of the `.cor`, the SDK library `sid` in TOML B (only the player uses it: `sid_lib()`, `trx64u_ops.cc:1192`).
  The machine's SIDs stay (gateware, settings, sockets, the reSID8 filter in `u64cfg.cc`).
- **D7 (§4 question 1):** the pipeline shape is T239.5's matrix, copied once MultiCore's first matrix run is green: "Matrix, wait
  briefly, exactly."
- **D8 (§4 question 2):** the switch starts now, from `u18` (102 commits ahead of `main`, which has no `software/trx64u`); Timewarp and the
  board test run in the new pipeline. "B. We build for nobody but us. That is the test whether the SDK etc. carry."
- **D9 (§4 question 3):** one builder serves N repositories: "I would now think the builder serves N repositories." MultiCore's
  `trx64mc-builder` learns a table of sources (per repository: source repo, builds repo, workflow path, entry names, keycode list, trusted
  branches, build command) instead of a second Gitea repository with its own secrets; one poll, one lock, the App installed on each source
  repository, the source-API token scoped to every source repository.
- **D10 (§4 question 4):** the same builds repository, `trx64mc-builds`, with a tag prefix of our own for our bitstreams and pre-releases;
  the upload token stays the one it is. "The same, please. The goal is a registry with a public download repository." (MultiCore's T239.4.)
- **D11 (§4 question 5):** MultiCore's SDK lane publishes each SDK version (tarball and sub-apps `.u2a`) as a release of `trx64mc-builds`;
  our `sdk.pin` names that release and keeps its SHA-256 checks; `fetch_sdk.sh` downloads from there instead of Gitea's registry. "Let us
  start with a, yes." (The registry of T239.4 comes later.)
- **D12 (§4 question 6):** MultiCore's runners take `trx64u.cor` from our pre-release in `trx64mc-builds` (follows from D10); the Gitea
  package path and their builder's import of `trx64u` end, and the hand-over contract (`docs/contracts/trx64u-firmware-handover.md`)
  changes on both sides. `trx64g.cor` stays in MultiCore's bundle at its pin of our run 481 until `trx64u` works well, then it goes (Alex: "B").
- **D13 (§4 question 7):** the update files stay until further notice: "No. Until further notice we build `.ue2` and `.cfw`, or deliver
  them like trx64g. But we can build those here on Gitea, on demand." The Gideon-app line (`ultimate.app` from the submodule, the `.cfw`/`.ue2`
  update files, `trx64g.cor`) stays on Gitea, built from the Gitea tree with the submodule by a dispatch-only Gitea pipeline (MultiCore's
  D45 form); GitHub's tree has no submodule. `trx64u.cor` is the product of the GitHub pipeline.
- **D14 (§4 question 9):** `constraints/c64u.xdc` goes public (Gideon posted it in his GPL-3 repository's issue #615; its header names the
  source; T68's 2026-10-21 link replacement ends; only `tools/pincheck` reads it, the builds read `designs/*/pins.xdc`). The four ROMs stay
  out of the public tree: the benches fetch them from a pinned upstream with SHA-256 checks (MultiCore's D12, `ci/roms.txt`/`roms.py`), the
  private side repository on Gitea keeps local copies. Gideon's third-party test files leave with the submodule (D3).
- **D15 (§4 question 10):** GitHub's export history goes on: `u18`'s commits are replayed as export commits (`tools/gh_export.py --replay`)
  onto a work branch from GitHub `main`, rewritten once before its first push (internal hosts and paths out of files and messages, as
  MultiCore's D36); after that the work is native on GitHub. Gitea keeps the full history as the archive (Alex: "A").
- **D16 (§4 question 11):** the CI images are MultiCore's `trx64u-build`, `trx64u-sim` and `trx64u-emu` on ghcr, pinned by digest (the
  SDK's own compiler; nothing of ours to maintain) (Alex: "A").
- **D17 (Alex, 2026-11-24, translated):** "If we do that tomorrow, then I would actually also like to test ephemeral runners." An experiment
  beside D1, on Hetzner only ("no, then only Hetzner tomorrow"; AWS not now): **a** the mechanism first (a small job on a Hetzner Cloud server
  created through the hcloud API, registered with a GitHub just-in-time runner config for exactly one job, always deleted afterwards),
  **b** then Vivado (a private snapshot with Vivado installed builds `c64_basic`, compared with the builder in time and cost). Alex brings a
  Hetzner Cloud project with an API token and a GitHub token that may register runners on the repository. D1 stands until the experiment
  says otherwise.

What "exactly like MultiCore" brings with it (T239.1, read from its code; §3 lists the parts):
- Branches `feature → integration → main`; rulesets let only Alex's account update `integration` and `main`; sessions push feature
  branches with his account and never merge (T239.1 D15, D25). This replaces "Gitea main merge is mine" for this repository.
- The builder decides from who pushed whether it builds (T239.1 D14): Alex's pushes build at once; anyone else's wait for his approval at
  the environment `vivado`.
- CI never makes a release: a push to `main` or a `v*` tag makes a pre-release in a builds repository; Alex tags and publishes (T100 D8).

## 3. The parts (MultiCore's, with what changes here)

| Part | MultiCore | Here |
|---|---|---|
| Branch model, rulesets `protected`, `no-rewrite`, `tags`; immutable releases | T239.1 step 14 | copy (without `t239.1`) |
| Trust: environment `vivado` (reviewer Alex), `bitstreams` (the builder's GitHub App as protection rule), `builds` (main + `v*`) | `pipeline.yml:136-144`, App `trx64-builder` | copy; same App or a second one: §4 |
| Builder pull: Gitea workflow polls GitHub every 5 min, recomputes keys with code of a trusted ref, builds the missing keys, publishes `bit-<entry>` releases, statuses back, approves `bitstreams` | `trx64mc-builder` (`pull.yml`, `poll.py`, `trust.py`, `order.py`, `build.sh`, `publish.py`) | adapt: it assumes MultiCore's entry names and a `host-` entry (`order.py:18`, `poll.py:212`, `build.sh:17`); we have one target, `c64_basic`. Shared slot-0 `heavy.lock` stays |
| Work order (`work-order.json`) | `ci/order.py` | adapt: one entry, no DFX lanes |
| Content keys with `--data` split, `ci/vivado.env`, `check_vivado_env.py` | `ci/keys.sh`, `ci/lib.sh:98-299` | adapt: ours hash blob ids plus the generated `cores/gideon` (`ci/lib.sh:57-64`) |
| Bitstreams as release assets per key in a public builds repo; pre-releases `main-<sha8>`, `v<x.y.z>-<sha8>`; `fetch_bits` with digest + `SHA256SUMS` checks | `trx64mc-builds`, `ci/fetch_bits.py`, `ci/publish_release.py` | adapt: one entry, no `host_sha` |
| Private side repo on Gitea; `ci/private.txt` (sizes + hashes, no bytes); bench ROMs from a pinned upstream (`ci/roms.txt`, `roms.py`) | `TRX64U-MultiCore-private`, T239.1 D12 | adapt: our 4 ROMs are read by the benches only (`tools/rom2mem.py`) |
| Internal hosts cleaned once, then refused by lint; public-tree check (hook, workflow, lint) | `tools/check_public_tree.py`, `.githooks/pre-push`, `public-tree.yml` | adapt the path rules; our tree names Gitea/NAS hosts in `.gitmodules`, `fetch_sdk.sh`, every Gitea workflow |
| Workflow lint (pinned actions and images, no self-hosted runner, `permissions: contents: read`) | `tools/check_workflows.py` | copy |
| CI images on ghcr by digest (`trx64u-build`, `trx64u-sim`, `trx64u-emu`) | `ci-images.yml`, `ci/*/Dockerfile` | use MultiCore's by digest or build ours: §4 |
| Caches: `actions/cache` per job/leg, ccache saved on protected branches, restored bitstreams | T239.1 D40, D41 | copy the pattern |
| Pipeline shape: lanes × 13 steps, dispatcher, lint per lane, core lane from TOML A + TOML B, emulator with the core's own app | T239.5 `matrix.yml`, `ci/lane.sh`, `ci/dispatch.py` | lanes: `tree`, `bits:c64_basic`, `core:trx64u`; no SDK lane (we take MultiCore's SDK by pin), no host lane |
| Sims on demand (`[sims]`, nightly, before Vivado) | `ci/select.sh`, `nightly.yml` | copy (today ours run after the package on every push) |
| Gideon vendored in git with credits | `vendor/gideon/VENDOR.md`, `tools/gideon_sources.sh`, `tools/vendor_gideon.sh`, `LICENSING.md:18-19`, `host-app/PROVENANCE.md` | adapt (D3, D5): ours is generated and untracked today (`tools/gideon_sources.sh`) |
| Release tool: checks digests and the public tree, tag must exist, then draft and publish | `tools/release_mc.sh` | adapt: the bundle is `trx64u.cor`; replaces `ci/release.sh` and `.gitea/workflows/release.yml` |
| Switch: work branch from GitHub `main` with only the new CI files, one rewrite before the first push, builder on, full green runs, last export, `integration` cut, Alex merges the switch PR, Gitea workflows to manual, `CLAUDE.md` and skills rewritten | T239.1 D9, D36, D39, D45, D46 | copy as the checklist |
| Not taken: host lane, DFX `pack_one.sh` and `host_sha`, `aux`/reference/`mono` cores, contract ports, D24 ledger, `imports.py` pins, multi-core bundle, 100T `ue2` target | | MultiCore-only |

Outward setup that only Alex can do (MultiCore's list, T239.1): the Actions allow-list, the GitHub App (or its second installation) and
its key on the builder, fine-grained tokens (builds upload, source API, pre-release), the Gitea secrets and variables of the builder repo,
the runner config change, the public builds repo with immutable releases, the rulesets and environments (set through his account and read
back), the last export push, the switch PR merge, tags and releases.

## 4. Open questions (to Alex, one at a time)

1. ~~Pipeline shape~~: answered, D7 (the matrix, after MultiCore's first green run).
2. ~~Order with U18~~: answered, D8 (start now, from `u18`).
3. ~~Builder~~: answered, D9 (one builder serves N repositories).
4. ~~Builds repository~~: answered, D10 (MultiCore's, our own tag prefix).
5. ~~SDK on GitHub~~: answered, D11 (a release per SDK version in `trx64mc-builds`).
6. ~~`trx64u.cor` to MultiCore, `trx64g.cor` in their bundle~~: answered, D12.
7. ~~Update files and `ultimate.app`~~: answered, D13 (they stay, built on demand on Gitea).
8. ~~Provenance of the copy~~: settled by the code and D4. No commit of ours touches the fork's `fpga/` (all its authors are Gideon and
   upstream contributors; newest `fpga/` commit 22c3a167, 2026-09-15); our VHDL changes are the six patches in `tools/patches/gideon`.
   So `vendor/gideon/VENDOR.md` records Gideon's upstream commit (the fork's merge base with `GideonZ/1541ultimate`, its `fpga/` tree
   checked equal at the copy) plus the patch list, MultiCore's form. Our own code from the fork records 0103c585 as its origin.
9. ~~Private set~~: answered, D14 (`c64u.xdc` public; ROMs from a pinned upstream, local copies in the side repository).
10. ~~History~~: answered, D15 (replay `u18` onto GitHub's export history, one rewrite before the first push).
11. ~~CI images~~: answered, D16 (MultiCore's, by digest).

All answered on 2026-11-24.

## 5. Build plan

In this order; each step is a commit with its docs. A, B and E are ours; C is MultiCore's (their repositories, their rules); D is Alex's.

- **A. Our tree, on `u18` (Gitea), before the switch**
  1. `vendor/gideon/` in git (D3, D5): the VHDL list of `tools/vendor_gideon.sh` from Gideon's upstream commit, `fpga/` tree checked equal to
     the fork's, our six patches applied; `VENDOR.md` (commit, patches), `LICENSING.md` rows, file-header notes; `tools/gideon_sources.sh`
     only checks the copy (MultiCore's form). `cores/gideon` and its generation go.
  2. Our code out of the fork (D3): `apps/trxmon/src`, `trx_lens.*`, `software/appsdk/include`, `software/app_loader` into `software/trx64u/`
     with their origin 0103c585; `build.sh` and `pack_cor.sh` without `$APP`.
  3. The SID and music player goes (D6): `trx64u_ops.cc` from :1140, the file types and menu rows that start it, `sidplayer/` in
     `pack_cor.sh`, the library `sid` in `app/toml_b.toml`, the ops golden; check that nothing else calls `sid_lib()`.
  4. Bench inputs that read the submodule (`ci/sims.deps:28`, `designs/c64_basic/sim/eth/run.sh` variant `orig`, `sim/sid/run_m1.sh`'s
     `sid_coeff.c`, `tests/e2e/lib`): from `vendor/gideon` or copied with provenance.
  5. The Gideon-app line keeps the submodule on a Gitea branch of its own, cut before the submodule leaves `u18`; its Gitea pipeline becomes
     dispatch-only (D13). Then the submodule, `.gitmodules`, `ci/app.sh`, `ci/submodule.sh` and the update/cor stages for `trx64g` leave the
     GitHub-bound tree.
  6. ROMs from a pinned upstream for the benches (`ci/roms.txt`, `ci/roms.py`; D14); `c64u.xdc` public, `gh_export.py`'s link replacement goes.
  7. Keys: `ci/keys.sh` with the `--data` split and `ci/vivado.env` (MultiCore's form), the gateware key over `vendor/gideon`.
  8. Public-tree check (tool, pre-push hook, lint) with our paths; the one cleanup commit of internal hosts and paths; workflow lint.
- **B. GitHub workflows** (after MultiCore's first green matrix run, D7): `matrix.yml` with the lanes `tree`, `bits:c64_basic`,
  `core:trx64u`; the dispatcher; lint per lane; MultiCore's images by digest (D16); caches; sims on demand; the emulator step with the
  core's own app; the pre-release to `trx64mc-builds` under our prefix (D10); the release tool for `trx64u.cor`.
- **C. MultiCore's side** (asked through the MultiCore session, Alex's go there): the builder serves N repositories (D9); our tag prefix in
  `trx64mc-builds` (D10); the SDK lane publishes each SDK version as a release there (D11), and `sdk.pin`/`fetch_sdk.sh` follow; their
  import of `trx64u.cor` from our pre-release and the hand-over contract on both sides (D12).
- **D. Outward setup (Alex):** the builder's App installed on `Jondalar/TRX64-Ultimate`; the source-API token scoped to it; environments
  `vivado`, `bitstreams`, `builds`; rulesets `protected`, `no-rewrite`, `tags`; immutable releases; the Actions allow-list.
- **E. The switch** (D2, D8, D15): `u18` replayed as export commits onto a work branch from GitHub `main`, rewritten once, first push;
  green matrix runs; `integration` cut from `main`; Alex merges the switch PR; Gitea workflows dispatch-only; `CLAUDE.md`, skills and the
  sessions' rules follow (only Alex merges `integration` and `main`; sessions push feature branches).

## 6. Build notes

### A1, A4 (Gideon's gateware in `vendor/gideon`, bench inputs)

2026-11-24, built on `u18` d725a710: commits 6893e087 (copy, users, docs) and fbc2c60c (workflows).
- **Upstream commit** 1ba94abd314186559dfdeee9690e16d5d251f552 (GideonZ/1541ultimate `master`, 2026-09-20): the merge base of the
  fork 0103c585 with upstream `master` (40188c7d at fetch). Proof: the 128 files of `tools/vendor_gideon.sh` exist in both and
  `git diff --quiet 1ba94abd 0103c585 -- <list>` exits 0; the `fpga/` tree is the same object (75101867) in both; in
  1ba94abd..0103c585 no non-merge commit touches `fpga/` (7 upstream merges do, net diff empty).
- **The copy:** `tools/gideon_sources.sh <checkout at 1ba94abd>` made `vendor/gideon` (128 files, our six patches, `VENDOR.md`) plus
  `software/io/audio/sid_coeff.c` (A4, copied by path, not opened). `diff -r` against a freshly generated `cores/gideon` (old scripts,
  fork 0103c585): identical but `VENDOR.md`, `.stamp` and `sid_coeff.c`. No file-header notes: MultiCore's patched VHDL has none
  (only its software files changed by third-party ports do). `tools/gideon_sources.sh` alone only checks the copy.
- **Users switched:** `build.tcl`, 16 bench scripts, `font2mem.py` (its output equals the committed `.mem`), `vivado.sh`; `ci/lib.sh`
  (`gateware_key` over `vendor/gideon` as tracked files, without `VENDOR.md` and `software/io/audio`; `tree_key` no longer finds a
  generated tree); `ci/sims.deps` (`@gideon` = `vendor/gideon/fpga tools/gideon_sources.sh`; eth adds patch 0003); `ci/sims.sh`
  (whole-tree key names `vendor`, not the submodule; the generation step gone); `ci/submodule.sh` (no Gideon step); `bitstream.yml`
  and `sims.yml` no longer fetch the submodule. `cores/gideon`, its `.gitignore` line and `.stamp` are gone.
- **A4:** eth `orig` reverses patch 0003 on the copy at bench time (result `cmp`-equal to upstream's `eth_transmit.vhd`);
  `sid/run_m1.sh` reads the copied `sid_coeff.c`.
- **Checks:** `ci/lint.sh` locally, all ok; SID M1 bench ran (tables equal, the 3 known 8580 OSC3 mismatches); every Gideon path of
  `build.tcl` (116) and of the benches (76) exists in the copy. Not run here (no ghdl): eth and the other GHDL benches; the next CI run
  rebuilds the bitstream (new key) and runs them.
- **Left for A2/A5:** the other submodule users: `ci/app.sh`, `ci/lib.sh` `build_update`, `ci/release.sh`, `ci/submodule.sh` with
  `app.yml`/`cor.yml`, `tools/make_update.sh`, `gh_export.py`, `release_notes.py`, `trx_pkg.py`, `hdmi_modes.py`
  (`trx_hdmi_modes.h`), `ef3_image.py`, `tests/bdd` (`tests/e2e/lib`), `tests/t55_board.py`, `tests/cart/*.py`, the app rows of
  `LICENSING.md`, `BUILD.md`, `CONTRIBUTING.md`.

### A2, A3 (our fork code in the tree, the SID and music player out)

- 2026-11-24, A2: what `build.sh` read from the fork (0103c585), measured from the build's `.d` files and its copy loop, is in the tree:
  `software/trx64u/trxmon/` (29 files of `apps/trxmon/src` without `main.c`, `libc_min.c` and `trxsvc.h`, which the library build never
  read, plus `software/io/c64/trx_lens.c`/`.h`) and `software/trx64u/fwabi/` (`app_api.h`, `u2p_app.h`, `u2p_app_api.h`; the last
  now includes `app_api.h` beside it). Every commit of each file in the fork, renames followed, is Alex Damhuis's: no `PROVENANCE.md`
  entry; none had an SPDX header. Each file's first line names its origin; the comments that named `main.c`, the host tests or "the
  only copy" say what holds in this tree.
- `build.sh` passes `FWABI` instead of `APPSDK`/`APPLOADER` and copies `trxmon/`; `$APP` is gone from it. Rebuilt from clean,
  `monitor.u2a` (211,256 bytes) and `ring.u2a` (137,088) are byte for byte the ones built from the submodule before.
- `trxmon/` is not in the reverse lint (`ci/lint.sh`: `base ring monitor uci`): `hw_trx.c` names four firmware windows by address
  (0x10050000, 0x10180003, 0x10188000, 0x1018C000), `ws.c`'s SHA-1 constant 0x10325476 lies in the I/O space. Open, as before the move.
- 2026-11-24, A3 (D6): gone are `trx64u_ops.cc`'s player (482 lines: `c64.sid_play`, `sid_songs`, `sid_valid`, `sid_info`, `dma.sidcrt`,
  the handshake in `c64.service`, the BASIC copy of `c64.put_basic`, the init hook's `sid_cancel` and `player_end`), `u64cfg.cc`'s
  autoconfig (its only caller was the player) with its two settings (group `SID_player_behavior`, read by nothing else), the file
  types `.sid`, `.mus`, `.str`, `sidplayer/` with 64tass in `pack_cor.sh` (`$APP` is gone from it), and the library `sid` in TOML B.
  Kept: sockets, reDIP:SIDs, reSID8, their settings, the socket detection, the UCI's SID map.
- `sid` served only the player: its export holds only `parse`, `info_text`, `plan`, `requests`, `mus_stereo`, `mus_header`; in our tree
  only `sid_lib()` asked for it (`ring/ringsys.cc` asks for c64cart, cbm1541, uci; monitor, uci, trex_crt nothing); `online.u2a` has no
  string "sid"; the SDK's base finds no export "sid" and `libtrxbase.a` has no sid or resid8 symbol; the library's `resid8.cc` has no
  caller (`u64cfg.cc` writes reSID8 through the driver table). The link without it closes and the symbol check passes.
- Runs, the submodule emptied (`git submodule deinit`), SDK 0.4.6, bitstream 975aa59b: `build.sh` rc 0 (trx64u.u2a image 77,096, budget
  now 77,096 + 86,664 bss = 163,760, ops 62); `pack_cor.sh --emu` rc 0: app 1,458,896 bytes (2,640 less), heap 10,972,496, record 8 one
  file (`apps/trex_crt.u2a`), `.cor` 3,948,516 bytes; READY., ping answered, the ops list 388 lines = `base/ops_trx64u.txt` both ways
  (the five player operations out of the golden). `ci/lint.sh` rc 0.
- Open for A5: `.gitea/workflows/cor.yml` still fetches the submodule; `ci/cor.sh` and the scripts it runs read nothing of it now
  (`trx64g.cor` takes `ultimate.app` from the package).

### A5, A6, A7 (submodule out, the Gideon-app line on Gitea, ROMs, keys)

Not started.

### A8 (public-tree check)

Not started.

### F (D17: ephemeral runners on Hetzner, experiment)

Not started.
