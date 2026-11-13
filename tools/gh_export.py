#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the flattened GitHub export of TRX64 Ultimate (docs/todo/T68.md).

The GitHub repository Jondalar/TRX64-Ultimate is private and holds exports of
the tree, never the host repository's own history: the submodule
software/ultimate-app as a plain folder at its pinned commit, no ROMs, no task
list, and nothing that names the other project. Up to 0.2.0 it was one commit;
since then each host commit goes out as an export commit of its own (--replay).
This script does all of that, checks the result and commits it; it pushes only
when told to.

    tools/gh_export.py --check                 # stage and check, touch nothing
    tools/gh_export.py -m "U5: ..."            # stage, check, sync, commit
    tools/gh_export.py -m "U5: ..." --push     # and push to GitHub
    tools/gh_export.py --replay 1eb3944c --onto bb3ffb4
                                               # one export commit per host commit
                                               # since 1eb3944c, in the clone only

The checks are the point: an export that would carry the other project's name,
a link to a file that is not exported, or a binary the last export did not have
stops with a report instead of being committed.

Rules live in the four tables below. REWRITES is the one that needs attention
when a document changes: every rule must still find its text, or the run stops.
This file names the other project, so it is not exported itself.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

HOST_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP_PREFIX = "software/ultimate-app"
DEFAULT_CLONE = os.path.expanduser("~/Development/TRX64-Ultimate-gh")
GH_REPO = "Jondalar/TRX64-Ultimate"

# ---------------------------------------------------------------- what stays out

# Paths (file or directory) that never go into the export, host paths and app
# paths alike. docs/todo/T68.md says why each one is here.
EXCLUDE = [
    # the task list and the submodule wiring
    "TODO.md",
    ".gitmodules",
    # ROMs: Commodore content, replaced by a README that says where they are
    "roms",
    f"{APP_PREFIX}/roms",
    # Gideon's C64U constraints, replaced by a link
    "constraints/c64u.xdc",
    # third-party test content the app carries at Gideon's paths
    f"{APP_PREFIX}/tests/e2e/io/c64/jupiter_lander.crt",
    f"{APP_PREFIX}/software/u64/default_chars.bin",
    f"{APP_PREFIX}/software/ModPlayer_16k/module.bin",
    f"{APP_PREFIX}/target/pc/linux/iecdrive/_261763_8_gc.d64",
    # the maintainer's own notes, the handover contract and the ADR about the
    # two sessions: they are about the two projects working together
    "docs/HANDOVER-maintainer.md",
    "docs/contracts",
    "docs/other-cores.md",
    # the task list in every shape (Alex, 2026-10-31: "Die todos kommen alles raus
    # bei GitHub"): a task's file says what is open, who decided what and when,
    # and that is ours, not the reader's
    "docs/todo",
    # the working documents (Alex, 2026-11-01: "Alles was angestrichen ist, bleibt
    # bitte NUR auf gitea, NICHT in Github"): this is a release repository, so it
    # carries the firmware, the manual and the decisions (docs/adr), not the specs
    # and not the analyses behind them
    "docs/specs",
    "docs/adr",
    "docs/boot-chain-concept.md",
    "docs/c64u-flash-layout.md",
    "docs/lorenz-on-the-core.md",
    "docs/lvgl_ui_project",
    "docs/lvgl_ui_terminal",
    "docs/AlexTest101026.md",
    "docs/architecture-reset-2026-10-05.md",
    "docs/board-tests-pending.md",
    "docs/cart-vice-baseline.md",
    "docs/done-audit-2026-10-04.md",
    "docs/gap-audit-gideon-app.md",
    "docs/gideon-menu-tree.md",
    "docs/lut-budget.md",
    "docs/machine-pause.md",
    "docs/refactor-candidates-2026-10-10.md",
    "docs/turbo-measurements-gideon.md",
    "docs/ultimate-app-hw-spec.md",
    # the publishing tooling: this script and its test name the other project,
    # and the wiki script belongs with them
    "tools/gh_export.py",
    "tools/test_gh_export.py",
    "tools/gh_wiki.py",
]

EXCLUDE_GLOBS = [
    "**/.DS_Store",
]

# ------------------------------------------------------------- what takes its place

_GIDEON_SAME_PATH = """# {name}

Not in this repository (Commodore or third-party content). Gideon Zweijtzer's 1541ultimate
repository carries it at the same path:
https://github.com/GideonZ/1541ultimate/blob/master/{app_path}
"""

