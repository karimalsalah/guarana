#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ingest/ingest.py and issue_forms.py. No network: the GitHub API is faked, and
git runs against a bare repository in a temporary directory.

  python -I tools/ingest/tests/test_ingest.py
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(HERE))

import ingest  # noqa: E402
import issue_forms  # noqa: E402
from fakes import REPO, FakeGitHub, git, make_issue  # noqa: E402

FIXTURES = os.path.join(HERE, "fixtures")
VALIDATE = ingest.load_bench_validator()
# Fixed, so that these tests (which run before every publish) do not depend on AGENTS.md's table.
TITLES = {"0400486024DEC000": "The Legend of Zelda: Ocarina of Time", "0400C3F00006E000": "Mario Kart World"}


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as handle:
        return handle.read()


def parse(body, number=1, logins=("reporter",)):
    return ingest.parse_report(body, number, "2026-11-20", logins, TITLES, VALIDATE)


def results(text):
    return ingest.parse_results(text)


def replace_answer(body, label, answer):
    """body with the answer under "### label" replaced."""
    head, _, tail = body.partition(f"### {label}\n\n")
    _, _, after = tail.partition("\n\n### ")
    return f"{head}### {label}\n\n{answer}\n\n### {after}"


class FormTests(unittest.TestCase):
    def test_sections_split_only_on_known_labels(self):
        body = ("### Device\n\nASUS\n\n### Scene\n\nfield\n### Not a field\nstill the scene\n### Device\nalso the scene\n\n"
                "### Results\n\n_No response_\n\n### Anything else\n\n")
        sections = issue_forms.parse_form(body)
        self.assertEqual(sections["Device"], "ASUS")
        self.assertEqual(sections["Scene"], "field\n### Not a field\nstill the scene\n### Device\nalso the scene")
        self.assertNotIn("Results", sections)
        self.assertNotIn("Anything else", sections)

    def test_bug_report_build_under_the_new_and_the_old_label(self):
        for label in ("Guarana build", "Lichii build"):
            body = f"### {label}\n\nbuild 177\n\n### Log file\n\n_No response_\n"
            self.assertEqual(issue_forms.parse_form(body), {label: "build 177"})
            self.assertEqual(issue_forms.form_headings(body), {label, "Log file"})

    def test_crlf_bodies_parse_the_same(self):
        body = fixture("device-report-manual.md")
        self.assertEqual(issue_forms.parse_form(body.replace("\n", "\r\n")), issue_forms.parse_form(body))
        self.assertEqual(parse(body.replace("\n", "\r\n")), parse(body))

    def test_checkboxes(self):
        boxes = issue_forms.parse_checkboxes("- [X] First one\n- [ ] Second\n- [x] Third")
        self.assertEqual(boxes, {"first one": True, "second": False, "third": True})

    def test_fenced_blocks_and_unclosed_fences(self):
        blocks = issue_forms.fenced_blocks("a\n```json\n{\"a\": 1}\n```\ntext\n```\nopen to the end")
        self.assertEqual(blocks, [("json", '{"a": 1}\n'), ("", "open to the end")])
        self.assertEqual(issue_forms.fenced_blocks("```json\n```"), [("json", "")])


