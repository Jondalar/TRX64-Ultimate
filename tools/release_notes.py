#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RELEASE_NOTES.md of a TRX64 Ultimate tester release (T49, spec "Release pipeline").

  tools/release_notes.py --id main-1a2b3c4d [--date 2026-10-08] [--repo .] [--ref HEAD]
      [--base <tag-or-commit>] [--manifest manifest.json] [--reports DIR]
      [--run-url URL] [--apps trxmon,trex_crt,...] [--excluded esp_probe,...]
      [--spec docs/specs/T49-update-packages.md] [--todo TODO.md] [-o RELEASE_NOTES.md]

Runs in CI and on the Mac alike; it only reads git, the manifest.json of the CI package, the
package's reports and two documents of the repository.

 * Commits: the superproject's first-parent history from the base to --ref, merges first, then
   direct commits; the app submodule's first-parent history between the commits the two
   releases pin. The base is --base, else the newest v* tag reachable from --ref, else (no
   previous release) the newest MAX_FIRST commits, and the text says so.
 * Install instructions: sections 6.1, 6.2 and 6.4 of the T49 spec.
 * Known open issues: the rows of TODO.md with milestone 1 whose status is not done/dropped,
   one line each (ID, title, the Open column).
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys

MAX_FIRST = 40          # commits listed when there is no base at all
APP_SUBMODULE = "software/ultimate-app"
DOC_PAT = re.compile(r"\[skip ci\]|^(docs?|note|move|spec|todo|adr)\b", re.I)
TASK_PAT = re.compile(r"\bT\d+(?:\.\d+)?\b")


