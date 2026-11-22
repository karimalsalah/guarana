#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for tools/ingest/evidence.py: counts per GPU family, game and build, and the effects of paired
tools/bench rows. No network.

  python -I tools/ingest/tests/test_evidence.py
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))  # -I leaves the script's directory off sys.path.

import device_classes  # noqa: E402
import evidence  # noqa: E402
import ingest  # noqa: E402

OOT = "The Legend of Zelda: Ocarina of Time"


def run(fps=20.0, p99=60.0, switches=(), settings=None, at="10:00", machine="6bfb7b55b5", scene="oot-hyrule-field",
        build="v20.4.0", power="ac", quiet=True, within=None, variant=None, day="2026-11-20"):
    return {"variant": variant or (",".join(switches) or "baseline"), "scene": scene, "fps": fps, "p50_ms": 50.0,
            "p99_ms": p99, "switches": list(switches), "settings": dict(settings or {}),
            "measured": f"{day}T{at}:00Z" if at else None, "machine_id": machine, "build": build, "power": power,
            "quiet": quiet, "within_2ms_pct": within, "shown_fps": None, "warnings": []}


def record(issue, runs=(), family="IntelArrowLakeH", status="family", candidates=(), device="ASUS Zenbook S 14 (UX5406)",
           cpu="Intel Core Ultra 9 285H", game=OOT, build="v20.4.0", power_source="Plugged in",
           option="Intel Arc 140T / 140V", kind="integrated", vendor="intel"):
    source = "tools/bench" if runs else "manual"
    return {
        "issue": issue, "device": device, "cpu": cpu, "gpu": option, "gpu_option": option, "gpu_vendor": vendor,
        "gpu_type": kind, "game": game, "game_id": "oot", "title_id": "0400486024DEC000", "build": build,
        "power_source": power_source, "source": source, "scene_ok": True if runs else None,
        "fps": runs[0]["fps"] if runs else 20.0, "runs": list(runs),
        "device_class": {"family": family if status == "family" else None, "status": status,
                         "candidates": list(candidates), "from": device_classes.FROM_BENCH, "name": None},
    }


def doc(*records, updated="2026-11-20"):
    return {"schema_version": 1, "updated": updated, "reports": {str(r["issue"]): r for r in records}}


def gm_pair(off_fps=15.0, on_fps=22.5, off_p99=96.0, on_p99=51.0, **kwargs):
    return [run(off_fps, off_p99, ["EDEN_NX2_GM=0"], at="10:00", **kwargs),
            run(on_fps, on_p99, ["EDEN_NX2_GM=11"], at="10:05", **kwargs)]


class EmptyTests(unittest.TestCase):
    def test_no_reports_lists_every_family_with_nothing(self):
        out = evidence.build(doc())
        self.assertEqual(list(out["families"]), list(device_classes.FAMILIES))
        for summary in out["families"].values():
            self.assertEqual((summary["reports"], summary["devices"], summary["verified_devices"],
                              summary["paired_devices"]), (0, 0, 0, 0))
        self.assertEqual((out["coverage"], out["effects"], out["unresolved"]), ([], [], []))
        self.assertEqual(out["outside"], {"reports": 0, "issues": []})
        self.assertEqual((out["updated"], out["reports"]), ("2026-11-20", 0))
        self.assertIsNone(evidence.build({})["updated"])


class CoverageTests(unittest.TestCase):
    def test_devices_are_counted_once_and_reports_each_time(self):
        out = evidence.build(doc(
            record(1), record(2, device="ASUS Zenbook S 14"), record(3, device="Asus Zenbook S14"),  # one model
            record(4, device="Lenovo Yoga Pro 7", cpu="Intel(R) Core(TM) Ultra 7 255H"),
        ))
        cell, = out["coverage"]
        self.assertEqual((cell["family"], cell["game"], cell["title_id"], cell["build"]),
                         ("IntelArrowLakeH", OOT, "0400486024DEC000", "v20.4.0"))
        self.assertEqual((cell["reports"], cell["devices"], cell["issues"]), (4, 2, [1, 2, 3, 4]))
        self.assertEqual(out["families"]["IntelArrowLakeH"]["devices"], 2)

    def test_tiers_power_and_builds(self):
        out = evidence.build(doc(
            record(1, power_source="On battery"),
            record(2, [run(20.0)]),
            record(3, gm_pair(), device="MSI Claw 8 AI+", cpu="Intel Core Ultra 7 258V"),
            record(4, build="v20.3.0", power_source=None),
        ))
        first, second = out["coverage"]
        self.assertEqual((first["build"], second["build"]), ("v20.3.0", "v20.4.0"))
        self.assertEqual((second["reports"], second["verified_reports"], second["paired_reports"]), (3, 2, 1))
        self.assertEqual((second["devices"], second["verified_devices"], second["paired_devices"]), (2, 2, 1))
        self.assertEqual((second["ac"], second["battery"], second["power_unknown"]), (2, 1, 0))
        self.assertEqual(first["power_unknown"], 1)
        # A tools/bench report's build and power are its rows', not the typed answers.
        cell, = evidence.build(doc(record(5, [run(build="v20.5.0", power="battery")], build="20.4")))["coverage"]
        self.assertEqual((cell["build"], cell["battery"]), ("v20.5.0", 1))

    def test_unresolved_and_outside(self):
        out = evidence.build(doc(
            record(1, family=None, status="unresolved", candidates=["IntelArrowLakeH", "IntelArcLunarLake"]),
            record(2, family=None, status="unresolved", candidates=["IntelArrowLakeH", "IntelArcLunarLake"]),
            record(3, family=None, status="none", option="NVIDIA RTX 40 series", kind="discrete", vendor="nvidia"),
            record(4, family="NotAFamily"),
        ))
        self.assertEqual(out["unresolved"], [
            {"answer": "Intel Arc 140T / 140V", "candidates": [], "reports": 1, "issues": [4]},
            {"answer": "Intel Arc 140T / 140V", "candidates": ["IntelArrowLakeH", "IntelArcLunarLake"],
             "reports": 2, "issues": [1, 2]},
        ])
        self.assertEqual(out["outside"], {"reports": 1, "issues": [3]})
        self.assertEqual(out["coverage"], [])

    def test_a_record_without_a_device_class_is_classified_from_its_gpu_fields(self):
        old = record(1, [run()])
        del old["device_class"]
        old["gpu"] = "Intel Arc 140T GPU"
        self.assertEqual(evidence.build(doc(old))["coverage"][0]["family"], "IntelArrowLakeH")
        manual = record(2)
        del manual["device_class"]
        self.assertEqual(evidence.build(doc(manual))["unresolved"][0]["candidates"],
                         ["IntelArrowLakeH", "IntelArcLunarLake"])


