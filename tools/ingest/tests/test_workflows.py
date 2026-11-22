#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Structural checks of ingest-reports.yml and triage.yml.

The files are read with tests/yaml_subset.py, a strict reader for the YAML this repository uses
(the standard library has no YAML parser). These tests check structure and safety rules; they do
not run the workflows.

  python -I tools/ingest/tests/test_workflows.py
"""

import glob
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.

import yaml_subset  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
WORKFLOWS = os.path.join(ROOT, ".github", "workflows")


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def workflow(name):
    return yaml_subset.load(read(os.path.join(WORKFLOWS, name)))


def steps(doc):
    return [(job_name, step) for job_name, job in doc["jobs"].items() for step in job["steps"]]


class CommonRules:
    name = None

    def setUp(self):
        self.text = read(os.path.join(WORKFLOWS, self.name))
        self.doc = yaml_subset.load(self.text)

    def test_no_permissions_by_default(self):
        self.assertEqual(self.doc["permissions"], {})
        for job in self.doc["jobs"].values():
            self.assertIsInstance(job["permissions"], dict)

    def test_jobs_run_on_the_siblings_runner_with_a_timeout(self):
        for job in self.doc["jobs"].values():
            self.assertEqual(job["runs-on"], "ubuntu-24.04")
            self.assertRegex(job["timeout-minutes"], r"^\d+$")

    def test_every_step_uses_or_runs_one_thing(self):
        for job_name, step in steps(self.doc):
            self.assertEqual(len({"uses", "run"} & set(step)), 1, (job_name, step))
            self.assertIn("name", step)
            if "uses" in step:
                self.assertRegex(step["uses"], r"^actions/checkout@v\d+$", "as in labels.yml and pages.yml")

    def test_issue_text_never_reaches_a_run_block(self):
        # Everything a script needs comes through env or $GITHUB_EVENT_PATH, never ${{ }} in run.
        for job_name, step in steps(self.doc):
            if "run" in step:
                self.assertNotIn("${{", step["run"], job_name)
        self.assertNotRegex(self.text, r"github\.event\.(?:issue|comment|pull_request)\.(?:title|body)")

    def test_only_the_repositorys_token(self):
        self.assertNotIn("secrets.", self.text)
        for _, step in steps(self.doc):
            for key, value in (step.get("env") or {}).items():
                if "TOKEN" in key:
                    self.assertEqual(value, "${{ github.token }}")

    def test_the_scripts_it_runs_exist(self):
        for _, step in steps(self.doc):
            for path in re.findall(r"tools/ingest/[\w./-]+", step.get("run", "")):
                self.assertTrue(os.path.exists(os.path.join(ROOT, path)), path)
            if "run" in step:
                self.assertTrue(step["run"].startswith("python3 -I "), step["run"])


class IngestWorkflowTests(CommonRules, unittest.TestCase):
    name = "ingest-reports.yml"

    def test_triggers(self):
        on = self.doc["on"]
        self.assertEqual(on["issues"]["types"], ["opened", "edited", "labeled"])
        self.assertEqual(on["workflow_dispatch"]["inputs"]["dry_run"]["type"], "boolean")
        self.assertEqual(on["workflow_dispatch"]["inputs"]["dry_run"]["default"], "false")
        self.assertIn("tools/ingest/**", on["pull_request"]["paths"])

    def test_each_job_gets_only_what_it_needs(self):
        jobs = self.doc["jobs"]
        self.assertEqual(jobs["test"]["permissions"], {"contents": "read"})
        self.assertEqual(jobs["publish"]["permissions"],
                         {"contents": "write", "pull-requests": "write", "issues": "read"})
        self.assertEqual(jobs["comment"]["permissions"],
                         {"contents": "read", "issues": "write", "pull-requests": "read"})

    def test_one_publish_at_a_time_and_one_comment_per_issue(self):
        jobs = self.doc["jobs"]
        self.assertEqual(jobs["publish"]["concurrency"], {"group": "ingest-reports-publish", "cancel-in-progress": "false"})
        self.assertEqual(jobs["comment"]["concurrency"]["group"],
                         "ingest-reports-comment-${{ github.event.issue.number }}")
        self.assertEqual(jobs["comment"]["concurrency"]["cancel-in-progress"], "false")
        self.assertNotIn("concurrency", self.doc)

    def test_conditions(self):
        jobs = self.doc["jobs"]
        self.assertEqual(jobs["test"]["if"], "github.event_name == 'pull_request'")
        publish, comment = jobs["publish"]["if"], jobs["comment"]["if"]
        for condition in (publish, comment):
            self.assertIn("contains(github.event.issue.labels.*.name, 'device-report')", condition)
            self.assertIn("github.event.action != 'labeled' || github.event.label.name == 'device-report'", condition)
            self.assertNotIn("\n", condition)
        self.assertTrue(publish.startswith("github.event_name == 'workflow_dispatch' ||"))
        self.assertTrue(comment.startswith("always() && github.event_name == 'issues'"))
        self.assertEqual(jobs["comment"]["needs"], "publish")

    def test_only_the_publish_job_keeps_the_token_in_its_checkout(self):
        for job_name, step in steps(self.doc):
            if step.get("uses", "").startswith("actions/checkout"):
                persist = (step.get("with") or {}).get("persist-credentials")
                self.assertEqual(persist, None if job_name == "publish" else "false", job_name)

    def test_publish_tests_the_parser_first(self):
        runs = [step.get("run", "") for step in self.doc["jobs"]["publish"]["steps"]]
        self.assertEqual(runs[1], "python3 -I tools/ingest/tests/test_ingest.py")
        self.assertEqual(runs[2], "python3 -I tools/ingest/ingest.py publish")
        self.assertIn("unittest discover -s tools/ingest/tests", self.doc["jobs"]["test"]["steps"][1]["run"])
        for path in ("tools/ingest/**", ".github/ISSUE_TEMPLATE/**", "tools/bench/bench.py", "site/data/community.json"):
            self.assertIn(path, self.doc["on"]["pull_request"]["paths"])
        env = self.doc["jobs"]["publish"]["steps"][2]["env"]
        self.assertEqual(env, {"GITHUB_TOKEN": "${{ github.token }}", "DRY_RUN": "${{ inputs.dry_run }}"})


class TriageWorkflowTests(CommonRules, unittest.TestCase):
    name = "triage.yml"

    def test_triggers_and_permissions(self):
        self.assertEqual(self.doc["on"], {"issues": {"types": ["opened"]}})
        job = self.doc["jobs"]["triage"]
        self.assertEqual(job["permissions"], {"contents": "read", "issues": "write"})
        self.assertEqual(job["if"], "github.event.issue.user.type != 'Bot'")
        checkout = job["steps"][0]["with"]
        self.assertEqual(checkout, {"persist-credentials": "false", "sparse-checkout": "tools/ingest"})
        self.assertEqual(job["steps"][1]["run"], "python3 -I tools/ingest/triage.py")


class ReaderTests(unittest.TestCase):
    def test_it_reads_the_files_the_bots_read(self):
        files = glob.glob(os.path.join(ROOT, ".github", "ISSUE_TEMPLATE", "*.yml")) + [
            os.path.join(ROOT, ".github", "labels.yml")] + [
            os.path.join(WORKFLOWS, name) for name in ("ingest-reports.yml", "triage.yml", "label-issues.yml")]
        self.assertGreater(len(files), 12)
        for path in files:
            self.assertIsNotNone(yaml_subset.load(read(path)), path)

    def test_structure(self):
        doc = yaml_subset.load('a: 1\nb:\n  - x\n  - "y: z"\n  - k: v\n    l: |\n      line 1\n\n      line 3\n'
                               "c: [p, 'q r', \"s\"]\nd: {}\ne: >-\n  one\n  two\nf:\n")
        self.assertEqual(doc, {"a": "1", "b": ["x", "y: z", {"k": "v", "l": "line 1\n\nline 3\n"}],
                               "c": ["p", "q r", "s"], "d": {}, "e": "one two", "f": None})

    def test_it_refuses_what_it_does_not_understand(self):
        for text in ("a: b: c", "a:\n\tb: c", "a: 1\na: 2", "a: &x 1", "a: 'open", "a: b\n  c", "a: {b: c}",
                     "a: [b, [c]]", "a: *x", "- a\nb: c"):
            with self.assertRaises(yaml_subset.YamlError, msg=text):
                yaml_subset.load(text)


if __name__ == "__main__":
    unittest.main()
