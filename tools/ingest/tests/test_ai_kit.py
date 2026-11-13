#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks of the AI plug-and-play kit, the Discord links and the README:

docs/AI-QUICKSTART.md, the instruction shims, the one-paste prompt on site/agents.html, the Discord
contact links, and the Hall of fame links. Standard library only.

  python -I tools/ingest/tests/test_ai_kit.py
"""

import glob
import html
import json
import os
import re
import sys
import unittest
from xml.etree import ElementTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)  # -I leaves the script's directory off sys.path.
sys.path.insert(0, os.path.dirname(HERE))

import triage  # noqa: E402
import yaml_subset  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DISCORD = "discord.gg/urz7vMdRM"
SHIMS = (".github/copilot-instructions.md", ".cursor/rules/guarana.mdc", "GEMINI.md", ".windsurfrules",
         "CONVENTIONS.md", ".clinerules/guarana.md")
TOOLS = ("Claude Code", "Codex", "Gemini CLI", "Cursor", "Copilot", "Windsurf", "Aider", "Cline")
DOCS_MAP = ("docs/README.md", "docs/switches.md", "docs/frame-generation.md", "docs/FAQ.md", "docs/TROUBLESHOOTING.md",
            "AGENTS.md", "CONTRIBUTING.md", "ROADMAP.md")


def read(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as handle:
        return handle.read().replace("\r\n", "\n")


def site_pages():
    pages = glob.glob(os.path.join(ROOT, "site", "**", "*.html"), recursive=True)
    return [page for page in pages if os.sep + "promo" + os.sep not in page]


def prompt_of(text):
    return re.search(r"<!-- one-paste-prompt:start -->\n```text\n(.*?)\n```\n<!-- one-paste-prompt:end -->", text, re.S).group(1)


class ShimTests(unittest.TestCase):
    def test_each_shim_points_at_agents_md_and_is_short(self):
        for shim in SHIMS:
            text = read(*shim.split("/"))
            self.assertIn("AGENTS.md", text, shim)
            self.assertLess(len(text.split("\n")), 15, shim)
            self.assertLess(len(text), 900, shim)

    def test_shims_do_not_copy_the_rules(self):
        for shim in SHIMS:
            text = read(*shim.split("/"))
            for rule in ("scene_ok", "Ship only a measured gain", "Co-authored-by", "docs/switches.md"):
                self.assertNotIn(rule, text, shim)

    def test_the_tools_find_their_files(self):
        self.assertIn("@AGENTS.md", read("CLAUDE.md"))
        self.assertIn("@./AGENTS.md", read("GEMINI.md"))  # Gemini CLI's @file import
        self.assertIn("@AGENTS.md", read(".cursor", "rules", "guarana.mdc"))
        self.assertRegex(read(".cursor", "rules", "guarana.mdc"), r"(?m)^alwaysApply: true$")
        self.assertIn("read: [AGENTS.md]", read(".aider.conf.yml"))
        self.assertFalse(os.path.exists(os.path.join(ROOT, ".cursor", "rules", "project.mdc")))
        # Cline's rules are a folder; a file of the same name would hide it.
        self.assertTrue(os.path.isdir(os.path.join(ROOT, ".clinerules")))

    def test_mcp_json_registers_the_debug_server(self):
        server = json.loads(read(".mcp.json"))["mcpServers"]["emulator-debug"]
        self.assertEqual(server["args"][:3], ["-X", "utf8", "-I"])
        self.assertIn("tools/nx2/debug_mcp.py", server["args"])


class CodexBudgetTests(unittest.TestCase):
    """Codex CLI reads only project_doc_max_bytes of AGENTS.md (32 KiB by default) and cuts the rest."""

    def setUp(self):
        match = re.search(r"(?m)^project_doc_max_bytes = (\d+)$", read(".codex", "config.toml"))
        self.assertIsNotNone(match)
        self.budget = int(match.group(1))

    def test_the_budget_holds_all_of_agents_md_even_with_crlf(self):
        with open(os.path.join(ROOT, "AGENTS.md"), "rb") as handle:
            lf = handle.read().replace(b"\r\n", b"\n")
        # A Windows checkout turns every LF into CRLF, and Codex counts the bytes on disk.
        self.assertLess(len(lf) + lf.count(b"\n"), self.budget)

    def test_the_docs_give_the_same_override(self):
        flag = f"project_doc_max_bytes={self.budget}"
        for path in ("README.md", "docs/AI-QUICKSTART.md", "docs/agents/README.md", ".codex/config.toml"):
            self.assertIn(flag, read(*path.split("/")), path)


class QuickstartTests(unittest.TestCase):
    def setUp(self):
        self.text = read("docs", "AI-QUICKSTART.md")

    def test_every_tool_is_there_with_one_command(self):
        for tool in TOOLS:
            self.assertIn(tool, self.text)
        for command in ('claude "Read AGENTS.md', 'codex "Read AGENTS.md', "gemini", 'agent "Read AGENTS.md',
                        'aider --message "Read AGENTS.md', 'cline "Read AGENTS.md', "npm install -g @openai/codex",
                        "npm install -g @google/gemini-cli", "npm install -g cline", "aider-install"):
            self.assertIn(command, self.text)

    def test_the_prefill_parameters_are_the_triage_bots(self):
        for _, base in triage.CHAT_LINKS:
            self.assertIn(base + "<encoded prompt>", self.text)

    def test_the_prompt_names_llms_full_and_the_rules(self):
        prompt = prompt_of(self.text)
        self.assertIn("https://karimalsalah.github.io/guarana/llms-full.txt", prompt)
        self.assertIn("Assisted-by:", prompt)
        self.assertIn("never include keys, firmware, game files", prompt)
        self.assertLess(len(prompt), 3000)

    def test_discord_and_the_hall_of_fame(self):
        self.assertIn(DISCORD, self.text)
        self.assertIn("legacy.html", self.text)

    def test_relative_links_exist(self):
        for target in re.findall(r"\]\(([^)\s]+)\)", self.text):
            if re.match(r"[a-z]+:|#", target):
                continue
            self.assertTrue(os.path.exists(os.path.normpath(os.path.join(ROOT, "docs", target.split("#")[0]))), target)


class AgentsPageTests(unittest.TestCase):
    def test_the_copy_button_copies_the_quickstarts_prompt(self):
        page = read("site", "agents.html")
        shown = re.search(r'<pre id="one-paste-prompt"><code>(.*?)</code></pre>', page, re.S).group(1)
        self.assertEqual(html.unescape(shown), prompt_of(read("docs", "AI-QUICKSTART.md")))
        self.assertRegex(page, r'<button type="button" class="btn" id="copy-prompt"')
        self.assertIn("docs/AI-QUICKSTART.md", page)
        self.assertIn(DISCORD, page)
        self.assertIn('href="llms-full.txt"', page)

    def test_agents_json_knows_the_kit(self):
        doc = json.loads(read("site", "agents.json"))
        self.assertEqual(doc["quickstart"]["doc"], "docs/AI-QUICKSTART.md")
        self.assertEqual(doc["contact"]["chat"], "https://" + DISCORD)
        self.assertEqual(doc["agent_entry_files"]["gemini_cli"], ["GEMINI.md"])


class DiscordTests(unittest.TestCase):
    def test_every_page_of_the_site_has_it_in_the_footer(self):
        pages = site_pages()
        self.assertGreater(len(pages), 20)
        for page in pages:
            with open(page, encoding="utf-8") as handle:
                text = handle.read()
            footer = text[text.index('<footer class="site-footer">'):]
            self.assertIn(f'<a href="https://{DISCORD}">', footer, page)
            self.assertIn("legacy.html", footer, page)

    def test_it_is_a_text_link_without_artwork(self):
        for page in site_pages() + [os.path.join(ROOT, "README.md")]:
            with open(page, encoding="utf-8") as handle:
                text = handle.read()
            self.assertNotRegex(text, r"(?i)logo=discord|discord[-_ ]?(logo|mark|icon)|simple-icons/discord", page)

    def test_the_places_it_belongs(self):
        for path in ("README.md", "CONTRIBUTING.md", "SUPPORT.md", "docs/AGENTS-INVITE.md", "site/agents.html",
                     "site/llms.txt", "docs/AI-QUICKSTART.md", "site/legacy.html"):
            self.assertIn(DISCORD, read(*path.split("/")), path)

    def test_issue_chooser_sends_chat_to_discord_and_bugs_stay(self):
        config = yaml_subset.load(read(".github", "ISSUE_TEMPLATE", "config.yml"))
        self.assertEqual(config["blank_issues_enabled"], "false")
        links = {link["url"]: link for link in config["contact_links"]}
        self.assertIn("https://" + DISCORD, links)
        self.assertIn("stay here on GitHub", links["https://" + DISCORD]["about"])
        self.assertIn("https://github.com/karimalsalah/guarana/discussions", links)

    def test_support_md_keeps_bugs_on_github(self):
        self.assertRegex(read("SUPPORT.md"), r"(?i)bugs.*stay on GitHub")


class ReadmeTests(unittest.TestCase):
    def setUp(self):
        self.text = read("README.md")

    def test_first_screen_sells_the_mission_with_the_measured_numbers(self):
        head = self.text[:self.text.index("## Get it")]
        for needed in ("14.8", "22.8", "15.7", "5.8", "16.7", "100%", "75%", "33-54%", "16.4", "4,515", "155 s",
                       "Core Ultra 9 285H", "Arc 140T", "32.0.101.9175", "120 Hz OLED", "GPL-3.0-or-later",
                       "599 of 600", "best run", "531-599", "two paired rounds", "0 late flips", "99.5%", "45-47%",
                       "about 200", "24 ms", "20 Hz"):
            self.assertIn(needed, head)

    def test_sections_and_honesty_markers(self):
        for heading in ("## Measured wins", "## Get it", "## Built for AI agents", "## Why contribute now",
                        "## Hall of fame", "## How the frame pacing works", "## Devices", "## Roadmap", "## Docs",
                        "## FAQ", "## Legal"):
            self.assertIn(heading, self.text)
        self.assertIn("guarana setup** (since v20.1)", self.text.lower())
        self.assertIn("In v20.1, opt-in", self.text)
        self.assertIn("no keys, firmware, games or system files", self.text.lower())
        self.assertIn("Not affiliated with Nintendo", self.text)
        self.assertIn("emulator-debug", self.text)
        self.assertIn("Measured first on the Arc 140T", self.text)
        self.assertIn("Measured on the Arc 140T only", self.text)
        self.assertIn("```mermaid", self.text)
        # Links into the README from elsewhere keep working after the agents section was renamed.
        self.assertIn('<a name="ai-native-plug-in-your-agent"></a>', self.text)

    def test_badges_images_and_languages(self):
        for needed in ("img.shields.io/github/v/release", "build-check.yml", "license-GPL", "discord-urz7vMdRM",
                       "assets/readme/hero.svg", "assets/readme/hero-light.svg", "assets/readme/frametime.svg",
                       "assets/readme/frametime-light.svg", "assets/readme/metrics.svg",
                       "assets/readme/metrics-light.svg", "assets/readme/pacing.svg", "assets/readme/pacing-light.svg"):
            self.assertIn(needed, self.text)
        for language in ("zh-CN", "es", "pt-BR", "ja", "ko", "fr", "de", "ru", "ar"):
            self.assertIn(f"docs/i18n/README.{language}.md", self.text)

    def test_each_one_liner_is_in_the_readme(self):
        for command in ('claude "Read AGENTS.md', 'codex "Read AGENTS.md', "`gemini`", 'agent "Read AGENTS.md',
                        "aider --message", 'cline "Read AGENTS.md'):
            self.assertIn(command, self.text)

    def test_local_links_exist(self):
        targets = re.findall(r"\]\(([^)\s]+)\)", self.text) + re.findall(r'(?:href|src|srcset)="([^"]+)"', self.text)
        for target in targets:
            if re.match(r"[a-z]+:|#", target):
                continue
            path = target.split("#")[0]
            self.assertTrue(os.path.exists(os.path.join(ROOT, path)), target)

    def test_the_docs_map_and_the_docs_index_agree(self):
        section = self.text.split("\n## Docs\n")[1].split("\n## ")[0]
        index = read("docs", "README.md")
        for path in DOCS_MAP:
            self.assertIn(f"]({path})", section, path)
            if path != "docs/README.md":
                target = path[len("docs/"):] if path.startswith("docs/") else "../" + path
                self.assertIn(f"]({target})", index, path)

    def test_the_artwork_is_small_self_contained_svg(self):
        paths = glob.glob(os.path.join(ROOT, "assets", "readme", "*.svg"))
        self.assertGreaterEqual(len(paths), 8)
        for path in paths:
            self.assertLess(os.path.getsize(path), 60 * 1024, path)
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            root = ElementTree.fromstring(text)
            self.assertEqual(root.tag, "{http://www.w3.org/2000/svg}svg", path)
            self.assertIsNotNone(root.find("{http://www.w3.org/2000/svg}title"), path)
            self.assertNotRegex(text, r'(?i)<script|<foreignObject|href="https?:', path)

    def test_translations_carry_the_discord_link(self):
        paths = glob.glob(os.path.join(ROOT, "docs", "i18n", "README*.md"))
        self.assertGreaterEqual(len(paths), 10)
        for path in paths:
            with open(path, encoding="utf-8") as handle:
                self.assertIn(DISCORD, handle.read(), path)


if __name__ == "__main__":
    unittest.main()
