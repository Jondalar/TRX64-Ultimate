#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore ci/order.py (t239.3 d9ddadeb1; Alex Damhuis' code in both repositories), U20 B.
# Changed here: one entry (c64u-<key>), no lanes of cores, our tag prefix (ci/pkg.env BUILDS_TAG_PREFIX).
"""U20 B (MultiCore T239.1 D11, D13, D19): the work order. The dispatcher job asks which gateware entries this run needs and which of them the builds
repository does not hold yet.

  ci/order.py
    env  PLAN_JSON_FILE   the run's plan.json (ci/dispatch.py; checked to be one)
         WORK_ORDER_OUT   where work-order.json goes
         GITHUB_OUTPUT    gets `missing=true|false` (optional)
         GH_TOKEN         optional: only lifts the rate limit of the read (the builds repository is public)
         MC_BUILDS_REPO, BUILDS_TAG_PREFIX   default from ci/pkg.env; BUILDS_API default https://api.github.com
         ORDER_SHA        the commit of the run (default: HEAD of the checkout)
         ORDER_KEYS       a file with the entry names instead of ci/keys.sh (tests)

The entries are the names ci/keys.sh prints (c64u-<16 hex>); the release of an entry is <BUILDS_TAG_PREFIX>bit-<entry>. An entry is missing when that
release does not exist or is still a draft. work-order.json is {"version": 1, "sha": ..., "lanes": [...], "entries": [...]}: entries lists the missing
ones in the order of ci/keys.sh (the builder reads nothing else), lanes the matrix lanes that wait for them (bits:c64_basic). An empty list means no
gateware is built for this run. Every run needs every entry: the core lane packs its .cor with the bitstream of this commit.
"""
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ENTRY = re.compile(r"^(c64u)-([0-9a-f]{16})$")
LANE = {"c64u": "bits:c64_basic"}


class OrderError(Exception):
    pass


def env_file(path):
    d = {}
    for line in open(path):
        m = re.match(r"^(\w+)=(\S*)", line)
        if m:
            d[m.group(1)] = m.group(2)
    return d


def entry_names():
    """The entry names of this tree, in the order of ci/keys.sh."""
    override = os.environ.get("ORDER_KEYS")
    if override:
        text = open(override).read()
    else:
        r = subprocess.run([os.path.join(HERE, "keys.sh")], capture_output=True, text=True)
        if r.returncode != 0:
            raise OrderError("ci/keys.sh failed: " + r.stderr.strip()[-300:])
        text = r.stdout
    names = [ln.strip() for ln in text.splitlines() if ln.strip()]
    if not names:
        raise OrderError("ci/keys.sh printed no entry")
    for n in names:
        if not ENTRY.match(n):
            raise OrderError("not an entry name: %r" % n[:60])
    if len(set(names)) != len(names):
        raise OrderError("ci/keys.sh printed an entry twice")
    return names


class Releases:
    def __init__(self, base, repo, token):
        self.base, self.repo, self.token = base.rstrip("/"), repo, token

    def published(self, tag):
        url = "%s/repos/%s/releases/tags/%s" % (self.base, self.repo, urllib.parse.quote(tag))
        hdr = {"Accept": "application/vnd.github+json", "User-Agent": "trx64-order"}
        if self.token:
            hdr["Authorization"] = "Bearer " + self.token
        last = None
        for _ in range(3):
            try:
                with urllib.request.urlopen(urllib.request.Request(url, headers=hdr), timeout=30) as r:
                    return not json.load(r).get("draft", False)
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    return False
                last = "HTTP %d" % e.code
                if e.code < 500 and e.code != 429:
                    break
            except (urllib.error.URLError, OSError) as e:
                last = str(e)
        raise OrderError("%s: cannot read the release %s (%s)" % (self.repo, tag, last))


def main():
    plan = os.environ.get("PLAN_JSON_FILE", "")
    out = os.environ.get("WORK_ORDER_OUT", "")
    if not plan or not out:
        raise OrderError("PLAN_JSON_FILE and WORK_ORDER_OUT are needed")
    try:
        if json.load(open(plan)).get("version") != 1:
            raise ValueError("version")
    except (OSError, ValueError, AttributeError):
        raise OrderError("%s is no plan.json" % plan)
    sha = os.environ.get("ORDER_SHA") or subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    pkg = env_file(os.path.join(HERE, "pkg.env"))
    repo = os.environ.get("MC_BUILDS_REPO") or pkg["MC_BUILDS_REPO"]
    prefix = os.environ.get("BUILDS_TAG_PREFIX", pkg["BUILDS_TAG_PREFIX"])
    names = entry_names()
    rel = Releases(os.environ.get("BUILDS_API", "https://api.github.com"), repo, os.environ.get("GH_TOKEN", ""))
    missing = [n for n in names if not rel.published(prefix + "bit-" + n)]
    lanes = []
    for n in missing:
        ln = LANE[ENTRY.match(n).group(1)]
        if ln not in lanes:
            lanes.append(ln)
    with open(out, "w") as f:
        json.dump({"version": 1, "sha": sha, "lanes": lanes, "entries": missing}, f, indent=2)
    print("work order: %d of %d needed entries are missing: %s" % (len(missing), len(names), " ".join(missing) or "none"))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a") as f:
            f.write("missing=%s\n" % ("true" if missing else "false"))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OrderError as e:
        print("ci/order.py: %s" % e, file=sys.stderr)
        sys.exit(1)
