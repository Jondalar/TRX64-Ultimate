# U20 — GitHub becomes TRX64-Ultimate's development home; GitHub's runners build everything but Vivado, our builder builds the bitstream

**Status:** IN PROGRESS (2026-11-24): Alex's decisions D1-D18; build plan §5. A1-A4 built (merged into `u18` 81cbb483, CI run 482
green: lint, app, bitstream, package, sims, update, cor). A5-A6 built (u18 bb0206e3, CI run 483 green: lint, bitstream, package, sims, cor; the ROM store on the builder filled from VICE's
svn-mirror by the `dbgsess` bench). A7 and B built (2026-11-25, merged into `u18` 5af48b49), checked on the Mac; CI run 484 green (lint, bitstream, package, sims, cor),
the Gitea key unchanged and no job from `.github/workflows` started on Gitea; they run on
GitHub after C (MultiCore's builder table, the SDK release) and D (Alex's setup); the names they use are proposals to MultiCore (§6 "B, A7").
F-a (D17's experiment) built and dry-run tested, not run: Alex defers F until MultiCore's T239.3 is through (D19). A8 built (2026-11-25, merged
into `u18` ad96228c, CI run 485 green): the public-tree check (tool, pre-push hook, workflow, lint), the one cleanup of the tree, the replay's scrub
and `--rewrite` (§6 "A8"); the check stays after the switch (D20). C: MultiCore's T261 published `sdk-0.4.6-6e280710` (our `fetch_sdk.sh --release`
fetched and checked it), the contract (copied 50e49dec), the builder's `fw` row deployed (keycode ref `u20` until the switch PR is merged; its
`SOURCES` switch waits for `u20` on GitHub). D done 2026-11-25 (§6 "D"). Next: E.
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
- **D18 (Alex, 2026-11-24, on A3):** "Gone: SID Player Autoconfig. Stays: UltiSID by .cfg file." The settings group "SID player behavior"
  (SID Player Autoconfig, Allow Autoconfig uses UltiSid; read only by the player's autoconfig) stays out with the player. The UltiSID
  settings (`u64cfg.cc` UltiSID 1/2 address, filter, waveforms, digis, ...) stay, and with them a `.cfg` that sets them: `settings.load_file`
  (menu `.cfg` "Load settings") and the `.cfg`/`.usr` beside a PRG, D64, G64 or CRT (`trx64u_ops.cc:265-269`).
- **D19 (Alex, 2026-11-25):** F (the Hetzner experiment) waits: "Hetzner machen wir später bitte .. ich will erst 239.3 durch haben" (until
  MultiCore's T239.3 is through). F-a stays built, not run; no Hetzner or runner token exists yet.
- **D20 (Alex, 2026-11-25, on A8):** the public-tree check stays after the switch, as at MultiCore, "minimal" in effort: the script
  `tools/check_public_tree.py` (regex and hashes, no agent; the whole tree in 1.7 s on the Mac) in the pre-push hook, `public-tree.yml` and lint.
  The export tooling (`tools/gh_export.py`, `--replay`, `--rewrite`) goes after the switch, as MultiCore's `github_snapshot.py` did.
- **D21 (Alex, 2026-11-25, on A8 open point 4):** GitHub `main`'s history is not rewritten: "Wir machen das so wie empfohlen." Its two ROM
  hits (`software/ultimate-app/recovery/u64ii/ultimate.bin`, `software/6502/unsorted/tcpimage.crt`) are the same blobs as in Gideon's public
  `GideonZ/1541ultimate` (8b78db63, bf857f6e, read 2026-11-25); the other hits are private-range addresses, hosts and paths of old export
  commits. A rewrite would need `no-rewrite` off and a force push that breaks every clone and fork; the check keeps new hits out from E on.

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

- 2026-11-24, A5 on the Gitea branch `gideon-app` (from origin 083a631f): 0aaf70d7, `pipeline.yml` and `release.yml` keep only
  `workflow_dispatch` (MultiCore T239.1 D45's form), the README says what the branch is; nothing else changed.
- A5 on `u18` (from 92645aeb): 3e8e5b09 (CI), 7e3d4efe (readers, docs), 184c26d7 (fonts). Gone: `.gitmodules`, the gitlink,
  `ci/submodule.sh`; the stages app, update (`build_update`, `select.sh update`) and the Gitea release (`ci/release.sh`,
  `release.yml`, `release_reuse_skip`); `software/trx64g` and cor's trx64g half; `tools/make_update.sh`, `trx_pkg.py`,
  `trx_pkg_check.py`, `release_notes.py` and their tests; `tests/bdd` (both features run Gideon's `ultimate.app` in ue2emu through the
  fork's `tests/e2e/lib`: its Cartridge Preference and its monitor service, no gateware of ours; nothing of `tests/e2e/lib` is copied,
  nothing here uses it); the two `LiberationSans-Regular.ttf` symlinks into the submodule under `docs/lvgl_ui_*/fonts` (they dangled
  and stopped the export's digest).
- Pipeline: lint -> bit-c64u -> package -> sims, cor (`trx64u.cor` only). The package holds the bitstream, its reports,
  `manifest.json` (without `"version"` and `"app_commit"`, which came from the app) and `SHA256SUMS`. `files_key` lost the
  `<submodule>//` form; the keys of other spec lists are unchanged (checked before and after: turbo's f4649a8d, `gateware_key 50t`
  a4de1cda).
- Rewired: `hdmi_modes.py` checks and writes `hdmi_modes.vh` only (the app header `trx_hdmi_modes.h` went; check PASS, selftest 17/17,
  `gen` writes the same file); `ef3_image.py` reads the EAPI driver from `software/trx64u/apps/trex_crt` (its insert test runs now);
  `gh_export.py` exports the tree alone and refuses a ref with a gitlink (step E: replay the commits before A5 with the tool of
  92645aeb, the rest with this one); `t55_board.py --app` has no default, `t16_session.py` takes `trex_crt.u2a` from `--app-u2a` or
  `TREX_CRT_U2A`; origin notes in `software/trx64u` and `tests/cart` name `TREX/1541ultimate:<path>`.
- A6, 05e327b7: `ci/roms.txt` pins the four ROMs at VICE's svn-mirror 4d283a2e (`vice/data/C64`, `vice/data/DRIVES`); downloaded,
  each equals the removed file (89878cea basic, 83c60d47 kernal, fd0d53b8 characters, 326c289c 1541-II) and Gideon's
  `roms/` at 1ba94abd (the same blob ids). `ci/roms.py`, `ci/test_roms.py`: MultiCore's (42e38499b, d17f3eb70), store
  `ROMS_STORE` (empty: none) or `/cache/roms`; lint runs the test. `ci/sims.sh` fetches what a bench names before it runs
  (unreachable: SKIPPED, not red; wrong hash: FAIL); `rom2mem.py` fetches a missing C64 ROM itself; `@vcore` and ultsys's
  boot-state key name `ci/roms.txt`. `roms/*.bin` left the tree (`.gitignore`), `roms/README.md` says from where.
- A6, e2803130: `constraints/c64u.xdc` goes out as it is; `gh_export.py`'s `EXCLUDE` and `PLACEHOLDERS` are empty.
- Checks: `ci/lint.sh` rc 0; `sims.sh -f keyboard` PASS; the ROM gate alone (a ROM bench fetched, a bench without ROMs, an
  unreachable pin SKIPPED, a wrong hash FAIL); `c64_rom.mem` from the fetched ROMs = from the old files (81b834ce);
  `test_gh_export.py` 27 OK and `gh_export.py --check` passes (1486 files); `test_ef3_image.py` OK; `build.sh` rc 0 (`trx64u.u2a`
  image 77,096, `ring.u2a` 137,088 and `monitor.u2a` 211,256 bytes, as in A2/A3); `pack_cor.sh --no-build --emu` with run 480's
  bitstream (975aa59b, sha256 4d2a74f6) rc 0: heap 10,972,496, `.cor` 3,948,515 bytes, READY., ping answered, the ops list 388
  lines = `base/ops_trx64u.txt` both ways. Not run here (no ghdl): every ROM bench.
- Open: the manifest's `"version"` is gone, so a MultiCore pin of a new `u18` package takes its version by hand (contract §2
  allows it; D12 changes the contract anyway). Board tests that boot `ultimate.app` from a package (`t55`/`t62`/`t67`
  `--package`, `t16_session` boot, `ue2_session`) need a package of the `gideon-app` line. `docs/` still names the submodule in
  44 files: specs, ADRs and dated notes (history, left), `gideon-menu-tree.md` (fork paths as text now).
- A7 (keys): built with B, see "B, A7" below.

### B, A7 (the GitHub workflows, the keys)

2026-11-25, branch `u20-b` from `u18` c63f7854, MultiCore's form from their `t239.3` d9ddadeb1 (read only; their matrix went green in run 45481704917 on
da3f0ee4f) and their builder's `pull/` (read only). Every copied file names its source in a PROVENANCE line and what changed.

Built:
- **A7, keys.** `ci/keys.sh [--env FILE] [--data DIR]` prints the one entry `c64u-<gateware_key>` (MultiCore's `vivado_env_load`: only `TRX_*`,
  `T79_*`, `T16_*` names with plain values, the caller's own option variables dropped first). `ci/lib.sh` splits `CI_CODE` (the scripts) from `ROOT`
  (the tree read; `CI_DATA_ROOT`), `GATEWARE_SPECS` holds the key's pathspecs (also read by the lane's step 3), `branch_slug` takes `CI_BRANCH` first.
  `ci/vivado.env` names `TRX_SID`, `T16_SLOT`, `TRX_FEATURES`, all empty: the Gitea bitstream stage sets none, and an empty value builds
  `build.tcl`'s default as an unset one does. `tools/check_vivado_env.py` (MultiCore's, with our rule: an empty option must be absent or empty in
  `.gitea/workflows/bitstream.yml`, a set one equal, an option the workflow sets that the file does not name is red). The builder's key code is
  `ci/keys.sh` and `ci/lib.sh` (lib.sh sources nothing else).
- **Gitea keys: unchanged.** `gateware_key xc7a50tfgg484-1` before and after: `0dce5fff4ef99009`; `ci/keys.sh` with `TRX_FEATURES=jtag T16_SLOT=0` in
  the caller's environment: `c64u-0dce5fff4ef99009`; the builder's way (a folder holding only `ci/keys.sh` and `ci/lib.sh`, `--data` a clone of
  c63f7854, `env -i`): the same name, nothing written into the data tree; `ci/vivado.env` with `PATH=/x` or `TRX_SID=a;b`: exit 1; with
  `TRX_FEATURES=jtag`: another key (`fa780545c06becf1`). No other key of `ci/lib.sh` changed (`files_key`, `tree_key` untouched). Lint checks it on
  every run ("keys").
- **1, the matrix.** `.github/workflows/pipeline.yml` (push of every branch but `scratch/**`, `v*` tags, a fork's pull request, dispatch with
  `extra: sims`; jobs `matrix`, `prerelease`, `summary`) calls `.github/workflows/matrix.yml`: `dispatch` (plan, lanes, work order), `approve`
  (environment `vivado`, MultiCore's rule: Alex's own push builds without it), `lint` (one leg per lane), `sims`, `bits` (environment `bitstreams`),
  `core`, `emu`, `summary`. `ci/dispatch.py` writes `plan.json` itself (base = the push's `before` or the pull request's base; MultiCore's
  `ci/plan.sh` decides per core, we have one of each) and prints `lint_groups`, `bits`, `cores`, `sims`, `sims_tier`, plus the matrix table in the job
  summary. `ci/lane.sh`: `bits:c64_basic key|fetch|report`, `core:trx64u changed|build|package|deliver|emu`. `ci/lint.sh [GROUP]`: no group = every
  check (the Gitea stage, unchanged), `tree` (shell, Python, workflows, `select.sh`, keys, vivado env, the builds scripts' test, ROMs test),
  `bits:c64_basic` (Verilog syntax, `sims.txt`), `core:trx64u` (the reverse lint, SDK by `SDK_FETCH`).
- **The steps the lanes leave empty, and why** (a lane leaves a step empty when it has none, T239.5): the core has no unit tests of its own (2) and the
  SDK's coreapp test units name none of it (8-10); 4 is the reverse lint of step 1, `rtl_check.py check` waits for the SDK fix (U18 §5); 5 and 7 run
  inside `pack_cor.sh` in step 11 (`app_build.py` against TOML A and B, `libtab.py symcheck`, `cor_pack.py --ops`, the heap check), not split out.
- **2, release side.** `ci/order.py` (one entry; work order `{"version": 1, "sha", "lanes": ["bits:c64_basic"], "entries"}`), `ci/fetch_bits.py`
  (digest and `SHA256SUMS` checks, `reports.tar.gz` members, manifest optional, MultiCore's D40 cache record), `ci/publish_release.py` (draft,
  digests, publish; only `main` and `v*` tags, only our repository's push, and a tag must start with `fw-`), `ci/prerelease.sh` (stage
  `trx64u.cor`, `manifest.json`, `SHA256SUMS`), `tools/builds.py`/`builds.sh` (MultiCore's reader; `latest` with our prefix), `ci/pkg.env`.
  `software/trx64u/fetch_sdk.sh --release` fetches `sdk_tarball` and `online_u2a` of `sdk.pin`'s new `sdk_release` through `tools/builds.sh`
  (a cached file with the pinned hash is kept); `--download` (Gitea) stays.
- **3, images and caches.** MultiCore's digests of `t239.3`: `trx64u-build@sha256:706a976b…0c08` (dispatch, bits, core, prerelease),
  `trx64u-sim@sha256:3e366a08…25e0` (lint, sims; the same OSS CAD Suite 2026-09-27 as our `trx64-sim-env:4`), `trx64u-emu@sha256:250f08bb…2281`
  (emu: build image plus ue2emu v0.7.0); actions pinned at their SHAs (checkout v4.2.2, cache v4.2.3, upload v4.6.2, download v4.3.0). Caches:
  the SDK release files (key: hash of `sdk.pin`), the restored bitstream entry (key: the entry name, D40), the bench markers and ROM store
  (`ci-cache-sims-c64_basic-<sha>`, shared with the nightly), Gideon's ROM folder (key: its commit). Not taken: ccache (D41): the SDK's makefiles
  call `$(RV_PREFIX)gcc`/`ld` with no launcher, and a launcher in `RV_PREFIX` would also front `ld`.
- **4, sims, emulator, workflow lint.** `ci/select.sh sims-demand` (MultiCore's rule: 2 on `integration`, `integ-*`, `release/*`, tags,
  `SIMS_INPUT=true` or `[sims]`, else 0; `sims-tier` stays for Gitea); the matrix's `sims` job runs `ci/sims.sh -t <tier>` before Vivado;
  `.github/workflows/nightly.yml` runs tier 2 (schedule, dispatch, or a push to `scratch/nightly/**` with `[nightly]` until the switch). The emulator
  step: `ci/emu_roms.sh` fetches `roms/` of GideonZ/1541ultimate 700c7f2f (MultiCore's pin; tree id e3085066 read from GitHub 2026-11-25) and checks
  the tree id, then `emu_ops.py --check base/ops_trx64u.txt` runs the `.cor` with its own app; no work directory is kept (it holds the ROMs).
  `tools/check_workflows.py`: MultiCore's rules 1-5, rule 2 with `main` and `v*`, rule 6 now "every `runs-on` is a GitHub-hosted image" with one listed
  exception (`ephemeral-a.yml` job `work`: closes F open point 4), rule 7 adds internal hosts and paths, a new rule 8 checks `needs.*.outputs.*` and
  `steps.*` references (actionlint could not be run here).
- **5, release tool.** `tools/release_trx64u.sh <fw-…tag> [--publish --notes FILE]` (MultiCore's `release_mc.sh` form): every asset against
  GitHub's digest and `SHA256SUMS`, `manifest.json` against the tag and the `.cor`, the `.cor`'s record 1 (id `trx64u`, version) against the
  manifest; `--publish` also wants a plain x.y.z, `tools/check_public_tree.py --files` clean on every asset (A8) and the tag `v<x.y.z>` on
  the manifest's commit, then `gh release create --verify-tag` as a draft, asset count, publish.

**Proposals to MultiCore** (part C decides; until then nothing of B can run on GitHub):
- D9, a row of the builder's table: source `Jondalar/TRX64-Ultimate`, builds `Jondalar/trx64mc-builds`, tag prefix `fw-`, workflow
  `.github/workflows/pipeline.yml` (the matrix is called from it, so its artifacts `work-order` and `plan` are that run's), entries
  `^c64u-[0-9a-f]{16}$` (one; lane `bits:c64_basic`), release tag `fw-bit-<entry>`, key code `ci/keys.sh`, `ci/lib.sh` from the trusted ref `main`,
  `ci/vivado.env` read from the commit (`TRX_*`, `T79_*`, `T16_*`), trusted branches `main` (`integration` once it exists), environments `vivado` and
  `bitstreams`, build command `ci/bitstream.sh c64u` with `CI_BIT_CACHE` = the source's own bit cache and `CI_HANDOFF` set; the entry it leaves
  is `$CI_BIT_CACHE/c64u-<key>/` with `DONE`, `c64_basic.bit`, `ddr2_test.bit`, `hdmi_test.bit`, `key`, `from`, `wns.txt`, reports (`*.rpt`,
  `*.vivado.log.gz`, `*.steps.txt`) and `SHA256SUMS` (the publisher drops it and writes its own). The shared slot-0 `/cache/heavy.lock` is taken by
  `ci/bitstream.sh` (`heavy_lock`).
- D11, the SDK release we expect: tag `sdk-<sdk_version>-<source_commit>` (now `sdk-0.4.6-6e280710`, `sdk.pin` `sdk_release`), assets
  `trx64-sdk-0.4.6.tar.gz`, `online.u2a`, `SHA256SUMS` (their lane's `trx64-sdk-*.tar.gz` and `*.u2a`).
- D12, our pre-release: tag `fw-<branch>-<sha8>` (`fw-main-<sha8>`, `fw-v<x.y.z>-<sha8>`), assets `trx64u.cor`, `manifest.json`
  (`source_repo`, `commit`, `ref`, `tag`, `version`, `bitstream` {`entry`, `release`, `c64_basic.bit`}, `sdk` {`version`, `commit`, `release`,
  `tarball_sha256`}, `files` {`trx64u.cor`: `sha256`, `size`}), `SHA256SUMS`. The contract's file name `trx64u_<sha8>.cor` becomes `trx64u.cor` (the
  tag names the commit); the contract changes with D12 on both sides.

**Gitea ignores `.github/workflows`:** Gitea's `modules/actions/workflows.go` (`ListWorkflows` → `listWorkflowsInDirs`) takes the first directory of
`setting.Actions.WorkflowDirs` that the commit has and breaks there; the default is `[".gitea/workflows", ".github/workflows"]`
(`modules/setting/actions.go`, read 2026-11-25 on Gitea's main branch). `u18` has `.gitea/workflows`, so the new files run on GitHub only. Not read:
our Gitea's version and its `app.ini` (a changed `WORKFLOW_DIRS` would change this); the next Gitea run of `u18` after the merge shows it (no run of
`Pipeline`/`Matrix` from `.github`).

**Checked on the Mac:** `ci/lint.sh` rc 0, and each group alone rc 0; `tools/check_workflows.py --selftest` (23 planted faults red, the clean file and
the exception green) and over `.github` ok; `tools/check_vivado_env.py --selftest` and the check ok; `ci/test_builds.py` 24 cases against a fake
GitHub API on 127.0.0.1 (order: missing, draft, published, another prefix, a foreign name; fetch: good entry, cache record reused, a changed cached
file refetched, wrong digest, `SHA256SUMS` listing a missing asset, a draft, nothing left behind; `tools/builds.py`: good asset, wrong digest; publish:
feature branch, a tag without `fw-`, a pull request event, a fork's run, no token, main draft→published, the same files again, a changed
`SHA256SUMS`, other files under a published tag, a `v*` tag); `ci/dispatch.py` and `ci/lane.sh` dry runs (`key`, `changed` against a real base,
the refusals: unknown lane and step, no restored entry); `ci/prerelease.sh` with `PRERELEASE_NO_PUBLISH` (manifest written), without a token on
`main` (red), on a feature branch (nothing published); `tools/release_trx64u.sh` dry runs against the fake API with a real `trx64u.cor` (good; the
version, the commit, the repository or the prefix wrong: stopped; `--publish` without notes: stopped); `ci/emu_roms.sh` fetch (176 files, tree id
right), from a copy, a planted extra file refused; the emulator step's own call (`emu_ops.py` with that ROM folder and ue2emu 0.7.4 on the Mac):
READY., the monitor's ping, 205 ops and 188 states read (the `.cor` of the main checkout predates A3, so its five SID player operations are the
whole difference to the golden); `bash -n` and `shellcheck` (infos only); `ruff`.

**Only GitHub can show** (after C and D): the jobs in MultiCore's images (the build image's xpack GCC 11.3.0 against Gitea's `u64-build-env`: the
`.cor` bytes may differ, its checks must pass); the emulator step with ue2emu v0.7.0 of the emu image (the flags `emu_ops.py` uses exist at v0.7.0;
0.7.1-0.7.4 changed turbo timing, `.c64re`, video modes, a `--usb-dir` path fallback, the W25Q flash); hashFiles and actions/cache in container jobs;
`concurrency.queue: max`; the environments' waits; the builder's work order and `fw-bit-` releases; `fetch_sdk.sh --release` against the real SDK
release; the pre-release upload.

Open points:
1. Part C: the builder learns the row above (its entry pattern, keys code list, tag prefix, build command per source); MultiCore's SDK lane publishes
   `sdk-0.4.6-6e280710` or our pin moves to a release it does publish; their import of `trx64u.cor` from `fw-…` pre-releases; the contract (D12).
2. ~~A8: `tools/check_public_tree.py`; until then `tools/release_trx64u.sh --publish` stops before publishing.~~ Closed by A8 (2026-11-25).
3. MultiCore's sims run one leg per bench owner; ours is one leg (`bits:c64_basic`) capped at 350 minutes. A full tier-2 round (slot 2942 s, turbo
   2043 s, ddr2_test 1671 s alone, 2026-11-01) fits only with the markers of earlier runs; a split by bench groups is open.
4. The `.github` workflows run nothing on GitHub until E: a push of `u20-b` to GitHub is part E's replay, not this step.

### D (Alex's setup on GitHub, 2026-11-25)

Done with Alex: settings by `gh api` with his account (his choice), the rest by him in Safari; read back by API.
- Environments: `vivado` (required reviewer Jondalar, admins cannot bypass), `bitstreams` (admins cannot bypass; custom protection rule:
  the App `trx64-builder`, enabled), `builds` (branch `main`, tag `v*`; secret `BUILDS_TOKEN`: a new fine-grained token T4 for
  `trx64mc-builds`, Contents read/write).
- Rulesets as MultiCore's: `protected` (`main`, `integration`, the switch branch `u20`: create/update/delete by the admin role only),
  `no-rewrite` (`main`, `integration`: no force push, no deletion, no bypass), `tags` (all tags, admin only). Immutable releases on.
- Actions: selected actions only, SHA pinning required; allow-list checkout v4.2.2 and v6.1.0 (`ephemeral-a.yml`), cache (+ save, restore),
  upload-artifact, download-artifact at the pinned SHAs; workflow token read-only, no pull-request approvals; fork pull requests of all
  external contributors need approval (stricter than MultiCore's first-time rule: the ephemeral runner).
- App `trx64-builder`: its installation now covers `Jondalar/TRX64-Ultimate` too (one installation, two repositories, MultiCore's choice);
  the builder's `SRC_API_TOKEN` (T2) extended to our repository (edited, not regenerated).

### A8 (public-tree check)

2026-11-25, branch `u20-a8` from `u18` 23eefe24 (Alex: "lass uns bis F alles machen jetzt"). MultiCore's form from `t239.3` 72e770261 (read only):
`tools/check_public_tree.py` (6e2807103), `tools/test_check_public_tree.py` (02ddd12b7), `.githooks/pre-push` (d20da02a3),
`.github/workflows/public-tree.yml` (537a10acd), T239.1 D6, D24, D36 and its step-4/13/15 notes. Each copied file names its source and what changed.

Built:
- **The check.** Modes as MultiCore's: tree (working tree or `--rev`), `--commits RANGE` (every path and blob a commit adds or changes), `--messages
  RANGE`, `--files` (folders, `.cor`, `.zip`, `.tar.gz`; `tools/release_trx64u.sh --publish` runs it on every asset), `--selftest`, `--gen-private DIR`.
  Text rules, generic forms with no value of ours, not even hashed (MultiCore removed its hashed list after a reviewer guessed it): private addresses,
  home paths, internal host names (`.box`, `.lan`, `.local`, `.home.arpa`), a login on a host without a dot, a NAS share path (`/share/<name>` at the
  root), a personal mail address (an address at a consumer mail provider: so the maintainer's own address is found without being named), claude.ai
  links. Path rules: `roms/*.bin` (D14), ROM and disk image types, `.claude/` outside `agents/` and `skills/`, `CLAUDE.local.md`. `ci/private.txt`: size,
  SHA-256 and four 64-byte windows (hashes only) of each ROM of `ci/roms.txt`, made by `--gen-private` from `ci/roms.py fetch`; the unit test keeps it
  equal to the pins. No side repository here, so no side lines and no `trx.side` rule in the hook. The builder repository's name is refused under
  `.github/` only (`tools/check_workflows.py` rule 7), as MultiCore does: MultiCore's public tree names it, so it is no secret.
- **Exceptions** (exact paths, a reason each): the ESP32 vendor default 192.168.2.x (`menu.toml`, `ultimate-app-hw-spec.md`, the two settings mock-ups);
  the emulator's SLIRP gateway in a bench comment; the Ultimate's cartridge-at-computer path form in T71; the authors' addresses in the headers of 31
  third-party files (`vendor/gideon`, `cores/icepi`); the planted examples of the selftests (`check_public_tree.py`, its test, `test_gh_export.py`,
  `check_workflows.py`). `GITEA_KEPT`: the cache volume `/share/.../trx64-build/cache` of the five Gitea stages, see "Gitea" below.
- **Deviations from MultiCore:** a message passes this repository's attribution line `Claude-Session:` with the session's address (exact form,
  `SESSION_TRAILER`; GitHub main has carried 81 of them since 0.2.0), any other claude.ai link is a hit; `--trailers` is gone; `--authors` (the hook only) also
  checks each commit's author and committer address, CI does not (a contributor's address is theirs); the hook checks only a push to a GitHub URL
  (Gitea stays the archive). `scrub()` turns every hit into a placeholder (192.0.2.<last octet>, `~/`, `<name>.example`, `<user>@<host>`, `/srv`,
  `<mail>`, `(a private page)`) for the export; it leaves the third-party trees (`vendor/`, `cores/icepi/`, the flattened `software/ultimate-app/` of the
  history) alone and the check reports what it finds there.
- **Hook, workflow, lint.** `.githooks/pre-push` (tree of the pushed commit, `--commits`, `--messages --authors` over what the remote lacks; enable with
  `git config core.hooksPath .githooks`); `.github/workflows/public-tree.yml` (every push and pull request, pinned checkout, `contents: read`; Gitea
  ignores `.github/workflows`); `ci/lint.sh` group `tree`: the check with its selftest and its unit tests. `tools/check_workflows.py` rule 7 now runs the
  check's text rules over `.github/` (one source) beside the builder's name.

The cleanup (rule hits of the `u18` tree, 1506 files, per file and rule): 56 fixed, 38 exceptions above, 5 kept for Gitea.
- 28 private addresses: the board's address in 19 board tests and 4 docs became the documentation address 192.0.2.189 (MultiCore's form), the override
  `TRX64_HOST` (or the script's own `TM_IP`, `TRX_HOST`, `--host`) stays and is added where a script had none; the NAS login in four NAS tools
  (`TRX_NAS`, default `admin@192.0.2.2`); the Mac's address as multicast interface in `lr_mc.py` (`TRX_MCAST_IF`, default any); the Gitea address
  fallback of `ue2_session.py` (gone: `GITEA_URL` takes several servers).
- 12 home paths (10 specs and notes, 2 bench scripts: `~/`, `os.path.expanduser`), 6 NAS share paths (four NAS tools: `TRX_NAS_DATA`, default `/srv`, and
  `TRX_NAS_DOCKER`, default `docker`; `vivado/README.md`; T31), 8 internal hosts (the Gitea server: `GITEA_URL`, else the `url:` of the tea login,
  in `fetch_sdk.sh`, `load.sh`, `ue2_session.py`, `t16_session.py`, `t55_board.py`; `ci/lint.sh` passes `GITHUB_SERVER_URL`; T95's link points to
  MultiCore on GitHub; the README says `$GITEA_URL`), 2 claude.ai artifact links (the text says a private page).
- `docs/` history files (dated notes, old specs): rewritten with the placeholders, MultiCore's D6 rule (one commit, placeholders; moving them away was
  asked and not chosen there). They are the same text the export's scrub writes.

**Gitea keeps working** (D13; nothing under `.gitea/` changed):
- The five stages mount `/share/.../trx64-build/cache` by host path and the runners mount only the volumes their configuration lists, so a variable
  there would need a runner change: the lines stay, `GITEA_KEPT` lets them pass on `u18`, and the export writes `/srv` into the public copy
  (MultiCore kept `.gitea/` public the same way: its snapshot export wrote `/srv`, its Gitea tree kept the host path). Part E deletes `GITEA_KEPT`.
- `ci/lint.sh` (stage lint): the SDK download takes `GITEA_URL`, else `GITHUB_SERVER_URL`, which Gitea's runner sets (the stage's own clone step uses
  it); before, `GITHUB_SERVER_URL` came first too and the host was only the fallback for a run on the Mac. `ci/cor.sh` passes `GITHUB_SERVER_URL` as
  before; `fetch_sdk.sh` without either reads the tea login (the Mac).
- The two new lint checks need `python3`, `bash` and `git` in the lint image (the stage clones with git); numpy is optional.

**Messages and the replay** (D15). Before A8 the replay took each host message as it was (the export line added after the subject) and only the
maintainer's address became `PUBLIC_EMAIL`. Now `tools/gh_export.py` puts every staged file and every message through `scrub()` (the session line
stays) and then through the check (a hit that no placeholder fixes, a private path, a ROM copy or window, stops it), and an author or committer
with a personal mail address stops it (`git config user.email` unset would have let the maintainer's address out). `--rewrite BASE` makes the
clone's commits BASE..HEAD once more the same way, author, committer and dates kept, blobs of BASE's history untouched, the old head kept as
`<branch>-before-rewrite`, then checks commits and messages: for the commits the tool of 92645aeb replays (A5 notes). The 134 `u18` messages since the
last export (27984422) hit only on the session line, which passes.

Checked on the Mac: `ci/lint.sh` rc 0 and `ci/lint.sh tree` rc 0; `check_public_tree.py --selftest` (each rule and each scrub taken out in turn makes it
fail, the session-line exception too); the tree: clean, 5 kept; `test_check_public_tree.py` 5 tests (the hook by hand: a planted path, a file added and
removed again, a session line passes, a host in a message, a personal author, a Gitea URL skipped); `test_gh_export.py` 33 tests (scrub, placeholders,
the message, the address stop, `--rewrite` in a scratch repository); `check_workflows.py --selftest` and over `.github`; shellcheck of the hook and
`fetch_sdk.sh`; the reverse lint fetched the SDK through the tea login. Dry runs in scratch clones of the GitHub export clone, nothing pushed:
`--replay 9b3cdf6b --ref <A8>` onto GitHub main, 6 export commits, then the clone's own hook as for a push of `main` to GitHub: tree, 6 commits
(472 files) and 6 messages clean, every author and committer `PUBLIC_EMAIL`, `.gitea` volumes `/srv`; `--rewrite origin/main~80` over GitHub main's
own last 80 export commits: 31 of our files rewritten, 5 hits reported in 4 files of the flattened fork (an OpenAPI example address, a device-at-computer
token of a test script, Gideon's home path in a Makefile comment, a Python thread-local member), none rewritten.

Not run: the new lint checks in Gitea's lint image (the next Gitea run shows them); `public-tree.yml` on GitHub (after E); the hook on a real push;
the tool of 92645aeb (part a of E needs the submodule); a `--files` run on a current `trx64u.cor` (none built here: the main checkout's older one,
whose record 7 is an `online.u2a` before the pin, holds 15 of the 64 aligned 64-byte blocks of the character ROM; the pinned `online.u2a`
061c7394 and the SDK 0.4.6 tarball hold no window of the four ROMs).

For part E:
1. Replay part a with the tool of 92645aeb, part b with this one, then `--rewrite <GitHub main>` over the work branch and read its report. Hits in the
   flattened fork (third party) are reported, not rewritten: an exact `TEXT_ALLOW` path each, or Alex decides.
2. This tool's `--replay` refuses a SINCE with a gitlink, so part b starts at the A5 merge 9b3cdf6b, whose export becomes the replay's base commit
   with `BASE_MESSAGE` (the 0.2.0 text): that commit needs its own message (open).
3. After the first push: delete `GITEA_KEPT`; the pushing checkout sets `core.hooksPath .githooks` and `user.email` to the public address (the hook
   refuses a personal one on a GitHub push).
4. Already public, untouched by any rewrite of the work branch: commits mode over GitHub main's 83 commits finds 320 hits (110 upstream authors'
   addresses in the flattened fork, none the maintainer's; 79 home paths, 75 private addresses, 17 NAS paths, 15 logins, 13 hosts, the two claude.ai
   links fixed here; 8 windows of the BASIC and character ROMs in the fork's `recovery/u64ii/ultimate.bin`; the fork's `tcpimage.crt`). Only a
   rewrite of GitHub main removes them: Alex's decision (MultiCore's D8, D32).
5. Board tests and NAS tools now need their addresses from the environment (`TRX64_HOST`, `TM_IP`, `TRX_HOST`, `--host`; `TRX_NAS`, `TRX_NAS_DATA`,
   `TRX_NAS_DOCKER`; `GITEA_URL` or the tea login): `CLAUDE.md` and the skills say so when E rewrites them.

### F (D17: ephemeral runners on Hetzner, experiment)

**F-a built 2026-11-24, not yet run** (it needs the accounts Alex brings on 2026-11-25). F-b (Vivado) not started.

Built:
- `.github/workflows/ephemeral-a.yml`, three jobs. `start` (ubuntu-latest, 20 min): sweeps this repository's experiment servers
  older than 90 min, gets a just-in-time runner config (`generate-jitconfig`, runner group 1, one label only:
  `eph-<run id>-<attempt>-<12 random hex>`), renders the user data, creates the server (labels `purpose=gh-ephemeral`,
  `repo=Jondalar.TRX64-Ultimate`, `run=<run id>-<attempt>`; Hetzner refuses `/` in a label value), waits until it runs and until
  GitHub lists the runner online (10 min, else red). `work` (`runs-on: [<that label>]`, 15 min): checkout, `uname -a`,
  `nproc`, `free -m`, `df -h /`, then `ci/ephemeral/proof.sh` writes the machine and the seconds from the create request to each
  step (server running, boot, user data, runner downloaded, dependencies, runner started, online, first step) into the job
  summary. `stop` (ubuntu-latest, `if: always()`, `needs: [start, work]`, 10 min): see the cleanup below.
- `ci/ephemeral/hcloud.sh` (`create`, `wait`, `delete`, `sweep`, `list`) and `ghrunner.sh` (`jit`, `wait-online`, `delete`,
  `user-data`), curl and jq, shared `common.sh` (the token goes to curl on stdin, never on a command line or in the log; GET and
  DELETE retried on 429/5xx, POST never). `user-data.sh`: the cloud-init script. `--dry-run` prints every request with the token
  and the user data left out and answers it with canned data.
- The user data: a lifetime cap first (`systemd-run` timer: power off 45 min later), sshd off (no SSH key, the root password of
  the create answer is dropped and masked), user `runner` with passwordless sudo (as on GitHub's hosted runners), actions/runner
  **2.338.2** linux-x64, SHA-256 `baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40` checked before unpacking,
  `bin/installdependencies.sh`, `run.sh --jitconfig` as `runner`; after the one job the machine powers off.
- Workflow rules kept: triggers only `push` to `scratch/ephemeral-*` and `workflow_dispatch` (no `pull_request`, no
  `pull_request_target`); top-level `permissions: contents: read`; `actions/checkout` pinned at
  `d23441a48e516b6c34aea4fa41551a30e30af803` (v6.1.0, node24); secrets only as env of the steps that need them, `work` gets
  none; the JIT config and the root password are masked and never printed. Job `work` is the documented exception to the rule
  "no self-hosted runner" (workflow header). MultiCore's `tools/check_workflows.py` over `.github/` found nothing: its rule 6
  looks for the word `self-hosted` in `runs-on`, and `work` names only the random label (open point 4, closed by B). Gitea runs
  `.gitea/workflows` only, so this file does nothing there.
- The label rule (a security fix after review, not a choice): the runner carries only the run's random label. A fork pull
  request can bring its own workflow (GitHub runs a pull request's own workflow files), whose job waits up to 24 hours for any
  runner labelled `self-hosted`; with that label our runner could take such a queued job before `work` and run it until the
  cap. `ghrunner.sh jit` refuses `self-hosted` and GitHub's OS and architecture names (`linux`, `windows`, `macos`, `x64`,
  `x86`, `arm`, `arm64`), and fails without writing the config when GitHub's answer lists any label but the requested one; job
  `stop` then removes the registration and no server was created. Default labels: GitHub's REST page on `generate-jitconfig`
  says only "the names of the custom labels to add to the runner" and nothing about defaults; in actions/runner 2.338.2 the
  defaults (`self-hosted`, OS, architecture) are added by `config.sh` (`ConfigurationManager.CreateNewAgent`), while the
  `--jitconfig` path (`Runner.cs`) only writes the decoded config files. So none are requested, the runner adds none, and
  what GitHub's server might add is caught by the answer check (a read-only default label could not be removed by the API).

What the code rests on (read 2026-11-24, not guessed): Hetzner's OpenAPI document (`docs.hetzner.cloud/cloud.spec.json`): the
create body (no `datacenter` field; the data-center endpoints answer 410 since 2026-10-01, changelog), `user_data` at most
32 KiB, the label rules, 412 `resource_unavailable` and 422 `placement_error`, `server_types[].locations[].available` and
`.deprecation`, image `ubuntu-24.04`, locations `nbg1`, `fsn1`. The smallest shared type is CPX02 (1 AMD vCPU, 1 GB, 20 GB;
What's New 2026-10-06, in FSN1, NBG1 and HEL1). Its API name `cpx02` follows the changelog's names (`cpx12` = CPX12, ID 108)
but no API answer was seen without a token, so `create` asks `GET /server_types?name=` first and falls back in the order
`cpx02 cpx12 cx23 cpx22`, `nbg1` before `fsn1` (the cost-optimized CX23 shows "not available" on Hetzner's page today). The
runner hash is the release notes' and was recomputed over the downloaded tarball (228,071,396 bytes). `generate-jitconfig`,
get and delete a runner need the fine-grained permission Administration (write; read for the GETs), GitHub's permission table.

Tests (on the Mac, no request to Hetzner or GitHub): `ci/ephemeral/test_dryrun.sh`, 46 checks, all pass: the whole path with
`--dry-run` and fake tokens; no token, JIT config or root password in any output (under `GITHUB_ACTIONS=true` only in their
`::add-mask::` lines); the user data passes `bash -n`, has no field left, holds the config (3 KB plus the config, of
32 KiB); create skips an unavailable pair; the sweep deletes the 200- and 120-minute servers and keeps the 5-minute one and
refuses a selector without `purpose=gh-ephemeral`; `run in (...)` finds its server; a Primary IP left unassigned is deleted;
the label rule (the JIT request's labels are exactly the random one; the workflow neither requests nor names `self-hosted`;
`jit` refuses `self-hosted` and `X64`; a canned answer with an added `self-hosted` fails and writes no config); shellcheck
clean. Thirteen broken copies each turn checks red: token printed, JIT printed, root password printed, sweep age in seconds,
IPv6 on, sweep guard off, IP cleanup off, user data unredacted, `--label self-hosted` back in the workflow's JIT step,
`self-hosted` back in `work`'s `runs-on`, `jit`'s refusal off, `jit`'s answer check off, the test's own JIT step asking for
`self-hosted`. The real curl path ran against a mock API on 127.0.0.1 (scratch only): bearer
header and JSON body arrive, 412 passes to the next pair, 503 is retried, two pages are joined. Not run anywhere yet: the user
data on Ubuntu 24.04 (no container runtime on the Mac); its first boot is the first real run.

**For Alex, 2026-11-25**, in this order:
1. Hetzner Cloud console: a new project only for this (e.g. `gh-ephemeral`), Security → API tokens → a token with **Read &
   Write**. Hetzner tokens do not expire: delete it when the experiment ends.
2. GitHub: a fine-grained personal access token, resource owner Jondalar, only the repository `TRX64-Ultimate`, repository
   permission **Administration: Read and write** (Metadata: read comes with it), a short expiry (7 days).
3. Repository secrets of `Jondalar/TRX64-Ultimate` (Settings → Secrets and variables → Actions): `HCLOUD_TOKEN` (step 1) and
   `RUNNER_REG_TOKEN` (step 2).
4. Settings → Actions → General: Actions on for the repository, `actions/checkout` allowed, and "Require approval for all
   external contributors" as defence in depth beside the label rule (no fork job starts unseen).
5. The push that starts it, from a GitHub clone (`main` has no such workflow yet, so `workflow_dispatch` is not offered; the
   push trigger is the way):

   ```
   git clone https://github.com/Jondalar/TRX64-Ultimate.git eph && cd eph
   git switch -c scratch/ephemeral-a
   git -C <checkout that holds this note> archive HEAD .github/workflows/ephemeral-a.yml ci/ephemeral | tar -x
   git add .github/workflows/ephemeral-a.yml ci/ephemeral
   git commit -m "U20 F-a: ephemeral runner experiment"
   git push origin scratch/ephemeral-a
   ```

   Every further push to that branch is another run; "Re-run all jobs" too ("Re-run failed jobs" only for a failed `stop`). The
   result is `work`'s job summary; `stop`'s summary lists what is left. From the Mac: `HCLOUD_TOKEN=... ci/ephemeral/hcloud.sh list`.

Cost (Hetzner's pages, 2026-11-24, EUR excl. VAT, IPv4 included): CPX02 "€ 0.0088 per hour" (the page's lowest, the HEL1 price
whose month is € 5.49), € 6.49 a month in FSN1/NBG1, so about € 0.0104 an hour in Germany (same ratio; the start job prints the
API's exact hourly price for the location it took)
([regular-performance](https://www.hetzner.com/cloud/regular-performance/)). Hetzner rounds a server's use up to a whole hour,
bills it in every state until it is deleted, and never above the monthly cap
([billing FAQ](https://docs.hetzner.com/cloud/billing/faq/)). One run: about € 0.01. The fallbacks: CPX12 from € 0.0160 an hour
(€ 11.99 a month in FSN1/NBG1), CX23 from € 0.0096 (€ 5.99; [cost-optimized](https://www.hetzner.com/cloud/cost-optimized/)),
CPX22 from € 0.0272. A server left behind costs its monthly cap at most (€ 6.49 for CPX02) until it is deleted.

Cleanup:
- `stop` runs after success, failure and cancel (`if: always()`): it deletes the server by `start`'s id and every server labelled
  with one of this run's attempts (`run in (<id>-1,...,<id>-<n>)`), so a `start` that died before its outputs leaves nothing;
  waits until the API answers 404; deletes the Primary IPs the server leaves unassigned (Hetzner's docs do not say whether an IP
  made with the server goes with it; the code does not rely on it); removes the runner registration by id and by name. It goes
  red when anything is left, and its summary lists every `purpose=gh-ephemeral` server.
- `start` fails (and `stop` deletes) when the runner is not online within 10 minutes; before creating, it sweeps this
  repository's experiment servers older than 90 minutes (a live run's server is at most 45).
- The server powers itself off 45 minutes after its user data starts, and after its one job. That stops the runner, not the
  bill. No token is on the server.
- By hand: `HCLOUD_TOKEN=... ci/ephemeral/hcloud.sh sweep --older-than 60` deletes every `purpose=gh-ephemeral` server older
  than 60 minutes (a sweep refuses a selector without that term).

Residual risk:
- `stop` does not run or fails: a force-cancel of the run (it skips `always()` jobs), a GitHub outage, the Hetzner API down
  through six attempts over 75 seconds, a revoked or expired token. The server stays, off after 45 minutes and billed, until the next run's
  `start` or a hand sweep deletes it; at most its monthly cap.
- `work` never starts although `start` saw the runner online (the runner died in between): GitHub keeps the
  job queued up to 24 hours and `stop` waits for it; the switched-off server costs about 24 × € 0.0104 ≈ € 0.25 meanwhile.
  Cancelling the run starts `stop` at once.

Open points:
1. The user data has not run on a real Ubuntu 24.04 yet; the first run shows its times. A boot failure is diagnosed through
   Hetzner's web console (root password reset there) or `hcloud.sh create --ssh-key NAME` by hand from the Mac. The first run
   also shows the label list GitHub's answer carries: if its server adds a default label, `start` fails at the JIT step by
   design, and the experiment stops there until that is understood.
2. The runner pin: a runner updates itself when GitHub releases a newer one (that download is the runner's own, not checked
   by our SHA-256), and one not updated within 63 days of a newer release gets no job (GitHub's autoscaling page; it does not
   say how a JIT runner behaves). The first runs' logs show whether 2.338.2 updates itself; `RUNNER_VERSION` and
   `RUNNER_SHA256` in `ci/ephemeral/user-data.sh` move together.
3. F-b (Vivado from a private snapshot) needs its own cap and sweep limit: a Vivado run outlasts the 45 and 90 minutes here.
4. Closed by B (2026-11-25): `tools/check_workflows.py` rule 6 wants a GitHub-hosted image in every `runs-on` and lists this workflow's job `work`
   as the one exception, with its exact label expression.
