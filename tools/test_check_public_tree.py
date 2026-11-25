#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PROVENANCE: TRX64U-MultiCore tools/test_check_public_tree.py (t239.3 72e770261, last changed in 02ddd12b7; Alex Damhuis' code in both
# repositories), U20 A8. Changed here: ci/private.txt is checked against ci/roms.txt; the hook gets a GitHub URL, a Gitea push is skipped, the
# address rule and the session trailer are tested; no side lines.
"""Unit tests of tools/check_public_tree.py (U20 A8): its selftest, ci/private.txt against ci/roms.txt, tree mode on the git objects of a scratch
repository (a symlink, a planted path, a clean tree) and the pre-push hook run by hand with the arguments git passes."""
import os, shutil, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TOOL = os.path.join(HERE, "check_public_tree.py")
sys.path.insert(0, HERE)
import check_public_tree as cpt  # noqa: E402

GH = "git@github.com:owner/repo.git"


def run(*a, cwd=None, inp=None):
    return subprocess.run(a, cwd=cwd, input=inp, capture_output=True, text=True)


def scratch(td, files=("tools/check_public_tree.py", "ci/private.txt", "ci/roms.txt", ".githooks/pre-push")):
    for f in files:
        os.makedirs(os.path.dirname(os.path.join(td, f)), exist_ok=True)
        shutil.copy(os.path.join(ROOT, f), os.path.join(td, f))
    open(os.path.join(td, "README.md"), "w").write("clean\n")


