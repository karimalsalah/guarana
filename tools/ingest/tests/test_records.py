#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the Records board: the "Credit me as" field, ingest.records_board, the "records" list of
site/data/community.json and the Records block of site/benchmarks.html. No network.

  python -I tools/ingest/tests/test_records.py
"""

import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(HERE))

import ingest  # noqa: E402
import issue_forms  # noqa: E402
import yaml_subset  # noqa: E402
from fakes import REPO  # noqa: E402
from test_site import Page, read  # noqa: E402

FIXTURES = os.path.join(HERE, "fixtures")
VALIDATE = ingest.load_bench_validator()
TITLES = {"0400486024DEC000": "The Legend of Zelda: Ocarina of Time", "0400C3F00006E000": "Mario Kart World"}


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as handle:
        return handle.read()


def with_credit(body, answer):
    return body.rstrip("\n") + "\n\n### Credit me as\n\n" + answer + "\n"


def parse(body, logins=("reporter",)):
    return ingest.parse_report(body, 12, "2026-11-20", logins, TITLES, VALIDATE)


def rec(issue, fps, p99=50.0, gpu="Intel Arc 140T / 140V", game="Ocarina of Time", scene="oot-hyrule-field",
        credit=None, scene_ok=True, source="tools/bench", approximate=False):
    return {"issue": issue, "fps": fps, "p99_ms": p99, "p50_ms": 40.0, "gpu_option": gpu, "gpu": gpu, "game": game,
            "bench_scene": scene, "scene": "free text", "credit": credit, "scene_ok": scene_ok, "source": source,
            "fps_approximate": approximate, "device": f"Device {issue}", "driver": "1.0", "build": "v1",
            "submitted": "2026-11-20", "bench": {"measured": "2026-11-19"}}


class CreditFieldTests(unittest.TestCase):
    def test_the_form_has_an_optional_credit_field_the_parser_knows(self):
        doc = yaml_subset.load(read(".github", "ISSUE_TEMPLATE", "device-report.yml"))
        fields = {item["attributes"]["label"]: item for item in doc["body"] if item.get("attributes", {}).get("label")}
        self.assertIn(ingest.CREDIT_LABEL, fields)
        self.assertNotIn("validations", fields[ingest.CREDIT_LABEL])
        self.assertIn(ingest.CREDIT_LABEL, issue_forms.FORM_LABELS)

    def test_no_answer_means_no_name(self):
        record, problems, warnings = parse(fixture("device-report-bench.md"))
        self.assertEqual(problems, [])
        self.assertIsNone(record["credit"])
        self.assertFalse([w for w in warnings if "Credit me as" in w])

    def test_no_response_means_no_name(self):
        record, _, warnings = parse(with_credit(fixture("device-report-bench.md"), "_No response_"))
        self.assertIsNone(record["credit"])
        self.assertFalse([w for w in warnings if "Credit me as" in w])

    def test_the_reporters_own_handle_is_kept_with_or_without_at(self):
        for answer in ("reporter", "@reporter", "@Reporter"):
            record, _, _ = parse(with_credit(fixture("device-report-bench.md"), answer), logins=("reporter",))
            self.assertEqual(record["credit"], "reporter", answer)

    def test_the_handle_is_the_account_that_opened_the_report_not_what_was_typed(self):
        record, _, _ = parse(with_credit(fixture("device-report-bench.md"), "reporter"), logins=("Reporter",))
        self.assertEqual(record["credit"], "Reporter")

    def test_someone_elses_handle_is_refused_with_a_warning(self):
        record, problems, warnings = parse(with_credit(fixture("device-report-bench.md"), "@somebody-else"))
        self.assertEqual(problems, [])
        self.assertIsNone(record["credit"])
        self.assertTrue([w for w in warnings if "Credit me as" in w])

    def test_not_a_handle_is_refused(self):
        for answer in ('evil"><script>', "two words", "-leading", "a" * 40, "Real Name"):
            record, _, warnings = parse(with_credit(fixture("device-report-bench.md"), answer), logins=(answer,))
            self.assertIsNone(record["credit"], answer)
            self.assertTrue([w for w in warnings if "Credit me as" in w], answer)

    def test_without_an_author_login_nobody_is_credited(self):
        record, _, _ = parse(with_credit(fixture("device-report-bench.md"), "reporter"), logins=("",))
        self.assertIsNone(record["credit"])


class BoardTests(unittest.TestCase):
    def board(self, *records):
        return ingest.records_board({str(r["issue"]): r for r in records})

    def test_the_best_row_wins_per_gpu_class_and_scene(self):
        board = self.board(rec(1, 20.0, credit="ann"), rec(2, 24.5, credit="bob"), rec(3, 22.0),
                           rec(4, 30.0, scene="another-scene", credit="cy"),
                           rec(5, 40.0, gpu="AMD Radeon 780M / 760M", credit="dee"))
        winners = {(row["gpu"], row["scene"]): (row["issue"], row["credit"]) for row in board}
        self.assertEqual(winners, {("Intel Arc 140T / 140V", "oot-hyrule-field"): (2, "bob"),
                                   ("Intel Arc 140T / 140V", "another-scene"): (4, "cy"),
                                   ("AMD Radeon 780M / 760M", "oot-hyrule-field"): (5, "dee")})

    def test_only_validated_exact_rows_count(self):
        board = self.board(rec(1, 99.0, scene_ok=None, source="manual"), rec(2, 98.0, scene_ok=None),
                           rec(3, 97.0, approximate=True), rec(4, 0.0), rec(5, 12.0, credit="ann"))
        self.assertEqual([row["issue"] for row in board], [5])

    def test_ties_go_to_the_lower_p99_then_the_earlier_report(self):
        self.assertEqual(self.board(rec(7, 25.0, p99=60.0), rec(8, 25.0, p99=45.0))[0]["issue"], 8)
        self.assertEqual(self.board(rec(9, 25.0), rec(8, 25.0))[0]["issue"], 8)
        self.assertEqual(self.board(rec(9, 25.0, p99=None), rec(8, 25.0, p99=70.0))[0]["issue"], 8)

    def test_an_anonymous_record_stays_anonymous(self):
        row = self.board(rec(1, 25.0))[0]
        self.assertIsNone(row["credit"])
        self.assertEqual((row["fps"], row["p99_ms"], row["device"], row["measured"]), (25.0, 50.0, "Device 1", "2026-11-19"))

    def test_empty_when_there_is_no_data(self):
        self.assertEqual(ingest.records_board({}), [])
        self.assertEqual(ingest.make_document({}, REPO)["records"], [])

    def test_the_document_carries_the_board_and_is_stable(self):
        records = {"1": rec(1, 20.0, credit="ann"), "2": rec(2, 24.0)}
        doc = ingest.make_document(records, REPO, today="2026-11-20")
        self.assertEqual([row["issue"] for row in doc["records"]], [2])
        self.assertEqual(ingest.dump_document(ingest.make_document(records, REPO, doc, today="2026-12-01")),
                         ingest.dump_document(doc))

    def test_a_parsed_bench_report_reaches_the_board_with_its_handle(self):
        record, problems, _ = parse(with_credit(fixture("device-report-bench.md"), "@reporter"))
        self.assertEqual(problems, [])
        row = ingest.records_board({"12": record})[0]
        self.assertEqual((row["credit"], row["issue"], row["gpu"]), ("reporter", 12, "Intel Arc 140T / 140V"))


class DataFileTests(unittest.TestCase):
    def test_community_json_has_the_records_list(self):
        doc = json.loads(read("site", "data", "community.json"))
        self.assertIn("records", doc)
        self.assertEqual(doc["records"], ingest.records_board(doc["reports"]))
        self.assertIn("credit", doc["fields"])
        self.assertIn("records", doc["fields"])


class RecordsBlockTests(unittest.TestCase):
    def setUp(self):
        self.text = read("site", "benchmarks.html")
        self.page = Page(self.text)

    def test_the_block_sits_above_the_leaderboard_and_outside_bench_app(self):
        self.assertIn("records-app", self.page.ids)
        self.assertLess(self.text.index('id="records"'), self.text.index('id="community"'))
        self.assertIn("records", self.page.ancestors["records-app"])
        self.assertNotIn("bench-app", self.page.ancestors["records-app"])
        self.assertEqual(self.page.errors, [])
        self.assertEqual(len(self.page.ids), len(set(self.page.ids)))

    def test_the_script_is_text_only_es5_with_a_beat_it_button_and_an_empty_state(self):
        scripts = [script for script in self.page.scripts if "records-app" in script]
        self.assertEqual(len(scripts), 1)
        script = scripts[0]
        for forbidden in ("innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function"):
            self.assertNotIn(forbidden, script)
        self.assertNotRegex(script, r"=>|`|\blet\b|\bconst\b")
        self.assertIn('"Beat it"', script)
        self.assertIn("No record yet", script)
        self.assertIn("Anonymous", script)
        self.assertIn("LOGIN.test(handle)", script)
        self.assertIn("template=device-report.yml", self.text[self.text.index('id="records-app"'):][:400])

    def test_the_leaderboard_script_is_still_alone(self):
        self.assertEqual(len([s for s in self.page.scripts if "community-app" in s]), 1)


if __name__ == "__main__":
    unittest.main()
