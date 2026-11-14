#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ci/switch_registry.py on fixture trees: python -I tools/ci/tests/test_switch_registry.py

The --write-upstream test runs git in a temporary directory.
"""

import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
CI = os.path.dirname(HERE)
ROOT = Path(CI).parent.parent
sys.path.insert(0, CI)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))

import switch_registry  # noqa: E402
import yaml_subset  # noqa: E402

REGISTRY = """\
# Switch registry

## Experimental

| Switch | Values | What it does | Measured | Code |
|---|---|---|---|---|
| `EDEN_NX2_DOCUMENTED` | `1` | Something; see also `EDEN_NX2_ONLY_MENTIONED`. | Not measured. | `a.cpp` |
| `EDEN_NX2_PAIR_A`, `EDEN_NX2_PAIR_B` | `1` | Two switches in one row. | Not measured. | `a.cpp` |

## Removed experiments

| Switch | What it did | Why removed |
|---|---|---|
| `EDEN_NX2_GONE` | Nothing. | No gain. |
"""

UPSTREAM = """\
# A comment line, EDEN_NX2_NOT_A_NAME_HERE.
EDEN_NX2_UPSTREAM  # trailing comment
EDEN_DKB_FRAME_TRACE
"""

DOCUMENTED = """\
#include <cstdlib>
static const bool documented = std::getenv("EDEN_NX2_DOCUMENTED") != nullptr;
static const bool pair_a = std::getenv("EDEN_NX2_PAIR_A") != nullptr;
static const bool pair_b = std::getenv("EDEN_NX2_PAIR_B") != nullptr;
static const bool upstream = std::getenv("EDEN_NX2_UPSTREAM") != nullptr;
static const bool trace = std::getenv("EDEN_DKB_FRAME_TRACE") != nullptr;
"""


class Tree:
    """A fixture repository root in a temporary directory."""

    def __init__(self, files, registry=REGISTRY, upstream=UPSTREAM):
        self.temp = tempfile.TemporaryDirectory()
        self.root = self.temp.name
        files = dict(files)
        files["docs/switches.md"] = registry
        if upstream is not None:
            files["tools/ci/switch-registry-upstream.txt"] = upstream
        for path, text in files.items():
            full = Path(self.root, path)
            full.parent.mkdir(parents=True, exist_ok=True)
            full.write_text(text, encoding="utf-8", newline="")

    def run(self, annotate=False):
        out = io.StringIO()
        code = switch_registry.report(switch_registry.check(self.root), out, annotate=annotate)
        return code, out.getvalue()

    def close(self):
        self.temp.cleanup()


class CheckTests(unittest.TestCase):
    def tree(self, files, **kwargs):
        tree = Tree(files, **kwargs)
        self.addCleanup(tree.close)
        return tree

    def test_documented_and_upstream_switches_pass(self):
        code, out = self.tree({"src/a.cpp": DOCUMENTED}).run()
        self.assertEqual(code, 0, out)
        self.assertIn("Switches used in src/ and launcher/: 5; with a row in docs/switches.md: 3; "
                      "upstream's without a row: 2; missing a row: 0.", out)
        self.assertNotIn("error:", out)

    def test_a_new_undocumented_switch_fails_with_its_name_file_and_line(self):
        source = DOCUMENTED + 'static const char* example = std::getenv("EDEN_NX2_EXAMPLE_TEST");\n'
        code, out = self.tree({"src/video_core/a.cpp": source}).run()
        self.assertEqual(code, 1)
        self.assertIn("error: EDEN_NX2_EXAMPLE_TEST is used at src/video_core/a.cpp:7, but "
                      "docs/switches.md has no row for it.", out)
        self.assertIn("missing a row: 1.", out)

    def test_adding_the_row_makes_it_pass(self):
        source = DOCUMENTED + 'static const char* example = std::getenv("EDEN_NX2_EXAMPLE_TEST");\n'
        registry = REGISTRY.replace(
            "\n## Removed", "| `EDEN_NX2_EXAMPLE_TEST` | `1` | Example. | Not measured. | `a.cpp` |\n\n## Removed", 1)
        code, out = self.tree({"src/a.cpp": source}, registry=registry).run()
        self.assertEqual(code, 0, out)

    def test_annotations_point_at_the_read(self):
        source = DOCUMENTED + 'auto x = std::getenv("EDEN_NX2_EXAMPLE_TEST");\n'
        _, out = self.tree({"src/a.cpp": source}).run(annotate=True)
        self.assertIn("::error file=src/a.cpp,line=7::EDEN_NX2_EXAMPLE_TEST is used at src/a.cpp:7", out)

    def test_switches_read_through_helpers_are_found(self):
        source = DOCUMENTED + """\
