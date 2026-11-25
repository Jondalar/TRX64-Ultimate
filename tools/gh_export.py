#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the flattened GitHub export of TRX64 Ultimate (docs/todo/T68.md).

The GitHub repository Jondalar/TRX64-Ultimate holds exports of the tree, never
the host repository's own history (that carries the ROMs up to U20 A6), without
Commodore's ROMs (since A6 the tree has none: ci/roms.py fetches the bench ROMs,
roms/README.md says from where). Gideon's pin constraints constraints/c64u.xdc
go out as they are since A6 (D14); before, a link stood in their place. Up to U20 A5 (2026-11-24) the export also flattened the firmware
submodule (Gideon's application, our fork) into a plain folder at its pinned
commit and kept the third-party test files it carried back; since A5 the tree has no
submodule, and a ref that still has one is refused: export it with this tool as
of the last commit before A5 (u18 92645aeb). Up to 0.2.0 it was one commit
and also kept back the working documents and the other project's name; since
then (Alex, 2026-11-13) everything else goes out and each host commit is an
export commit of its own (--replay). This script does that, checks the result
and commits it; it pushes only when told to. Its commits carry PUBLIC_EMAIL in
place of the maintainer's own address (Alex, 2026-11-16). Since U20 A8 every file
and every message of an export goes through the public-tree check's scrub
(tools/check_public_tree.py: internal hosts, private addresses, home and NAS
paths, logins, personal mail addresses and claude.ai links become placeholders)
and the check itself; an author or committer with a personal mail address stops it.

    tools/gh_export.py --check                 # stage and check, touch nothing
    tools/gh_export.py -m "U5: ..."            # stage, check, sync, commit
    tools/gh_export.py -m "U5: ..." --push     # and push to GitHub
    tools/gh_export.py --replay 1eb3944c --onto bb3ffb4
                                               # one export commit per host commit
                                               # since 1eb3944c, in the clone only
    tools/gh_export.py --rewrite origin/main   # U20 D15: the one rewrite of the clone's
                                               # commits since origin/main (made by an
                                               # older tool) before their first push

The checks are the point: an export with a link to a file that is not exported,
or a binary the last export did not have, stops with a report instead of being
committed.

Rules live in the tables below: EXCLUDE (what may not go out), PLACEHOLDERS
(what stands in its place) and REWRITES (text changed in the staged copy only;
empty since 2026-11-13, every rule must still find its text or the run stops).
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
import textwrap

HOST_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HOST_REPO, "tools"))
import check_public_tree as cpt  # noqa: E402  (U20 A8: the scrub and the check of what goes out)
DEFAULT_CLONE = os.path.expanduser("~/Development/TRX64-Ultimate-gh")
GH_REPO = "Jondalar/TRX64-Ultimate"

# ---------------------------------------------------------------- what stays out

# Paths (file or directory) that never go into the export. docs/todo/T68.md says
# why each one is here. Empty since U20 A6: the ROMs are not tracked any more and
# constraints/c64u.xdc goes out (D14: Gideon posted it in his GPL-3 project's
# issue #615, its header names that source).
EXCLUDE = []

EXCLUDE_GLOBS = [
    "**/.DS_Store",
    # ROMs: Commodore content. The tree tracks none since U20 A6 (.gitignore); a ROM image never goes out.
    "roms/*.bin",
]

# ------------------------------------------------------------- what takes its place

PLACEHOLDERS = {}

# ------------------------------------------------------------------- the rewrites

# Each rule replaces `old` with `new` in `path`, in the staged copy only. `old`
# must occur exactly `count` times, so a document that changed under a rule
# stops the run instead of going out half-neutralised.
REWRITES = []

# ---------------------------------------------------------------------- the checks

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


