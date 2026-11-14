#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keeps the parsers in step with the issue forms in .github/ISSUE_TEMPLATE/: a label or option
renamed there must be renamed in issue_forms.py, ingest.py or triage.py too.

  python -I tools/ingest/tests/test_forms.py
"""

import glob
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(HERE))

import ingest  # noqa: E402
import issue_forms  # noqa: E402
import triage  # noqa: E402
import yaml_subset  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
FORMS = os.path.join(ROOT, ".github", "ISSUE_TEMPLATE")


def form(name):
    with open(os.path.join(FORMS, name), encoding="utf-8") as handle:
        return yaml_subset.load(handle.read())


def fields(doc):
    return {item["attributes"]["label"]: item for item in doc["body"] if item["type"] != "markdown"}


def options(doc, label):
    return fields(doc)[label]["attributes"]["options"]


class FormTests(unittest.TestCase):
    def test_form_labels_are_exactly_the_forms_labels(self):
        labels = set()
        for path in glob.glob(os.path.join(FORMS, "*.yml")):
            if not path.endswith("config.yml"):
                labels |= set(fields(form(os.path.basename(path))))
        self.assertEqual(labels, set(issue_forms.FORM_LABELS))

    def test_gpu_options_of_every_form_are_known(self):
        for name in ("device-report.yml", "crash.yml", "game_bug.yml", "visual_bug.yml", "ui_bug.yml", "perf-idea.yml"):
            unknown = set(options(form(name), "Graphics card")) - set(issue_forms.GPU_OPTIONS)
            self.assertEqual(unknown, set(), name)

    def test_device_report_options(self):
        doc = form("device-report.yml")
        self.assertEqual(options(doc, "Game"), list(ingest.GAMES))
        self.assertEqual(options(doc, "Form factor"), list(ingest.FORM_FACTORS))
        self.assertEqual(options(doc, "Power source"), list(ingest.POWER_SOURCES))
        self.assertIn(ingest.LABEL, doc["labels"])

    def test_device_report_fields_the_parser_reads(self):
        doc = fields(form("device-report.yml"))
        for label in ingest.REQUIRED + ingest.OPTIONAL:
            self.assertEqual(doc[label]["validations"], {"required": "true"}, label)
        self.assertEqual(doc["tools/bench rows"]["attributes"]["render"], "json")
        boxes = [option["label"].casefold() for option in doc["Before submitting"]["attributes"]["options"]]
        for prefix, _ in ingest.REQUIRED_CHECKS:
            self.assertTrue(any(box.startswith(prefix) for box in boxes), prefix)

    def test_agent_task_fields_triage_reads(self):
        doc = form("agent-task.yml")
        for label in ("Goal", "Files", "Acceptance test", "Size"):
            self.assertIn(label, fields(doc))
        self.assertTrue(any(option.startswith("Large") for option in options(doc, "Size")))
        self.assertNotIn("agent-ready", doc["labels"])  # added by triage, not by the form

    def test_crash_forms_log_field(self):
        for name in ("crash.yml", "game_bug.yml", "visual_bug.yml", "ui_bug.yml"):
            self.assertIn(triage.LOG_SECTION, fields(form(name)), name)
        self.assertIn(triage.CRASH_LABEL, form("crash.yml")["labels"])


if __name__ == "__main__":
    unittest.main()