class ScrubTests(unittest.TestCase):
    KEEP = ["ASUS Zenbook S 14 (UX5406)", "32.0.101.9175", "Commit 1a2b3c4 of v20.0.2", "32 GB LPDDR5X-8400",
            "1280x720 window", "Intel Arc 140T / 140V (Arrow Lake H, Lunar Lake)", "EDEN_NX2_GM=11",
            "Windows 11 24H2 (26100.4202)", "AMD Ryzen Z1 / Z2", "Mesa 25.2.3", "the tools/bench scene oot-hyrule-field",
            "p50/p99 43/51 ms", "~26.7 fps", "display 1920x1080@60", "Best performance, Turbo, 28 W",
            "Legion Go 2 (83N0)", "on/off", "fps/watt"]
    REMOVE = [
        ("C:\\Users\\karim\\AppData\\Roaming\\lichii\\log", "<path>", "karim"),
        ("save in D:/Games/zelda/oot.nup", "<path>", "zelda"),
        ("/home/karim/.local/share/lichii", "<path>", "karim"),
        ("from ~/.config/lichii/qt-config.ini", "<path>", "config"),
        ("%APPDATA%\\lichii\\log", "<path>", "APPDATA"),
        ("C:\\Users\\karim\\AppData\\Roaming\\guarana\\log", "<path>", "karim"),
        ("%APPDATA%\\guarana\\log", "<path>", "APPDATA"),
        ("from ~/.config/guarana/qt-config.ini", "<path>", "config"),
        ("mail karim@example.com", "<email>", "example"),
        ("MAC 00:1A:2B:3C:4D:5E", "<mac>", "4D:5E"),
        ("IP 192.168.1.20", "<ip>", "192.168"),
        ("host DESKTOP-4UD89UT", "<host>", "4UD89UT"),
        ("S/N: N7AXCV09F123456", "<serial>", "N7AXCV09F123456"),
        ("serial number 5CD1234XYZ", "<serial>", "5CD1234XYZ"),
        ("Service Tag: 7XKJ3Q2", "<serial>", "7XKJ3Q2"),
        ("account id 0123456789abcdef", "<id>", "0123456789abcdef"),
        ("machine 123e4567-e89b-12d3-a456-426614174000", "<id>", "426614174000"),
        ("thanks @karim-dev!", "<user>", "karim-dev"),
        ("clip at https://example.com/clip?u=karim, ok", "<link>", "example.com"),
    ]

    def test_model_numbers_versions_and_hashes_stay(self):
        for text in self.KEEP:
            self.assertEqual(issue_forms.scrub(text), text)

    def test_personal_data_goes(self):
        for text, token, secret in self.REMOVE:
            out = issue_forms.scrub(text)
            self.assertIn(token, out, text)
            self.assertNotIn(secret, out, text)

    def test_the_reporters_login_goes_unless_it_is_a_common_word(self):
        self.assertEqual(issue_forms.scrub("ask karimtester or KarimTester", ["karimtester"]), "ask <user> or <user>")
        self.assertEqual(issue_forms.scrub("Intel Arc 140T", ["intel", "arc"]), "Intel Arc 140T")

    def test_comment_text_is_neutralised(self):
        out = issue_forms.md_escape("`code` @karim <script>alert(1)</script> | **bold** [x](y)")
        for raw in ("`code`", "@karim", "<script>", "| **", "[x]"):
            self.assertNotIn(raw, out)
        self.assertIn("@\u200bkarim", out)

    def test_one_line_strips_markup(self):
        text = issue_forms.one_line("- **Resolution** 1x\n- `EDEN_NX2_GM=11`\n<!-- hidden -->\n![shot](https://x.test/a.png)", 200)
        self.assertEqual(text, "Resolution 1x; EDEN_NX2_GM=11")
        self.assertEqual(issue_forms.one_line("word " * 40, 30), "word word word word word\u2026")


