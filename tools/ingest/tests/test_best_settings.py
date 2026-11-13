#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ingest/best_settings.py. No network.

  python -I tools/ingest/tests/test_best_settings.py
"""

import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # -I leaves the script's directory off sys.path.

import best_settings  # noqa: E402


def row(issue, gpu_option="Intel Arc 140T / 140V", vendor="intel", kind="integrated", fps=20.0, p99=60.0,
        switches=(), game_id="oot", scene_ok=True, source="tools/bench", form="Laptop", notes=(),
        frame_generation="off", power_source="Plugged in"):
    return {
        "issue": issue, "gpu": gpu_option, "gpu_option": gpu_option, "gpu_vendor": vendor, "gpu_type": kind,
        "form_factor": form, "game_id": game_id, "game": "Ocarina" if game_id == "oot" else "Other game",
        "fps": fps, "p50_ms": 45.0, "p99_ms": p99, "scene_ok": scene_ok if source == "tools/bench" else None,
        "source": source, "switches": list(switches), "notes": list(notes), "build": "20.0.2",
        "driver": "32.0.101.9175", "frame_generation": frame_generation, "power_source": power_source,
    }


def doc(*rows, updated="2026-11-20"):
    return {"schema_version": 1, "updated": updated, "reports": {str(r["issue"]): r for r in rows}}


class ClassTests(unittest.TestCase):
    def test_classes(self):
        cases = [
            (dict(), "intel-arc-igpu"),
            (dict(gpu_option="Intel Arc (Meteor Lake)"), "intel-arc-igpu"),
            (dict(gpu_option="Intel Arc A / B series", kind="discrete"), "other"),
            (dict(gpu_option="Intel Iris Xe / UHD"), "other"),
            (dict(gpu_option="AMD Radeon 780M / 760M", vendor="amd"), "amd-rdna3-igpu"),
            (dict(gpu_option="AMD Radeon 880M / 890M", vendor="amd"), "amd-rdna3-igpu"),
            (dict(gpu_option="AMD Ryzen Z1 / Z2", vendor="amd", form="Handheld"), "amd-rdna3-igpu"),
            (dict(gpu_option="AMD integrated", vendor="amd"), "other"),
            (dict(gpu_option="NVIDIA RTX 40 series", vendor="nvidia", kind="discrete", form="Desktop"), "nvidia-rtx-desktop"),
            (dict(gpu_option="NVIDIA RTX 40 series", vendor="nvidia", kind="discrete", form="Mini PC"), "nvidia-rtx-desktop"),
            (dict(gpu_option="NVIDIA RTX 40 series", vendor="nvidia", kind="discrete", form="Laptop"), "nvidia-rtx-laptop"),
            (dict(gpu_option="NVIDIA RTX 30 series or older", vendor="nvidia", kind="discrete", form="Handheld"), "nvidia-rtx-laptop"),
            (dict(gpu_option="AMD RX 7000 series", vendor="amd", kind="discrete", form="Desktop"), "amd-desktop"),
            (dict(gpu_option="Other", vendor="other", kind="unknown"), "other"),
        ]
        for kwargs, expected in cases:
            with self.subTest(kwargs=kwargs):
                self.assertEqual(best_settings.gpu_class(row(1, **kwargs)), expected)

    def test_gpu_name_overwritten_by_bench_does_not_matter(self):
        r = row(1)
        r["gpu"] = "Intel(R) Arc(TM) 140T GPU (16GB)"
        self.assertEqual(best_settings.gpu_class(r), "intel-arc-igpu")


class SettingsTests(unittest.TestCase):
    def test_bench_row_lists_off_switches_as_false(self):
        s = best_settings.row_settings(row(1, switches=["EDEN_NX2_FLUID=1"]))
        self.assertEqual(s, {"fluid_presenter": True, "gm_routing": False, "igpu_power_policy": False,
                             "frame_generation": False})

    def test_manual_row_states_only_what_it_names(self):
        s = best_settings.row_settings(row(1, source="manual", switches=["EDEN_NX2_GM=11"], frame_generation=None))
        self.assertEqual(s, {"gm_routing": True})

    def test_values(self):
        s = best_settings.row_settings(row(1, switches=["EDEN_NX2_FLUID=0", "EDEN_NX2_EXTRAPOLATE=0",
                                                        "EDEN_NX2_POWER_POLICY=igpu", "EDEN_NX2_GM=11"],
                                           frame_generation="on"))
        self.assertEqual(s, {"fluid_presenter": False, "ocarina_extrapolation": False, "gm_routing": True,
                             "igpu_power_policy": True, "frame_generation": True})

    def test_unrelated_switches_are_ignored(self):
        s = best_settings.row_settings(row(1, switches=["EDEN_NX2_SHOWN_STATS=1", "garbage"], source="manual",
                                           frame_generation=None))
        self.assertEqual(s, {})


class BuildTests(unittest.TestCase):
    def test_all_classes_always_present(self):
        out = best_settings.build(doc())
        self.assertEqual(list(out["classes"]), list(best_settings.CLASSES))
        for entry in out["classes"].values():
            self.assertEqual((entry["samples"], entry["validated"], entry["best"], entry["settings"]), (0, 0, None, {}))
        self.assertEqual(out["schema_version"], 1)
        self.assertEqual(out["updated"], "2026-11-20")

    def test_best_validated_row_wins_with_counts(self):
        out = best_settings.build(doc(
            row(1, fps=18.0, switches=[]),
            row(2, fps=24.0, switches=["EDEN_NX2_FLUID=1"]),
            row(3, fps=22.0, switches=["EDEN_NX2_GM=11"]),
            row(4, fps=40.0, source="manual", switches=["EDEN_NX2_GM=11"]),   # not validated
            row(5, fps=50.0, scene_ok=False),                               # scene check failed
        ))
        arc = out["classes"]["intel-arc-igpu"]
        self.assertEqual((arc["samples"], arc["validated"]), (5, 3))
        self.assertEqual(arc["best"]["issue"], 2)
        self.assertEqual(arc["best"]["fps"], 24.0)
        self.assertTrue(arc["settings"]["fluid_presenter"])
        self.assertFalse(arc["settings"]["gm_routing"])
        self.assertEqual(out["classes"]["other"]["samples"], 0)

    def test_ties_go_to_the_lower_p99_then_the_older_issue(self):
        out = best_settings.build(doc(row(7, fps=20.0, p99=70.0), row(8, fps=20.0, p99=50.0), row(9, fps=20.0, p99=50.0)))
        self.assertEqual(out["classes"]["intel-arc-igpu"]["best"]["issue"], 8)

    def test_battery_rows_rank_after_mains_rows(self):
        out = best_settings.build(doc(
            row(1, fps=30.0, power_source="On battery", notes=["Measured on battery."]),
            row(2, fps=20.0),
        ))
        best = out["classes"]["intel-arc-igpu"]["best"]
        self.assertEqual(best["issue"], 2)
        self.assertFalse(best["on_battery"])

    def test_fps_of_different_games_is_not_compared(self):
        out = best_settings.build(doc(
            row(1, fps=20.0), row(2, fps=21.0), row(3, fps=60.0, game_id="dkb"),
        ))
        arc = out["classes"]["intel-arc-igpu"]
        self.assertEqual(arc["validated"], 3)
        self.assertEqual(arc["best"]["issue"], 2)   # Ocarina has two reports, the other game one

    def test_classes_are_separate(self):
        out = best_settings.build(doc(
            row(1, fps=20.0),
            row(2, gpu_option="NVIDIA RTX 40 series", vendor="nvidia", kind="discrete", form="Desktop", fps=90.0,
                switches=["EDEN_NX2_FLUID=1"]),
        ))
        self.assertEqual(out["classes"]["intel-arc-igpu"]["best"]["issue"], 1)
        self.assertEqual(out["classes"]["nvidia-rtx-desktop"]["best"]["issue"], 2)
        self.assertEqual(out["classes"]["nvidia-rtx-laptop"]["samples"], 0)

    def test_garbage_rows_are_skipped(self):
        community = {"updated": "2026-11-20", "reports": {"1": "text", "2": None, "3": row(3)}}
        out = best_settings.build(community)
        self.assertEqual(out["classes"]["intel-arc-igpu"]["samples"], 1)
        self.assertEqual(best_settings.build({})["classes"]["other"]["samples"], 0)
        self.assertIsNone(best_settings.build({})["updated"])

    def test_output_is_deterministic_json(self):
        community = doc(row(1), row(2, fps=25.0))
        first = best_settings.dump(best_settings.build(community))
        self.assertEqual(first, best_settings.dump(best_settings.build(json.loads(json.dumps(community)))))
        self.assertTrue(first.endswith("\n"))
        json.loads(first)


class CliTests(unittest.TestCase):
    def test_write_then_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "community.json")
            out = os.path.join(tmp, "data", "best-settings.json")
            with open(src, "w", encoding="utf-8") as handle:
                json.dump(doc(row(1), row(2, fps=25.0)), handle)
            self.assertEqual(best_settings.main(["--community", src, "--out", out, "--check"]), 1)
            self.assertEqual(best_settings.main(["--community", src, "--out", out]), 0)
            self.assertEqual(best_settings.main(["--community", src, "--out", out, "--check"]), 0)
            with open(out, encoding="utf-8") as handle:
                written = json.load(handle)
            self.assertEqual(written["classes"]["intel-arc-igpu"]["best"]["issue"], 2)

    def test_the_real_data_file_builds(self):
        with open(best_settings.COMMUNITY_PATH, encoding="utf-8") as handle:
            out = best_settings.build(json.load(handle))
        self.assertEqual(list(out["classes"]), list(best_settings.CLASSES))


if __name__ == "__main__":
    unittest.main()
