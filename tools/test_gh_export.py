#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/gh_export.py: the exclusions, the placeholders, the rewrite
rules and the three checks that stop an export. Runs without git and without
touching either repository."""

import os
import shutil
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
        write(self.d, "docs/contracts/handover.md", "the two projects\n")
        write(self.d, "README.md", "stays\n")
        write(self.d, "docs/.DS_Store", "junk")
        write(self.d, gx.APP_PREFIX + "/software/u64/default_chars.bin", "chars")
        dropped = gx.drop_excluded(self.d)

        self.assertFalse(os.path.exists(os.path.join(self.d, "TODO.md")))
        self.assertFalse(os.path.isdir(os.path.join(self.d, "roms")))
        self.assertFalse(os.path.isdir(os.path.join(self.d, "docs/contracts")))
        self.assertFalse(os.path.exists(os.path.join(self.d, "docs/.DS_Store")))
        self.assertFalse(os.path.exists(
            os.path.join(self.d, gx.APP_PREFIX + "/software/u64/default_chars.bin")))
        self.assertTrue(os.path.exists(os.path.join(self.d, "README.md")))
        self.assertIn("roms/", dropped)

    def test_placeholders_replace_what_was_dropped(self):
        write(self.d, "roms/kernal.901227-03.bin", "rom bytes")
        write(self.d, "constraints/c64u.xdc", "pins")
        gx.drop_excluded(self.d)
        gx.write_placeholders(self.d)

        with open(os.path.join(self.d, "roms/README.md")) as fh:
            roms = fh.read()
        self.assertIn("No ROMs are in this repository", roms)
        self.assertIn("vice-emu.sourceforge.io", roms)
        self.assertTrue(os.path.exists(os.path.join(self.d, "constraints/c64u.xdc.md")))
        self.assertFalse(os.path.exists(os.path.join(self.d, "constraints/c64u.xdc")))

        name = gx.APP_PREFIX + "/software/ModPlayer_16k/module.bin.md"
        with open(os.path.join(self.d, name)) as fh:
            self.assertIn("software/ModPlayer_16k/module.bin", fh.read())

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


class SubmoduleNotes(unittest.TestCase):
    """write_submodule_notes reads git; the fake answers the two commands it runs."""

    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-export-test-")
        self.real_run = gx.run

    def tearDown(self):
        gx.run = self.real_run
        shutil.rmtree(self.d, ignore_errors=True)

    def test_an_empty_submodule_folder_gets_its_repository_and_commit(self):
        cfg = ('[submodule "software/lwip"]\n\tpath = software/lwip\n'
               '\turl = http://github.com/GideonZ/lwip\n')
        sha = "26a22151f4b7ebb2925523192edc83fa7f31cba9"

        def fake_run(args, **kw):
            if args[-1].endswith(":.gitmodules"):
                return cfg
            return "160000 commit %s\tsoftware/lwip" % sha

        gx.run = fake_run
        written = gx.write_submodule_notes(self.d, "d36dcf78")

        self.assertEqual([gx.APP_PREFIX + "/software/lwip/README.md"], written)
        with open(os.path.join(self.d, written[0])) as fh:
            note = fh.read()
        self.assertIn(sha, note)
        self.assertIn("https://github.com/GideonZ/lwip", note)
        self.assertNotIn("http://github.com", note)

    def test_a_path_that_is_not_a_gitlink_gets_nothing(self):
        def fake_run(args, **kw):
            if args[-1].endswith(":.gitmodules"):
                return '[submodule "x"]\n\tpath = x\n\turl = https://example.invalid/x\n'
            return "100644 blob 0000000000000000000000000000000000000000\tx"

        gx.run = fake_run
        self.assertEqual([], gx.write_submodule_notes(self.d, "d36dcf78"))


class Checks(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="gh-export-test-")

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_the_other_projects_name_is_found(self):
        write(self.d, "docs/specs/U5.md", "agreed with MultiCore on 2026-10-30\n")
        write(self.d, "docs/specs/U4.md", "nothing to see\n")
        hits = gx.scan_forbidden(self.d)
        self.assertEqual(1, len(hits))
        self.assertTrue(hits[0].startswith("docs/specs/U5.md:1:"))

    def test_the_name_in_a_source_comment_is_found_too(self):
        write(self.d, "software/x.cc", "// handed over to MultiCore\n")
        self.assertEqual(1, len(gx.scan_forbidden(self.d)))

    def test_lab_addresses_stay_allowed(self):
        write(self.d, "tools/load.sh", "scp x builder.fritz.box:/tmp\nping 192.168.1.9\n")
        self.assertEqual([], gx.scan_forbidden(self.d))

    def test_the_name_stays_in_the_application_sources(self):
        write(self.d, gx.APP_PREFIX + "/software/components/config.cc",
              "// Started by MultiCore's host the flash belongs to that host\n")
        self.assertEqual([], gx.scan_forbidden(self.d))

    def test_the_name_stays_in_the_readme(self):
        write(self.d, "README.md", "This firmware also runs as a core under MultiCore's host.\n")
        write(self.d, "docs/manual/README.md", "decoupling moves to MultiCore\n")
        hits = gx.scan_forbidden(self.d)
        self.assertEqual(1, len(hits))
        self.assertTrue(hits[0].startswith("docs/manual/README.md:1:"))

    def test_a_link_to_an_excluded_file_loses_its_link_and_keeps_its_text(self):
        write(self.d, "docs/manual/10-differences.md",
              "see [the contract](../contracts/trx64u-firmware-handover.md) and "
              "[0055](0055.md)\n")
        write(self.d, "docs/manual/0055.md", "there\n")
        stripped = gx.strip_links_to_excluded(self.d)

        with open(os.path.join(self.d, "docs/manual/10-differences.md")) as fh:
            out = fh.read()
        self.assertIn("see the contract and [0055](0055.md)", out)
        self.assertEqual(1, len(stripped))
        self.assertEqual([], gx.scan_links(self.d))

    def test_stripping_leaves_links_to_exported_files_alone(self):
        write(self.d, "docs/a.md", "[b](b.md) [TODO](../TODO.md)\n")
        write(self.d, "docs/b.md", "b\n")
        gx.strip_links_to_excluded(self.d)
        with open(os.path.join(self.d, "docs/a.md")) as fh:
            self.assertEqual("[b](b.md) TODO\n", fh.read())

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

    def test_the_script_excludes_itself_and_its_test(self):
        self.assertIn("tools/gh_export.py", gx.EXCLUDE)
        self.assertIn("tools/test_gh_export.py", gx.EXCLUDE)

    def test_the_dropped_rom_directories_get_a_readme(self):
        self.assertIn("roms", gx.EXCLUDE)
        self.assertIn("roms/README.md", gx.PLACEHOLDERS)
        self.assertIn(gx.APP_PREFIX + "/roms", gx.EXCLUDE)
        self.assertIn(gx.APP_PREFIX + "/roms/README.md", gx.PLACEHOLDERS)

    def test_the_placeholder_table_covers_every_dropped_single_file(self):
        for rel in gx.EXCLUDE:
            full = os.path.join(gx.HOST_REPO, rel)
            if not os.path.isfile(full):
                continue
            if rel.endswith(".md") or rel in ("TODO.md", ".gitmodules"):
                continue
            if os.path.splitext(rel)[1].lower() in gx.BINARY_SUFFIXES or rel.endswith(".xdc"):
                self.assertIn(rel + ".md", gx.PLACEHOLDERS, rel)


class Replay(unittest.TestCase):
    def test_a_message_with_an_entry_is_replaced(self):
        self.assertEqual(gx.message_for("8f4761ee0123", "the fix"), "")
        self.assertTrue(gx.message_for("7a9c5bb4abcd", "x").startswith("T46: "))
        self.assertEqual(gx.message_for("0123abcd", "its own"), "its own")

    def test_no_replacement_message_names_the_other_project(self):
        for text in gx.MESSAGES.values():
            self.assertEqual(gx.forbidden_in(text), [])

    def test_the_other_projects_name_in_a_message_is_found(self):
        self.assertTrue(gx.forbidden_in("as the " + "Multi" + "Core host does it"))
        self.assertEqual(gx.forbidden_in("as the host does it"), [])

    def test_the_export_line_follows_the_first_subject(self):
        m = gx.export_message(["U9: a load\n\nThe body.\n"], ["b2506f6"], "f3c8e991aa")
        self.assertEqual(m, "U9: a load\n\nExport of TRX64 Ultimate b2506f6 (application f3c8e991)."
                            "\n\nThe body.\n")

    def test_folded_messages_follow_each_other_and_an_empty_one_goes(self):
        m = gx.export_message(["T46: out\n\nwhy", ""], ["7a9c5bb", "8f4761e"], "fb3a7821")
        self.assertTrue(m.startswith("T46: out\n\nExport of TRX64 Ultimate 7a9c5bb, 8f4761e"))
        self.assertTrue(m.endswith("why\n"))

    def test_no_message_at_all_is_an_error(self):
        with self.assertRaises(gx.Fail):
            gx.export_message(["", "  "], ["8f4761e"], "fb3a7821")

if __name__ == "__main__":
    unittest.main(verbosity=2)
