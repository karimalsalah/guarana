#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pipeline_stats  # noqa: E402

VERTEX = "1917b5b970b5f601"
FRAGMENT = "78134ce8125d5959"
OTHER_FRAGMENT = "52402d8e8ab55263"
COMPUTE = "5f43233f59d916f8"
ZERO = "0" * 16


def statistic(name, value, kind="uint64"):
    return {"name": name, "description": name + " of the executable", "format": kind,
            "value": value}


def executable(index, name, stages, subgroup, statistics):
    return {"index": index, "name": name, "description": "", "stages": stages,
            "subgroup_size": subgroup, "statistics": statistics}


def graphics(pipeline, fragment, executables, vertex=VERTEX, mesh=False):
    return json.dumps({"kind": "graphics", "pipeline": pipeline,
                       "hashes": [ZERO, vertex, ZERO, ZERO, ZERO, fragment], "shader": fragment,
                       "mesh": mesh, "flags": 64, "executables": executables})


def compute(pipeline, shader, executables):
    return json.dumps({"kind": "compute", "pipeline": pipeline, "hashes": [shader],
                       "shader": shader, "mesh": False, "flags": 64, "executables": executables})


FRAGMENT_SIMD16 = executable(1, "SIMD16 fragment", 0x10, 16, [
    statistic("Code size", 8192), statistic("Instruction Count", 1234),
    statistic("Spill count", 12), statistic("Fill count", 30)])
MESH = executable(0, "mesh", 0x80, 32, [statistic("Instruction Count", 400)])

LOG = (
    "[  42.9] Render.Vulkan <Info> vk_scheduler.cpp:1132:ReportWorkTimes: DKB gpu_work "
    "renderer_frame=2 timed=2 dropped=0 timed_sum_ms=17.375 submission_sum_ms=20.000 "
    "untimed_ms=2.125 emu_ms=0.500 guest_dispatch_ms=2.250 emu: emu:image_copy=0.500ms/2x(max "
    f"0.300ms size 0x0x0) slowest: s1:{VERTEX}+s5:{FRAGMENT}#m=15.125ms/40x(max 2.500ms size "
    f"1920x1080x1) s6:{COMPUTE}=2.250ms/3x(max 1.000ms size 240x135x1)\n"
    "[  43.0] Frontend <Info> an unrelated line\n"
    "[  43.9] Render.Vulkan <Info> vk_scheduler.cpp:1132:ReportWorkTimes: DKB gpu_work "
    "renderer_frame=62 timed=1 dropped=0 timed_sum_ms=5.000 submission_sum_ms=6.000 "
    f"untimed_ms=1.000 emu_ms=0.000 guest_dispatch_ms=0.000 emu: - slowest: s1:{VERTEX}+"
    f"s5:{FRAGMENT}#m=5.000ms/10x(max 1.500ms size 1920x1080x1)\n")