class ResultsTests(unittest.TestCase):
    def check(self, text, fps, p50=None, p99=None, baseline=None, approximate=False):
        parsed, problem = results(text)
        self.assertIsNone(problem, text)
        self.assertEqual((parsed["fps"], parsed["p50_ms"], parsed["p99_ms"], parsed["approximate"]),
                         (fps, p50, p99, approximate), text)
        self.assertEqual(parsed["baseline"] and parsed["baseline"]["fps"], baseline, text)
        return parsed

    def test_the_forms_own_placeholder(self):
        parsed = self.check("Baseline 14.8 fps, p99 96 ms. With EDEN_NX2_GM=11: 22.8 fps, p99 51 ms. "
                            "Three 15 s windows each, tools/bench.", 22.8, p99=51.0, baseline=14.8)
        self.assertEqual(parsed["baseline"]["p99_ms"], 96.0)

    def test_one_figure_with_frame_times(self):
        self.check("22.8 fps, p50 43.8 ms, p99 51 ms", 22.8, 43.8, 51.0)
        self.check("FPS: 30, p50 33.3 ms", 30.0, 33.3)
        self.check("Average 22.8fps; 51 ms p99", 22.8, p99=51.0)
        self.check("p50/p99 43/51 ms at 22.8 fps", 22.8, 43.0, 51.0)
        self.check("Max settings, docked, 1280x720, 30 fps", 30.0)
        self.check("Stable 30,5 fps", 30.5)

    def test_lows_peaks_and_caps_are_not_the_result(self):
        self.check("Average 22.8 fps, 1% low 18 fps, p99 70 ms", 22.8, p99=70.0)
        self.check("Runs 25 fps, drops to 18 fps in Kakariko", 25.0)
        self.check("The game targets 30 fps and I get 27 fps", 27.0)
        self.check("27 fps (40 fps max)", 27.0)

    def test_baseline_and_change(self):
        self.check("14.75 \u2192 22.83 fps", 22.83, baseline=14.75)
        self.check("14.75 -> 22.83 fps", 22.83, baseline=14.75)
        self.check("from 14.8 to 22.8 fps with the fix", 22.8, baseline=14.8)
        self.check("22.8 fps without the switch, 30.1 fps with it", 30.1, baseline=22.8)
        self.check("With GM=11: 22.8 fps (baseline 14.8 fps)", 22.8, baseline=14.8)
        self.check("Before: 14.8 fps, p99 96 ms; after: 22.8 fps, p99 51 ms", 22.8, p99=51.0, baseline=14.8)

    def test_ranges_and_approximate_figures(self):
        self.check("22-24 fps in gameplay", 23.0, approximate=True)
        self.check("~26.7 fps", 26.7, approximate=True)
        self.check("about 30 fps", 30.0, approximate=True)

    def test_anything_ambiguous_is_a_problem(self):
        for text in ("22.8 fps on the first lap, 30 fps in the second lap", "14.8, 22.8 fps", "30/60 fps",
                     "20 fps, 25 fps, 30 fps", "runs fine", "p50 33 ms, p99 50 ms", "drops to 18 fps",
                     "EDEN_NX2_GM=11 helps"):
            parsed, problem = results(text)
            self.assertIsNone(parsed, text)
            self.assertTrue(problem, text)