class PairTests(unittest.TestCase):
    def effects(self, *records):
        return evidence.build(doc(*records))["effects"]

    def test_two_rows_that_both_set_the_switch_are_a_pair(self):
        effect, = self.effects(record(1, gm_pair()))
        self.assertEqual((effect["family"], effect["scene"], effect["change"], effect["set"], effect["against"]),
                         ("IntelArrowLakeH", "oot-hyrule-field", "EDEN_NX2_GM=11 against EDEN_NX2_GM=0",
                          {"EDEN_NX2_GM": "11"}, {"EDEN_NX2_GM": "0"}))
        self.assertEqual((effect["build"], effect["power"], effect["context"]), ("v20.4.0", "ac", []))
        self.assertEqual((effect["pairs"], effect["devices"], effect["issues"]), (1, 1, [1]))
        self.assertEqual(effect["fps_change_pct"], {"min": 50.0, "median": 50.0, "max": 50.0})
        self.assertEqual(effect["p99_change_pct"]["median"], -46.9)
        self.assertIsNone(effect["within_2ms_change_pt"])

    def test_the_baseline_pairs_with_nothing(self):
        # The baseline sets nothing: with the shipped nx2_switches.txt it may already run EDEN_NX2_GM=11.
        self.assertEqual(self.effects(record(1, [run(14.75), run(22.83, switches=["EDEN_NX2_GM=11"], at="10:07")])), [])

    def test_rows_that_are_not_one_session_on_one_machine_are_not_a_pair(self):
        cases = {
            "another scene": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], scene="x")],
            "over two hours": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], at="12:01")],
            "another day": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], day="2026-11-21")],
            "another machine": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], machine="0123456789")],
            "another build": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], build="v20.5.0")],
            "mains and battery": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], power="battery")],
            "background CPU not measured": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], quiet=False)],
            "no time": [run(15.0, switches=["EDEN_NX2_GM=0"]), run(22.0, switches=["EDEN_NX2_GM=11"], at=None)],
            "a switch only one row sets": [run(15.0, switches=["EDEN_NX2_GM=0"]),
                                           run(22.0, switches=["EDEN_NX2_GM=11", "EDEN_NX2_FLUID=2"])],
            "the same switches": [run(15.0, switches=["EDEN_NX2_GM=11"]), run(22.0, switches=["EDEN_NX2_GM=11"])],
        }
        for name, runs in cases.items():
            with self.subTest(name):
                self.assertEqual(self.effects(record(1, runs)), [])

    def test_off_comes_first_whatever_ran_first(self):
        effect, = self.effects(record(1, [run(22.0, switches=["EDEN_NX2_EXTRAPOLATE=1"], at="10:00"),
                                          run(20.0, switches=["EDEN_NX2_EXTRAPOLATE=0"], at="10:04")]))
        self.assertEqual(effect["change"], "EDEN_NX2_EXTRAPOLATE=1 against EDEN_NX2_EXTRAPOLATE=0")
        self.assertEqual(effect["fps_change_pct"]["median"], 10.0)
        effect, = self.effects(record(1, [run(25.0, switches=["EDEN_NX2_GM=11"], at="10:00"),
                                          run(20.0, switches=["EDEN_NX2_GM=3"], at="10:04")]))
        self.assertEqual(effect["change"], "EDEN_NX2_GM=11 against EDEN_NX2_GM=3")

    def test_abab_pairs_each_set_row_with_the_nearest_other(self):
        runs = [run(15.0, switches=["EDEN_NX2_GM=0"], at="10:00"), run(22.5, switches=["EDEN_NX2_GM=11"], at="10:05"),
                run(16.0, switches=["EDEN_NX2_GM=0"], at="10:10"), run(24.0, switches=["EDEN_NX2_GM=11"], at="10:15")]
        effect, = self.effects(record(1, runs))
        self.assertEqual((effect["pairs"], effect["devices"]), (2, 1))
        # 22.5 against 15 (nearest, earlier on a tie), 24 against 16: +50% both.
        self.assertEqual(effect["per_device"], [{"pairs": 2, "fps_change_pct": 50.0, "p99_change_pct": 0.0,
                                                 "within_2ms_change_pt": None}])

    def test_context_settings_and_power_keep_effects_apart(self):
        fluid = [run(15.0, switches=["EDEN_NX2_FLUID=2", "EDEN_NX2_GM=0"], at="11:00", within=90.0),
                 run(21.0, switches=["EDEN_NX2_FLUID=2", "EDEN_NX2_GM=11"], at="11:05", within=95.5)]
        resolution = [run(30.0, settings={"Renderer/resolution_setup": "2"}, at="12:00"),
                      run(24.0, settings={"Renderer/resolution_setup": "3"}, at="12:05")]
        battery = gm_pair(power="battery")
        effects = self.effects(record(1, gm_pair() + fluid + resolution), record(2, battery, device="ROG Ally X"))
        self.assertEqual([(e["change"], e["context"], e["power"]) for e in effects], [
            ("EDEN_NX2_GM=11 against EDEN_NX2_GM=0", [], "ac"),
            ("EDEN_NX2_GM=11 against EDEN_NX2_GM=0", [], "battery"),
            ("EDEN_NX2_GM=11 against EDEN_NX2_GM=0", ["EDEN_NX2_FLUID=2"], "ac"),
            ("setting:Renderer/resolution_setup=3 against setting:Renderer/resolution_setup=2", [], "ac"),
        ])
        self.assertEqual(effects[2]["within_2ms_change_pt"]["median"], 5.5)
        self.assertEqual(effects[3]["fps_change_pct"]["median"], -20.0)

    def test_devices_get_one_value_each_and_the_summary_is_over_devices(self):
        effect, = self.effects(
            record(1, gm_pair(15.0, 22.5)),
            record(2, gm_pair(15.0, 21.0), device="ROG Ally X"),
            record(3, gm_pair(20.0, 21.0), device="MSI Claw 8 AI+"),
            record(4, gm_pair(20.0, 22.0), device="MSI Claw 8 AI+"),  # the same device again
        )
        self.assertEqual((effect["pairs"], effect["devices"], effect["issues"]), (4, 3, [1, 2, 3, 4]))
        self.assertEqual([item["fps_change_pct"] for item in effect["per_device"]], [7.5, 40.0, 50.0])
        self.assertEqual(effect["fps_change_pct"], {"min": 7.5, "median": 40.0, "max": 50.0})

    def test_only_reports_with_a_family_have_effects(self):
        self.assertEqual(self.effects(record(1, gm_pair(), family=None, status="unresolved")), [])
        self.assertEqual(self.effects(record(1, gm_pair(), family=None, status="none")), [])