class PipelineStatsTest(unittest.TestCase):
    def test_load_keeps_the_last_line_of_a_pipeline_and_counts_bad_lines(self):
        lines = [
            graphics("00000000000000a1", FRAGMENT, []),
            "",
            graphics("00000000000000A1", FRAGMENT.upper(), [FRAGMENT_SIMD16]),
            compute("00000000000000c1", COMPUTE, []),
            '{"kind":"graphics","pipeline":"00000000000000a2","hashes":["12"],"executables":[]}',
            '{"kind":"graphics","pipeli',
        ]
        pipelines, bad = pipeline_stats.load_pipelines(lines)
        self.assertEqual(bad, 2)
        self.assertEqual(len(pipelines), 2)
        self.assertEqual(pipelines[0]["hashes"][5], FRAGMENT)
        self.assertEqual(pipelines[0]["executables"][0]["name"], "SIMD16 fragment")
        self.assertEqual(pipeline_stats.work_key(pipelines[0]), ("draw", VERTEX, FRAGMENT))
        self.assertEqual(pipeline_stats.work_key(pipelines[1]), ("compute", COMPUTE))

    def test_work_log_sums_the_frames(self):
        costs, frames = pipeline_stats.parse_work_log(LOG.splitlines())
        self.assertEqual(frames, 2)
        self.assertEqual(set(costs), {("draw", VERTEX, FRAGMENT), ("compute", COMPUTE)})
        draw = costs[("draw", VERTEX, FRAGMENT)]
        self.assertAlmostEqual(draw["ms"], 20.125)
        self.assertEqual(draw["count"], 50)
        self.assertAlmostEqual(draw["max_ms"], 2.5)
        self.assertEqual(draw["kinds"], {"m"})
        self.assertAlmostEqual(costs[("compute", COMPUTE)]["ms"], 2.25)

    def test_costs_json_is_found_inside_other_json(self):
        reply = {"frame": 7, "work": [
            {"vertex_hash": VERTEX, "fragment_hash": FRAGMENT, "kind": "", "compute": False,
             "draws": 4, "total_ms": 3.5, "max_ms": 1.25},
            {"vertex_hash": COMPUTE, "fragment_hash": ZERO, "kind": "", "compute": True,
             "draws": 1, "total_ms": 0.5, "max_ms": 0.5}]}
        costs, frames = pipeline_stats.load_costs(json.dumps({"result": {"gpu_costs": reply}}))
        self.assertEqual(frames, 1)
        self.assertAlmostEqual(costs[("draw", VERTEX, FRAGMENT)]["ms"], 3.5)
        self.assertAlmostEqual(costs[("compute", COMPUTE)]["ms"], 0.5)
        # No sampled frame yet: nothing to average over
        self.assertEqual(pipeline_stats.load_costs('{"frame": 0, "work": []}'), ({}, 0))
        # A log is not JSON
        self.assertEqual(pipeline_stats.load_costs(LOG)[1], 2)

    def test_highlight_and_stage_names(self):
        for name in ("Spill count", "Fill count", "Scratch Memory Size", "SIMD width",
                     "Max dispatch width", "Instruction Count", "SEND Count", "Cycle Count",
                     "Loop Count"):
            self.assertTrue(pipeline_stats.is_highlighted(name), name)
        self.assertFalse(pipeline_stats.is_highlighted("Code size"))
        self.assertEqual(pipeline_stats.stage_names(0x10), "fragment")
        self.assertEqual(pipeline_stats.stage_names(0xc0), "task+mesh")
        self.assertEqual(pipeline_stats.stage_names(0), "-")

    def test_rows_are_ordered_by_time_per_frame(self):
        pipelines, _ = pipeline_stats.load_pipelines([
            graphics("00000000000000a1", FRAGMENT, [MESH, FRAGMENT_SIMD16], mesh=True),
            graphics("00000000000000a2", FRAGMENT, [MESH, FRAGMENT_SIMD16], mesh=True),
            graphics("00000000000000a3", OTHER_FRAGMENT, [FRAGMENT_SIMD16]),
            compute("00000000000000c1", COMPUTE, [
                executable(0, "compute", 0x20, 16, [statistic("Instruction Count", 9000)])]),
        ])
        costs, frames = pipeline_stats.parse_work_log(LOG.splitlines())
        rows = pipeline_stats.build_rows(pipelines, costs, frames)
        self.assertEqual([row["shaders"] for row in rows],
                         [f"s1:{VERTEX}+s5:{FRAGMENT}", f"s6:{COMPUTE}"])
        self.assertEqual([row["kind"] for row in rows], ["mesh", "compute"])
        self.assertAlmostEqual(rows[0]["ms"], 10.0625)
        self.assertAlmostEqual(rows[0]["draws"], 25.0)
        self.assertEqual(rows[0]["pipelines"], 2)
        fragment = rows[0]["executables"][1]
        self.assertEqual((fragment["name"], fragment["stages"], fragment["subgroup_size"],
                          fragment["pipelines"]), ("SIMD16 fragment", "fragment", 16, 2))
        self.assertEqual([entry["name"] for entry in fragment["statistics"]],
                         ["Instruction Count", "Spill count", "Fill count", "Code size"])

        # Without costs: every shader, the largest program first
        rows = pipeline_stats.build_rows(pipelines)
        self.assertEqual(rows[0]["shaders"], f"s6:{COMPUTE}")
        self.assertIsNone(rows[0]["ms"])
        self.assertEqual(len(rows), 3)
        # A hash that was not measured is still found, and fragment shaders can be added up
        rows = pipeline_stats.build_rows(pipelines, costs, frames, hashes=[OTHER_FRAGMENT],
                                         by_fragment=True)
        self.assertEqual([row["shaders"] for row in rows], [f"s5:{OTHER_FRAGMENT}"])

    def test_main_prints_the_table(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, pipeline_stats.STATS_FILE), "w",
                      encoding="utf-8") as file:
                file.write(graphics("00000000000000a1", FRAGMENT, [FRAGMENT_SIMD16]) + "\n")
            log = os.path.join(directory, "eden_log.txt")
            with open(log, "w", encoding="utf-8") as file:
                file.write(LOG)
            output = io.StringIO()
            with redirect_stdout(output):
                self.assertEqual(pipeline_stats.main([directory, "--costs", log, "--top", "1"]), 0)
        text = output.getvalue()
        self.assertIn(f"s1:{VERTEX}+s5:{FRAGMENT}", text)
        self.assertIn("[1] SIMD16 fragment  stages=fragment  subgroup=16  in 1 of 1 pipelines",
                      text)
        self.assertIn("* Spill count: 12", text)
        self.assertIn("  Code size: 8192", text)
        self.assertNotIn(COMPUTE, text)


if __name__ == "__main__":
    unittest.main()