class ReportTests(unittest.TestCase):
    def test_a_manual_report(self):
        record, problems, warnings = parse(fixture("device-report-manual.md"), 7)
        self.assertEqual(problems, [])
        self.assertEqual(warnings, [])
        expected = {
            "issue": 7, "submitted": "2026-11-20", "device": "ASUS Zenbook S 14 (UX5406)", "form_factor": "Laptop",
            "cpu": "Intel Core Ultra 9 285H", "gpu": "Intel Arc 140T / 140V", "gpu_vendor": "intel",
            "gpu_type": "integrated", "driver": "32.0.101.9175", "build": "v20.0.2",
            "game": "The Legend of Zelda: Ocarina of Time", "game_id": "oot", "power_source": "Plugged in",
            "scene": "Hyrule Field, the tools/bench scene oot-hyrule-field; docked; 1280x720 window",
            "settings": "Resolution 1x, frame generation off. EDEN_NX2_GM=11", "switches": ["EDEN_NX2_GM=11"],
            "frame_generation": "off", "fps": 22.8, "p50_ms": None, "p99_ms": 51.0,
            "baseline": {"fps": 14.8, "p50_ms": None, "p99_ms": 96.0}, "source": "manual", "scene_ok": None,
            "bench": None, "runs": [], "screenshot": True,
        }
        for key, value in expected.items():
            self.assertEqual(record[key], value, key)

    def test_a_bench_report_takes_the_newest_valid_baseline_row(self):
        record, problems, warnings = parse(fixture("device-report-bench.md"), 12)
        self.assertEqual(problems, [])
        self.assertEqual((record["source"], record["scene_ok"], record["fps"], record["p50_ms"], record["p99_ms"]),
                         ("tools/bench", True, 14.75, 66.9, 96.4))
        self.assertEqual(record["bench"], {"variant": "baseline", "machine_id": "6bfb7b55b5", "measured": "2026-11-19",
                                           "window_s": 15.0, "rows": 4, "valid_rows": 2})
        self.assertEqual([run["variant"] for run in record["runs"]], ["baseline", "gm11"])
        self.assertEqual(record["runs"][1]["switches"], ["EDEN_NX2_GM=11"])
        self.assertEqual(record["gpu"], "Intel Arc 140T GPU")
        self.assertEqual(record["game"], "The Legend of Zelda: Ocarina of Time")
        self.assertIn("wrong_scene", warnings[0])
        self.assertIn("probe", warnings[0])
        # The highest fps in the paste is the probe's 31.2 and the wrong scene's 30.1: neither is listed.
        self.assertNotIn(31.2, [run["fps"] for run in record["runs"]])

    def test_rows_that_are_all_invalid_keep_the_report_off(self):
        body = fixture("device-report-bench.md")
        lines = body.split("\n")
        only_invalid = "\n".join(line for line in lines if not line.startswith("{") or '"valid": false' in line)
        record, problems, _ = parse(only_invalid)
        self.assertIsNone(record)
        self.assertIn("None of the 1 tools/bench row(s) can be listed", problems[0])
        self.assertIn("wrong_scene", problems[0])

    def test_a_row_edited_by_hand_does_not_validate(self):
        body = fixture("device-report-bench.md")
        rows = [line for line in body.split("\n") if line.startswith("{") and '"variant": "gm11"' in line]
        edited = rows[0].replace('"fps": 22.83', '"fps": 29.5')
        record, problems, _ = parse(replace_answer(body, "tools/bench rows", "```json\n" + edited + "\n```"))
        self.assertIsNone(record)
        self.assertIn("median", problems[0])

    def test_a_row_with_raw_unicode_keeps_its_machine_id(self):
        # bench.py escapes non-ASCII, but a row pretty-printed by another tool has "™" as is.
        body = fixture("device-report-bench.md")
        row = json.loads(next(line for line in body.split("\n") if line.startswith("{") and '"gm11"' in line))
        row["hardware"]["gpus"][0]["name"] = "AMD Radeon™ 890M Graphics"
        row["machine_id"] = VALIDATE.__globals__["machine_id"](row["hardware"])
        self.assertEqual(VALIDATE(row), [])
        pasted = "```json\n" + json.dumps(row, indent=2, ensure_ascii=False) + "\n```"
        record, problems, _ = parse(replace_answer(body, "tools/bench rows", pasted))
        self.assertEqual(problems, [])
        self.assertEqual((record["scene_ok"], record["fps"]), (True, 22.83))

    def test_long_names_in_rows_are_cut(self):
        body = fixture("device-report-bench.md")
        line = next(line for line in body.split("\n") if line.startswith("{") and '"gm11"' in line)
        row = json.loads(line)
        row["variant_env"] = {"EDEN_NX2_" + "X" * 200: "1" * 300}
        record, problems, _ = parse(replace_answer(body, "tools/bench rows", "```json\n" + json.dumps(row) + "\n```"))
        self.assertEqual(problems, [])
        self.assertLessEqual(max(len(item) for item in record["switches"]), 141)

    def test_rows_pasted_pretty_printed_or_in_another_answer_are_found(self):
        body = fixture("device-report-bench.md")
        row = json.loads(next(line for line in body.split("\n") if line.startswith("{") and '"gm11"' in line))
        pretty = "```json\n" + json.dumps(row, indent=2) + "\n```"
        moved = replace_answer(replace_answer(body, "tools/bench rows", "_No response_"), "Anything else", pretty)
        record, problems, _ = parse(moved)
        self.assertEqual(problems, [])
        self.assertEqual((record["fps"], record["switches"]), (22.83, ["EDEN_NX2_GM=11"]))

    def test_an_invalid_report_lists_every_problem(self):
        record, problems, _ = parse(fixture("device-report-invalid.md"))
        self.assertIsNone(record)
        joined = "\n".join(problems)
        self.assertIn('Tick "Nothing I attach contains personal data"', joined)
        self.assertIn('The "GPU driver" answer is missing.', joined)
        self.assertIn("does not say which one is the baseline", joined)
        self.assertIn("screenshot", joined)
        self.assertEqual(len(problems), 4)

    def test_personal_data_never_reaches_the_record(self):
        record, problems, _ = parse(fixture("device-report-personal.md"), logins=("karimtester",))
        self.assertEqual(problems, [])
        dumped = json.dumps(record, ensure_ascii=False)
        for secret in ("karimtester", "Users", "/home/", "N7AXCV09F123456", "example.com", "192.168", "00:1A",
                       "DESKTOP-", "4UD89UT", "0123456789abcdef", "@", "https:", "imgur"):
            self.assertNotIn(secret, dumped, secret)
        self.assertEqual(record["build"], "nightly 2026-11-20 (1a2b3c4)")
        self.assertEqual(record["switches"], ["EDEN_NX2_DEBUG_DIR=<path>", "EDEN_NX2_VRS=1"])
        self.assertEqual((record["fps"], record["fps_approximate"], record["p50_ms"], record["p99_ms"]),
                         (30.0, True, 33.4, 48.0))
        self.assertEqual(record["driver"], "Adrenalin 25.9.1")
        self.assertEqual((record["gpu_vendor"], record["form_factor"]), ("amd", "Handheld"))
        self.assertIn("Measured on battery.", record["notes"])
        self.assertIn("Frame generation was on: game fps counts only the frames the game renders.", record["notes"])

    def test_options_must_be_the_forms(self):
        body = replace_answer(fixture("device-report-manual.md"), "Form factor", "Tablet")
        record, problems, _ = parse(body)
        self.assertIsNone(record)
        self.assertIn('"Form factor" is not one of the form\'s options', problems[0])

    def test_numbers_outside_the_ranges_are_problems(self):
        body = fixture("device-report-manual.md")
        record, problems, _ = parse(replace_answer(body, "Results", "900 fps"))
        self.assertIsNone(record)
        self.assertIn("The fps 900 is outside", problems[0])
        record, problems, _ = parse(replace_answer(body, "Results", "22 fps, p50 45 ms, p99 30 ms"))
        self.assertIn("below the p50", problems[0])

    def test_a_bench_row_of_another_game_wins_with_a_warning(self):
        body = replace_answer(fixture("device-report-bench.md"), "Game", "Mario Kart World")
        record, problems, warnings = parse(body)
        self.assertEqual(problems, [])
        self.assertEqual(record["game"], "The Legend of Zelda: Ocarina of Time")
        self.assertTrue(any("the form says \"Mario Kart World\"" in warning for warning in warnings))

    def test_issue_states_and_labels(self):
        body = fixture("device-report-manual.md")
        self.assertTrue(ingest.parse_issue(make_issue(1, body), TITLES, VALIDATE).listed)
        self.assertTrue(ingest.parse_issue(make_issue(1, body, state="closed", state_reason="completed"),
                                           TITLES, VALIDATE).listed)
        for issue in (make_issue(1, body, state="closed", state_reason="not_planned"),
                      make_issue(1, body, state="closed", state_reason="duplicate"),
                      make_issue(1, body, labels=("needs-triage",))):
            report = ingest.parse_issue(issue, TITLES, VALIDATE)
            self.assertTrue(report.excluded)
            self.assertFalse(report.listed)

    def test_an_unreadable_body_is_a_problem_not_a_crash(self):
        for body in (None, "", "no form at all", "### Device\n\n" + "{" * 5000):
            report = ingest.parse_issue(make_issue(3, body), TITLES, VALIDATE)
            self.assertFalse(report.listed)
            self.assertTrue(report.problems)

    def test_the_bench_validator_is_the_one_ci_runs(self):
        self.assertIsNotNone(VALIDATE)
        self.assertEqual(VALIDATE.__module__, "guarana_bench")

    def test_program_titles_come_from_the_agents_md_table(self):
        with tempfile.TemporaryDirectory() as root:
            with open(os.path.join(root, "AGENTS.md"), "w", encoding="utf-8") as handle:
                handle.write("| Program id | Game |\n|---|---|\n| 0400486024DEC000 | The Legend of Zelda: Ocarina of Time |\n"
                             "| 0400c3f00006e000 | Mario Kart World |\n")
            self.assertEqual(ingest.program_titles(root), {"0400486024DEC000": "The Legend of Zelda: Ocarina of Time",
                                                           "0400C3F00006E000": "Mario Kart World"})
            self.assertEqual(ingest.program_titles(os.path.join(root, "missing")), {})


