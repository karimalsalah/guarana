#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class SwitchDiscovery(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        self.bin = self.root / "mount/usr/bin"
        self.bin.mkdir(parents=True)
        self.config = self.root / "config/lichii"
        self.config.mkdir(parents=True)
        source = self.root / "probe.cpp"
        source.write_text('''#include "lichii/nx2_switches.h"
#include <iostream>
namespace Common::FS {
std::filesystem::path GetDataDirectory(const std::string&) {
    return std::getenv("XDG_CONFIG_HOME");
}
}
int main(int argc, char** argv) {
    LoadNx2Switches(argv[0]);
    const char* name = argc > 1 ? argv[1] : "EDEN_NX2_PRELOAD_ALL_PIPELINES";
    if (argc > 2) {
        // "origin": the value the shipped copy alone put in the environment ("-" when it did not),
        // then "set" when the loader put the name there itself ("-" when it did not).
        const auto shipped = Common::SwitchOrigin::ShippedOnlyValue(name);
        std::cout << (shipped ? *shipped : std::string{"-"}) << ' '
                  << (Common::SwitchOrigin::SetByProcess().contains(name) ? "set" : "-");
        return 0;
    }
    const char* value = std::getenv(name);
    std::cout << (value ? value : "unset");
}
''')
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "src"), str(source), str(ROOT / "src/common/switch_origin.cpp"), "-o", str(self.bin / "lichii")], check=True)

    def tearDown(self):
        self.tmp.cleanup()

    def run_probe(self, executable, name="EDEN_NX2_PRELOAD_ALL_PIPELINES", mode=None, **env):
        clean = os.environ.copy()
        clean.pop("EDEN_NX2_PRELOAD_ALL_PIPELINES", None)
        clean.pop(name, None)
        clean.pop("APPIMAGE", None)
        clean["XDG_CONFIG_HOME"] = str(self.config.parent)
        clean.update(env)
        command = [str(executable), name] + ([mode] if mode else [])
        return subprocess.check_output(command, env=clean, cwd=self.root, text=True)

    def test_only_the_shipped_copy_counts_as_shipped(self):
        # A game's switch defaults replace only what the copy next to the executable alone set;
        # every other file is the player's, and so is the real environment.
        name = "EDEN_NX2_PRELOAD_ALL_PIPELINES"
        origin = lambda **env: self.run_probe(self.bin / "lichii", mode="origin", **env)
        (self.bin / "nx2_switches.txt").write_text(f"{name}=1\n")
        self.assertEqual(origin(), "1 set")
        self.assertEqual(origin(**{name: "1"}), "- -")
        # The player's own copy, at the shipped value or another one.
        for value in ("1", "0"):
            with self.subTest(value=value):
                (self.config / "nx2_switches.txt").write_text(f"{name}={value}\n")
                self.assertEqual(origin(), "- set")
        (self.config / "nx2_switches.txt").unlink()
        outer = self.root / "app"
        outer.mkdir()
        image = outer / "Lichii.AppImage"
        image.touch()
        (outer / "nx2_switches.txt").write_text(f"{name}=1\n")
        self.assertEqual(origin(APPIMAGE=str(image)), "- set")
        self.assertEqual(origin(), "1 set")
        portable = self.root / "user"
        portable.mkdir()
        self.assertEqual(origin(), "1 set")
        (portable / "nx2_switches.txt").write_text(f"{name}=2\n")
        self.assertEqual(origin(), "- set")

    def test_untrusted_sidecars_cannot_set_host_environment(self):
        outer = self.root / "app"
        outer.mkdir()
        image = outer / "Lichii.AppImage"
        image.touch()
        forbidden = ("LD_PRELOAD", "LD_LIBRARY_PATH", "LIBVULKAN_PATH", "QT_PLUGIN_PATH",
                     "PATH", "HOME", "XDG_CONFIG_HOME", "LICHII_DO_STARTUP_CHECKS",
                     "EDEN_STREAMLINE_DIR")
        for folder in (self.bin, outer):
            sidecar = folder / "nx2_switches.txt"
            sidecar.write_text("".join(f"{name}=/untrusted\n" for name in forbidden))
            for name in forbidden:
                with self.subTest(folder=folder, name=name):
                    expected = str(self.config.parent) if name == "XDG_CONFIG_HOME" else "unset"
                    self.assertEqual(self.run_probe(self.bin / "lichii", name, APPIMAGE=str(image)), expected)
            sidecar.unlink()

    def test_diagnostics_and_explicit_user_environment(self):
        supported = ("EDEN_NX2_PRELOAD_ALL_PIPELINES", "EDEN_DKB_FRAME_TRACE",
                     "EDEN_DKB_INDIRECT_READBACK", "EDEN_NATIVE_DRAW_CAPTURE",
                     "EDEN_NATIVE_DRAW_DIAGNOSTICS", "EDEN_NATIVE_FRAME_CAPTURE",
                     "EDEN_NATIVE_SHADER_OVERRIDE_DIR", "EDEN_PUNCH_HOLE",
                     "EDEN_STARFOX_WINDOW_TRACE", "EDEN_VI2_MAX_RESOLUTION_MODE",
                     "EGLANTINE_EXPERIMENTAL_STATIC_VERTEX_REPACK", "EGLANTINE_TEST_VERTEX_REPACK")
        (self.bin / "nx2_switches.txt").write_text("".join(f"{name}=1\n" for name in supported))
        for name in supported:
            with self.subTest(name=name):
                self.assertEqual(self.run_probe(self.bin / "lichii", name), "1")
                self.assertEqual(self.run_probe(self.bin / "lichii", name, **{name: "2"}), "2")
        (self.config / "nx2_switches.txt").write_text("EDEN_STREAMLINE_DIR=/trusted\n")
        self.assertEqual(self.run_probe(self.bin / "lichii", "EDEN_STREAMLINE_DIR"), "/trusted")

    def test_invalid_config_base_does_not_search_cwd(self):
        cwd_config = self.root / "lichii"
        cwd_config.mkdir()
        (cwd_config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=99\n")
        for base in ("", "."):
            with self.subTest(base=base):
                self.assertEqual(self.run_probe(self.bin / "lichii", XDG_CONFIG_HOME=base), "unset")

    def test_elf_path_symlink_and_precedence(self):
        (self.root / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=99\n")
        self.assertEqual(self.run_probe(self.bin / "lichii"), "unset")
        (self.bin / "nx2_switches.txt").write_text('EDEN_NX2_PRELOAD_ALL_PIPELINES="1"\n')
        linkdir = self.root / "links"
        linkdir.mkdir()
        (linkdir / "lichii").symlink_to(self.bin / "lichii")
        self.assertEqual(self.run_probe("lichii", PATH=str(linkdir)), "1")
        (self.config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=2\n")
        self.assertEqual(self.run_probe(linkdir / "lichii"), "2")
        self.assertEqual(self.run_probe(linkdir / "lichii", EDEN_NX2_PRELOAD_ALL_PIPELINES="3"), "3")

    def test_appimage_external_overrides_internal(self):
        (self.bin / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=1\n")
        outer = self.root / "app"
        outer.mkdir()
        image = outer / "Lichii.AppImage"
        image.touch()
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "1")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(outer / "missing.AppImage")), "1")
        (outer / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=2\n")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "2")
        link = self.root / "link.AppImage"
        link.symlink_to(image)
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(link)), "2")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE="app/Lichii.AppImage"), "1")
        (self.config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=3\n")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "3")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image), EDEN_NX2_PRELOAD_ALL_PIPELINES="4"), "4")

    def test_portable_user_folder_replaces_per_user_copy(self):
        # Outside Windows the path manager looks for the portable "user" folder in the current
        # directory, and run_probe starts the probe in self.root.
        (self.bin / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=1\n")
        (self.config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=3\n")
        self.assertEqual(self.run_probe(self.bin / "lichii"), "3")
        portable = self.root / "user"
        portable.mkdir()
        self.assertEqual(self.run_probe(self.bin / "lichii"), "1")
        (portable / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=2\n")
        self.assertEqual(self.run_probe(self.bin / "lichii"), "2")
        self.assertEqual(self.run_probe(self.bin / "lichii", EDEN_NX2_PRELOAD_ALL_PIPELINES="4"), "4")
        outer = self.root / "app"
        outer.mkdir()
        image = outer / "Lichii.AppImage"
        image.touch()
        (outer / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=5\n")
        self.assertEqual(self.run_probe(self.bin / "lichii", APPIMAGE=str(image)), "2")

    def test_portable_copy_is_limited_like_sidecars(self):
        portable = self.root / "user"
        portable.mkdir()
        (portable / "nx2_switches.txt").write_text("LD_PRELOAD=/untrusted\nEDEN_STREAMLINE_DIR=/untrusted\n")
        (self.config / "nx2_switches.txt").write_text("EDEN_STREAMLINE_DIR=/trusted\n")
        for name in ("LD_PRELOAD", "EDEN_STREAMLINE_DIR"):
            with self.subTest(name=name):
                self.assertEqual(self.run_probe(self.bin / "lichii", name), "unset")

    def test_user_file_is_not_portable_mode(self):
        (self.root / "user").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=2\n")
        (self.config / "nx2_switches.txt").write_text("EDEN_NX2_PRELOAD_ALL_PIPELINES=3\n")
        self.assertEqual(self.run_probe(self.bin / "lichii"), "3")


if __name__ == "__main__":
    unittest.main()
