#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host test for tools/release_notes.py: a throw-away superproject with a nested app repo,
one tagged release, a merge, a direct commit, a docs commit and two app commits."""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import release_notes as R  # noqa: E402

ENV = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t",
           GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")


def g(repo, *a):
    r = subprocess.run(["git", "-C", repo] + list(a), capture_output=True, text=True, env=ENV)
    assert r.returncode == 0, (a, r.stderr)
    return r.stdout.strip()


TODO = """| ID | M | Status | Title | Open | Details |
|---|---|---|---|---|---|
| T01 | 1 | ✅ | Done thing | – | – |
| T02 | 1 | 🔨 | Open thing | Board test with [link](x.md) | – |
| T03 | 2 | ⏸ | Later milestone | x | – |
| T04 | 1 | ✖ | Dropped | – | – |
| T05 | 1 | ⏸ | Not built | Spec first | – |
"""
SPEC = """# spec
## 6. The two paths
### 6.1 (a) First

1. Copy `trx64-<id>.cfw` to a stick.

### 6.2 (b) Second

Stick, YES.

### 6.3 Not touched

ESP32 stays.

### 6.4 Backup and power-off

A file TRXBK000.BIN.

## 7. Memory
"""


class T(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        r = self.r = self.d
        g(r, "init", "-q", "-b", "main")
        app = os.path.join(r, R.APP_SUBMODULE)
        os.makedirs(app)
        g(app, "init", "-q", "-b", "main")
        os.makedirs(os.path.join(r, "docs/specs"))
        open(os.path.join(r, "docs/specs/T49-update-packages.md"), "w").write(SPEC)
        open(os.path.join(r, "TODO.md"), "w").write(TODO)
        self.app_commit("a1")
        self.old_app = g(app, "rev-parse", "HEAD")
        self.pin(self.old_app)
        g(r, "add", "TODO.md", "docs")
        g(r, "commit", "-q", "-m", "T01 first")
        g(r, "tag", "v0.1")
        self.app_commit("A2 app change")
        self.app_commit("A3 app change two")
        self.new_app = g(app, "rev-parse", "HEAD")
        g(r, "checkout", "-q", "-b", "feat")
        open(os.path.join(r, "f"), "w").write("1")
        g(r, "add", "f")
        g(r, "commit", "-q", "-m", "T20 feature work")
        g(r, "checkout", "-q", "main")
        g(r, "merge", "-q", "--no-ff", "-m", "Merge feat (T20 ring)", "feat")
        open(os.path.join(r, "h"), "w").write("1")
        g(r, "add", "h")
        g(r, "commit", "-q", "-m", "Fix the thing")
        self.pin(self.new_app)
        g(r, "commit", "-q", "-m", "Docs: tidy [skip ci]")

    def app_commit(self, msg):
        app = os.path.join(self.r, R.APP_SUBMODULE)
        open(os.path.join(app, "x"), "a").write(msg)
        g(app, "add", "x")
        g(app, "commit", "-q", "-m", msg)

    def pin(self, sha):
        g(self.r, "update-index", "--add", "--cacheinfo", "160000,%s,%s" % (sha, R.APP_SUBMODULE))

    def run_notes(self, **kw):
        class A:
            pass
        a = A()
        a.__dict__.update(dict(id="main-12345678", date="2026-10-08", repo=self.r, ref="HEAD", base=None, manifest=None,
                               reports=None, run_url="http://ci/run/1", apps="trxmon,unzip", excluded="esp_probe",
                               spec="docs/specs/T49-update-packages.md", todo="TODO.md"))
        a.__dict__.update(kw)
        return R.build(a)

    def test_notes(self):
        n = self.run_notes()
        self.assertIn("# TRX64 Ultimate release main-12345678", n)
        self.assertIn("previous release tag v0.1", n)
        self.assertIn("Merge feat (**T20** ring)", n)
        self.assertIn("Fix the thing", n)
        self.assertIn("Documentation and bookkeeping", n)
        self.assertNotIn("T01 first", n.split("## Known open issues")[0].split("### Ultimate app")[0])
        self.assertIn("A2 app change", n)
        self.assertIn("A3 app change two", n)
        self.assertIn("`trx64_update_<commit>_<YYYYMMDD>.cfw`", n)   # id name replaced in the spec text
        self.assertRegex(n, r"`trx64_update_12345678_\d{8}\.cfw`")
        self.assertIn("TRXBK000.BIN", n)
        self.assertNotIn("ESP32 stays", n)                 # 6.3 is not an install step
        self.assertIn("- `trxmon.u2a`: user app", n)
        self.assertIn("`esp_probe`", n)
        iss = n.split("## Known open issues")[1]
        self.assertIn("**T02** Open thing: Board test with link", iss)
        self.assertIn("**T05**", iss)
        for no in ("T01", "T03", "T04"):
            self.assertNotIn(no, iss)

    def test_no_release_and_manifest(self):
        g(self.r, "tag", "-d", "v0.1")
        m = os.path.join(self.d, "m.json")
        json.dump({"trx64_commit": "a" * 40, "app_commit": self.new_app, "wns_ns": {"wns": "0.123"},
                   "gateware_key": "k1", "bitstream_from": "cache x"}, open(m, "w"))
        n = self.run_notes(manifest=m)
        self.assertIn("No previous release and no base given", n)
        self.assertIn("wns 0.123", n)
        self.assertIn("`" + "a" * 40 + "`", n)
        self.assertIn("No previous app commit", n)

    def test_utilization(self):
        rep = os.path.join(self.d, "rep")
        os.makedirs(rep)
        with open(os.path.join(rep, "c64_basic.utilization.rpt"), "w") as fh:
            fh.write("| Slice LUTs*                | 26019 |     0 |          0 | 32600 | 79.81 |\n"
                     "|   LUT as Logic             | 25000 |     0 |          0 | 32600 | 76.69 |\n")
        n = self.run_notes(reports=rep)
        self.assertIn("c64_basic.utilization.rpt: 26019 of 32600 LUTs (79.81 %)", n)

    def test_base_given_and_unchanged_app(self):
        n = self.run_notes(base=g(self.r, "rev-parse", "HEAD"))
        self.assertIn("the given base", n)
        self.assertIn("Unchanged", n)


if __name__ == "__main__":
    unittest.main()
