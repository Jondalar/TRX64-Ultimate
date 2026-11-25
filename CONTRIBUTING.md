# Contributing

TRX64 Ultimate is maintained by a very small team. These rules keep issues something we can actually
finish.

## How we work

- **An issue needs no fix.** Nobody can be expected to debug a bitstream, so a report we can reproduce
  is enough. A pull request is welcome, not required.
- **We group issues by topic and plan them**: when we look at them, and in which release the fix ships.
  A fix waits for its topic's turn.
- **There is no service level.** No response time, no fix time, no release date.
- **This is not Commodore's firmware and not Gideon Zweijtzer's.** Running it leaves their warranty
  ([README.md](README.md)). Report what happens on it here, never to them.

## Before you open an issue

- **Install the current release first** and try it again. If an open issue already covers it, comment
  there instead of opening another one.
- **One defect per issue.** Do not bundle findings, and do not add new ones to an issue that was closed
  after a fix. Open a new issue for each.
- **Read the manual chapter for the function** ([docs/manual](docs/manual/README.md)). What this firmware
  deliberately does differently is listed in
  [chapter 10](docs/manual/10-differences.md) — no analog video, reDIP SID instead of UltiSID, no
  lighting, no internal I/O devices while a cartridge is active, and Gideon's unmodified application
  does not run on this bitstream. None of that is a defect.
- **The machine has to be on our firmware.** We cannot look at a fault on the stock firmware.

## What an issue must contain

We take an issue only if we can reproduce it **end to end, exactly as you saw it**. If we cannot, we
close it and ask for what is missing. We do not fix half of an issue on a guess.

- **Release:** the name of the package you installed (`trx64_update_<id>_<date>.cfw`), or the version and
  build id from `GET /v1/info` on the board.
- **Board:** C64 Ultimate or Ultimate 64 Elite II.
- **The exact steps, from switching on**, one per line: menus, keys, files. A fault that needs a
  cartridge in the port, a drive or a network says so.
- **What you expected, and what happened.**
- **Photos or a short video of the screen**, showing the fault and the menu settings.
- **The media you used, by name only:** title, release, file name and sha256. We use our own copy.
- For the monitor or the RPC server: the request you sent and the answer you got, as text.
- **Acceptance criteria** we can check, numbered.

If you cannot share an input, say so. We will then decide whether the issue can be reproduced at all
before anyone starts on it.

## What must not go into an issue or a pull request

- **No ROMs**, no disk, tape or cartridge images, and nothing made from them: dumps, extracted payloads,
  listings. Name them and where they came from, that is all we need.
- **No firmware package of Commodore's or Gideon's**, and no part of one.
- **No backup files** the installer wrote (`TRXBK<nnn>.BIN`, `TRXBK<nnn>.CFW`). They contain your
  settings, and that includes your network password in clear.

## Retests

We close an issue as soon as the fix is in `main`. Retest it with the release that carries the fix; if it
does not hold, reopen the issue and report only that. Anything new from a retest is a new issue.

## Pull requests

**This repository is an export.** The work happens on our own Gitea in the LAN, with its branches and its CI.
GitHub gets `main` from there: every commit since release 0.2.0 is an export commit here, with its message,
author and date, and with the tree as it stands on Gitea, less Commodore's ROMs, Gideon's pin constraints for
the C64 Ultimate and four third-party files. The specifications (`docs/specs`), the decisions (`docs/adr`) and
the task list (`TODO.md`) come along. The export is pushed with each release and now and then in between; the
history before 0.2.0 stays on Gitea, because it carries the ROMs.

A pull request here is therefore a patch, not something we can merge — a merge on GitHub is overwritten by the
next export, which writes the tree as it stands on Gitea, and nothing warns you. What we do instead: we take the
patch, apply it on Gitea with your name on the commit, run it through our CI and ship it in a release; the
export then brings your commit here. The pull request is closed, naming the build that carries it.
**Closed does not mean refused.** A refusal says so in words, with the reason.

One thing follows from the export for the diff itself (every file reads here as it does with us; Gideon's
application, the firmware submodule, is not in the tree since U20 A5, and the bench ROMs are fetched since A6,
`roms/README.md`):

- The specifications are written on Gitea, with the code they describe. Point the PR at the spec it touches
  and describe the behaviour you want; we change the spec.

The rules for the patch itself:

- One change per PR, with a link to its issue.
- **Gateware** (`designs/`, `cores/`): a change to VHDL or Verilog is only reviewable with a Vivado run
  behind it. Put the worst slack and the LUT count of your run in the PR
  ([BUILD.md](BUILD.md) has the build). A design that does not close timing does not go in.
- **Software**: the checks are `ci/lint.sh`, `ci/sims.sh` and, for `software/trx64u`, its `build.sh` and
  `pack_cor.sh`. Run what your change touches, and say in the PR which you ran and what came out.
- Add or adapt the simulation bench that fails without your change.
- Never make a check weaker so that it passes.
- A change that alters what the user sees updates [docs/manual](docs/manual/README.md) in the same PR.
- Our code is GPL-3.0-or-later; parts that keep their own licence are listed in
  [LICENSING.md](LICENSING.md). By opening a pull request you put your change under that licence.
