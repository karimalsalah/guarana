#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks that Guarana does not install or announce itself under upstream Lichii's identity (issue
#107): the desktop and AppStream id, the MIME package, the Wayland app id, the theme icon and the
macOS bundle id are Guarana's, and the built-in console news reads Guarana's releases.
Source checks only (nothing is built): python -I tools/ci/tests/test_app_identity.py
"""

import os
import re
import unittest
from pathlib import Path

ROOT = Path(os.path.dirname(os.path.abspath(__file__))).parent.parent.parent
APP_ID = "io.github.karimalsalah.guarana"
UPSTREAM_ID = "io.github.zortos293.lichii"
REPO = "karimalsalah/guarana"


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


class DesktopIdentityTests(unittest.TestCase):
    def test_the_dist_files_carry_the_id_in_their_names(self):
        for suffix in ("desktop", "metainfo.xml", "xml"):
            self.assertTrue(ROOT.joinpath("dist", f"{APP_ID}.{suffix}").is_file(), suffix)
            self.assertFalse(ROOT.joinpath("dist", f"{UPSTREAM_ID}.{suffix}").exists(), suffix)

    def test_the_desktop_entry_names_its_icon_after_the_id(self):
        entry = read("dist", f"{APP_ID}.desktop")
        self.assertRegex(entry, rf"(?m)^Icon={re.escape(APP_ID)}\s*$")
        self.assertNotIn(UPSTREAM_ID, entry)

    def test_the_appstream_id_and_launchable_match(self):
        meta = read("dist", f"{APP_ID}.metainfo.xml")
        self.assertIn(f"<id>{APP_ID}</id>", meta)
        self.assertIn(f'<launchable type="desktop-id">{APP_ID}.desktop</launchable>', meta)
        self.assertNotIn(UPSTREAM_ID, meta)

    def test_the_mime_package_icons_are_the_id(self):
        mime = read("dist", f"{APP_ID}.xml")
        icons = re.findall(r'<icon name="([^"]+)"/>', mime)
        self.assertTrue(icons)
        self.assertEqual(set(icons), {APP_ID})

    def test_cmake_installs_the_renamed_files(self):
        cmake = read("CMakeLists.txt")
        for name in (f"dist/{APP_ID}.desktop", f"dist/{APP_ID}.xml", f"dist/{APP_ID}.metainfo.xml",
                     f'RENAME "{APP_ID}.png"'):
            self.assertIn(name, cmake)
        self.assertNotIn(UPSTREAM_ID, cmake)

    def test_the_appimage_uses_the_renamed_desktop_entry_and_icon(self):
        workflow = read(".github", "workflows", "release-binaries.yml")
        self.assertIn(f"dist/{APP_ID}.desktop", workflow)
        self.assertIn(f'icon="$RUNNER_TEMP/{APP_ID}.png"', workflow)
        self.assertNotIn(UPSTREAM_ID, workflow)

    def test_the_running_program_announces_the_same_id(self):
        # Wayland matches a window to its desktop entry by this name.
        self.assertIn(f'setDesktopFileName(QStringLiteral("{APP_ID}"))', read("src", "lichii", "main.cpp"))
        self.assertIn(f'QIcon::fromTheme(QStringLiteral("{APP_ID}")', read("src", "lichii", "about_dialog.cpp"))
        plist = read("src", "lichii", "Info.plist")
        self.assertRegex(plist, rf"<key>CFBundleIdentifier</key>\s*<string>{re.escape(APP_ID)}</string>")


class BuiltinNewsTests(unittest.TestCase):
    def test_the_news_repository_is_guaranas(self):
        cmake = read("CMakeModules", "GenerateSCMRev.cmake")
        self.assertIn(f'set(BUILD_NEWS_REPO "{REPO}")', cmake)
        self.assertIn(f'set(BUILD_NEWS_API_PATH "/repos/{REPO}/releases?per_page=100")', cmake)
        template = read("src", "common", "scm_rev.cpp.in")
        for name in ("BUILD_NEWS_API", "BUILD_NEWS_REPO", "BUILD_NEWS_API_PATH"):
            self.assertIn(f'#define {name} "@{name}@"', template)

    def test_the_news_fetch_reads_the_news_repository(self):
        net = read("src", "common", "net", "net.cpp")
        body = net[net.index("std::optional<std::string> GetReleasesBody()"):]
        body = body[:body.index("\n}\n") if "\n}\n" in body else body.index("\r\n}\r\n")]
        self.assertIn("g_build_news_api_path", body)
        self.assertIn("g_build_news_api", body)
        self.assertNotIn("g_build_auto_update", body)

    def test_the_news_entries_name_guarana_and_github(self):
        news = read("src", "core", "hle", "service", "bcat", "news", "builtin_news.cpp")
        self.assertEqual(news.count("ListFromJson(body, Common::g_build_news_api, Common::g_build_news_repo)"), 2)
        self.assertNotIn("g_build_auto_update", news)
        for upstream in ("Forgejo", "git.eden-emu.dev", '"Eden"'):
            self.assertNotIn(upstream, news)
        self.assertIn('w.WriteString("Open GitHub");', news)


if __name__ == "__main__":
    unittest.main()