static bool EnvOn(const char* name) { const char* v = std::getenv(name); return v && *v != '0'; }
static const bool hitch = EnvOn("EDEN_NX2_HELPER_ON");
static const u64 frames = number("EDEN_NX2_HELPER_NUMBER", 600);
static const bool fast = Settings::EnvironmentFlag<"EDEN_NX2_HELPER_TEMPLATE">();
for (const auto& [name, target] : {std::tuple{"EDEN_NX2_HELPER_TUPLE", &expected}}) {}
constexpr const char* kName = "EDEN_NX2_HELPER_CONSTANT";
const char* which = mesh ? "EDEN_NX2_HELPER_TERNARY_A" : "EDEN_NX2_HELPER_TERNARY_B";
"""
        code, out = self.tree({"src/common/b.cpp": source}).run()
        self.assertEqual(code, 1)
        for name, line in (("HELPER_ON", 8), ("HELPER_NUMBER", 9), ("HELPER_TEMPLATE", 10),
                           ("HELPER_TUPLE", 11), ("HELPER_CONSTANT", 12), ("HELPER_TERNARY_A", 13),
                           ("HELPER_TERNARY_B", 13)):
            self.assertIn(f"error: EDEN_NX2_{name} is used at src/common/b.cpp:{line},", out)

    def test_the_launcher_and_setters_count_as_uses(self):
        rust = """\
fn launch(cmd: &mut Command) {
    // EDEN_NX2_IN_A_COMMENT
    cmd.env("EDEN_NX2_LAUNCHER_SET", "1");
    let s: &'static str = r#"EDEN_NX2_RAW_RUST"#;
}
"""
        cpp = DOCUMENTED + 'setenv("EDEN_NX2_CPP_SET", "1", 0);\n'
        code, out = self.tree({"launcher/src-tauri/src/launch.rs": rust, "src/main.cpp": cpp}).run()
        self.assertEqual(code, 1)
        self.assertIn("error: EDEN_NX2_LAUNCHER_SET is set at launcher/src-tauri/src/launch.rs:3,", out)
        self.assertIn("error: EDEN_NX2_RAW_RUST is used at launcher/src-tauri/src/launch.rs:4,", out)
        self.assertIn("error: EDEN_NX2_CPP_SET is set at src/main.cpp:7,", out)
        self.assertNotIn("EDEN_NX2_IN_A_COMMENT", out)

    def test_comments_and_strings_that_only_mention_a_name_are_not_uses(self):
        source = DOCUMENTED + """\
// EDEN_NX2_LINE_COMMENT=1 turns it on, std::getenv("EDEN_NX2_IN_LINE_COMMENT")
/* std::getenv("EDEN_NX2_IN_BLOCK_COMMENT");
   still a comment */