class DocumentTests(unittest.TestCase):
    def test_numeric_order_lf_and_a_date_that_moves_only_with_the_records(self):
        record = parse(fixture("device-report-manual.md"))[0]
        records = {"9": record, "10": record}
        reports = [ingest.Report(10, record), ingest.Report(9, record)]
        self.assertEqual(list(ingest.records_of(reports)), ["9", "10"])
        first = ingest.make_document(records, REPO, None, today="2026-11-20")
        again = ingest.make_document(records, REPO, first, today="2026-11-30")
        self.assertEqual(again["updated"], "2026-11-20")
        changed = ingest.make_document({"9": record}, REPO, first, today="2026-11-30")
        self.assertEqual(changed["updated"], "2026-11-30")
        text = ingest.dump_document(first)
        self.assertTrue(text.endswith("}\n"))
        self.assertNotIn("\r", text)
        self.assertEqual(json.loads(text)["issue_url"], "https://github.com/karimalsalah/guarana/issues/{issue}")

    def test_the_published_file_can_be_read(self):
        # Only what publish needs: it runs these tests on the default branch, whose file may hold
        # any reports and an older layout that the run is about to rewrite. tests/test_site.py
        # checks the full layout on pull requests.
        with open(os.path.join(ingest.ROOT, ingest.DATA_PATH), encoding="utf-8") as handle:
            doc = ingest.load_document(handle.read())
        self.assertIsInstance(doc["reports"], dict)
        self.assertEqual(ingest.load_document("not json")["reports"], {})


