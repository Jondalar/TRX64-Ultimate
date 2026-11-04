#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Carries docs/manual into the GitHub wiki (docs/todo/T68.md).

The wiki pages are the manual's files with the links rewritten: a chapter link
becomes a wiki page name, a link that leaves docs/manual becomes a URL into the
repository on GitHub, and the pictures keep their `img/` path. Nothing else is
touched, so the wiki and `docs/manual` stay the same text.

    tools/gh_wiki.py --check            # convert and check, change nothing
    tools/gh_wiki.py -m "..."           # convert, check, commit in the clone
    tools/gh_wiki.py -m "..." --push    # and push

Pages the wiki has of its own (Home, Architecture, Capabilities, Resources) are
left alone: they are written there, not here.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

HOST_REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANUAL = os.path.join(HOST_REPO, "docs", "manual")
DEFAULT_CLONE = os.path.expanduser("~/Development/TRX64-Ultimate-gh-wiki")
WIKI_URL = "https://github.com/Jondalar/TRX64-Ultimate.wiki.git"
BLOB = "https://github.com/Jondalar/TRX64-Ultimate/blob/main"

# docs/manual file -> wiki page. The wiki links by page name, without .md.
PAGES = {
    "README.md": "User-Manual",
    "01-basics.md": "Manual-1-Basics",
    "02-file-browser.md": "Manual-2-File-Browser",
    "03-action-menu.md": "Manual-3-Action-Menu",
    "04-settings.md": "Manual-4-Settings",
    "05-cartridges.md": "Manual-5-Cartridges",
    "06-monitor.md": "Manual-6-Monitor",
    "07-timewarp.md": "Manual-7-Timewarp",
    "08-apps.md": "Manual-8-Apps",
    "09-network.md": "Manual-9-Network",
    "10-differences.md": "Manual-10-Differences",
}

LINK = re.compile(r"\]\(\s*(?!https?:|mailto:|#)([^)\s]+)\s*\)")


class Fail(Exception):
    pass


def run(args, cwd=None, capture=True):
    r = subprocess.run(args, cwd=cwd, check=False,
                       stdout=subprocess.PIPE if capture else None,
                       stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise Fail("%s failed: %s" % (" ".join(args), (r.stderr or "").strip()))
    return (r.stdout or "").strip()


def convert(text, errors, page):
    """The manual's links as the wiki needs them."""

    def sub(m):
        target = m.group(1)
        path, _, anchor = target.partition("#")
        anchor = ("#" + anchor) if anchor else ""
        if not path:                                   # a link inside the page
            return m.group(0)
        if path.startswith("img/"):                    # pictures: same path in the wiki
            if not os.path.exists(os.path.join(MANUAL, path)):
                errors.append("%s: %s is not in docs/manual" % (page, path))
            return m.group(0)
        if path in PAGES:
            return "](%s%s)" % (PAGES[path], anchor)
        if path.startswith("../"):                     # out of docs/manual: into the repository
            repo_rel = os.path.normpath(os.path.join("docs/manual", path))
            if repo_rel.startswith("..") or not os.path.exists(os.path.join(HOST_REPO, repo_rel)):
                errors.append("%s: %s points outside the repository" % (page, target))
                return m.group(0)
            return "](%s/%s%s)" % (BLOB, repo_rel, anchor)
        errors.append("%s: cannot place the link %s" % (page, target))
        return m.group(0)

    return LINK.sub(sub, text)


def ensure_clone(clone_dir):
    if os.path.isdir(os.path.join(clone_dir, ".git")):
        run(["git", "-C", clone_dir, "fetch", "--quiet", "origin"])
        run(["git", "-C", clone_dir, "checkout", "--quiet", "-B", "master", "origin/master"])
        return
    os.makedirs(os.path.dirname(os.path.abspath(clone_dir)), exist_ok=True)
    run(["git", "clone", "--quiet", WIKI_URL, clone_dir])


def main():
    ap = argparse.ArgumentParser(description="Carry docs/manual into the GitHub wiki.")
    ap.add_argument("--clone-dir", default=DEFAULT_CLONE,
                    help="working clone of the wiki (default %s)" % DEFAULT_CLONE)
    ap.add_argument("-m", "--message", help="commit message")
    ap.add_argument("--check", action="store_true", help="convert and check only")
    ap.add_argument("--push", action="store_true", help="push the commit")
    args = ap.parse_args()

    if not args.check and not args.message:
        ap.error("a commit needs -m/--message (or use --check)")

    clone_dir = os.path.abspath(os.path.expanduser(args.clone_dir))
    errors = []
    pages = {}

    have = {f for f in os.listdir(MANUAL) if f.endswith(".md")}
    for f in sorted(have - set(PAGES)):
        errors.append("docs/manual/%s has no wiki page in PAGES" % f)
    for f in sorted(set(PAGES) - have):
        errors.append("PAGES names %s, which is not in docs/manual" % f)

    for src, page in sorted(PAGES.items()):
        p = os.path.join(MANUAL, src)
        if not os.path.exists(p):
            continue
        with open(p) as fh:
            pages[page + ".md"] = convert(fh.read(), errors, src)

    if errors:
        print("the manual does not fit the wiki yet:")
        for e in errors:
            print("  " + e)
        return 1

    print("%d pages, %d pictures" % (len(pages), len(os.listdir(os.path.join(MANUAL, "img")))))
    if args.check:
        return 0

    ensure_clone(clone_dir)
    for name, text in pages.items():
        with open(os.path.join(clone_dir, name), "w") as fh:
            fh.write(text)
    img_dst = os.path.join(clone_dir, "img")
    os.makedirs(img_dst, exist_ok=True)
    src_img = os.path.join(MANUAL, "img")
    for name in os.listdir(src_img):
        shutil.copy2(os.path.join(src_img, name), os.path.join(img_dst, name))
    for name in os.listdir(img_dst):            # a picture the manual no longer has
        if not os.path.exists(os.path.join(src_img, name)):
            os.remove(os.path.join(img_dst, name))

    status = run(["git", "-C", clone_dir, "status", "--short"])
    if not status:
        print("the wiki already has this manual; no commit made")
        return 0
    print("\n" + status)
    run(["git", "-C", clone_dir, "add", "-A"])
    run(["git", "-C", clone_dir, "commit", "-q", "-m", args.message])
    print("\ncommitted: %s" % run(["git", "-C", clone_dir, "log", "--oneline", "-1"]))
    if args.push:
        run(["git", "-C", clone_dir, "push", "origin", "HEAD"], capture=False)
        print("pushed to the wiki")
    else:
        print("not pushed. Run again with --push, or push from %s" % clone_dir)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as e:
        print("error: %s" % e, file=sys.stderr)
        sys.exit(2)