/// EDEN_NX2_DOC_COMMENT
LOG_WARNING(Render_Vulkan, "EDEN_NX2_IN_LOG: bad value \\"{}\\"", value);
const auto line = "EDEN_NX2_NAME_VALUE=1\\n";
const char* url = "http://example.invalid/"; auto after = std::getenv("EDEN_NX2_AFTER_URL");
const char c = '"'; auto after_char = std::getenv("EDEN_NX2_AFTER_CHAR");
const char* raw = R"x(// not a comment "EDEN_NX2_IN_RAW_TEXT")x"; auto r = std::getenv("EDEN_NX2_AFTER_RAW");
const int million = 1'000'000; auto d = std::getenv("EDEN_NX2_AFTER_DIGITS");
"""
        code, out = self.tree({"src/c.cpp": source}).run()
        for name in ("LINE_COMMENT", "IN_LINE_COMMENT", "IN_BLOCK_COMMENT", "DOC_COMMENT", "IN_LOG",
                     "NAME_VALUE", "IN_RAW_TEXT"):
            self.assertNotIn(f"EDEN_NX2_{name}", out)
        for name, line in (("AFTER_URL", 13), ("AFTER_CHAR", 14), ("AFTER_RAW", 15), ("AFTER_DIGITS", 16)):
            self.assertIn(f"error: EDEN_NX2_{name} is used at src/c.cpp:{line},", out)
        self.assertEqual(code, 1)

    def test_typescript_single_quotes_and_templates_are_strings(self):
        ts = """\
const single = process.env['EDEN_NX2_TS_SINGLE'];
const quote = 'a"b'; const after = process.env["EDEN_NX2_AFTER_SQ"];
const built = process.env[`EDEN_NX2_${name}`];
"""
        code, out = self.tree({"src/a.cpp": DOCUMENTED, "launcher/ui/src/a.ts": ts}).run()
        self.assertEqual(code, 1)
        self.assertIn("error: EDEN_NX2_TS_SINGLE is used at launcher/ui/src/a.ts:1,", out)
        self.assertIn("error: EDEN_NX2_AFTER_SQ is used at launcher/ui/src/a.ts:2,", out)
        self.assertIn('error: launcher/ui/src/a.ts:3: "EDEN_NX2_${name}" looks like a switch name built '
                      "at run time", out)

    def test_a_name_outside_every_literal_fails(self):
        ts = "spawn(exe, { env: { EDEN_NX2_TS_KEY: '1' } });\n"
        code, out = self.tree({"src/a.cpp": DOCUMENTED, "launcher/ui/src/a.ts": ts}).run(annotate=True)
        self.assertEqual(code, 1)
        self.assertIn("error: launcher/ui/src/a.ts:1: EDEN_NX2_TS_KEY appears outside a string literal; "
                      "this check cannot classify it.", out)
        self.assertIn("::error file=launcher/ui/src/a.ts,line=1::", out)

    def test_putenv_sets_and_names_running_on_fail(self):
        source = DOCUMENTED + 'putenv("EDEN_NX2_PUTENV=1");\nauto f = std::getenv("EDEN_NX2_Foo");\n'
        code, out = self.tree({"src/a.cpp": source}).run()
        self.assertEqual(code, 1)
        self.assertIn("error: EDEN_NX2_PUTENV is set at src/a.cpp:7,", out)
        self.assertIn('error: src/a.cpp:8: "EDEN_NX2_Foo" looks like a switch name built at run time', out)

    def test_files_outside_src_and_launcher_and_build_output_are_not_scanned(self):
        read = 'auto x = std::getenv("EDEN_NX2_ELSEWHERE");\n'
        code, out = self.tree({
            "src/a.cpp": DOCUMENTED,
            "tools/nx2/x.cpp": read,
            "launcher/ui/node_modules/pkg/index.js": 'process.env["EDEN_NX2_ELSEWHERE"];\n',
            "launcher/src-tauri/target/debug/x.rs": read,
            "src/notes.md": read,
        }).run()
        self.assertEqual(code, 0, out)

    def test_a_stale_row_is_a_warning_not_a_failure(self):
        source = DOCUMENTED.replace('std::getenv("EDEN_NX2_PAIR_B") != nullptr', "false")
        code, out = self.tree({"src/a.cpp": source}).run(annotate=True)
        self.assertEqual(code, 0, out)
        self.assertIn("warning: EDEN_NX2_PAIR_B has a row (docs/switches.md:8) but is used nowhere in "
                      "src/ or launcher/", out)
        self.assertIn("::warning file=docs/switches.md,line=8::EDEN_NX2_PAIR_B", out)
        self.assertIn("Rows whose switch is used nowhere: 1.", out)
        self.assertNotIn("EDEN_NX2_GONE", out)  # a removed experiment's row is not stale

    def test_only_the_first_cell_makes_a_row(self):
        source = DOCUMENTED + 'auto m = std::getenv("EDEN_NX2_ONLY_MENTIONED");\n'
        code, out = self.tree({"src/a.cpp": source}).run()
        self.assertEqual(code, 1)
        self.assertIn("error: EDEN_NX2_ONLY_MENTIONED is used at src/a.cpp:7,", out)

    def test_a_removed_experiment_read_again_fails_and_says_so(self):
        source = DOCUMENTED + 'auto g = std::getenv("EDEN_NX2_GONE");\n'
        code, out = self.tree({"src/a.cpp": source}).run()
        self.assertEqual(code, 1)
        self.assertIn('(docs/switches.md:14 lists it under "Removed experiments")', out)

    def test_names_built_at_run_time_fail_unless_listed(self):
        source = DOCUMENTED + """\