PLACEHOLDERS = {
    "roms/README.md": """# ROMs

No ROMs are in this repository. The simulations expect the stock C64 and 1541-II ROMs here
(`basic.901226-01.bin`, `kernal.901227-03.bin`, `characters.901225-01.bin`, `1541-ii.bin`).
They come with VICE (https://vice-emu.sourceforge.io/, its `data/C64` and `data/DRIVES`) and are in
Gideon Zweijtzer's 1541ultimate repository: https://github.com/GideonZ/1541ultimate/tree/master/roms
""",
    "constraints/c64u.xdc.md": """# c64u.xdc

Gideon Zweijtzer's pin constraints for the C64 Ultimate / Ultimate 64 Elite II, not in this repository.
He published them in https://github.com/GideonZ/1541ultimate/issues/615#issuecomment-3853426291 .
The builds read `designs/*/pins.xdc`, not this file.
""",
    f"{APP_PREFIX}/roms/README.md": """# ROMs

No ROMs are in this repository. The application's ROM files (Commodore and cartridge ROMs) are in
Gideon Zweijtzer's 1541ultimate repository, at the same path:
https://github.com/GideonZ/1541ultimate/tree/master/roms
The Commodore ROMs also come with VICE (https://vice-emu.sourceforge.io/).
""",
}

for _p in (
    "tests/e2e/io/c64/jupiter_lander.crt",
    "software/u64/default_chars.bin",
    "software/ModPlayer_16k/module.bin",
    "target/pc/linux/iecdrive/_261763_8_gc.d64",
):
    PLACEHOLDERS[f"{APP_PREFIX}/{_p}.md"] = _GIDEON_SAME_PATH.format(
        name=os.path.basename(_p), app_path=_p
    )

# ------------------------------------------------------------------- the rewrites

# Each rule replaces `old` with `new` in `path`, in the staged copy only. `old`
# must occur exactly `count` times, so a document that changed under a rule
# stops the run instead of going out half-neutralised.
REWRITES = [
    {
        "path": "BUILD.md",
        "old": "What comes next and in which order: [TODO.md](TODO.md).\n",
        "new": "",
        "why": "the task list is not exported",
    },
    # ---- the CI files
    {
        "path": "ci/lib.sh",
        "old": "; MultiCore does the same since its run 898",
        "new": "",
    },
    {
        "path": ".gitea/workflows/pipeline.yml",
        "old": "(T31 decision 15, 2026-10-30; taken from\n  # MultiCore T100 D7). They",
        "new": "(T31 decision 15, 2026-10-30). They",
    },
    # ---- the ADRs
]

# ---------------------------------------------------------------------- the checks

# A hit in the staged tree stops the run. fritz.box host names and lab IP
# addresses are in the export on purpose (they are the defaults of the test
# tooling); the other project's name never is.
FORBIDDEN = [
    r"MultiCore",
    r"Multicore",
    r"TRX64U-MC\b",
    r"TRX64-MC\b",
    r"TREX/TRX64U",
]

# Where the name may stand: the places that tell a reader what is in front of
# them. The application fork is ours (contract §1 and §7: this project builds
# the gateware and the application and may change the application, its config
# code included, alone), and its sources describe a run-time interface the
# shipped firmware really has, down to a popup the user sees — an export that
# renamed a visible string or a comment about the handover block would describe
# firmware nobody builds. The README is the other one (Alex, 2026-10-31: the
# text has to point at the other project so the two are not confused). The rule
# of docs/todo/T68.md is about our specs, ADRs and task files, where the name
# carries internal process, and that is where it is enforced.
FORBIDDEN_EXEMPT = (
    APP_PREFIX + "/",
    "README.md",
)

# Relative links that are allowed to point at nothing in the export.
KNOWN_DANGLING = set()

TEXT_SUFFIXES = {
    ".md", ".txt", ".py", ".sh", ".c", ".h", ".cc", ".cpp", ".v", ".vhd", ".vhdl", ".tcl",
    ".xdc", ".json", ".yml", ".yaml", ".cfg", ".ini", ".mk", ".ld", ".x", ".s", ".asm",
    ".prj", ".csv", ".html", ".css", ".js", ".toml", ".sv", ".svh", ".inc", ".do", ".sdc",
}