class FileTests(unittest.TestCase):
    def test_the_committed_file_is_what_the_committed_records_give(self):
        community = os.path.join(evidence.ROOT, "site", "data", "community.json")
        with contextlib.redirect_stderr(io.StringIO()) as err:
            status = evidence.main(["--community", community, "--out", evidence.OUT_PATH, "--check"])
        self.assertEqual(status, 0, err.getvalue() + " Run python -I tools/ingest/evidence.py and commit the file.")

    def test_check_write_and_a_stable_file(self):
        with tempfile.TemporaryDirectory() as temp:
            source, out = os.path.join(temp, "community.json"), os.path.join(temp, "data", "evidence.json")
            with open(source, "w", encoding="utf-8") as handle:
                json.dump(doc(record(1, gm_pair()), record(2)), handle)
            with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(evidence.main(["--community", source, "--out", out, "--check"]), 1)
                self.assertEqual(evidence.main(["--community", source, "--out", out]), 0)
                self.assertEqual(evidence.main(["--community", source, "--out", out, "--check"]), 0)
            with open(out, "rb") as handle:
                raw = handle.read()
            self.assertNotIn(b"\r", raw)
            self.assertEqual(json.loads(raw)["effects"][0]["issues"], [1])
            with open(source, encoding="utf-8") as handle:
                self.assertEqual(evidence.dump(evidence.build(json.load(handle))), raw.decode("utf-8"))

    def test_the_document_carries_its_field_docs_and_source(self):
        out = evidence.build(doc())
        self.assertEqual(out["fields"], evidence.FIELD_DOCS)
        self.assertEqual((out["schema_version"], out["source"]), (1, "site/data/community.json"))
        self.assertEqual(evidence.DATA_PATH, "site/data/community-evidence.json")
        self.assertEqual(os.path.dirname(evidence.DATA_PATH), os.path.dirname(ingest.DATA_PATH))


if __name__ == "__main__":
    unittest.main()