auto a = std::getenv((std::string("EDEN_NX2_FLUID_") + suffix).c_str());
auto b = std::getenv(fmt::format("EDEN_NX2_{}", suffix).c_str());
"""
        code, out = self.tree({"src/a.cpp": source}).run()
        self.assertEqual(code, 1)
        self.assertIn('error: src/a.cpp:7: "EDEN_NX2_FLUID_" looks like a switch name built at run time', out)
        self.assertIn('error: src/a.cpp:8: "EDEN_NX2_{}" looks like a switch name built at run time', out)
        listed = 'return !(name.starts_with("EDEN_NX2_") || name == "EDEN_DKB_FRAME_TRACE");\n'
        code, out = self.tree({"src/a.cpp": DOCUMENTED, "src/lichii/nx2_switches.h": listed}).run()
        self.assertEqual(code, 0, out)

    def test_without_the_upstream_list_every_name_needs_a_row(self):
        code, out = self.tree({"src/a.cpp": DOCUMENTED}, upstream=None).run()
        self.assertEqual(code, 1)
        self.assertIn("error: EDEN_NX2_UPSTREAM is used at src/a.cpp:5,", out)
        self.assertIn("error: EDEN_DKB_FRAME_TRACE is used at src/a.cpp:6,", out)

    def test_upstream_names_no_longer_used_are_noted(self):
        source = DOCUMENTED.replace('std::getenv("EDEN_NX2_UPSTREAM") != nullptr', "false")
        code, out = self.tree({"src/a.cpp": source}).run()
        self.assertEqual(code, 0, out)
        self.assertIn("note: 1 names of tools/ci/switch-registry-upstream.txt are used nowhere any more: "
                      "EDEN_NX2_UPSTREAM", out)


def git(root, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    return subprocess.run(["git", "-C", root, *args], check=True, capture_output=True, text=True,
                          env=env).stdout.strip()


class WriteUpstreamTests(unittest.TestCase):
    def test_the_list_is_rebuilt_from_src_of_a_commit(self):
        with tempfile.TemporaryDirectory() as root:
            git(root, "init", "-q")
            Path(root, "src").mkdir()
            Path(root, "launcher").mkdir()
            Path(root, "src", "a.cpp").write_text(
                '// EDEN_NX2_COMMENTED\nauto b = std::getenv("EDEN_NX2_B");\nauto a = EnvOn("EDEN_NX2_A");\n',
                encoding="utf-8")
            Path(root, "launcher", "x.rs").write_text('cmd.env("EDEN_NX2_LAUNCHER", "1");\n', encoding="utf-8")
            git(root, "add", "-A")
            git(root, "commit", "-q", "-m", "fixture")
            commit = git(root, "rev-parse", "HEAD")
            Path(root, "src", "a.cpp").write_text('auto c = std::getenv("EDEN_NX2_C");\n', encoding="utf-8")
            Path(root, "tools", "ci").mkdir(parents=True)
            with contextlib.redirect_stdout(io.StringIO()):
                switch_registry.write_upstream(root, "HEAD")
            text = Path(root, "tools", "ci", "switch-registry-upstream.txt").read_text(encoding="utf-8")
        self.assertIn(f"at commit {commit}.", text)
        self.assertIn("--write-upstream", text)
        self.assertEqual(switch_registry.read_upstream(text), {"EDEN_NX2_A", "EDEN_NX2_B"})
        self.assertTrue(text.endswith("EDEN_NX2_A\nEDEN_NX2_B\n"))

    def test_upstream_ref_takes_a_commits_switches_as_upstreams_without_writing_the_list(self):
        with tempfile.TemporaryDirectory() as root:
            git(root, "init", "-q")
            Path(root, "src").mkdir()
            Path(root, "docs").mkdir()
            Path(root, "docs", "switches.md").write_text(REGISTRY, encoding="utf-8")
            Path(root, "src", "a.cpp").write_text('auto n = std::getenv("EDEN_NX2_NEW_UPSTREAM");\n',
                                                 encoding="utf-8")
            git(root, "add", "-A")
            git(root, "commit", "-q", "-m", "upstream adds a switch")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                failed = switch_registry.main(["--root", root])
                passed = switch_registry.main(["--root", root, "--upstream-ref", "HEAD"])
            listed = Path(root, "tools", "ci", "switch-registry-upstream.txt").exists()
        self.assertEqual((failed, passed), (1, 0), out.getvalue())
        self.assertIn("regenerate tools/ci/switch-registry-upstream.txt as its header says", out.getvalue())
        self.assertFalse(listed)


class RepositoryTests(unittest.TestCase):
    def test_the_checked_in_list_says_how_to_regenerate_it(self):
        text = (ROOT / "tools" / "ci" / "switch-registry-upstream.txt").read_text(encoding="utf-8")
        self.assertIn("python3 -I tools/ci/switch_registry.py --write-upstream", text)
        names = switch_registry.read_upstream(text)
        self.assertGreater(len(names), 100)
        self.assertTrue(all(switch_registry.NAME.fullmatch(name) for name in names))

    def test_the_checked_in_list_matches_the_commit_in_its_header(self):
        text = (ROOT / "tools" / "ci" / "switch-registry-upstream.txt").read_text(encoding="utf-8")
        commit = re.search(r"at commit ([0-9a-f]{40})\.", text).group(1)
        if subprocess.run(["git", "-C", str(ROOT), "cat-file", "-e", commit],
                          capture_output=True).returncode != 0:
            self.skipTest(f"{commit} is not in this clone")
        _, names = switch_registry.upstream_names(str(ROOT), commit)
        self.assertEqual(switch_registry.read_upstream(text), set(names))

    def test_the_workflow_runs_on_the_paths_it_checks_with_a_read_only_token(self):
        doc = yaml_subset.load((ROOT / ".github" / "workflows" / "switch-registry.yml").read_text(encoding="utf-8"))
        paths = doc["on"]["pull_request"]["paths"]
        for path in ("src/**", "launcher/**", "docs/switches.md", "tools/ci/switch_registry.py",
                     "tools/ci/switch-registry-upstream.txt", "tools/ci/tests/test_switch_registry.py",
                     "tools/ingest/tests/yaml_subset.py"):
            self.assertIn(path, paths)
        self.assertEqual(doc["permissions"], {})
        job = doc["jobs"]["check"]
        self.assertEqual(job["permissions"], {"contents": "read"})
        self.assertEqual(job["steps"][0]["with"]["persist-credentials"], "false")
        check = job["steps"][-1]
        # Upstream's live names count too, so an upstream merge needs no regenerated list first.
        # Read from upstream itself; the mirror branch, which can lag, is the fallback.
        self.assertIn("if ! git fetch --no-tags --depth=1 https://github.com/zortos293/Lichii nx2; then\n",
                      check["run"])
        self.assertIn("git fetch --no-tags --depth=1 origin upstream\n", check["run"])
        self.assertIn("python3 -I tools/ci/switch_registry.py --upstream-ref FETCH_HEAD", check["run"])
        self.assertNotIn("${{", check["run"])


if __name__ == "__main__":
    unittest.main()