BINARY_SUFFIXES = {
    ".bin", ".rom", ".crt", ".d64", ".d71", ".d81", ".g64", ".prg", ".t64", ".tap", ".p00",
    ".img", ".bit", ".mcs", ".zip", ".gz", ".tar", ".u2a", ".ue2", ".cfw", ".cor",
}

LINK_RE = re.compile(r"\]\(\s*(?!https?:|mailto:|#)([^)\s]+)")


class Fail(Exception):
    pass


def run(args, cwd=None, capture=True):
    r = subprocess.run(args, cwd=cwd, check=False,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise Fail("%s failed: %s" % (" ".join(args), (r.stderr or "").strip()))
    return (r.stdout or "").strip()


# ----------------------------------------------------------------------- staging

def stage(ref, stage_dir):
    """The export tree as it should be, before the checks: both repositories'
    tracked files at `ref` and at the commit `ref` pins the submodule to."""
    app_commit = run(["git", "-C", HOST_REPO, "rev-parse", "%s:%s" % (ref, APP_PREFIX)])
    app_repo = os.path.join(HOST_REPO, APP_PREFIX)
    with tempfile.TemporaryDirectory() as tmp:
        for repo, rev, dest in ((HOST_REPO, ref, stage_dir),
                                (app_repo, app_commit, os.path.join(stage_dir, APP_PREFIX))):
            tar = os.path.join(tmp, "x.tar")
            with open(tar, "wb") as fh:
                r = subprocess.run(["git", "-C", repo, "archive", "--format=tar", rev],
                                   stdout=fh, stderr=subprocess.PIPE, text=True, check=False)
            if r.returncode != 0:
                raise Fail("git archive %s failed: %s" % (rev, (r.stderr or "").strip()))
            os.makedirs(dest, exist_ok=True)
            with tarfile.open(tar) as tf:
                tf.extractall(dest)
    return app_commit


def drop_excluded(stage_dir):
    dropped = []
    for rel in EXCLUDE:
        p = os.path.join(stage_dir, rel)
        if os.path.isdir(p):
            shutil.rmtree(p)
            dropped.append(rel + "/")
        elif os.path.exists(p):
            os.remove(p)
            dropped.append(rel)
    import glob
    for pat in EXCLUDE_GLOBS:
        for p in glob.glob(os.path.join(stage_dir, pat), recursive=True):
            os.remove(p)
            dropped.append(os.path.relpath(p, stage_dir))
    return dropped


def write_placeholders(stage_dir):
    for rel, text in PLACEHOLDERS.items():
        p = os.path.join(stage_dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as fh:
            fh.write(text)
    return sorted(PLACEHOLDERS)


_SUBMODULE_NOTE = """# {name}

This folder is empty in this repository. The export is a flat tree: it carries the files of the
application, not the submodules the application pins, and not their history. What belongs here is one
commit of someone else's repository:

| | |
|---|---|
| Repository | {url} |
| Commit | `{sha}` |

To build the application, put that commit in place. From the root of this repository:

```sh
git clone {url} {path}
git -C {path} checkout {sha}
```
"""


def write_submodule_notes(stage_dir, app_commit):
    """A README.md in every empty submodule folder of the application, naming the
    repository and the commit it pins. Without it the export says where lwIP comes
    from (the application's .gitmodules is exported) but not which state, and a
    reader has nothing but the other repository's master to guess with."""
    app_repo = os.path.join(HOST_REPO, APP_PREFIX)
    try:
        cfg = run(["git", "-C", app_repo, "show", "%s:.gitmodules" % app_commit])
    except Fail:
        return []
    subs, path = [], None
    for line in cfg.splitlines():
        line = line.strip()
        if line.startswith("path ="):
            path = line.split("=", 1)[1].strip()
        elif line.startswith("url =") and path:
            url = line.split("=", 1)[1].strip()
            subs.append((path, url.replace("http://github.com", "https://github.com")))
            path = None
    written = []
    for sub, url in subs:
        entry = run(["git", "-C", app_repo, "ls-tree", app_commit, sub])
        if not entry.startswith("160000"):
            continue
        sha = entry.split()[2]
        rel = "%s/%s/README.md" % (APP_PREFIX, sub)
        full = os.path.join(stage_dir, rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as fh:
            fh.write(_SUBMODULE_NOTE.format(name=os.path.basename(sub), url=url, sha=sha,
                                            path="%s/%s" % (APP_PREFIX, sub)))
        written.append(rel)
    return sorted(written)


def apply_rewrites(stage_dir):
    errors = []
    for rule in REWRITES:
        p = os.path.join(stage_dir, rule["path"])
        if not os.path.exists(p):
            errors.append("%s: not in the export, but a rewrite rule wants it" % rule["path"])
            continue
        with open(p) as fh:
            s = fh.read()
        want = rule.get("count", 1)
        have = s.count(rule["old"])
        if have != want:
            errors.append("%s: the rule for %r matches %d times, expected %d — the document "
                          "changed, check the wording" % (rule["path"], rule["old"][:60], have, want))
            continue
        with open(p, "w") as fh:
            fh.write(s.replace(rule["old"], rule["new"]))
    return errors


# ------------------------------------------------------------------------ checks

def walk(stage_dir):
    for root, dirs, files in os.walk(stage_dir):
        dirs[:] = [d for d in dirs if d != ".git"]
        for name in files:
            full = os.path.join(root, name)
            yield os.path.relpath(full, stage_dir), full


def is_text(rel):
    return os.path.splitext(rel)[1].lower() in TEXT_SUFFIXES


def strip_links_to_excluded(stage_dir):
    """A document that links to a file the export leaves out keeps its text and
    loses the link. BUILD.md had that done by hand; every other one is here."""
    excluded = [e.replace(os.sep, "/") for e in EXCLUDE]

    def is_excluded(path):
        return any(path == e or path.startswith(e + "/") for e in excluded)

    link = re.compile(r"\[([^\]\n]+)\]\(\s*(?!https?:|mailto:|#)([^)\s]+)\s*\)")
    stripped = []
    for rel, full in walk(stage_dir):
        if not rel.endswith(".md"):
            continue
        base = os.path.dirname(rel)
        with open(full) as fh:
            s = fh.read()

        def sub(m):
            target = os.path.normpath(os.path.join(base, m.group(2).split("#")[0]))
            if is_excluded(target.replace(os.sep, "/")):
                stripped.append("%s -> %s" % (rel, m.group(2)))
                return m.group(1)
            return m.group(0)

        out = link.sub(sub, s)
        if out != s:
            with open(full, "w") as fh:
                fh.write(out)
    return stripped


def scan_forbidden(stage_dir):
    pats = [re.compile(p) for p in FORBIDDEN]
    hits = []
    for rel, full in walk(stage_dir):
        if not is_text(rel) or rel.replace(os.sep, "/").startswith(FORBIDDEN_EXEMPT):
            continue
        try:
            with open(full, errors="replace") as fh:
                for n, line in enumerate(fh, 1):
                    for pat in pats:
                        if pat.search(line):
                            hits.append("%s:%d: %s" % (rel, n, line.strip()[:160]))
                            break
        except OSError:
            continue
    return hits


def scan_links(stage_dir):
    """Relative links in exported Markdown that point at nothing. A document
    that links to an excluded file has to be rewritten, like BUILD.md was."""
    present = {rel for rel, _ in walk(stage_dir)}
    present |= {os.path.dirname(r) for r in present if os.path.dirname(r)}
    bad = []
    for rel, full in walk(stage_dir):
        if not rel.endswith(".md"):
            continue
        base = os.path.dirname(rel)
        with open(full, errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                for target in LINK_RE.findall(line):
                    t = target.split("#")[0].strip()
                    if not t:
                        continue
                    resolved = os.path.normpath(os.path.join(base, t))
                    if resolved.startswith(".."):
                        continue
                    if resolved in present or "%s:%s" % (rel, t) in KNOWN_DANGLING:
                        continue
                    bad.append("%s:%d: link to %s is not in the export" % (rel, n, t))
    return bad


def scan_new_binaries(stage_dir, clone_dir):
    """Binary content the last export did not have. ROMs and disk images must
    not slip in; a new one is a decision, not a side effect."""
    if not clone_dir or not os.path.isdir(clone_dir):
        return []
    known = {rel for rel, _ in walk(clone_dir)}
    new = []
    for rel, _ in walk(stage_dir):
        if os.path.splitext(rel)[1].lower() in BINARY_SUFFIXES and rel not in known:
            new.append(rel)
    return sorted(new)


# ------------------------------------------------------------------------- output

def sync(stage_dir, clone_dir):
    for rel, full in walk(clone_dir):
        if not os.path.exists(os.path.join(stage_dir, rel)):
            os.remove(full)
    for rel, full in walk(stage_dir):
        dest = os.path.join(clone_dir, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        shutil.copy2(full, dest)
    for root, dirs, files in os.walk(clone_dir, topdown=False):
        if ".git" in root.split(os.sep):
            continue
        if not os.listdir(root) and os.path.abspath(root) != os.path.abspath(clone_dir):
            os.rmdir(root)


def ensure_clone(clone_dir):
    if os.path.isdir(os.path.join(clone_dir, ".git")):
        run(["git", "-C", clone_dir, "fetch", "--quiet", "origin"])
        run(["git", "-C", clone_dir, "checkout", "--quiet", "-B", "main", "origin/main"])
        return
    os.makedirs(os.path.dirname(os.path.abspath(clone_dir)), exist_ok=True)
    run(["gh", "repo", "clone", GH_REPO, clone_dir, "--", "--quiet"])


def digest(stage_dir):
    h = hashlib.sha256()
    for rel, full in sorted(walk(stage_dir)):
        h.update(rel.encode())
        with open(full, "rb") as fh:
            h.update(hashlib.sha256(fh.read()).digest())
    return h.hexdigest()[:12]


def build_export(ref, stage_dir, clone_dir, allow_new_binaries, out=print):
    """Stage the export of `ref` into stage_dir and run the checks. Returns (app commit, the
    report lines of every check that failed); an empty list means the tree may go out."""
    host_rev = run(["git", "-C", HOST_REPO, "rev-parse", "--short", ref])
    app_commit = stage(ref, stage_dir)
    dropped = drop_excluded(stage_dir)
    write_placeholders(stage_dir)
    notes = write_submodule_notes(stage_dir, app_commit)
    errors = apply_rewrites(stage_dir)
    stripped = strip_links_to_excluded(stage_dir)

    files = sum(1 for _ in walk(stage_dir))
    out("export of host %s, app %s: %d files, %d paths left out, %d placeholders, "
        "%d submodule notes, %d rewrites, %d links stripped"
        % (host_rev, app_commit[:8], files, len(dropped), len(PLACEHOLDERS),
           len(notes), len(REWRITES), len(stripped)))

    problems = []
    if errors:
        problems.append("rewrite rules that no longer fit:")
        problems += ["  " + e for e in errors]
    hits = scan_forbidden(stage_dir)
    if hits:
        problems.append("the other project's name is in the export (%d lines):" % len(hits))
        problems += ["  " + h for h in hits[:60]]
        if len(hits) > 60:
            problems.append("  ... and %d more" % (len(hits) - 60))
    links = scan_links(stage_dir)
    if links:
        problems.append("links to files that are not exported (%d):" % len(links))
        problems += ["  " + l for l in links[:60]]
        if len(links) > 60:
            problems.append("  ... and %d more" % (len(links) - 60))
    bins = [] if allow_new_binaries else scan_new_binaries(stage_dir, clone_dir)
    if bins:
        problems.append("binary files the last export did not have (%d):" % len(bins))
        problems += ["  " + b for b in bins[:40]]
    return app_commit, problems


# ------------------------------------------------------------------- the replay

# One export commit per host commit (Alex, 2026-11-13: "jetzt duerfen die Commits mit", from 0.2.0 on),
# instead of one commit for the whole tree. The history of the host repository itself never goes out: it
# carries what EXCLUDE keeps back (ROMs, specs, the task list) and the other project's name. So every host
# commit is exported through the same rules as a single export, and its message goes with it:
#   - a commit whose export changes nothing (specs, ADRs, the task list only) makes no export commit;
#   - a commit whose tree fails the checks is folded into the next one that passes (its change goes out
#     there, with both messages);
#   - a message that names the other project needs an entry below, or the replay stops.
# MESSAGES: host commit (prefix) -> the message that goes out instead; "" leaves the message out (the
# change still goes out in the export commit it lands in).
MESSAGES = {
    # T46 named the other project in its comments (folded into 8f4761ee) and in its message
    "7a9c5bb4": """T46: the c64u build leaves out the JTAG debug master

U13 did not place (Place 30-99): with U11 it added 793 LUT to a c64u build
that had no room left (27,406 LUT, 99.98 % of the slices). Alex: out with
the JTAG debug master.

top.v has `ifdef NO_JTAG_DBG around jtag_dbg (its outputs tied to 0);
build.tcl defines it unless TRX_FEATURES lists jtag; gateware_key carries
features=. The debug registers, the console and keyboard-edge read-out, the
RISC-V tap and the JTAG ports into DDR2, C64 RAM and the trx registers fall
away with it (about 850 LUT). The registers JTAG writes keep their power-up
values of 0. A softload does not use it: the loader that starts a .cfw
hands the app over in DDR2 with the boot magic at 0xFFFC. tools/trxdbg.sh
needs a TRX_FEATURES=jtag build.""",
    # the fix of those comments: on GitHub the comments never had the name
    "8f4761ee": "",
}


def message_for(sha, text):
    """The message a host commit's export carries (MESSAGES, else its own)."""
    for prefix, replacement in MESSAGES.items():
        if sha.startswith(prefix):
            return replacement
    return text


def forbidden_in(text):
    return [p for p in FORBIDDEN if re.search(p, text)]


def export_message(parts, hosts, app_commit):
    """The message of one export commit: the messages of the host commits it carries (oldest first),
    with the line that names them after the first subject."""
    msgs = [m.strip() for m in parts if m.strip()]
    if not msgs:
        raise Fail("no message for the export of %s" % ", ".join(hosts))
    first = msgs[0].split("\n", 1)
    line = "Export of TRX64 Ultimate %s (application %s)." % (", ".join(hosts), app_commit[:8])
    head = first[0] + "\n\n" + line
    rest = [first[1].strip()] if len(first) > 1 and first[1].strip() else []
    return "\n\n".join([head] + rest + msgs[1:]) + "\n"


def replay(since, ref, onto, clone_dir, allow_new_binaries):
    commits = run(["git", "-C", HOST_REPO, "rev-list", "--reverse", "--first-parent",
                   "%s..%s" % (since, ref)]).split()
    if not commits:
        raise Fail("no commits in %s..%s" % (since, ref))
    for sha in commits:
        msg = message_for(sha, run(["git", "-C", HOST_REPO, "log", "-1", "--format=%B", sha]))
        if forbidden_in(msg):
            raise Fail("the message of %s names the other project: give it an entry in MESSAGES"
                       % sha[:8])
    ensure_clone(clone_dir)
    run(["git", "-C", clone_dir, "checkout", "--quiet", "-B", "main", onto])
    print("replay of %d host commits %s..%s onto %s" % (len(commits), since, ref, onto))
    pending, made, skipped, folded = [], [], [], []
    for i, sha in enumerate(commits):
        short = sha[:7]
        msg = message_for(sha, run(["git", "-C", HOST_REPO, "log", "-1", "--format=%B", sha]))
        stage_dir = tempfile.mkdtemp(prefix="trx64-gh-replay-")
        try:
            app_commit, problems = build_export(sha, stage_dir, clone_dir, allow_new_binaries,
                                                out=lambda *_: None)
            if problems:
                if i == len(commits) - 1:
                    raise Fail("the last commit %s does not pass the checks:\n%s"
                               % (short, "\n".join(problems)))
                pending.append((short, msg))
                folded.append(short)
                print("  %s  fails the checks, folded into the next commit" % short)
                continue
            sync(stage_dir, clone_dir)
        finally:
            shutil.rmtree(stage_dir, ignore_errors=True)
        if not run(["git", "-C", clone_dir, "status", "--short"]):
            skipped += [h for h, _ in pending] + [short]
            pending = []
            continue
        hosts = [h for h, _ in pending] + [short]
        body = export_message([m for _, m in pending] + [msg], hosts, app_commit)
        who = run(["git", "-C", HOST_REPO, "log", "-1",
                   "--format=%an%x00%ae%x00%aI%x00%cn%x00%ce%x00%cI", sha]).split("\x00")
        env = dict(os.environ, GIT_AUTHOR_NAME=who[0], GIT_AUTHOR_EMAIL=who[1], GIT_AUTHOR_DATE=who[2],
                   GIT_COMMITTER_NAME=who[3], GIT_COMMITTER_EMAIL=who[4], GIT_COMMITTER_DATE=who[5])
        run(["git", "-C", clone_dir, "add", "-A"])
        r = subprocess.run(["git", "-C", clone_dir, "commit", "-q", "-F", "-"], input=body, env=env,
                           text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if r.returncode != 0:
            raise Fail("commit of the export of %s failed: %s" % (short, r.stderr.strip()))
        made.append(run(["git", "-C", clone_dir, "log", "--oneline", "-1"]))
        print("  %s  -> %s" % (short, made[-1]))
        pending = []
    print("\n%d export commits, %d host commits without a change in the export (%s), %d folded (%s)"
          % (len(made), len(skipped), " ".join(skipped) or "-", len(folded), " ".join(folded) or "-"))
    print("in %s on main; nothing is pushed. A replay onto another commit than origin/main replaces "
          "GitHub's history: push it with --force-with-lease, and only with Alex's OK." % clone_dir)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Build the GitHub export (docs/todo/T68.md).")
    ap.add_argument("--ref", default="HEAD", help="the commit to export (default HEAD)")
    ap.add_argument("--clone-dir", default=DEFAULT_CLONE,
                    help="working clone of the GitHub repository (default %s)" % DEFAULT_CLONE)
    ap.add_argument("-m", "--message", help="commit message for the export")
    ap.add_argument("--check", action="store_true", help="stage and check only, change nothing")
    ap.add_argument("--push", action="store_true", help="push the commit to GitHub")
    ap.add_argument("--allow-new-binaries", action="store_true",
                    help="let binary files the last export did not have through")
    ap.add_argument("--keep-stage", help="keep the staged tree in this directory")
    ap.add_argument("--replay", metavar="SINCE",
                    help="one export commit per host commit of SINCE..REF (first parent), committed in the "
                         "clone, never pushed")
    ap.add_argument("--onto", default="origin/main",
                    help="with --replay: the export commit to build on (default origin/main)")
    args = ap.parse_args()

    clone_dir = os.path.abspath(os.path.expanduser(args.clone_dir))
    if args.replay:
        if args.check or args.message or args.push or args.keep_stage:
            ap.error("--replay takes none of --check, -m, --push, --keep-stage")
        return replay(args.replay, args.ref, args.onto, clone_dir, args.allow_new_binaries)

    if not args.check and not args.message:
        ap.error("a commit needs -m/--message (or use --check)")

    host_rev = run(["git", "-C", HOST_REPO, "rev-parse", "--short", args.ref])
    dirty = run(["git", "-C", HOST_REPO, "status", "--porcelain"])
    if dirty:
        print("note: the working tree has changes; the export is made from %s, not from the "
              "working tree" % host_rev)

    stage_dir = args.keep_stage or tempfile.mkdtemp(prefix="trx64-gh-export-")
    os.makedirs(stage_dir, exist_ok=True)

    try:
        if not args.check:
            ensure_clone(clone_dir)
        app_commit, problems = build_export(args.ref, stage_dir, clone_dir, args.allow_new_binaries)
        if problems:
            print("\n" + "\n".join(problems))
            print("\nnothing was changed. Fix the report above (a rewrite rule in %s, or a path "
                  "in EXCLUDE) and run again." % os.path.basename(__file__))
            return 1

        print("checks passed, tree digest %s" % digest(stage_dir))
        if args.check:
            if args.keep_stage:
                print("staged tree: %s" % stage_dir)
            return 0

        sync(stage_dir, clone_dir)
        status = run(["git", "-C", clone_dir, "status", "--short"])
        if not status:
            print("the export is already what GitHub has; no commit made")
            return 0
        print("\n" + status)
        run(["git", "-C", clone_dir, "add", "-A"])
        body = "%s\n\nExport of TRX64 Ultimate %s (application %s).\n" % (
            args.message, host_rev, app_commit[:8])
        run(["git", "-C", clone_dir, "commit", "-q", "-m", body])
        print("\ncommitted in %s: %s" % (clone_dir,
                                         run(["git", "-C", clone_dir, "log", "--oneline", "-1"])))
        if args.push:
            run(["git", "-C", clone_dir, "push", "origin", "main"], capture=False)
            print("pushed to %s" % GH_REPO)
        else:
            print("not pushed. Run again with --push, or push from %s" % clone_dir)
        return 0
    finally:
        if not args.keep_stage and os.path.isdir(stage_dir):
            shutil.rmtree(stage_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        sys.exit(2)
