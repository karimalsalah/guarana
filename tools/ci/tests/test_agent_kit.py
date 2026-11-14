#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Structural checks of the agent kit: the claim workflow, the optional agent workflow, Copilot's
setup steps, the MCP registry entry of the debug server and its bundle, and the front-door docs.
They read the files and build the bundle in memory; no workflow runs.

  python -I tools/ci/tests/test_agent_kit.py
"""

import contextlib
import hashlib
import io
import json
import os
import re
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = Path(HERE).parent.parent.parent
sys.path.insert(0, str(ROOT / "tools" / "ingest" / "tests"))  # -I leaves the script's directory off sys.path.
sys.path.insert(0, str(ROOT / "tools" / "nx2"))

import mcpb_pack  # noqa: E402
import yaml_subset  # noqa: E402

MAINTAINERS = "fromJSON('[\"OWNER\", \"MEMBER\", \"COLLABORATOR\"]')"
PINNED = re.compile(r"^\s*(?:- )?uses: ([\w.-]+/[\w.-]+)@([0-9a-f]{40}) # v\d+(?:\.\d+)*$", re.M)


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8").replace("\r\n", "\n")


def workflow(name):
    text = read(".github", "workflows", name)
    return text, yaml_subset.load(text)


def steps(doc):
    return [(name, step) for name, job in doc["jobs"].items() for step in job["steps"]]


class ClaimWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.text, self.doc = workflow("agent-claims.yml")
        self.jobs = self.doc["jobs"]
        self.scripts = {name: job["steps"][0]["with"]["script"] for name, job in self.jobs.items()}

    def test_triggers(self):
        on = self.doc["on"]
        self.assertEqual(on["issue_comment"], {"types": ["created"]})
        self.assertRegex(on["schedule"][0]["cron"], r"^\d+ \d+ \* \* \*$")  # daily
        self.assertEqual(on["workflow_dispatch"]["inputs"]["dry_run"]["type"], "boolean")

    def test_only_the_repositorys_token_and_least_permissions(self):
        self.assertNotIn("secrets.", self.text)
        self.assertEqual(self.doc["permissions"], {})
        self.assertEqual(self.jobs["command"]["permissions"], {"issues": "write"})
        self.assertEqual(self.jobs["release"]["permissions"], {"issues": "write", "pull-requests": "read"})
        for _, step in steps(self.doc):
            self.assertEqual(step["uses"], "actions/github-script@v9", "pinned to a major, as label-issues.yml")
            self.assertIn("name", step)

    def test_pull_requests_and_bots_are_ignored(self):
        condition = self.jobs["command"]["if"]
        self.assertIn("github.event_name == 'issue_comment'", condition)
        self.assertIn("!github.event.issue.pull_request", condition)
        self.assertIn("github.event.comment.user.type != 'Bot'", condition)
        self.assertIn("startsWith(github.event.comment.body, '/claim')", condition)
        self.assertIn("startsWith(github.event.comment.body, '/unclaim')", condition)
        self.assertIn('user.endsWith("[bot]")', self.scripts["command"])
        self.assertIn("if (issue.pull_request) return;", self.scripts["command"])
        self.assertIn("!item.pull_request", self.scripts["release"])

    def test_comment_text_never_reaches_a_script_through_an_expression(self):
        for script in self.scripts.values():
            self.assertNotIn("${{", script)
        self.assertNotRegex(self.text, r"github\.event\.comment\.body \}\}")

    def test_the_schedule_runs_only_in_the_main_repository_and_one_at_a_time(self):
        self.assertIn("github.repository_owner == 'karimalsalah'", self.jobs["release"]["if"])
        self.assertEqual(self.jobs["command"]["concurrency"],
                         {"group": "agent-claims-${{ github.event.issue.number }}", "cancel-in-progress": "false"})
        self.assertEqual(self.jobs["release"]["concurrency"]["cancel-in-progress"], "false")

    def test_both_scripts_read_the_same_record(self):
        for constant in ('const LABEL = "claimed";', "const HOURS = 72;", 'const MARKER = "<!-- agent-claim ";',
                         'const RELEASED = "<!-- agent-claim-released -->";', 'const BOT = "github-actions[bot]";'):
            for name, script in self.scripts.items():
                self.assertIn(constant, script, name)
        # Only markers written by this workflow count, so nobody can forge or extend a claim.
        for script in self.scripts.values():
            self.assertIn('c.user?.type !== "Bot" || c.user.login !== BOT', script)

    def test_a_release_removes_only_the_label_and_the_claimant(self):
        release = self.scripts["release"]
        self.assertIn("assignees: [claim.user]", release)
        self.assertIn("closedByPullRequestsReferences", release)
        self.assertIn("CROSS_REFERENCED_EVENT", release)
        self.assertIn("!event.isCrossRepository", release)
        for forbidden in ("issues.update(", "issues.lock(", "state: \"closed\""):
            self.assertNotIn(forbidden, self.text)

    def test_the_label_exists_and_the_docs_state_the_same_rules(self):
        labels = {label["name"] for label in yaml_subset.load(read(".github", "labels.yml"))}
        self.assertIn("claimed", labels)
        guide = read("docs", "agents", "README.md")
        for needed in ("`/claim`", "`/unclaim`", "72 hours", "`claimed`", "05:41 UTC", "needs-triage"):
            self.assertIn(needed, guide)
        self.assertIn("41 5 * * *", self.text)


class AgentsWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.text, self.doc = workflow("agents.yml")
        self.jobs = self.doc["jobs"]

    def test_triggers_are_mentions(self):
        self.assertEqual(set(self.doc["on"]), {"issue_comment", "pull_request_review_comment",
                                               "pull_request_review", "issues"})
        self.assertNotIn("pull_request_target", self.text)

    def test_only_maintainers_can_call_the_agents(self):
        condition = self.jobs["setup"]["if"]
        self.assertIn("github.event.sender.type == 'User'", condition)
        self.assertIn(MAINTAINERS, condition)
        for agent, phrase in (("claude", "'@claude'"), ("gemini", "'@gemini-cli'")):
            self.assertEqual(self.jobs[agent]["needs"], "setup")
            self.assertIn(f"needs.setup.outputs.{agent} == 'true'", self.jobs[agent]["if"])
            self.assertIn(phrase, self.jobs[agent]["if"])

    def test_least_permissions(self):
        self.assertEqual(self.doc["permissions"], {})
        self.assertEqual(self.jobs["setup"]["permissions"], {})
        self.assertEqual(self.jobs["claude"]["permissions"], {"contents": "read", "id-token": "write"})
        self.assertEqual(self.jobs["gemini"]["permissions"],
                         {"contents": "read", "issues": "write", "pull-requests": "write"})

    def test_the_setup_job_sees_whether_a_secret_exists_never_its_value(self):
        env = self.jobs["setup"]["steps"][0]["env"]
        for value in env.values():
            for use in re.findall(r"secrets\.\w+[^|}]*", value):
                self.assertRegex(use.strip(), r"^secrets\.\w+ != ''$")
        self.assertNotIn("${{", self.jobs["setup"]["steps"][0]["run"])
        self.assertEqual(set(self.jobs["setup"]["outputs"]), {"claude", "gemini"})

    def test_third_party_actions_are_pinned_to_a_release_commit(self):
        pins = dict(PINNED.findall(self.text))
        self.assertEqual(set(pins), {"actions/checkout", "anthropics/claude-code-action",
                                     "google-github-actions/run-gemini-cli"})
        # The same commit as build-check.yml, read from it so a dependabot bump of both stays green.
        self.assertIn(f"uses: actions/checkout@{pins['actions/checkout']}\n",
                      read(".github", "workflows", "build-check.yml"))
        uses = [step["uses"] for _, step in steps(self.doc) if "uses" in step]
        self.assertEqual(len(uses), 4)
        for use in uses:
            self.assertRegex(use, r"@[0-9a-f]{40}$")

    def test_gemini_reads_the_request_from_the_environment_and_never_checks_out_a_pull_request(self):
        checkout, run = self.jobs["gemini"]["steps"]
        self.assertEqual(checkout["with"], {"ref": "${{ github.event.repository.default_branch }}",
                                            "persist-credentials": "false"})
        self.assertIn("REQUEST", run["env"])
        self.assertIn('echo "$REQUEST"', run["with"]["prompt"])
        self.assertNotIn("github.event.comment.body", run["with"]["prompt"])
        settings = json.loads(run["with"]["settings"])
        self.assertTrue(all(tool.startswith("run_shell_command(") for tool in settings["tools"]["core"]))
        self.assertNotIn("run_shell_command(gh", run["with"]["settings"])

    def test_no_run_block_takes_an_expression(self):
        for _, step in steps(self.doc):
            if "run" in step:
                self.assertNotIn("${{", step["run"])


class CopilotSetupStepsTests(unittest.TestCase):
    def test_the_format_copilot_reads(self):
        text, doc = workflow("copilot-setup-steps.yml")
        self.assertEqual(list(doc["jobs"]), ["copilot-setup-steps"])
        job = doc["jobs"]["copilot-setup-steps"]
        self.assertLessEqual(set(job), {"steps", "permissions", "runs-on", "services", "snapshot", "timeout-minutes"})
        self.assertLessEqual(int(job["timeout-minutes"]), 59)
        self.assertEqual(job["permissions"], {"contents": "read"})
        self.assertIn("workflow_dispatch", doc["on"])
        self.assertNotIn("secrets.", text)

    def test_copilots_instructions_lead_to_the_front_door(self):
        text = read(".github", "copilot-instructions.md")
        self.assertIn("AGENTS.md", text)
        self.assertIn("docs/agents/README.md", text)
        self.assertIn("/claim", text)


class ServerJsonTests(unittest.TestCase):
    def setUp(self):
        self.server = json.loads(read("tools", "nx2", "server.json"))
        self.package = self.server["packages"][0]

    def test_the_official_registry_format(self):
        server = self.server
        self.assertEqual(server["$schema"], "https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json")
        self.assertRegex(server["name"], r"^io\.github\.karimalsalah/[a-zA-Z0-9._-]+$")
        self.assertLessEqual(len(server["description"]), 100)
        self.assertLessEqual(len(server["title"]), 100)
        self.assertRegex(server["version"], r"^\d+\.\d+\.\d+$")
        self.assertEqual(server["repository"], {"url": "https://github.com/karimalsalah/guarana", "source": "github",
                                                "subfolder": "tools/nx2"})
        self.assertEqual(len(server["packages"]), 1)

    def test_the_bundle_entry_passes_the_registrys_mcpb_rules(self):
        package, version = self.package, self.server["version"]
        self.assertEqual(package["registryType"], "mcpb")
        self.assertNotIn("registryBaseUrl", package)
        self.assertEqual(package["version"], version)
        self.assertEqual(package["identifier"], mcpb_pack.download_url(version))
        self.assertTrue(package["identifier"].startswith("https://github.com/karimalsalah/guarana/releases/download/"))
        self.assertIn("mcp", package["identifier"].lower())
        self.assertRegex(package["fileSha256"], r"^[a-f0-9]{64}$")
        self.assertNotEqual(package["fileSha256"], "0" * 64)
        self.assertEqual(package["transport"], {"type": "stdio"})


class BundleTests(unittest.TestCase):
    def test_the_same_sources_give_the_same_bytes(self):
        first, second = mcpb_pack.build("1.2.3"), mcpb_pack.build("1.2.3")
        self.assertEqual(hashlib.sha256(first).digest(), hashlib.sha256(second).digest())
        self.assertNotEqual(first, mcpb_pack.build("1.2.4"))

    def test_the_bundle_runs_the_adapter_with_its_helper(self):
        with zipfile.ZipFile(io.BytesIO(mcpb_pack.build("1.2.3"))) as bundle:
            self.assertEqual(bundle.namelist(), ["manifest.json", "server/debug_mcp.py", "server/frame_dump_index.py"])
            for info in bundle.infolist():
                self.assertEqual(info.compress_type, zipfile.ZIP_STORED)
                self.assertNotIn(b"\r\n", bundle.read(info))
            manifest = json.loads(bundle.read("manifest.json"))
        for field in ("manifest_version", "name", "version", "description", "author", "server"):
            self.assertIn(field, manifest)
        self.assertEqual(manifest["version"], "1.2.3")
        self.assertEqual(manifest["server"]["type"], "python")
        self.assertEqual(manifest["server"]["entry_point"], "server/debug_mcp.py")
        args = manifest["server"]["mcp_config"]["args"]
        self.assertIn("${__dirname}/server/debug_mcp.py", args)
        self.assertEqual(args[args.index("--server") + 1], "${user_config.server}")
        self.assertEqual(manifest["user_config"]["server"]["default"], "127.0.0.1:6545")  # loopback only

    def test_the_manifest_lists_the_adapters_tools(self):
        import debug_mcp
        names = [tool["name"] for tool in mcpb_pack.manifest("1.2.3")["tools"]]
        self.assertEqual(names, [tool["name"] for tool in debug_mcp.TOOLS])

    def test_main_writes_the_bundle_and_updates_server_json(self):
        with tempfile.TemporaryDirectory() as directory:
            server = os.path.join(directory, "server.json")
            with open(server, "w", encoding="utf-8") as handle:
                handle.write(read("tools", "nx2", "server.json"))
            digest = hashlib.sha256(mcpb_pack.build("9.8.7")).hexdigest()
            mcpb_pack.update_server_json("9.8.7", digest, server)
            with open(server, encoding="utf-8") as handle:
                updated = json.load(handle)
            self.assertEqual(updated["version"], "9.8.7")
            self.assertEqual(updated["packages"][0]["fileSha256"], digest)
            self.assertEqual(updated["packages"][0]["identifier"], mcpb_pack.download_url("9.8.7"))
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(mcpb_pack.main(["--version", "9.8.7", "--out-dir", directory]), 0)
            self.assertIn(f"sha256 {digest}", out.getvalue())
            with open(os.path.join(directory, mcpb_pack.bundle_name("9.8.7")), "rb") as handle:
                self.assertEqual(hashlib.sha256(handle.read()).hexdigest(), digest)

    def test_a_bad_version_is_refused(self):
        with self.assertRaises(SystemExit):
            mcpb_pack.build("latest")


class FrontDoorDocsTests(unittest.TestCase):
    DOCS = ("docs/agents/README.md", "docs/agents/RESEARCH-2026-11.md", "HANDOVER.md")

    def test_relative_links_exist(self):
        for doc in self.DOCS:
            base = (ROOT / doc).parent
            for target in re.findall(r"\]\(([^)\s]+)\)", read(*doc.split("/"))):
                if re.match(r"[a-z]+:|#", target):
                    continue
                self.assertTrue((base / target.split("#")[0]).exists(), f"{doc}: {target}")

    def test_no_machine_paths(self):
        for doc in self.DOCS:
            self.assertNotRegex(read(*doc.split("/")), r"(?<![A-Za-z])[A-Za-z]:[\\/]|/home/|/Users/", doc)

    def test_the_handover_has_its_sections(self):
        text = read("HANDOVER.md")
        for heading in ("## Where things stand", "## Lanes in progress", "## How a change is measured",
                        "## Where things are", "## If you are an agent starting now: do these five things first"):
            self.assertIn(heading, text)
        five = text.split("do these five things first")[1].split("\n## ")[0]
        self.assertEqual(len(re.findall(r"(?m)^\d\. ", five)), 5)

    def test_the_publish_steps_match_the_files(self):
        guide = read("docs", "agents", "README.md")
        for needed in ("mcp-publisher login github", "mcp-publisher publish tools/nx2/server.json",
                       "python -I tools/nx2/mcpb_pack.py", "gh release upload", "io.github.karimalsalah/guarana-debug"):
            self.assertIn(needed, guide)
        self.assertIn(json.loads(read("tools", "nx2", "server.json"))["name"], guide)


if __name__ == "__main__":
    unittest.main()