def git(repo, *args, check=True):
    r = subprocess.run(["git", "-C", repo] + list(args), capture_output=True, text=True)
    if r.returncode != 0:
        if check:
            raise SystemExit("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
        return None
    return r.stdout.strip()


def find_base(repo, ref, base):
    """(base commit or None, how it was found)"""
    if base:
        c = git(repo, "rev-parse", "--verify", "--quiet", base + "^{commit}", check=False)
        if not c:
            raise SystemExit("base %r is not a commit of %s" % (base, repo))
        return c, "the given base %s" % base
    tags = git(repo, "tag", "--list", "v*", "--merged", ref, "--sort=-creatordate", check=False) or ""
    head = git(repo, "rev-parse", ref + "^{commit}")
    for t in tags.split():
        c = git(repo, "rev-parse", t + "^{commit}")
        if c != head:           # the release itself does not count as its own predecessor
            return c, "the previous release tag %s" % t
    return None, None


def has_commit(repo, sha):
    return git(repo, "cat-file", "-e", sha + "^{commit}", check=False) is not None


def log(repo, rng, limit=None):
    """First-parent log as [(sha, parents, subject)]."""
    args = ["log", "--first-parent", "--format=%H%x1f%P%x1f%s"]
    if limit:
        args += ["-n", str(limit)]
    args.append(rng)
    out = git(repo, *args, check=False) or ""
    rows = []
    for line in out.splitlines():
        sha, par, subj = line.split("\x1f", 2)
        rows.append((sha, par.split(), subj))
    return rows


def group(rows):
    merges, changes, docs = [], [], []
    for sha, par, subj in rows:
        if len(par) > 1:
            merges.append((sha, subj))
        elif DOC_PAT.search(subj):
            docs.append((sha, subj))
        else:
            changes.append((sha, subj))
    return merges, changes, docs


def bullet(sha, subj):
    subj = TASK_PAT.sub(lambda m: "**%s**" % m.group(0), subj.replace("[skip ci]", "").strip())
    return "- %s `%s`" % (subj, sha[:8])


def commit_lines(rows):
    if not rows:
        return ["No commits.", ""]
    merges, changes, docs = group(rows)
    out = []
    for head, items in (("Merges (features and integrations)", merges),
                        ("Changes", changes), ("Documentation and bookkeeping", docs)):
        if items:
            out += ["**%s**" % head, ""] + [bullet(*i) for i in items] + [""]
    return out


def app_pin(repo, ref):
    return git(repo, "rev-parse", "--verify", "--quiet", "%s:%s" % (ref, APP_SUBMODULE), check=False) or None


def spec_section(path, numbers):
    """Text of the numbered ### sections (e.g. '6.1') of the spec; the heading becomes a bold line."""
    out, take = [], False
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    for line in lines:
        m = re.match(r"^###\s+(\d+\.\d+)\.?\s+(.*)", line)
        if m:
            take = m.group(1) in numbers
            if take:
                out += ["**%s %s**" % (m.group(1), m.group(2)), ""]
            continue
        if re.match(r"^##\s", line):
            take = False
        if take and not (not line.strip() and out and not out[-1]):
            out.append(line)
    text = "\n".join(out).strip()
    # the package files are named trx64_update_<commit>_<YYYYMMDD>.* (spec decision 2026-10-10)
    return re.sub(r"`trx64-<id>\.(cfw|ue2)`", r"`trx64_update_<commit>_<YYYYMMDD>.\1`", text)


def open_issues(path):
    rows = []
    with open(path, encoding="utf-8") as fh:
        todo = fh.read().splitlines()
    for line in todo:
        f = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(f) < 5 or not re.fullmatch(r"T\d+(\.\d+)?", f[0]) or f[1] != "1":
            continue
        if f[2] in ("✅", "✖", "❌"):
            continue
        rows.append("- **%s** %s: %s" % (f[0], f[3], re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", f[4])))
    return rows


def utilization(reports):
    """One line per utilization report of the package that has a 'Slice LUTs' row."""
    lines = []
    if not reports or not os.path.isdir(reports):
        return lines
    for n in sorted(os.listdir(reports)):
        if "utilization" in n and n.endswith(".rpt") and "synth" not in n:
            with open(os.path.join(reports, n), errors="replace") as fh:
                rpt = fh.read().splitlines()
            for l in rpt:
                c = [x.strip() for x in l.split("|")]
                # | Slice LUTs* | Used | Fixed | Prohibited | Available | Util% |
                if len(c) >= 8 and c[1].startswith("Slice LUTs") and c[2].isdigit() and c[-3].isdigit():
                    lines.append("%s: %s of %s LUTs (%s %%)" % (n, c[2], c[-3], c[-2]))
                    break
    return lines


def build(a):
    repo = a.repo
    head = git(repo, "rev-parse", a.ref + "^{commit}")
    base, how = find_base(repo, a.ref, a.base)
    man = {}
    if a.manifest and os.path.exists(a.manifest):
        with open(a.manifest) as fh:
            man = json.load(fh)
    bit_commit = man.get("trx64_commit") or head
    app_commit = man.get("app_commit") or app_pin(repo, a.ref) or ""
    o = ["# TRX64 Ultimate release %s" % a.id, ""]
    o += ["| | |", "|---|---|",
          "| Release id | `%s` |" % a.id,
          "| Date | %s |" % a.date,
          "| Release commit | `%s` |" % head,
          "| Bitstream commit | `%s` |" % bit_commit,
          "| App commit | `%s` |" % (app_commit or "unknown")]
    if man.get("gateware_key"):
        o.append("| Gateware key | `%s` (bitstream from: %s) |" % (man["gateware_key"], man.get("bitstream_from", "?")))
    if a.run_url:
        o.append("| CI run | %s |" % a.run_url)
    wns = man.get("wns_ns") or {}
    if wns:
        o.append("| Timing (ns) | %s |" % ", ".join("%s %s" % kv for kv in sorted(wns.items())))
    for l in utilization(a.reports):
        o.append("| Utilization | %s |" % l)
    o += ["| Target | C64 Ultimate and Ultimate 64 Elite II (50T bitstream, no 100T) |", ""]

    o += ["## Install", ""]
    spec = os.path.join(repo, a.spec)
    o += [spec_section(spec, {"6.1", "6.2", "6.4"}) if os.path.exists(spec) else "(%s not found)" % a.spec, ""]

    o += ["## Contents", ""]
    apps = [x for x in (a.apps or "").split(",") if x]
    exc = [x for x in (a.excluded or "").split(",") if x]
    name = "trx64_update_%s_%s" % (a.id.rsplit("-", 1)[-1], datetime.date.today().strftime("%Y%m%d"))
    o += ["- `%s.cfw`, `%s.ue2`: the updater with the bitstream and the app (id `%s`)" % (name, name, a.id)]
    o += ["- `%s.u2a`: user app" % x for x in apps]
    o += ["- `SHA256SUMS`, `RELEASE_NOTES.md`", ""]
    if exc:
        o += ["Not in this release (test and diagnostic apps): %s." % ", ".join("`%s`" % x for x in exc), ""]

    o += ["## Changes", "", "### TRX64 Ultimate (gateware, tools, CI, docs)", ""]
    if base:
        o += ["Since %s (`%s`)." % (how, base[:8]), ""]
        o += commit_lines(log(repo, "%s..%s" % (base, head)))
    else:
        o += ["No previous release and no base given: the newest %d first-parent commits." % MAX_FIRST, ""]
        o += commit_lines(log(repo, head, limit=MAX_FIRST))

    o += ["### Ultimate app (%s)" % APP_SUBMODULE, ""]
    app_repo = os.path.join(repo, APP_SUBMODULE)
    old_app = app_pin(repo, base) if base else None
    if not app_commit or not os.path.exists(os.path.join(app_repo, ".git")):
        o += ["App history is not available in this checkout.", ""]
    elif old_app and old_app == app_commit:
        o += ["Unchanged (`%s`)." % app_commit[:8], ""]
    elif old_app and not (has_commit(app_repo, old_app) and has_commit(app_repo, app_commit)):
        o += ["App history from `%s` to `%s` is not in this checkout." % (old_app[:8], app_commit[:8]), ""]
    elif old_app:
        o += ["From `%s` to `%s`." % (old_app[:8], app_commit[:8]), ""]
        o += commit_lines(log(app_repo, "%s..%s" % (old_app, app_commit)))
    else:
        o += ["No previous app commit: the newest %d first-parent commits up to `%s`." % (MAX_FIRST, app_commit[:8]), ""]
        o += commit_lines(log(app_repo, app_commit, limit=MAX_FIRST))

    o += ["## Known open issues (milestone 1, TODO.md)", ""]
    todo = os.path.join(repo, a.todo)
    iss = open_issues(todo) if os.path.exists(todo) else []
    o += (iss or ["(none, or %s not found)" % a.todo]) + [""]
    return "\n".join(o)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--id", required=True)
    p.add_argument("--date", default=datetime.date.today().isoformat())
    p.add_argument("--repo", default=".")
    p.add_argument("--ref", default="HEAD")
    p.add_argument("--base")
    p.add_argument("--manifest")
    p.add_argument("--reports")
    p.add_argument("--run-url")
    p.add_argument("--apps")
    p.add_argument("--excluded")
    p.add_argument("--spec", default="docs/specs/T49-update-packages.md")
    p.add_argument("--todo", default="TODO.md")
    p.add_argument("-o", "--out")
    a = p.parse_args()
    text = build(a)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