def run(args, cwd=None, capture=True, env=None):
    r = subprocess.run(args, cwd=cwd, check=False, env=env,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise Fail("%s failed: %s" % (" ".join(args), (r.stderr or "").strip()))
    return (r.stdout or "").strip()


# ------------------------------------------------------------------ the address

# The address the export commits carry (Alex, 2026-11-16, before GitHub goes public). The maintainer's host commits
# carry his own address, which stays on Gitea: an author or committer with the host repository's address
# (`git config user.email`) goes out as PUBLIC_EMAIL under the same name. Any other address (a contributor's, from a
# pull request applied on Gitea) goes out as it is. gh_wiki.py commits under PUBLIC_EMAIL as well.
PUBLIC_EMAIL = "dkl@trx64.org"


def own_email():
    """The host repository's own address (git config user.email), "" when none is set."""
    r = subprocess.run(["git", "-C", HOST_REPO, "config", "user.email"], check=False,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return r.stdout.strip()


def public_email(email, own):
    """The address a commit by `email` carries on GitHub, `own` being the host repository's address."""
    return PUBLIC_EMAIL if own and email.strip().lower() == own.strip().lower() else email


def public_env():
    """The environment of a commit the maintainer makes in a clone: PUBLIC_EMAIL as author and committer."""
    return dict(os.environ, GIT_AUTHOR_EMAIL=PUBLIC_EMAIL, GIT_COMMITTER_EMAIL=PUBLIC_EMAIL)


# ----------------------------------------------------------------------- staging

def gitlinks(ref):
    """The submodule paths (gitlinks) of the tree at `ref`."""
    out = run(["git", "-C", HOST_REPO, "ls-tree", "-r", ref])
    return [line.split("\t", 1)[1] for line in out.splitlines() if line.startswith("160000 ")]


def stage(ref, stage_dir):
    """The export tree as it should be, before the checks: the tracked files at `ref`.
    A ref with a submodule is refused (see the top of this file)."""
    subs = gitlinks(ref)
    if subs:
        raise Fail("%s has the submodule %s: export it with this tool as of u18 92645aeb, the last "
                   "commit before U20 A5, which flattens it" % (ref, ", ".join(subs)))
    with tempfile.TemporaryDirectory() as tmp:
        tar = os.path.join(tmp, "x.tar")
        with open(tar, "wb") as fh:
            r = subprocess.run(["git", "-C", HOST_REPO, "archive", "--format=tar", ref],
                               stdout=fh, stderr=subprocess.PIPE, text=True, check=False)
        if r.returncode != 0:
            raise Fail("git archive %s failed: %s" % (ref, (r.stderr or "").strip()))
        os.makedirs(stage_dir, exist_ok=True)
        with tarfile.open(tar) as tf:
            tf.extractall(stage_dir)


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


def build_export(ref, stage_dir, clone_dir, allow_new_binaries, out=print, notes_out=None):
    """Stage the export of `ref` into stage_dir and run the checks. Returns the report lines
    of every check that failed; an empty list means the tree may go out. With
    notes_out (a list), dead links are collected there instead of failing the export: a replay
    passes it for the commits before the last, whose history may link ahead to a file a later
    commit adds."""
    host_rev = run(["git", "-C", HOST_REPO, "rev-parse", "--short", ref])
    stage(ref, stage_dir)
    dropped = drop_excluded(stage_dir)
    write_placeholders(stage_dir)
    errors = apply_rewrites(stage_dir)
    stripped = strip_links_to_excluded(stage_dir)
    scrubbed = scrub_stage(stage_dir)

    files = sum(1 for _ in walk(stage_dir))
    out("export of host %s: %d files, %d paths left out, %d placeholders, "
        "%d rewrites, %d links stripped, %d files scrubbed"
        % (host_rev, files, len(dropped), len(PLACEHOLDERS), len(REWRITES), len(stripped), len(scrubbed)))

    problems = []
    if errors:
        problems.append("rewrite rules that no longer fit:")
        problems += ["  " + e for e in errors]
    links = scan_links(stage_dir)
    if links and notes_out is not None:
        notes_out += links
        links = []
    if links:
        problems.append("links to files that are not exported (%d):" % len(links))
        problems += ["  " + l for l in links[:60]]
        if len(links) > 60:
            problems.append("  ... and %d more" % (len(links) - 60))
    bins = [] if allow_new_binaries else scan_new_binaries(stage_dir, clone_dir)
    if bins:
        problems.append("binary files the last export did not have (%d):" % len(bins))
        problems += ["  " + b for b in bins[:40]]
    hits = check_stage(stage_dir)
    if hits:
        problems.append("the public-tree check finds (%d, tools/check_public_tree.py):" % len(hits))
        problems += ["  " + h for h in hits[:60]]
    return problems


def scrub_stage(stage_dir):
    """U20 D15, A8: every text file of the export through the public-tree check's scrub, so the public copy holds no internal host, address or
    path (the hits a commit made before A8 still carries, and the GITEA_KEPT lines the Gitea runners need). Returns [(path, rules)]."""
    done = []
    for rel, full in walk(stage_dir):
        if os.path.islink(full):
            continue
        with open(full, "rb") as fh:
            data = fh.read()
        new, rules = cpt.scrub(data, rel)
        if rules:
            with open(full, "wb") as fh:
                fh.write(new)
            done.append((rel, rules))
    return done


def check_stage(stage_dir, private=cpt.PRIVATE):
    """The public-tree check over the staged tree (after the scrub only what no placeholder can fix is left: a private path, a copy or a window
    of a private file, a symlink to one)."""
    items = []
    for rel, full in sorted(walk(stage_dir)):
        if os.path.islink(full):
            t = os.readlink(full)
            items.append((rel, t.encode("utf-8", "surrogateescape"), t))
        else:
            with open(full, "rb") as fh:
                items.append((rel, fh.read()))
    files, wins = cpt.load_private(private)
    return cpt.check_items(items, files, wins)


def scrub_message(body):
    """The message as it goes out: through the scrub (the Claude-Session trailer stays), then checked; (text, rules that rewrote something)."""
    new, rules = cpt.scrub(body.encode("utf-8", "surrogateescape"), message=True)
    left = [r[0] for r in cpt.RULES if cpt.rule_fires(r, cpt.message_text(new))]
    if left:
        raise Fail("the message still has %s after the scrub" % ", ".join(left))
    return new.decode("utf-8", "surrogateescape"), rules


def checked_email(email, what):
    """An author's or committer's address as it goes out; a personal mail address stops the export (the host repository's own address
    becomes PUBLIC_EMAIL only when git config user.email names it)."""
    if cpt.text_hits(email.encode("utf-8", "surrogateescape")):
        raise Fail("%s would go out with a personal mail address: set the host repository's git config user.email to the maintainer's own "
                   "address (it goes out as %s), or rewrite that commit's author" % (what, PUBLIC_EMAIL))
    return email


# ------------------------------------------------------------------- the replay

# One export commit per host commit (Alex, 2026-11-13: "jetzt duerfen die Commits mit", from 0.2.0 on),
# instead of one commit for the whole tree. The host repository's own history still never goes out: it
# carries what EXCLUDE keeps back. So every host commit is exported through the same rules and committed
# with its own message, author and dates (the maintainer's address replaced by PUBLIC_EMAIL):
#   - first the export of SINCE itself, when it differs from the commit the replay builds on, with
#     SINCE's own message (U20 E: SINCE is the A5 merge, the commit the tool of 92645aeb left off at);
#     a note in it counts the files whose only change there is the scrub (scrub_only), against ONTO as it
#     is: replay onto commits that --rewrite has already made, or the count is not what goes out;
#   - a commit whose export changes nothing (one that touches only what EXCLUDE keeps back) makes no
#     export commit;
#   - a commit whose tree fails the checks is folded into the next one that passes (its change goes out
#     there, with both messages). A dead link does not fail a commit before the last: the host commit
#     had it too (a link ahead to a file the next commit adds), and the last state must be clean.


def export_message(parts, hosts, note=None):
    """The message of one export commit: the messages of the host commits it carries (oldest first),
    with the line that names them after the first subject, and `note` as a paragraph after that line."""
    msgs = [m.strip() for m in parts if m.strip()]
    if not msgs:
        raise Fail("no message for the export of %s" % ", ".join(hosts))
    first = msgs[0].split("\n", 1)
    line = "Export of TRX64 Ultimate %s." % ", ".join(hosts)
    head = first[0] + "\n\n" + line + ("\n\n" + note.strip() if note and note.strip() else "")
    rest = [first[1].strip()] if len(first) > 1 and first[1].strip() else []
    return scrub_message("\n\n".join([head] + rest + msgs[1:]) + "\n")[0]


def commit_export(clone_dir, body, sha):
    """Commit the synced clone with the author, committer and dates of host commit `sha`, the maintainer's
    address replaced by PUBLIC_EMAIL."""
    who = run(["git", "-C", HOST_REPO, "log", "-1",
               "--format=%an%x00%ae%x00%aI%x00%cn%x00%ce%x00%cI", sha]).split("\x00")
    own = own_email()
    env = dict(os.environ, GIT_AUTHOR_NAME=who[0], GIT_AUTHOR_DATE=who[2], GIT_COMMITTER_NAME=who[3], GIT_COMMITTER_DATE=who[5],
               GIT_AUTHOR_EMAIL=checked_email(public_email(who[1], own), "the author of %s" % sha[:8]),
               GIT_COMMITTER_EMAIL=checked_email(public_email(who[4], own), "the committer of %s" % sha[:8]))
    run(["git", "-C", clone_dir, "add", "-A"])
    r = subprocess.run(["git", "-C", clone_dir, "commit", "-q", "-F", "-"], input=body, env=env,
                       text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if r.returncode != 0:
        raise Fail("commit of the export of %s failed: %s" % (sha[:8], r.stderr.strip()))


def scrub_only(clone_dir, since):
    """The files of the synced clone that differ from its HEAD although the host file at `since` is HEAD's blob: their change is the
    export's scrub alone (a file that went out before the scrub with a hit of the public-tree check)."""
    out = []
    for p in run(["git", "-C", clone_dir, "diff", "--name-only", "-z", "HEAD"]).split("\0"):
        if not p:
            continue
        host = subprocess.run(["git", "-C", HOST_REPO, "rev-parse", "-q", "--verify", "%s:%s" % (since, p)],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False).stdout.strip()
        if host and host == run(["git", "-C", clone_dir, "rev-parse", "HEAD:%s" % p]):
            out.append(p)
    return out


def base_note(host, scrubbed):
    """The paragraph of the replay's first commit that names what it carries besides the host commit's change."""
    if not scrubbed:
        return None
    n = len(scrubbed)
    return textwrap.fill("The replay's first commit: besides %s's change it writes the placeholders of the public-tree scrub "
                         "(U20 A8, tools/check_public_tree.py) into %d %s that went out before with a hit of the check and %s "
                         "otherwise unchanged." % (host, n, "file" if n == 1 else "files", "is" if n == 1 else "are"), 72)


def replay(since, ref, onto, clone_dir, allow_new_binaries):
    commits = run(["git", "-C", HOST_REPO, "rev-list", "--reverse", "--first-parent",
                   "%s..%s" % (since, ref)]).split()
    if not commits:
        raise Fail("no commits in %s..%s" % (since, ref))
    ensure_clone(clone_dir)
    run(["git", "-C", clone_dir, "checkout", "--quiet", "-B", "main", onto])
    print("replay of %d host commits %s..%s onto %s" % (len(commits), since, ref, onto))
    made = []
    stage_dir = tempfile.mkdtemp(prefix="trx64-gh-replay-")
    try:
        problems = build_export(since, stage_dir, clone_dir, allow_new_binaries,
                                out=lambda *_: None)
        if problems:
            raise Fail("the export of %s does not pass the checks:\n%s" % (since, "\n".join(problems)))
        sync(stage_dir, clone_dir)
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)
    if run(["git", "-C", clone_dir, "status", "--short"]):
        host = run(["git", "-C", HOST_REPO, "rev-parse", "--short=7", since])
        msg = run(["git", "-C", HOST_REPO, "log", "-1", "--format=%B", since])
        commit_export(clone_dir, export_message([msg], [host], base_note(host, scrub_only(clone_dir, since))), since)
        made.append(run(["git", "-C", clone_dir, "log", "--oneline", "-1"]))
        print("  %s  -> %s" % (host, made[-1]))
    pending, skipped, folded = [], [], []
    for i, sha in enumerate(commits):
        short = sha[:7]
        msg = run(["git", "-C", HOST_REPO, "log", "-1", "--format=%B", sha])
        stage_dir = tempfile.mkdtemp(prefix="trx64-gh-replay-")
        dead = []
        try:
            problems = build_export(sha, stage_dir, clone_dir, allow_new_binaries,
                                    out=lambda *_: None,
                                    notes_out=dead if i < len(commits) - 1 else None)
            for d in dead:
                print("  %s  note: %s (as in the host commit itself)" % (short, d))
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
        commit_export(clone_dir, export_message([m for _, m in pending] + [msg], hosts), sha)
        made.append(run(["git", "-C", clone_dir, "log", "--oneline", "-1"]))
        print("  %s  -> %s" % (short, made[-1]))
        pending = []
    print("\n%d export commits, %d host commits without a change in the export (%s), %d folded (%s)"
          % (len(made), len(skipped), " ".join(skipped) or "-", len(folded), " ".join(folded) or "-"))
    print("in %s on main; nothing is pushed. A replay onto another commit than origin/main replaces "
          "GitHub's history: push it with --force-with-lease, and only with Alex's OK." % clone_dir)
    return 0


# ------------------------------------------------------------------- the rewrite

def rewrite(base, clone_dir, private=cpt.PRIVATE):
    """U20 D15 (MultiCore D36's form): the one rewrite of the clone's commits base..HEAD before their first push, for export commits that a
    tool before A8 made (the replay of the commits before U20 A5 runs with the tool of u18 92645aeb, which has no scrub). The commits must form
    one line on base. Each is made again with every text file and its message through the public-tree scrub, author, committer and dates kept;
    a blob that base's history already holds stays as it is (it is public already; MultiCore D36 left GitHub main's blobs alone, too). The old
    head stays as the branch <branch>-before-rewrite. Then the check's commits mode and message mode (with the addresses) over the new commits
    must be clean. Returns the new head."""
    def git(*a, **kw):
        return run(["git", "-C", clone_dir] + list(a), **kw)
    branch = git("symbolic-ref", "--short", "HEAD")
    old_head = git("rev-parse", "HEAD")
    parent = git("rev-parse", base + "^{commit}")
    public = {ln.split()[0] for ln in git("rev-list", "--objects", parent).splitlines() if ln}
    lines = git("rev-list", "--reverse", "--parents", "%s..HEAD" % parent).splitlines()
    if not lines:
        raise Fail("no commits in %s..HEAD" % base)
    cat = subprocess.Popen(["git", "-C", clone_dir, "cat-file", "--batch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def obj(sha):
        cat.stdin.write(sha.encode() + b"\n"); cat.stdin.flush()
        size = int(cat.stdout.readline().split()[2])
        data = cat.stdout.read(size); cat.stdout.read(1)
        return data

    blobs, made, files_changed, msgs_changed = {}, parent, 0, 0
    index = tempfile.NamedTemporaryFile(prefix="trx64-rewrite-index-", delete=False).name
    try:
        prev = parent
        for ln in lines:
            c, *parents = ln.split()
            if parents != [prev]:
                raise Fail("%s is no single-parent commit on %s: the rewrite takes one line of commits" % (c[:8], prev[:8]))
            prev = c
            ents = []
            for e in subprocess.run(["git", "-C", clone_dir, "ls-tree", "-r", "-z", c], stdout=subprocess.PIPE, check=True).stdout.split(b"\0"):
                if not e:
                    continue
                meta, path = e.split(b"\t", 1)
                mode, kind, sha = meta.decode().split()
                if kind == "blob" and mode in ("100644", "100755") and sha not in public:
                    key = (path, sha)
                    if key not in blobs:
                        new, rules = cpt.scrub(obj(sha), path.decode("utf-8", "surrogateescape"))
                        blobs[key] = sha if not rules else subprocess.run(
                            ["git", "-C", clone_dir, "hash-object", "-w", "--stdin"], input=new, stdout=subprocess.PIPE, check=True).stdout.decode().strip()
                        files_changed += bool(rules)
                    sha = blobs[key]
                ents.append(b"%s %s\t%s" % (mode.encode(), sha.encode(), path))
            env = dict(os.environ, GIT_INDEX_FILE=index)
            os.path.exists(index) and os.remove(index)
            subprocess.run(["git", "-C", clone_dir, "update-index", "--add", "-z", "--index-info"], input=b"\0".join(ents) + b"\0", env=env, check=True)
            tree = run(["git", "-C", clone_dir, "write-tree"], env=env)
            raw = obj(c)
            head, _, msg = raw.partition(b"\n\n")
            who = {}
            for h in head.split(b"\n"):
                k, _, v = h.partition(b" ")
                if k in (b"author", b"committer"):
                    name, _, rest = v.decode("utf-8", "surrogateescape").partition(" <")
                    mail, _, date = rest.partition("> ")
                    who[k.decode()] = (name, checked_email(mail, "the %s of %s" % (k.decode(), c[:8])), date)
            new_msg, rules = scrub_message(msg.decode("utf-8", "surrogateescape"))
            msgs_changed += bool(rules)
            cenv = dict(os.environ, GIT_AUTHOR_NAME=who["author"][0], GIT_AUTHOR_EMAIL=who["author"][1], GIT_AUTHOR_DATE=who["author"][2],
                        GIT_COMMITTER_NAME=who["committer"][0], GIT_COMMITTER_EMAIL=who["committer"][1], GIT_COMMITTER_DATE=who["committer"][2])
            r = subprocess.run(["git", "-C", clone_dir, "commit-tree", tree, "-p", made, "-F", "-"], input=new_msg, env=cenv, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            if r.returncode != 0:
                raise Fail("commit-tree for %s failed: %s" % (c[:8], r.stderr.strip()))
            made = r.stdout.strip()
    finally:
        cat.stdin.close(); cat.wait()
        os.path.exists(index) and os.remove(index)
    git("branch", "-f", branch + "-before-rewrite", old_head)
    git("update-ref", "refs/heads/" + branch, made, old_head)
    git("reset", "-q", "--hard", made)
    files, wins = cpt.load_private(private)
    bad, n, nb = cpt.check_commits(["%s..%s" % (parent, made)], files, wins, clone_dir)
    bad += cpt.check_messages(["%s..%s" % (parent, made)], True, clone_dir)[0]
    print("rewrote %d commits on %s (%d files and %d messages changed); the old head %s stays as %s-before-rewrite"
          % (len(lines), base, files_changed, msgs_changed, old_head[:8], branch))
    if bad:
        raise Fail("the public-tree check still finds, over the rewritten commits:\n  " + "\n  ".join(bad[:60]))
    print("public-tree check over the %d rewritten commits (%d files): clean; nothing is pushed" % (n, nb))
    return made


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
    ap.add_argument("--rewrite", metavar="BASE",
                    help="U20 D15: rewrite the clone's commits BASE..HEAD once through the public-tree scrub, never pushed")
    ap.add_argument("--onto", default="origin/main",
                    help="with --replay: the export commit to build on (default origin/main)")
    args = ap.parse_args()

    clone_dir = os.path.abspath(os.path.expanduser(args.clone_dir))
    if args.rewrite:
        if args.replay or args.check or args.message or args.push or args.keep_stage:
            ap.error("--rewrite takes none of --replay, --check, -m, --push, --keep-stage")
        rewrite(args.rewrite, clone_dir)
        return 0
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
        problems = build_export(args.ref, stage_dir, clone_dir, args.allow_new_binaries)
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
        body = scrub_message("%s\n\nExport of TRX64 Ultimate %s.\n" % (args.message, host_rev))[0]
        run(["git", "-C", clone_dir, "commit", "-q", "-m", body], env=public_env())
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