class T(unittest.TestCase):
    def test_selftest(self):
        r = run(sys.executable, "-I", TOOL, "--selftest")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_private_list_is_the_pinned_roms(self):
        files, wins = cpt.load_private()
        listed = sorted((sha, size, name) for size, v in files.items() for sha, name in v)
        self.assertEqual(listed, sorted(cpt.pinned_roms()), "ci/private.txt: run --gen-private after a change of ci/roms.txt")
        self.assertEqual(sum(len(v) for v in wins.values()), cpt.NWIN * len(listed))
        for ln in open(cpt.PRIVATE):
            f = ln.split()
            if f and f[0] == "win":
                self.assertTrue(len(f[1]) == 8 and len(f[2]) == 64, "window lines hold hashes only")

    def test_no_value_list(self):
        src = open(TOOL).read()
        for word in ("HASHED", "gen-hashes", "add_argument(\"--side\""):
            self.assertNotIn(word, src)

    def test_symlinks_tree_rev_and_commits(self):
        td = tempfile.mkdtemp()
        try:
            g = lambda *a: run("git", "-c", "user.name=t", "-c", "user.email=t@example.org", *a, cwd=td)
            scratch(td)
            g("init", "-q"); g("add", "."); g("commit", "-q", "-m", "clean")
            clean = g("rev-parse", "HEAD").stdout.strip()
            tool = lambda *a: run("python3", "-I", "tools/check_public_tree.py", *a, cwd=td)
            self.assertEqual(tool().returncode, 0)
            os.symlink("README.md", os.path.join(td, "ok_link")); g("add", "ok_link")
            self.assertEqual(tool().returncode, 0)
            g("commit", "-q", "-m", "clean link")
            for target, why in (("/Users/jdoe/notes", "home path"), ("roms/x.bin", "private file type")):
                os.symlink(target, os.path.join(td, "bad_link")); g("add", "bad_link")
                r = tool(); self.assertEqual(r.returncode, 1, r.stdout); self.assertIn("PUBLIC-TREE: bad_link:", r.stdout); self.assertIn(why, r.stdout)
                g("commit", "-q", "-m", "link")
                head = g("rev-parse", "HEAD").stdout.strip()
                r = tool("--rev", head); self.assertEqual(r.returncode, 1, r.stdout); self.assertIn("bad_link", r.stdout)
                r = tool("--commits", clean + ".." + head); self.assertEqual(r.returncode, 1, r.stdout); self.assertIn("bad_link", r.stdout)
                g("reset", "-q", "--hard", clean); os.path.lexists(os.path.join(td, "bad_link")) and os.unlink(os.path.join(td, "bad_link"))
        finally:
            shutil.rmtree(td, ignore_errors=True)

    def test_scratch_repo_and_hook(self):
        td = tempfile.mkdtemp()
        try:
            def g(*a, mail="t@example.org"):
                return run("git", "-c", "user.name=t", "-c", "user.email=" + mail, *a, cwd=td)
            scratch(td)
            g("init", "-q"); g("add", "."); g("commit", "-q", "-m", "clean")
            clean = g("rev-parse", "HEAD").stdout.strip()
            zero = "0" * 40
            hook = lambda sha, url=GH: run("bash", os.path.join(td, ".githooks/pre-push"), "origin", url, cwd=td,
                                           inp="refs/heads/b %s refs/heads/b %s\n" % (sha, zero))
            h = hook(clean); self.assertEqual(h.returncode, 0, h.stdout + h.stderr)
            # a planted private path
            os.makedirs(os.path.join(td, "roms")); open(os.path.join(td, "roms/x.bin"), "w").write("x")
            g("add", "-f", "."); g("commit", "-q", "-m", "planted path")
            r = hook(g("rev-parse", "HEAD").stdout.strip())
            self.assertEqual(r.returncode, 1); self.assertIn("PUBLIC-TREE: roms/x.bin", r.stdout)
            # a push to another remote (Gitea) is not checked
            r = hook(g("rev-parse", "HEAD").stdout.strip(), "http://gitea.example/owner/repo.git")
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr); self.assertIn("no GitHub remote", r.stdout)
            g("reset", "-q", "--hard", clean)
            # a private file added in one pushed commit and removed by the next: the head's tree is clean, the history is not
            os.makedirs(os.path.join(td, "roms")); open(os.path.join(td, "roms/y.bin"), "w").write("y")
            g("add", "-f", "."); g("commit", "-q", "-m", "adds a private file")
            g("rm", "-q", "-r", "roms"); g("commit", "-q", "-m", "removes it again")
            head = g("rev-parse", "HEAD").stdout.strip()
            self.assertEqual(run("python3", "-I", "tools/check_public_tree.py", "--rev", head, cwd=td).returncode, 0)
            r = hook(head)
            self.assertEqual(r.returncode, 1); self.assertIn("PUBLIC-TREE: commit", r.stdout); self.assertIn("roms/y.bin", r.stdout)
            g("reset", "-q", "--hard", clean)
            # this repository's attribution trailer passes; an internal host in the message does not
            open(os.path.join(td, "m"), "w").write("m"); g("add", "m")
            g("commit", "-q", "-m", "x\n\nCo-Authored-By: Claude <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01abc")
            r = hook(g("rev-parse", "HEAD").stdout.strip()); self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            open(os.path.join(td, "m"), "w").write("n"); g("add", "m"); g("commit", "-q", "-m", "built on devhost.lan")
            r = hook(g("rev-parse", "HEAD").stdout.strip()); self.assertEqual(r.returncode, 1); self.assertIn("internal host in the message", r.stdout)
            g("reset", "-q", "--hard", clean)
            # a personal address as author: refused by the hook (--authors), not by the messages of CI
            open(os.path.join(td, "m"), "w").write("p"); g("add", "m"); g("commit", "-q", "-m", "plain", mail="jdoe@gmail.com")
            head = g("rev-parse", "HEAD").stdout.strip()
            r = hook(head); self.assertEqual(r.returncode, 1); self.assertIn("as author or committer", r.stdout)
            r = run("python3", "-I", "tools/check_public_tree.py", "--messages", clean + ".." + head, cwd=td)
            self.assertEqual(r.returncode, 0, r.stdout)
        finally:
            shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