class Origin:
    """A bare repository whose main holds site/data/community.json, and a clone with it as origin."""

    def __init__(self, root, doc_text):
        self.bare = os.path.join(root, "origin.git")
        self.work = os.path.join(root, "work")
        git("init", "-q", "--bare", "-b", "main", self.bare)
        git("init", "-q", "-b", "main", self.work)
        git("remote", "add", "origin", self.bare, cwd=self.work)
        empty = git("mktree", cwd=self.work, data=b"")
        seed = git("commit-tree", empty, "-m", "seed", cwd=self.work)
        self.set_main(doc_text, seed)

    def set_main(self, doc_text, parent=None):
        parent = parent or self.ref("main")
        commit = ingest.Git(self.work).commit_file(parent, ingest.DATA_PATH, doc_text, "data")
        git("push", "-q", "-f", "origin", f"{commit}:refs/heads/main", cwd=self.work)
        return commit

    def ref(self, name):
        out = git("ls-remote", self.bare, f"refs/heads/{name}")
        return out.split()[0] if out else None

    def show(self, ref):
        return git("show", f"{ref}:{ingest.DATA_PATH}", cwd=self.bare)

    def parents(self, ref):
        return ingest.Git(self.bare).parents(ref)


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.origin = Origin(self.temp.name, ingest.dump_document(ingest.make_document({}, REPO, today="2026-11-13")))
        self.github = FakeGitHub([
            make_issue(7, fixture("device-report-manual.md")),
            make_issue(12, fixture("device-report-bench.md")),
            make_issue(15, fixture("device-report-invalid.md")),
        ])
        self.log = []

    def publish(self, git_client=None, **kwargs):
        kwargs.setdefault("today", "2026-11-21")
        return ingest.publish(self.github, git_client or ingest.Git(self.origin.work), REPO, "main", titles=TITLES,
                              validate=VALIDATE, log=self.log.append, **kwargs)

    def branch_doc(self):
        return json.loads(self.origin.show(ingest.BRANCH))

    def test_the_first_run_opens_the_pull_request(self):
        summary = self.publish()
        self.assertEqual((summary["listed"], summary["problems"], summary["pushed"], summary["pull"]),
                         (2, [15], True, 500))
        doc = self.branch_doc()
        self.assertEqual(list(doc["reports"]), ["7", "12"])
        self.assertEqual((doc["count"], doc["updated"]), (2, "2026-11-21"))
        self.assertEqual(self.origin.parents(ingest.BRANCH), [self.origin.ref("main")])
        pull = self.github.pulls[500]
        self.assertEqual((pull["title"], pull["head"]["ref"], pull["base"]["ref"]),
                         (ingest.PULL_TITLE, ingest.BRANCH, "main"))
        for text in (ingest.PR_MARKER, "| #7 | added |", "| #12 | added |", "verified scene (tools/bench)",
                     "Left out until their problems are fixed", "#15", "close and reopen it"):
            self.assertIn(text, pull["body"])

    def test_a_second_run_changes_nothing(self):
        self.publish()
        head = self.origin.ref(ingest.BRANCH)
        writes = len(self.github.writes())
        summary = self.publish(today="2026-11-22")
        self.assertFalse(summary["pushed"])
        self.assertEqual(self.origin.ref(ingest.BRANCH), head)
        self.assertEqual(len(self.github.writes()), writes)

    def test_an_edit_makes_one_new_commit_on_main(self):
        self.publish()
        body = self.github.issues[7]["body"].replace("22.8 fps, p99 51 ms", "23.4 fps, p99 50 ms")
        self.github.issues[7]["body"] = body
        summary = self.publish()
        self.assertTrue(summary["pushed"])
        self.assertEqual(self.branch_doc()["reports"]["7"]["fps"], 23.4)
        self.assertEqual(self.origin.parents(ingest.BRANCH), [self.origin.ref("main")])
        self.assertIn("14.8 \u2192 23.4", self.github.pulls[500]["body"])

    def test_a_branch_moved_by_someone_else_is_recomputed_not_overwritten_blindly(self):
        origin = self.origin

        class RacingGit(ingest.Git):
            raced = False

            def push(self, commit, branch, expected):
                if not RacingGit.raced:
                    RacingGit.raced = True
                    other = ingest.Git(origin.work).commit_file(origin.ref("main"), ingest.DATA_PATH, "{}\n", "other")
                    git("push", "-q", "origin", f"{other}:refs/heads/{branch}", cwd=origin.work)
                return super().push(commit, branch, expected)

        summary = self.publish(RacingGit(origin.work))
        self.assertTrue(summary["pushed"])
        self.assertTrue(any("starting over" in line for line in self.log))
        self.assertEqual(list(self.branch_doc()["reports"]), ["7", "12"])

    def test_nothing_left_to_add_closes_the_pull_request(self):
        self.publish()
        for number in (7, 12):
            self.github.issues[number].update(state="closed", state_reason="not_planned")
        summary = self.publish()
        self.assertEqual(summary.get("closed"), 500)
        self.assertEqual(self.github.pulls[500]["state"], "closed")
        self.assertIsNone(self.origin.ref(ingest.BRANCH))
        self.assertIn("Nothing left to add", self.github.comments[500][0]["body"])

    def test_merged_data_needs_no_pull_request(self):
        self.publish()
        self.origin.set_main(self.origin.show(ingest.BRANCH))
        self.github.pulls[500]["state"] = "closed"  # merged
        summary = self.publish()
        self.assertEqual((summary["changes"], summary["pull"], summary["pushed"]), (0, None, False))
        self.assertIsNone(self.origin.ref(ingest.BRANCH))
        self.assertEqual(len(self.github.pulls), 1)

    def test_a_refused_pull_request_says_which_setting_to_change(self):
        self.github.forbid_pull_create = True
        with self.assertRaises(ingest.PublishError) as caught:
            self.publish()
        self.assertIn("Allow GitHub Actions to create pull requests", str(caught.exception))
        self.assertIsNotNone(self.origin.ref(ingest.BRANCH))

    def test_a_dry_run_writes_nothing(self):
        summary = self.publish(dry_run=True)
        self.assertEqual(summary["changes"], 2)
        self.assertEqual(self.github.writes(), [])
        self.assertIsNone(self.origin.ref(ingest.BRANCH))
        self.assertTrue(any("[dry run] would push" in line for line in self.log))

    def test_a_shallow_checkout_like_actions_checkout_works(self):
        # actions/checkout fetches one commit: fetch, parents and the lease must work there too.
        shallow = os.path.join(self.temp.name, "shallow")
        git("clone", "-q", "--depth=1", "--no-local", self.origin.bare, shallow)
        self.assertEqual(git("rev-parse", "--is-shallow-repository", cwd=shallow), "true")
        summary = self.publish(ingest.Git(shallow))
        self.assertTrue(summary["pushed"])
        self.assertEqual(self.origin.parents(ingest.BRANCH), [self.origin.ref("main")])
        self.assertEqual(list(self.branch_doc()["reports"]), ["7", "12"])
        again = self.publish(ingest.Git(shallow), today="2026-11-30")
        self.assertFalse(again["pushed"])
        self.github.issues[7]["body"] = self.github.issues[7]["body"].replace("22.8 fps", "23.1 fps")
        self.assertTrue(self.publish(ingest.Git(shallow))["pushed"])
        self.assertEqual(self.branch_doc()["reports"]["7"]["fps"], 23.1)

    def test_issues_are_never_edited(self):
        self.publish()
        self.github.issues[7]["labels"] = []
        self.publish()
        self.assertFalse([call for call in self.github.writes() if call[1].startswith(f"/repos/{REPO}/issues/")
                          and call[0] == "PATCH" and "/comments/" not in call[1]])


class CommentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.data = os.path.join(self.temp.name, "community.json")
        self.write_main({})
        self.github = FakeGitHub([make_issue(7, fixture("device-report-manual.md") + "\nthanks @reporter"),
                                  make_issue(15, fixture("device-report-invalid.md")),
                                  make_issue(16, fixture("device-report-manual.md"), state="closed",
                                             state_reason="not_planned")])

    def write_main(self, records):
        with open(self.data, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(ingest.dump_document(ingest.make_document(records, REPO, today="2026-11-13")))

    def comment(self, number):
        return ingest.comment(self.github, REPO, number, self.data, TITLES, VALIDATE)

    def body(self, number):
        comments = self.github.comments.get(number, [])
        self.assertEqual(len(comments), 1)
        return comments[0]["body"]

    def test_a_listed_report_gets_its_row_once(self):
        self.github.pulls[500] = {"number": 500, "title": "t", "body": "b", "state": "open",
                                  "head": {"ref": ingest.BRANCH}, "base": {"ref": "main"}}
        self.assertEqual(self.comment(7), "created")
        body = self.body(7)
        for text in (ingest.COMMENT_MARKER, "Thanks for the device report!", "Pull request #500 adds it",
                     "| Game fps | 14.8 \u2192 22.8 |", "| Frame time p50 / p99 | n/a / 51 ms |",
                     "not verified (manual measurement)", "EDEN\\_NX2\\_GM=11", "```json"):
            self.assertIn(text, body)
        self.assertNotIn("@reporter", body)
        record = json.loads(body.split("```json\n", 1)[1].split("\n```", 1)[0])
        self.assertEqual(record, ingest.parse_issue(self.github.issues[7], TITLES, VALIDATE).record)
        self.assertEqual(self.comment(7), "unchanged")
        self.github.issues[7]["body"] = self.github.issues[7]["body"].replace("22.8 fps", "23.0 fps")
        self.assertEqual(self.comment(7), "updated")
        self.assertIn("14.8 \u2192 23", self.body(7))

    def test_a_report_with_problems_is_told_what_to_fix(self):
        self.assertEqual(self.comment(15), "created")
        body = self.body(15)
        self.assertIn("cannot go on the", body)
        self.assertIn('- The "GPU driver" answer is missing.', body)
        self.assertNotIn("```json", body)

    def test_a_merged_report_is_on_the_leaderboard(self):
        record = ingest.parse_issue(self.github.issues[7], TITLES, VALIDATE).record
        self.write_main({"7": record})
        self.comment(7)
        self.assertIn("It is on the leaderboard.", self.body(7))

    def test_a_report_closed_as_not_planned_gets_no_comment(self):
        self.assertEqual(self.comment(16), "skipped")
        self.assertEqual(self.github.comments, {})


class CommandLineTests(unittest.TestCase):
    def test_parse_prints_the_record(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = ingest.main(["parse", os.path.join(FIXTURES, "device-report-manual.md"), "--number", "7"])
        self.assertEqual(status, 0)
        self.assertEqual(json.loads(out.getvalue())["record"]["fps"], 22.8)

    def test_parse_fails_on_problems(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            status = ingest.main(["parse", os.path.join(FIXTURES, "device-report-invalid.md")])
        self.assertEqual(status, 1)
        self.assertIsNone(json.loads(out.getvalue())["record"])


if __name__ == "__main__":
    unittest.main()
