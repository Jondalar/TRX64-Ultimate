#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/gh_export.py: the exclusions, the placeholders, the rewrite
rules, the checks that stop an export, the public-tree scrub of files, messages
and addresses (U20 A8) and the one rewrite of a clone (U20 D15). Touches neither
repository; only the rewrite tests run git, in a scratch repository."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gh_export as gx  # noqa: E402


def write(root, rel, text=""):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w") as fh:
        fh.write(text)
    return p


class StageTree(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-export-test-")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_excluded_paths_and_directories_go(self):
        write(self.d, "TODO.md", "the task list\n")
        write(self.d, "roms/kernal.901227-03.bin", "rom bytes")
        write(self.d, "roms/README.md", "where they come from\n")
        write(self.d, "docs/contracts/handover.md", "the two projects\n")
        write(self.d, "README.md", "stays\n")
        write(self.d, "docs/.DS_Store", "junk")
        dropped = gx.drop_excluded(self.d)

        # since 2026-11-13 the task list and the working documents go out (Alex)
        self.assertTrue(os.path.exists(os.path.join(self.d, "TODO.md")))
        self.assertTrue(os.path.isdir(os.path.join(self.d, "docs/contracts")))
        # since U20 A6 the ROM images stay out and the tree's roms/README.md goes out
        self.assertFalse(os.path.exists(os.path.join(self.d, "roms/kernal.901227-03.bin")))
        self.assertTrue(os.path.exists(os.path.join(self.d, "roms/README.md")))
        self.assertFalse(os.path.exists(os.path.join(self.d, "docs/.DS_Store")))
        self.assertTrue(os.path.exists(os.path.join(self.d, "README.md")))
        self.assertIn("roms/kernal.901227-03.bin", dropped)

    def test_the_pin_constraints_go_out_as_they_are(self):
        # U20 D14: constraints/c64u.xdc is public, no link stands in its place
        write(self.d, "constraints/c64u.xdc", "pins")
        gx.drop_excluded(self.d)
        gx.write_placeholders(self.d)

        with open(os.path.join(self.d, "constraints/c64u.xdc")) as fh:
            self.assertEqual("pins", fh.read())
        self.assertFalse(os.path.exists(os.path.join(self.d, "constraints/c64u.xdc.md")))

    def test_a_rewrite_applies_once(self):
        write(self.d, "BUILD.md",
              "# Build\n\nWhat comes next and in which order: [TODO.md](TODO.md).\n\nmore\n")
        rules = [{"path": "BUILD.md",
                  "old": "What comes next and in which order: [TODO.md](TODO.md).\n",
                  "new": ""}]
        errors = self._with_rules(rules)
        self.assertEqual([], errors)
        with open(os.path.join(self.d, "BUILD.md")) as fh:
            self.assertNotIn("TODO.md", fh.read())

    def test_a_rewrite_that_no_longer_fits_is_an_error(self):
        write(self.d, "BUILD.md", "# Build\n\nthe line was reworded\n")
        rules = [{"path": "BUILD.md", "old": "the old line\n", "new": ""}]
        errors = self._with_rules(rules)
        self.assertEqual(1, len(errors))
        self.assertIn("matches 0 times", errors[0])

    def test_a_rewrite_for_a_file_that_is_not_exported_is_an_error(self):
        rules = [{"path": "docs/gone.md", "old": "x", "new": "y"}]
        errors = self._with_rules(rules)
        self.assertEqual(1, len(errors))
        self.assertIn("not in the export", errors[0])

    def test_a_rewrite_must_be_unambiguous(self):
        write(self.d, "a.md", "twice\ntwice\n")
        errors = self._with_rules([{"path": "a.md", "old": "twice\n", "new": "once\n"}])
        self.assertEqual(1, len(errors))
        self.assertIn("matches 2 times", errors[0])

    def _with_rules(self, rules):
        keep = gx.REWRITES
        gx.REWRITES = rules
        try:
            return gx.apply_rewrites(self.d)
        finally:
            gx.REWRITES = keep


class Submodule(unittest.TestCase):
    """Since U20 A5 a ref with a submodule is refused; the fake answers git ls-tree."""

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-export-test-")
        self.real_run = gx.run

    def tearDown(self):
        gx.run = self.real_run
        shutil.rmtree(self.d, ignore_errors=True)

    def test_a_ref_with_a_gitlink_is_refused(self):
        gx.run = lambda args, **kw: ("100644 blob 1111111111111111111111111111111111111111\tREADME.md\n"
                                     "160000 commit 0103c58581ae900c9b97ea739a2140a52adfb010\tsoftware/x")
        with self.assertRaises(gx.Fail) as cm:
            gx.stage("old", self.d)
        self.assertIn("software/x", str(cm.exception))
        self.assertIn("92645aeb", str(cm.exception))

    def test_gitlinks_lists_only_gitlinks(self):
        gx.run = lambda args, **kw: "100644 blob 1111111111111111111111111111111111111111\tREADME.md"
        self.assertEqual([], gx.gitlinks("HEAD"))


class Checks(unittest.TestCase):
    """The link rules against a table of our own (EXCLUDE as it was: roms and the pin file)."""

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-export-test-")
        self.real_exclude = gx.EXCLUDE
        gx.EXCLUDE = ["roms", "constraints/c64u.xdc"]

    def tearDown(self):
        gx.EXCLUDE = self.real_exclude
        shutil.rmtree(self.d, ignore_errors=True)

    def test_a_link_to_an_excluded_file_loses_its_link_and_keeps_its_text(self):
        write(self.d, "docs/manual/10-differences.md",
              "see [the constraints](../../constraints/c64u.xdc) and "
              "[0055](0055.md)\n")
        write(self.d, "docs/manual/0055.md", "there\n")
        stripped = gx.strip_links_to_excluded(self.d)

        with open(os.path.join(self.d, "docs/manual/10-differences.md")) as fh:
            out = fh.read()
        self.assertIn("see the constraints and [0055](0055.md)", out)
        self.assertEqual(1, len(stripped))
        self.assertEqual([], gx.scan_links(self.d))

    def test_stripping_leaves_links_to_exported_files_alone(self):
        write(self.d, "docs/a.md", "[b](b.md) [ROMs](../roms)\n")
        write(self.d, "docs/b.md", "b\n")
        gx.strip_links_to_excluded(self.d)
        with open(os.path.join(self.d, "docs/a.md")) as fh:
            self.assertEqual("[b](b.md) ROMs\n", fh.read())

    def test_a_link_to_an_excluded_file_is_found(self):
        write(self.d, "BUILD.md", "see [TODO.md](TODO.md)\n")
        write(self.d, "docs/a.md", "see [b](b.md)\n")
        write(self.d, "docs/b.md", "here\n")
        bad = gx.scan_links(self.d)
        self.assertEqual(1, len(bad))
        self.assertIn("TODO.md", bad[0])

    def test_links_up_out_of_the_tree_and_urls_are_left_alone(self):
        write(self.d, "docs/a.md",
              "[x](https://example.invalid/y) [up](../../outside.md) [anchor](#here) "
              "[spec](specs/s.md#part)\n")
        write(self.d, "docs/specs/s.md", "s\n")
        self.assertEqual([], gx.scan_links(self.d))

    def test_a_new_binary_is_found_against_the_last_export(self):
        clone = tempfile.mkdtemp(prefix="gh-export-clone-")
        try:
            write(clone, "tests/known.prg", "old")
            write(self.d, "tests/known.prg", "old")
            write(self.d, "roms/secret.bin", "rom bytes")
            write(self.d, "docs/manual/01.md", "text")
            new = gx.scan_new_binaries(self.d, clone)
            self.assertEqual(["roms/secret.bin"], new)
        finally:
            shutil.rmtree(clone, ignore_errors=True)


class Sync(unittest.TestCase):
    def test_sync_adds_replaces_and_removes(self):
        stage = tempfile.mkdtemp(prefix="gh-export-stage-")
        clone = tempfile.mkdtemp(prefix="gh-export-clone-")
        try:
            write(clone, ".git/config", "keep me")
            write(clone, "docs/old.md", "gone after the sync")
            write(clone, "README.md", "old text")
            write(stage, "README.md", "new text")
            write(stage, "docs/new.md", "added")
            gx.sync(stage, clone)

            with open(os.path.join(clone, "README.md")) as fh:
                self.assertEqual("new text", fh.read())
            self.assertTrue(os.path.exists(os.path.join(clone, "docs/new.md")))
            self.assertFalse(os.path.exists(os.path.join(clone, "docs/old.md")))
            self.assertTrue(os.path.exists(os.path.join(clone, ".git/config")))
        finally:
            shutil.rmtree(stage, ignore_errors=True)
            shutil.rmtree(clone, ignore_errors=True)


class Tables(unittest.TestCase):
    """The rules in gh_export.py have to agree with each other."""

    def test_no_rewrite_rule_points_at_an_excluded_path(self):
        for rule in gx.REWRITES:
            self.assertNotIn(rule["path"], gx.EXCLUDE, rule["path"])

    def test_every_rewrite_rule_has_a_file_in_the_repository(self):
        for rule in gx.REWRITES:
            self.assertTrue(os.path.exists(os.path.join(gx.HOST_REPO, rule["path"])),
                            "%s is gone; the rule has to go too" % rule["path"])

    def test_every_rewrite_rule_still_finds_its_text(self):
        for rule in gx.REWRITES:
            with open(os.path.join(gx.HOST_REPO, rule["path"])) as fh:
                s = fh.read()
            self.assertEqual(rule.get("count", 1), s.count(rule["old"]),
                             "%s: %r" % (rule["path"], rule["old"][:60]))

    def test_only_third_party_binaries_stay_out(self):
        # Alex, 2026-11-13: specs, ADRs, the task list and the other project's name go out; U20 D14: the pin file too
        self.assertNotIn("constraints/c64u.xdc", gx.EXCLUDE)
        self.assertNotIn("constraints/c64u.xdc.md", gx.PLACEHOLDERS)
        for rel in gx.EXCLUDE:
            self.assertIn(os.path.splitext(rel)[1].lower(), gx.BINARY_SUFFIXES, rel)

    def test_no_rom_image_goes_out_and_the_trees_rom_readme_does(self):
        self.assertIn("roms/*.bin", gx.EXCLUDE_GLOBS)
        self.assertNotIn("roms", gx.EXCLUDE)
        self.assertNotIn("roms/README.md", gx.PLACEHOLDERS)
        self.assertTrue(os.path.exists(os.path.join(gx.HOST_REPO, "roms/README.md")))

    def test_the_placeholder_table_covers_every_dropped_single_file(self):
        for rel in gx.EXCLUDE:
            full = os.path.join(gx.HOST_REPO, rel)
            if not os.path.isfile(full):
                continue
            if rel.endswith(".md") or rel == "TODO.md":
                continue
            if os.path.splitext(rel)[1].lower() in gx.BINARY_SUFFIXES or rel.endswith(".xdc"):
                self.assertIn(rel + ".md", gx.PLACEHOLDERS, rel)


class Replay(unittest.TestCase):
    def test_the_export_line_follows_the_first_subject(self):
        m = gx.export_message(["U9: a load\n\nThe body.\n"], ["b2506f6"])
        self.assertEqual(m, "U9: a load\n\nExport of TRX64 Ultimate b2506f6.\n\nThe body.\n")

    def test_folded_messages_follow_each_other(self):
        m = gx.export_message(["T46: out\n\nwhy", "T46: the comments"], ["7a9c5bb", "8f4761e"])
        self.assertTrue(m.startswith("T46: out\n\nExport of TRX64 Ultimate 7a9c5bb, 8f4761e"))
        self.assertTrue(m.endswith("why\n\nT46: the comments\n"))

    def test_no_message_at_all_is_an_error(self):
        with self.assertRaises(gx.Fail):
            gx.export_message(["", "  "], ["8f4761e"])


class PublicAddress(unittest.TestCase):
    def test_the_own_address_goes_out_as_the_public_one(self):
        self.assertEqual(gx.public_email("Me@Example.org", "me@example.org "), gx.PUBLIC_EMAIL)

    def test_another_address_goes_out_as_it_is(self):
        self.assertEqual(gx.public_email("pr@example.net", "me@example.org"), "pr@example.net")

    def test_without_an_own_address_nothing_is_replaced(self):
        self.assertEqual(gx.public_email("me@example.org", ""), "me@example.org")

    def test_a_commit_in_a_clone_carries_the_public_address(self):
        env = gx.public_env()
        self.assertEqual((env["GIT_AUTHOR_EMAIL"], env["GIT_COMMITTER_EMAIL"]), (gx.PUBLIC_EMAIL,) * 2)

    def test_a_personal_address_stops_the_export(self):
        # U20 A8: an address the replay does not map (no git config user.email, a contributor's) must not go out as a personal one
        with self.assertRaises(gx.Fail):
            gx.checked_email("jdoe@gmail.com", "the author of x")
        self.assertEqual(gx.checked_email(gx.PUBLIC_EMAIL, "x"), gx.PUBLIC_EMAIL)
        self.assertEqual(gx.checked_email("pr@example.net", "x"), "pr@example.net")


class Scrub(unittest.TestCase):
    """U20 A8 / D15: what the public-tree check refuses, the export rewrites into placeholders; what it cannot rewrite stops it."""

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-export-test-")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_hits_become_placeholders(self):
        write(self.d, "tests/t.py", 'HOST = "10.1.2.189"\nsys.path.insert(0, "/Users/jdoe/tools")\n')
        write(self.d, "docs/a.md", "see http://ci.corp.lan/x and https://claude.ai/artifact/abc\n")
        write(self.d, "software/trx64u/base/menu.toml", 'default = "192.168.2.64"\n')
        write(self.d, ".gitea/workflows/lint.yml", "      volumes:\n        - /share/CACHEDEV2_DATA/trx64-build/cache:/cache\n")
        done = dict(gx.scrub_stage(self.d))
        with open(os.path.join(self.d, "tests/t.py")) as fh:
            self.assertEqual(fh.read(), 'HOST = "192.0.2.189"\nsys.path.insert(0, "~/tools")\n')
        with open(os.path.join(self.d, "docs/a.md")) as fh:
            self.assertEqual(fh.read(), "see http://ci.example/x and (a private page)\n")
        # the vendor default stays (an allowed pair); the cache volume the Gitea runners need goes out as the placeholder
        self.assertNotIn("software/trx64u/base/menu.toml", done)
        with open(os.path.join(self.d, ".gitea/workflows/lint.yml")) as fh:
            self.assertIn("/srv/trx64-build/cache:/cache", fh.read())
        self.assertEqual(gx.check_stage(self.d), [])

    def test_what_no_placeholder_fixes_stops_the_export(self):
        write(self.d, "tests/media/game.crt", "x")
        self.assertTrue(any("game.crt" in h for h in gx.check_stage(self.d)))

    def test_the_message_is_scrubbed_and_keeps_the_session_line(self):
        m = gx.export_message(["U9: on devhost.lan\n\nfrom /home/jdoe/x\n\nClaude-Session: https://claude.ai/code/session_01Ab\n"], ["b2506f6"])
        self.assertEqual(m, "U9: on devhost.example\n\nExport of TRX64 Ultimate b2506f6.\n\nfrom ~/x\n\n"
                            "Claude-Session: https://claude.ai/code/session_01Ab\n")


class Rewrite(unittest.TestCase):
    """U20 D15: the one rewrite of a clone's commits, in a scratch repository."""

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-rewrite-test-")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def git(self, *a, mail=gx.PUBLIC_EMAIL, date="1700000000 +0100"):
        env = dict(os.environ, GIT_AUTHOR_NAME="A", GIT_AUTHOR_EMAIL=mail, GIT_AUTHOR_DATE=date, GIT_COMMITTER_NAME="C",
                   GIT_COMMITTER_EMAIL=mail, GIT_COMMITTER_DATE=date)
        return subprocess.run(["git", "-C", self.d] + list(a), env=env, check=True, stdout=subprocess.PIPE, text=True).stdout.strip()

    def commit(self, files, msg, **kw):
        for rel, text in files.items():
            write(self.d, rel, text)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", msg, **kw)
        return self.git("rev-parse", "HEAD")

    def test_files_and_messages_are_rewritten_once(self):
        self.git("init", "-q", "-b", "main")
        # a blob of base's history is public already and stays as it is (the export's base, MultiCore D36)
        base = self.commit({"README.md": "public\n", "docs/old.md": "at /Users/jdoe/old\n"}, "base")
        write(self.d, "tools/x.sh", "#!/bin/sh\nssh admin@10.0.0.2 ls /share/CACHEDEV1_DATA\n")
        os.chmod(os.path.join(self.d, "tools/x.sh"), 0o755)
        self.commit({"docs/n.md": "clean\n"}, "first on devhost.lan\n\nClaude-Session: https://claude.ai/code/session_01x\n")
        old = self.commit({"docs/n.md": "at /Users/jdoe/x\n"}, "second", date="1700000100 +0100")
        new = gx.rewrite(base, self.d, private=os.path.join(self.d, "none.txt"))
        self.assertNotEqual(new, old)
        self.assertEqual(self.git("rev-parse", "main-before-rewrite"), old)
        self.assertEqual(self.git("rev-parse", "main~2"), base)
        self.assertEqual(self.git("show", "main:tools/x.sh"), "#!/bin/sh\nssh admin@192.0.2.2 ls /srv")
        self.assertEqual(self.git("show", "main:docs/n.md"), "at ~/x")
        self.assertEqual(self.git("show", "main~1:docs/n.md"), "clean")
        self.assertEqual(self.git("show", "main:docs/old.md"), "at /Users/jdoe/old")
        self.assertEqual(self.git("log", "-1", "--format=%B", "main~1"), "first on devhost.example\n\nClaude-Session: https://claude.ai/code/session_01x")
        self.assertEqual(self.git("log", "-1", "--format=%an %ae %ad %cn %cd", "--date=raw", "main"),
                         "A %s 1700000100 +0100 C 1700000100 +0100" % gx.PUBLIC_EMAIL)
        self.assertIn("100755", self.git("ls-tree", "main", "tools/x.sh"))
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_a_personal_address_stops_the_rewrite(self):
        self.git("init", "-q", "-b", "main")
        base = self.commit({"README.md": "public\n"}, "base")
        self.commit({"a.md": "x\n"}, "by hand", mail="jdoe@gmail.com")
        with self.assertRaises(gx.Fail):
            gx.rewrite(base, self.d, private=os.path.join(self.d, "none.txt"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
