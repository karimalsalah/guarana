#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks of how releases are signed, attested and kept up to date (docs/SIGNING.md, docs/LANES.md).

The Windows programs and the installer are signed before they are uploaded and checksummed, every
release file is attested before it is published, the jobs that call release-binaries.yml and
launcher.yml grant what those ask for, no condition reads a secret, and CODEOWNERS and Dependabot's
configuration are where GitHub reads them. Source checks only (nothing is built or run):
python -I tools/ci/tests/test_supply_chain.py
"""

import os
import re
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools", "ingest", "tests"))  # -I leaves it off sys.path.

import yaml_subset  # noqa: E402

SIGNING_ACTION = re.compile(r"^Azure/artifact-signing-action@[0-9a-f]{40}$")
ATTEST_ACTION = "actions/attest-build-provenance@v4"
ATTESTING = {"contents": "write", "id-token": "write", "attestations": "write"}
SECRETS = ("AZURE_TENANT_ID", "AZURE_CLIENT_ID", "AZURE_CLIENT_SECRET",
           "ARTIFACT_SIGNING_ENDPOINT", "ARTIFACT_SIGNING_ACCOUNT", "ARTIFACT_SIGNING_PROFILE")


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as handle:
        return handle.read()


def workflow(name):
    return yaml_subset.load(read(".github", "workflows", name))


class StepOrder:
    def position(self, job, name):
        names = [step.get("name") for step in job["steps"]]
        self.assertIn(name, names)
        return names.index(name)

    def only(self, job, prefix):
        found = [i for i, step in enumerate(job["steps"]) if step.get("uses", "").startswith(prefix)]
        self.assertEqual(len(found), 1, prefix)
        return found[0]


class SigningTests(StepOrder, unittest.TestCase):
    def check(self, job, after, before, gate):
        steps = job["steps"]
        sign = self.only(job, "Azure/artifact-signing-action@")
        self.assertRegex(steps[sign]["uses"], SIGNING_ACTION)  # pinned to a commit
        self.assertLess(self.position(job, after), sign)
        self.assertLess(sign, self.position(job, before))
        check, verify = steps[sign - 1], steps[sign + 1]
        self.assertEqual(check.get("id"), "signing")
        for part in gate:
            self.assertIn(part, check["if"])
        self.assertEqual(steps[sign]["if"], "steps.signing.outputs.enabled == 'true'")
        self.assertEqual(verify["if"], steps[sign]["if"])
        self.assertIn("Get-AuthenticodeSignature", verify["run"])
        # The check only learns whether each secret is set; the values go to the signing action alone.
        for name in SECRETS:
            self.assertEqual(check["env"]["HAS_" + name], "${{ secrets.%s != '' }}" % name)
            self.assertIn("'%s'" % name, check["run"])
        values = [value for value in steps[sign]["with"].values() if "secrets." in value]
        self.assertEqual(sorted(values), sorted("${{ secrets.%s }}" % name for name in SECRETS))
        for index, step in enumerate(steps):
            if index != sign:
                self.assertNotRegex(str(step), r"secrets\.\w+ \}\}", step.get("name"))
        return steps[sign]["uses"]

    def test_release_binaries_signs_both_programs_before_their_upload(self):
        build = workflow("release-binaries.yml")["jobs"]["build"]
        self.check(build, "Package Windows binary", "Upload Windows binary",
                   ("startsWith(matrix.platform, 'windows')", "needs.prepare.outputs.publish == 'true'"))
        names = build["steps"][self.position(build, "Check the signing secrets")]["run"]
        for program in ("guarana.exe", "guarana-cli.exe", "lichii.exe", "eden-cli.exe"):  # and before the rename
            self.assertIn(program, names)

    def test_launcher_signs_the_installer_of_a_release_before_its_upload(self):
        build = workflow("launcher.yml")["jobs"]["build"]
        self.check(build, "Name the installer", "Upload the installer", ("steps.plan.outputs.release_tag != ''",))

    def test_both_workflows_pin_the_same_commit(self):
        pins = set()
        for name in ("release-binaries.yml", "launcher.yml"):
            for job in workflow(name)["jobs"].values():
                pins |= {step["uses"] for step in job.get("steps") or []
                         if step.get("uses", "").startswith("Azure/artifact-signing-action@")}
        self.assertEqual(len(pins), 1, pins)

    def test_no_condition_reads_a_secret(self):
        # GitHub refuses secrets in a job's or a step's if:, so the gates read step outputs instead.
        for name in sorted(os.listdir(os.path.join(ROOT, ".github", "workflows"))):
            for line in re.findall(r"^\s*if:.*$", read(".github", "workflows", name), re.M):
                self.assertNotIn("secrets.", line, name)


class ProvenanceTests(StepOrder, unittest.TestCase):
    def test_every_release_file_is_attested_before_it_is_published(self):
        publish = workflow("release-binaries.yml")["jobs"]["publish"]
        self.assertEqual(publish["permissions"], ATTESTING)
        attest = self.only(publish, ATTEST_ACTION)
        self.assertEqual(publish["steps"][attest]["uses"], ATTEST_ACTION)
        self.assertEqual(publish["steps"][attest]["with"], {"subject-path": "dist/*"})
        package = self.position(publish, "Package release files")
        self.assertIn("SHA256SUMS.txt", publish["steps"][package]["run"])
        self.assertLess(package, attest)
        self.assertLess(attest, self.position(publish, "Publish"))

    def test_the_installer_and_the_new_checksums_are_attested_before_their_upload(self):
        attach = workflow("launcher.yml")["jobs"]["attach"]
        self.assertEqual(attach["permissions"], ATTESTING)
        attest = self.only(attach, ATTEST_ACTION)
        subjects = attach["steps"][attest]["with"]["subject-path"].splitlines()
        self.assertEqual(subjects, ["dist/${{ needs.build.outputs.setup_name }}", "dist/SHA256SUMS.txt"])
        checksum = self.position(attach, "Add its checksum")
        upload = self.position(attach, "Upload it and its checksum")
        self.assertLess(checksum, attest)
        self.assertLess(attest, upload)
        self.assertNotIn("gh release upload", attach["steps"][checksum]["run"])
        self.assertIn("gh release upload", attach["steps"][upload]["run"])

    def test_the_callers_grant_what_the_called_workflows_ask_for(self):
        # A called workflow gets at most its caller's permissions, or the whole run fails to start.
        calls = ((workflow("nightly.yml")["jobs"]["release"], "./.github/workflows/release-binaries.yml"),
                 (workflow("release-binaries.yml")["jobs"]["installer"], "./.github/workflows/launcher.yml"))
        for job, called in calls:
            self.assertEqual(job["uses"], called)
            self.assertEqual(job["permissions"], ATTESTING)
            self.assertEqual(job["secrets"], "inherit")


class RepositoryFileTests(unittest.TestCase):
    def test_one_codeowners_file_where_github_looks_first(self):
        # GitHub takes the first of .github/, the root and docs/; another copy would be ignored.
        rules = [line for line in read(".github", "CODEOWNERS").splitlines() if line and not line.startswith("#")]
        self.assertEqual(rules, ["* @karimalsalah"])
        for other in (("CODEOWNERS",), ("docs", "CODEOWNERS")):
            self.assertFalse(os.path.exists(os.path.join(ROOT, *other)), other)

    def test_dependabot_updates_weekly_in_small_groups(self):
        config = yaml_subset.load(read(".github", "dependabot.yml"))
        self.assertEqual(config["version"], "2")
        updates = {update["package-ecosystem"]: update for update in config["updates"]}
        self.assertEqual(set(updates), {"github-actions", "cargo", "npm"})
        self.assertEqual(updates["github-actions"]["directory"], "/")
        self.assertEqual(updates["cargo"]["directory"], "/launcher/src-tauri")
        self.assertEqual(updates["npm"]["directories"], ["/launcher", "/launcher/ui"])
        for manifest in ("launcher/src-tauri/Cargo.lock", "launcher/package-lock.json", "launcher/ui/package-lock.json"):
            self.assertTrue(os.path.isfile(os.path.join(ROOT, *manifest.split("/"))), manifest)
        labels = {label["name"] for label in yaml_subset.load(read(".github", "labels.yml"))}
        for name, update in updates.items():
            self.assertEqual(update["schedule"], {"interval": "weekly"}, name)
            (group,) = update["groups"].values()
            self.assertEqual(group["update-types"], ["minor", "patch"], name)
            # Dependabot leaves out a label the repository does not have.
            self.assertIn("dependencies", update["labels"], name)
            self.assertEqual(set(update["labels"]) - labels, set(), name)


if __name__ == "__main__":
    unittest.main()
