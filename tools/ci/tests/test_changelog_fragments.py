#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/changelog_fragments.py: python -I tools/ci/tests/test_changelog_fragments.py

The check tests run git in a temporary repository.
"""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.dirname(HERE)
ROOT = Path(CI).parent.parent
sys.path.insert(0, CI)  # -I leaves the script's directory off sys.path.

import changelog_fragments as cf  # noqa: E402

CHANGELOG = """\
# Changelog

Intro.

## [Unreleased]

### Added

- Old added line.

### Fixed

- Old fixed line,
  continued.

## [1.0.0] - 2026-01-01

### Added

- Released line.
"""


def run_main(*args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = cf.main(list(args))
    return code, out.getvalue(), err.getvalue()


class Tree:
    """A temporary repository: files written, then committed."""

    def __init__(self, test):
        self.path = Path(tempfile.mkdtemp())
        test.addCleanup(shutil.rmtree, str(self.path), True)
        self.git("init", "-q")

    def write(self, name, text, newline="\n"):
        path = self.path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.replace("\n", newline).encode("utf-8"))

    def remove(self, name):
        (self.path / name).unlink()

    def git(self, *args):
        result = subprocess.run(
            ["git", "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "-c", "commit.gpgsign=false",
             "-c", "core.autocrlf=false", "-C", str(self.path)] + list(args),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, check=True)
        return result.stdout.strip()

    def commit(self, message="change"):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)
        return self.git("rev-parse", "HEAD")


class FoldTests(unittest.TestCase):
    def fold(self, fragments, body=None):
        lines = cf.split_lines(CHANGELOG if body is None else body)
        span = cf.section_range(lines, "Unreleased")
        return cf.fold(lines[span[0]:span[1]], fragments)

    def test_without_fragments_the_body_is_unchanged(self):
        lines = cf.split_lines(CHANGELOG)
        span = cf.section_range(lines, "Unreleased")
        self.assertEqual(self.fold([]), lines[span[0]:span[1]])
        self.assertEqual(self.fold([]), ["", "### Added", "", "- Old added line.", "", "### Fixed", "",
                                         "- Old fixed line,", "  continued.", ""])

    def test_entries_go_after_the_lines_already_in_their_section(self):
        out = self.fold([("b", "added", ["- B."]), ("a", "added", ["- A one.", "  more."]), ("c", "fixed", ["- C."])])
        self.assertEqual(out, ["", "### Added", "", "- Old added line.", "- B.", "- A one.", "  more.", "",
                               "### Fixed", "", "- Old fixed line,", "  continued.", "- C.", ""])

    def test_a_missing_section_is_created_in_keep_a_changelog_order(self):
        out = self.fold([("x", "changed", ["- X."]), ("y", "security", ["- Y."]), ("z", "removed", ["- Z."])])
        headings = [line for line in out if line.startswith("### ")]
        self.assertEqual(headings, ["### Added", "### Changed", "### Removed", "### Fixed", "### Security"])
        self.assertEqual(out[out.index("### Changed") + 2], "- X.")

    def test_an_empty_section_takes_the_fragments(self):
        self.assertEqual(self.fold([("x", "added", ["- X."])], "## [Unreleased]\n\n## [1.0.0]\n"),
                         ["", "### Added", "", "- X.", ""])

    def test_text_before_the_first_heading_stays(self):
        out = self.fold([("x", "fixed", ["- X."])], "## [Unreleased]\n\nNote.\n\n### Added\n\n- A.\n")
        self.assertEqual(out[:4], ["", "Note.", "", "### Added"])
        self.assertIn("### Fixed", out)

    def test_a_heading_this_tool_does_not_know_keeps_its_place(self):
        out = self.fold([("x", "added", ["- X."])], "## [Unreleased]\n\n### Docs\n\n- D.\n")
        self.assertEqual([line for line in out if line.startswith("### ")], ["### Added", "### Docs"])


class ReadFragmentTests(unittest.TestCase):
    def read(self, files):
        tree = Tree(self)
        for name, text in files.items():
            tree.write(cf.FRAGMENT_DIR + "/" + name, text)
        return cf.read_fragments(str(tree.path))

    def test_good_fragments_are_sorted_by_file_name(self):
        found = self.read({"b-two.fixed.md": "- Two.\n", "a-one.added.md": "- One,\r\n  more.\r\n\r\n", "README.md": "How.\n", ".gitkeep": ""})
        self.assertEqual(found, [("a-one", "added", ["- One,", "  more."]), ("b-two", "fixed", ["- Two."])])

    def test_no_folder_is_no_fragments(self):
        self.assertEqual(cf.read_fragments(str(Tree(self).path)), [])

    def test_every_malformed_file_is_named(self):
        bad = {"Upper.added.md": "- A.\n", "no-section.md": "- A.\n", "x.fixd.md": "- A.\n", "two.dots.added.md": "- A.\n",
               "empty.added.md": "\n\n", "plain.added.md": "A line.\n", "head.fixed.md": "- A.\n## [1.0.0]\n"}
        tree = Tree(self)
        for name, text in bad.items():
            tree.write(cf.FRAGMENT_DIR + "/" + name, text)
        with self.assertRaises(cf.FragmentError) as raised:
            cf.read_fragments(str(tree.path))
        message = str(raised.exception)
        for name in bad:
            self.assertIn(cf.FRAGMENT_DIR + "/" + name, message)

    def test_not_utf8_is_refused(self):
        tree = Tree(self)
        (tree.path / cf.FRAGMENT_DIR).mkdir()
        (tree.path / cf.FRAGMENT_DIR / "x.added.md").write_bytes(b"- \xff\n")
        with self.assertRaises(cf.FragmentError):
            cf.read_fragments(str(tree.path))

    def test_the_fragments_of_this_repository_are_well_formed(self):
        cf.read_fragments(str(ROOT))


class CollectAndReleaseTests(unittest.TestCase):
    def tree(self, newline="\n"):
        tree = Tree(self)
        tree.write("CHANGELOG.md", CHANGELOG, newline)
        tree.write("changelog.d/new-one.added.md", "- New one.\n")
        tree.write("changelog.d/new-two.changed.md", "- New two.\n")
        return tree

    def test_collect_prints_the_section_with_the_fragments_and_changes_nothing(self):
        tree = self.tree()
        text = cf.collect_text(str(tree.path))
        self.assertIn("- Old added line.\n- New one.\n", text)
        self.assertIn("### Changed\n\n- New two.\n", text)
        self.assertNotIn("Released line", text)
        self.assertTrue((tree.path / "changelog.d" / "new-one.added.md").exists())
        self.assertEqual((tree.path / "CHANGELOG.md").read_text(encoding="utf-8"), CHANGELOG)

    def test_collect_without_fragments_is_the_section_as_it_is(self):
        tree = Tree(self)
        tree.write("CHANGELOG.md", CHANGELOG)
        lines = cf.split_lines(CHANGELOG)
        span = cf.section_range(lines, "Unreleased")
        self.assertEqual(cf.collect_text(str(tree.path)), "".join(line + "\n" for line in lines[span[0]:span[1]]))

    def test_collect_through_main_writes_the_text(self):
        tree = self.tree()
        code, out, err = run_main("--root", str(tree.path), "collect")
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(out, cf.collect_text(str(tree.path)))

    def test_a_changelog_without_the_heading_fails(self):
        tree = Tree(self)
        tree.write("CHANGELOG.md", "# Changelog\n")
        code, _, err = run_main("--root", str(tree.path), "collect")
        self.assertEqual(code, 1)
        self.assertIn("## [Unreleased]", err)

    def test_release_folds_deletes_and_leaves_the_released_section_alone(self):
        tree = self.tree()
        names = cf.release(str(tree.path))
        self.assertEqual(names, ["new-one.added.md", "new-two.changed.md"])
        self.assertEqual(os.listdir(str(tree.path / "changelog.d")), [])
        text = (tree.path / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertTrue(text.endswith("## [1.0.0] - 2026-01-01\n\n### Added\n\n- Released line.\n"))
        self.assertIn("- Old added line.\n- New one.\n\n### Changed\n\n- New two.\n\n### Fixed", text)

    def test_release_keeps_crlf(self):
        tree = self.tree("\r\n")
        cf.release(str(tree.path))
        raw = (tree.path / "CHANGELOG.md").read_bytes()
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"))
        self.assertIn(b"- New one.\r\n", raw)

    def test_release_into_a_version_section(self):
        tree = self.tree()
        cf.release(str(tree.path), "1.0.0")
        text = (tree.path / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertTrue(text.endswith("- Released line.\n- New one.\n\n### Changed\n\n- New two.\n\n"))
        self.assertIn("- Old added line.\n\n### Fixed", text)

    def test_release_without_fragments_touches_nothing(self):
        tree = Tree(self)
        tree.write("CHANGELOG.md", CHANGELOG)
        self.assertEqual(cf.release(str(tree.path)), [])
        code, out, err = run_main("--root", str(tree.path), "release")
        self.assertEqual((code, out), (0, ""))
        self.assertIn("folded 0", err)
        self.assertEqual((tree.path / "CHANGELOG.md").read_text(encoding="utf-8"), CHANGELOG)

    def test_a_malformed_fragment_stops_the_release(self):
        tree = self.tree()
        tree.write("changelog.d/bad.added.md", "no bullet\n")
        code, _, err = run_main("--root", str(tree.path), "release")
        self.assertEqual(code, 1)
        self.assertIn("changelog.d/bad.added.md", err)
        self.assertTrue((tree.path / "changelog.d" / "new-one.added.md").exists())
        self.assertEqual((tree.path / "CHANGELOG.md").read_text(encoding="utf-8"), CHANGELOG)

    @unittest.skipIf(sys.platform == "win32" or shutil.which("bash") is None, "needs bash")
    def test_changelog_section_sh_prints_the_fragments_for_unreleased_only(self):
        tree = self.tree()
        tools = tree.path / "tools" / "ci"
        tools.mkdir(parents=True)
        for name in ("changelog-section.sh", "changelog_fragments.py"):
            shutil.copy(str(ROOT / "tools" / "ci" / name), str(tools / name))
        unreleased = subprocess.run(["bash", "tools/ci/changelog-section.sh", "Unreleased"], cwd=str(tree.path),
                                    stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
        self.assertEqual(unreleased, cf.collect_text(str(tree.path)))
        released = subprocess.run(["bash", "tools/ci/changelog-section.sh", "1.0.0"], cwd=str(tree.path),
                                  stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
        self.assertEqual(released, "\n### Added\n\n- Released line.\n")


class CheckTests(unittest.TestCase):
    def base(self):
        tree = Tree(self)
        tree.write("CHANGELOG.md", CHANGELOG)
        tree.write("src/a.cpp", "int a;\n")
        tree.write("README.md", "Readme.\n")
        return tree, tree.commit("base")

    def check(self, tree, base):
        return cf.check(str(tree.path), base, "HEAD")

    def test_which_paths_need_an_entry(self):
        self.assertFalse(cf.needs_entry(["docs/a.md", "README.md", "site/x.html", "VERSION", "CHANGELOG.md",
                                         "changelog.d/x.fixed.md", ".github/ISSUE_TEMPLATE/bug.yml",
                                         "tools/bench/results/r.json"]))
        for path in ("src/a.cpp", ".github/workflows/x.yml", "tools/ci/a.py", "launcher/src/main.rs"):
            self.assertTrue(cf.needs_entry([path, "README.md"]), path)

    def test_added_ranges(self):
        diff = "diff --git a/x b/x\n@@ -3,0 +4,2 @@\n+a\n+b\n@@ -9 +11 @@\n-c\n+d\n@@ -20,2 +21,0 @@\n-e\n-f\n"
        self.assertEqual(cf.added_ranges(diff), [(4, 5), (11, 11)])

    def test_documentation_alone_needs_no_entry(self):
        tree, base = self.base()
        tree.write("README.md", "Readme 2.\n")
        tree.commit()
        self.assertIsNone(self.check(tree, base))

    def test_code_without_an_entry_fails_and_says_what_to_add(self):
        tree, base = self.base()
        tree.write("src/a.cpp", "int b;\n")
        tree.commit()
        problem = self.check(tree, base)
        self.assertIn("changelog.d/<short-name>.<section>.md", problem)
        code, _, err = run_main("--root", str(tree.path), "check", "--base", base)
        self.assertEqual(code, 1)
        self.assertIn("changelog.d/", err)

    def test_code_with_a_fragment_passes(self):
        tree, base = self.base()
        tree.write("src/a.cpp", "int b;\n")
        tree.write("changelog.d/fix-b.fixed.md", "- B.\n")
        tree.commit()
        self.assertIsNone(self.check(tree, base))
        code, out, _ = run_main("--root", str(tree.path), "check", "--base", base)
        self.assertEqual((code, out), (0, "changelog: ok\n"))

    def test_code_with_a_line_under_unreleased_passes(self):
        tree, base = self.base()
        tree.write("src/a.cpp", "int b;\n")
        tree.write("CHANGELOG.md", CHANGELOG.replace("- Old added line.\n", "- Old added line.\n- Mine.\n"))
        tree.commit()
        self.assertIsNone(self.check(tree, base))

    def test_a_line_in_a_released_section_does_not_count(self):
        tree, base = self.base()
        tree.write("src/a.cpp", "int b;\n")
        tree.write("CHANGELOG.md", CHANGELOG.replace("- Released line.\n", "- Released line.\n- Mine.\n"))
        tree.commit()
        self.assertIn("adds no changelog entry", self.check(tree, base))

    def test_a_path_with_an_accent_is_read_as_it_is(self):
        tree, base = self.base()
        tree.write("docs/café menu.md", "Docs.\n")
        tree.commit()
        self.assertIsNone(self.check(tree, base))

    def test_a_malformed_fragment_fails_even_for_documentation(self):
        tree, base = self.base()
        tree.write("changelog.d/oops.md", "- B.\n")
        tree.commit()
        self.assertIn("changelog.d/oops.md", self.check(tree, base))

    def test_a_deleted_fragment_is_not_an_entry(self):
        tree = Tree(self)
        tree.write("changelog.d/old.fixed.md", "- Old.\n")
        tree.write("src/a.cpp", "int a;\n")
        base = tree.commit("base")
        tree.remove("changelog.d/old.fixed.md")
        tree.write("src/a.cpp", "int b;\n")
        tree.commit()
        self.assertIn("adds no changelog entry", self.check(tree, base))


class WiringTests(unittest.TestCase):
    def read(self, *parts):
        return ROOT.joinpath(*parts).read_text(encoding="utf-8")

    def test_the_section_script_folds_the_fragments_for_unreleased(self):
        script = self.read("tools", "ci", "changelog-section.sh")
        self.assertIn("changelog_fragments.py", script)
        self.assertIn("Unreleased", script)

    def test_the_release_notes_and_bump_script_use_the_tool(self):
        self.assertIn("changelog_fragments.py", self.read("tools", "ci", "release-notes.sh"))
        bump = self.read("tools", "ci", "bump-version.sh")
        self.assertIn("changelog_fragments.py release", bump)
        self.assertIn('--into "$core"', bump)

    def test_build_check_runs_the_check_on_pull_requests(self):
        workflow = self.read(".github", "workflows", "build-check.yml")
        self.assertIn("tools/ci/changelog_fragments.py check --base", workflow)

    def test_the_nightly_notes_carry_the_fragments(self):
        workflow = self.read(".github", "workflows", "release-binaries.yml")
        self.assertIn("changelog_fragments.py collect", workflow)
        self.assertRegex(workflow, r"sparse-checkout: \|\s+tools/ci\s+tools/ingest\s+changelog\.d")


if __name__ == "__main__":
    unittest.main()
