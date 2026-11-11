#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import base64
import contextlib
import io
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def png_chunk(kind, body):
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))


def filter_line(kind, line, previous, bpp):
    out = bytearray()
    for i, value in enumerate(line):
        a = line[i - bpp] if i >= bpp else 0
        b = previous[i]
        c = previous[i - bpp] if i >= bpp else 0
        p = a + b - c
        paeth = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
        out.append((value - (0, a, b, (a + b) >> 1, paeth)[kind]) & 255)
    return bytes(out)


def make_png(width, height, pixel, colour=2, depth=8, filters=(0,), palette=None):
    """PNG whose sample bytes come from pixel(x, y) -> tuple of channel bytes."""
    bpp = bench.PNG_CHANNELS[colour] * depth // 8
    raw = bytearray()
    previous = bytes(width * bpp)
    for y in range(height):
        line = b"".join(bytes(pixel(x, y)) for x in range(width))
        kind = filters[y % len(filters)]
        raw.append(kind)
        raw += filter_line(kind, line, previous, bpp)
        previous = line
    data = bench.PNG_MAGIC + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, colour, 0, 0, 0))
    if palette:
        data += png_chunk(b"PLTE", palette)
    compressed = zlib.compress(bytes(raw))
    half = len(compressed) // 2
    return data + png_chunk(b"IDAT", compressed[:half]) + png_chunk(b"IDAT", compressed[half:]) + png_chunk(b"IEND", b"")


def solid(value, width=64, height=36):
    return make_png(width, height, lambda x, y: (value, value, value))


def gradient(x, y):
    return ((x * 7 + y) & 255, (y * 5) & 255, (x * y) & 255)


class IniTest(unittest.TestCase):
    TEXT = ("[Renderer]\r\nbackend\\default=true\r\nbackend=1\r\nscale=2\r\n\r\n"
            "[Core]\r\nbackend\\default=true\r\nbackend=7\r\n\r\n[UI]\r\nfullscreen\\default=false\r\nfullscreen=true\r\n")

    def test_existing_key_gets_value_and_default_false(self):
        lines = bench.ini_set(["[Renderer]", "backend\\default=true", "backend=1", "", "[Core]", "x=1"],
                              "Renderer", {"backend": 2})
        self.assertEqual(lines, ["[Renderer]", "backend\\default=false", "backend=2", "", "[Core]", "x=1"])

    def test_section_boundaries(self):
        text = bench.ini_apply_text(self.TEXT, {"Core": {"backend": 9}})
        self.assertIn("[Renderer]\r\nbackend\\default=true\r\nbackend=1\r\n", text)
        self.assertIn("[Core]\r\nbackend\\default=false\r\nbackend=9\r\n", text)
        self.assertEqual(text.count("\r\n"), self.TEXT.count("\r\n"))
        self.assertNotIn("\r\r", text)

    def test_new_key_is_inserted_in_its_section_with_default_false(self):
        lines = bench.ini_set(["[A]", "x=1", "", "[B]", "y=1"], "B", {"z": True})
        self.assertEqual(lines, ["[A]", "x=1", "", "[B]", "z\\default=false", "z=true", "y=1"])

    def test_key_without_default_line_gets_one(self):
        lines = bench.ini_set(["[Renderer]", "scale=2"], "Renderer", {"scale": 3})
        self.assertEqual(lines, ["[Renderer]", "scale\\default=false", "scale=3"])

    def test_key_prefix_is_not_confused(self):
        lines = bench.ini_set(["[A]", "fullscreen_mode\\default=true", "fullscreen_mode=0"], "A", {"fullscreen": False})
        self.assertEqual(lines, ["[A]", "fullscreen\\default=false", "fullscreen=false",
                                 "fullscreen_mode\\default=true", "fullscreen_mode=0"])

    def test_none_resets_to_default(self):
        lines = bench.ini_set(["[UI]", "fullscreen\\default=false", "fullscreen=true"], "UI", {"fullscreen": None})
        self.assertEqual(lines, ["[UI]", "fullscreen\\default=true", "fullscreen=true"])

    def test_missing_section_is_appended(self):
        lines = bench.ini_set(["[A]", "x=1", ""], "New", {"k": "v"})
        self.assertEqual(lines, ["[A]", "x=1", "", "[New]", "k\\default=false", "k=v", ""])

    def test_several_keys_and_second_application_is_stable(self):
        once = bench.ini_apply_text(self.TEXT, {"Renderer": {"backend": 3, "fresh": "a"}, "UI": {"fullscreen": False}})
        self.assertEqual(once, bench.ini_apply_text(once, {"Renderer": {"backend": 3, "fresh": "a"},
                                                           "UI": {"fullscreen": False}}))
        self.assertIn("[UI]\r\nfullscreen\\default=false\r\nfullscreen=false\r\n", once)

    def test_guard_round_trip(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "qt-config.ini")
            original = self.TEXT.encode() + b"name=\xff\xfe odd bytes\r\n"
            with open(path, "wb") as file:
                file.write(original)
            guard = bench.IniGuard(path)
            guard.apply({"UI": {"fullscreen": False}})
            with open(path, "rb") as file:
                modified = file.read()
            self.assertIn(b"fullscreen=false", modified)
            self.assertIn(b"\xff\xfe odd bytes", modified)
            guard.restore()
            with open(path, "rb") as file:
                self.assertEqual(file.read(), original)
            self.assertFalse(os.path.exists(guard.backup))

    def test_guard_recovers_what_a_killed_run_left(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "qt-config.ini")
            with open(path, "w") as file:
                file.write("[UI]\nfullscreen=true\n")
            bench.IniGuard(path).apply({"UI": {"fullscreen": False}})  # never restored
            self.assertTrue(bench.IniGuard(path).recover())
            with open(path) as file:
                self.assertEqual(file.read(), "[UI]\nfullscreen=true\n")

    def test_merge_settings(self):
        merged = bench.merge_settings({"UI": {"a": 1}}, None, {"UI": {"b": 2}, "Core": {"c": 3}}, {"UI": {"a": 4}})
        self.assertEqual(merged, {"UI": {"a": 4, "b": 2}, "Core": {"c": 3}})


class ImageTest(unittest.TestCase):
    def test_every_filter_decodes_to_the_same_pixels(self):
        plain = bench.decode_png(make_png(40, 20, gradient))
        self.assertEqual(plain[:2], (40, 20))
        self.assertEqual(plain[2][:3], bytes(gradient(0, 0)))
        self.assertEqual(plain[2][(7 * 40 + 13) * 3:(7 * 40 + 13) * 3 + 3], bytes(gradient(13, 7)))
        for kind in (1, 2, 3, 4):
            self.assertEqual(bench.decode_png(make_png(40, 20, gradient, filters=(kind,))), plain, kind)
        self.assertEqual(bench.decode_png(make_png(40, 20, gradient, filters=(4, 1, 3, 2, 0))), plain)

    def test_colour_types(self):
        rgb = bench.decode_png(make_png(8, 4, gradient))[2]
        rgba = make_png(8, 4, lambda x, y: gradient(x, y) + (9,), colour=6, filters=(4,))
        self.assertEqual(bench.decode_png(rgba)[2], rgb)
        deep = make_png(8, 4, lambda x, y: tuple(v for c in gradient(x, y) for v in (c, 77)), depth=16, filters=(1,))
        self.assertEqual(bench.decode_png(deep)[2], rgb)
        grey = make_png(8, 4, lambda x, y: (x * 30,), colour=0)
        self.assertEqual(bench.decode_png(grey)[2][3 * 5:3 * 5 + 3], bytes((150, 150, 150)))
        grey_alpha = make_png(8, 4, lambda x, y: (x * 30, 1), colour=4)
        self.assertEqual(bench.decode_png(grey_alpha)[2], bench.decode_png(grey)[2])
        indexed = make_png(8, 4, lambda x, y: (x % 2,), colour=3, palette=bytes((1, 2, 3, 200, 100, 50)))
        self.assertEqual(bench.decode_png(indexed)[2][:6], bytes((1, 2, 3, 200, 100, 50)))

    def test_bad_input(self):
        with self.assertRaises(bench.BenchError):
            bench.decode_png(b"\x89PNG fake")
        interlaced = bench.PNG_MAGIC + png_chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 1))
        with self.assertRaises(bench.BenchError):
            bench.decode_png(interlaced)

    def test_box_thumbnail_averages(self):
        # Left half black, right half white, 64x36 -> 32x18 cells are pure; 2x2 blocks average.
        width, height, rgb = bench.decode_png(make_png(64, 36, lambda x, y: (255,) * 3 if x >= 32 else (0,) * 3))
        thumb = bench.box_thumbnail(width, height, rgb)
        self.assertEqual(len(thumb), 32 * 18 * 3)
        self.assertEqual(thumb[:3], [0, 0, 0])
        self.assertEqual(thumb[31 * 3:32 * 3], [255, 255, 255])
        checker = bench.decode_png(make_png(64, 36, lambda x, y: (200,) * 3 if (x + y) % 2 else (100,) * 3))
        self.assertEqual(set(bench.box_thumbnail(*checker)), {150})

    def test_diff(self):
        self.assertEqual(bench.image_diff(solid(0), solid(0)), 0)
        self.assertEqual(bench.image_diff(solid(0), solid(255)), 255)
        self.assertEqual(bench.image_diff(solid(100), solid(120)), 20)
        self.assertEqual(bench.image_diff(solid(100), solid(100, 960, 540)), 0)
        self.assertLess(bench.image_diff(make_png(64, 36, gradient), make_png(64, 36, gradient, filters=(4,))), 0.01)
        # Half the picture changes completely: far over the limit of 30.
        half = make_png(64, 36, lambda x, y: (255,) * 3 if y >= 18 else (0,) * 3)
        self.assertEqual(bench.image_diff(solid(0), half), 127.5)


class FakeEmulator:
    """The debug server's wire protocol: one JSON request line, one JSON response line."""

    def __init__(self, port=0, fps=(50.0, 60.0, 55.0), shot=None, methods=()):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.fps = list(fps)
        self.shot = shot or solid(100)
        self.methods = set(methods)
        self.requests = []
        self.presents = 0
        self.measured = 0
        threading.Thread(target=self.serve, daemon=True).start()

    def answer(self, request):
        method, params = request["method"], request.get("params") or {}
        if method == "status":
            self.presents += 20
            return {"result": {"title": "Test", "program_id": "0400486024DEC000", "build": "nx2-test",
                               "presents": self.presents}}
        if method == "input":
            if "Minus" in params.get("buttons", []):
                return {"error": "no frame was presented"}
            return {"result": {"controller": "player1", "frames": params["frames"]}}
        if method == "frame_times":
            fps = self.fps[self.measured % len(self.fps)]
            self.measured += 1
            if not isinstance(fps, float):
                return {"result": {"frames": 0}}
            return {"result": {"frames": 100, "fps": fps, "mean_ms": 1000 / fps, "p50_ms": 1000 / fps,
                               "p90_ms": 1100 / fps, "p99_ms": 1500 / fps, "max_ms": 40.0, "stutters": 1,
                               "vsync_buckets": {"1": 0.9, "2": 0.1}, "intervals_ms": [16.7]}}
        if method == "screenshot":
            return {"result": {"width": 64, "height": 36, "png_base64": base64.b64encode(self.shot).decode()}}
        if method == "power" and "power" in self.methods:
            return {"result": {"available": True, "package_w": 28.0 + self.measured, "cpu_w": 9.0, "gpu_w": 15.0}}
        if method == "memory" and "memory" in self.methods:
            return {"result": {"working_set_mb": 4096, "dump": "C:\\Users\\somebody\\dumps"}}
        return {"error": f"unknown method \"{method}\""}

    def serve(self):
        while True:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                return
            with connection, connection.makefile("rwb") as stream:
                for line in stream:
                    request = json.loads(line)
                    self.requests.append(request)
                    stream.write(json.dumps({"id": request["id"], **self.answer(request)}).encode() + b"\n")
                    stream.flush()

    def close(self):
        self.listener.close()


class ClientTest(unittest.TestCase):
    def setUp(self):
        self.emulator = FakeEmulator()
        self.addCleanup(self.emulator.close)
        self.client = bench.DebugClient(port=self.emulator.port)

    def test_input_script(self):
        slept = []
        script = [{"wait_s": 10, "buttons": ["A"], "frames": 6},
                  {"wait_s": 1.5, "buttons": ["Down"], "frames": 6},
                  {"wait_s": 3},
                  {"left_stick": [0, 1], "frames": 30, "timeout_s": 12}]
        bench.play_input_script(self.client, script, slept.append)
        self.assertEqual(slept, [10.0, 1.5, 3.0])
        self.assertEqual([request["method"] for request in self.emulator.requests], ["input"] * 3)
        self.assertEqual(self.emulator.requests[0]["params"], {"buttons": ["A"], "frames": 6, "timeout_s": 30})
        self.assertEqual(self.emulator.requests[1]["params"]["buttons"], ["Down"])
        self.assertEqual(self.emulator.requests[2]["params"], {"left_stick": [0, 1], "frames": 30, "timeout_s": 12})

    def test_shipped_scene_script(self):
        scene = bench.load_json(os.path.join(HERE, "scenes", "oot-hyrule-field.json"), "scene")
        slept = []
        bench.play_input_script(self.client, scene["input"], slept.append)
        self.assertEqual(slept, [10, 6, 6, 1.5, 1.5, 4])
        self.assertEqual([request["params"]["buttons"][0] for request in self.emulator.requests],
                         ["A", "A", "Down", "Down", "A", "A"])
        self.assertTrue(all(request["params"]["frames"] == 6 for request in self.emulator.requests))
        self.assertEqual(scene["settle_s"], 50)
        self.assertEqual(scene["title_id"], "0400486024DEC000")

    def test_input_failure_stops_the_script(self):
        with self.assertRaises(bench.DebugError):
            bench.play_input_script(self.client, [{"buttons": ["Minus"], "frames": 2}, {"buttons": ["A"]}],
                                    lambda _: None)
        self.assertEqual(len(self.emulator.requests), 1)

    def test_missing_methods_are_none(self):
        self.assertIsNone(self.client.try_call("power"))
        self.assertIsNone(bench.read_power(self.client, True))
        self.assertIsNone(bench.read_power(self.client, False))

    def test_screenshot_and_window(self):
        self.assertEqual(bench.take_screenshot(self.client), self.emulator.shot)
        window = bench.measure_window(self.client, 15, 60)
        self.assertEqual(window["fps"], 50.0)
        self.assertEqual(window["p50_ms"], 20.0)
        self.assertEqual(self.emulator.requests[-1]["params"], {"seconds": 15, "refresh_hz": 60})

    def test_nothing_listening(self):
        self.emulator.close()
        client = bench.DebugClient(port=self.emulator.port)
        self.assertFalse(client.listening())
        with self.assertRaises(bench.DebugError):
            client.call("status", timeout=2)

    def test_timeout(self):
        silent = socket.socket()
        silent.bind(("127.0.0.1", 0))
        silent.listen()
        self.addCleanup(silent.close)
        with self.assertRaisesRegex(bench.DebugError, "no answer within 0.5 s"):
            bench.DebugClient(port=silent.getsockname()[1]).call("status", timeout=0.5)


class MedianTest(unittest.TestCase):
    def test_median_of_three(self):
        windows = [{"fps": 50.0, "p50_ms": 20.0}, {"fps": 60.0, "p50_ms": 16.7}, {"fps": 55.0, "p50_ms": 18.2}]
        for order in ((0, 1, 2), (2, 1, 0), (1, 0, 2), (1, 2, 0)):
            self.assertEqual(bench.median_window([windows[i] for i in order]), windows[2])
        self.assertEqual(bench.median_window([{"fps": 30.0}, {"fps": 30.0}, {"fps": 12.0}]), {"fps": 30.0})

    def test_mean_power(self):
        self.assertIsNone(bench.mean_power([None, None]))
        self.assertEqual(bench.mean_power([{"package_w": 20.0, "gpu_w": 10.0}, None, {"package_w": 30.0}]),
                         {"package_w": 25.0, "gpu_w": 10.0})


HARDWARE = {"cpu": "Example CPU 9", "cpu_cores": 8, "cpu_threads": 16,
            "gpus": [{"name": "Example GPU", "driver_version": "1.2.3"}], "ram_gb": 31.4,
            "os": "Example OS 11", "os_build": "26340", "display": "2880x1800@120", "power_source": "AC",
            "battery_pct": 100, "power_plan": "Balanced"}


def good_row():
    return {
        "schema_version": 1, "timestamp_utc": "2026-11-11T12:00:00Z", "machine_id": bench.machine_id(HARDWARE),
        "scene": "oot-hyrule-field", "title_id": "0400486024DEC000", "variant": "baseline",
        "variant_settings": {}, "variant_env": {}, "emulator": {"exe": "lichii.exe"}, "game": "game.nup",
        "hardware": json.loads(json.dumps(HARDWARE)), "valid": True, "invalid_reasons": [], "warnings": [],
        "error": None, "scene_ok": True, "scene_diff": 3.2, "fps": 55.0, "mean_ms": 18.2, "p50_ms": 18.2,
        "p90_ms": 20.0, "p99_ms": 27.3, "max_ms": 40.0, "stutters": 1, "vsync_buckets": {"1": 1.0},
        "windows_fps": [50.0, 60.0, 55.0], "window_s": 15, "sustained": None, "watts": None, "power": None,
        "memory": None, "gpu_costs": None,
        "system": {"other_cpu_pct": 2.0, "other_cpu_max_pct": 3.0, "other_cpu_limit_pct": 10.0,
                   "emulator_cpu_pct": 20.0, "ram_free_min_gb": 12.0},
        "screenshot": "shots/x.png",
    }


class SchemaTest(unittest.TestCase):
    def assertProblem(self, row, text):
        problems = bench.validate_row(row)
        self.assertTrue(any(text in problem for problem in problems), problems)

    def test_good_row(self):
        self.assertEqual(bench.validate_row(good_row()), [])

    def test_missing_and_mistyped_fields(self):
        row = good_row()
        del row["fps"]
        self.assertProblem(row, "missing fps")
        row = good_row()
        row["scene_ok"] = "yes"
        self.assertProblem(row, "scene_ok has type str")
        row = good_row()
        row["schema_version"] = True
        self.assertProblem(row, "schema_version has type bool")
        row = good_row()
        row["schema_version"] = 2
        self.assertProblem(row, "schema_version is 2")
        row = good_row()
        del row["hardware"]["cpu"]
        self.assertProblem(row, "hardware: missing cpu")
        self.assertEqual(bench.validate_row([]), ["row is not an object"])

    def test_valid_rows_must_be_verified(self):
        row = good_row()
        row["scene_ok"] = False
        self.assertProblem(row, "a valid row needs scene_ok true")
        self.assertProblem(row, "wrong_scene")
        row = good_row()
        row["scene_ok"] = None
        self.assertProblem(row, "a valid row needs scene_ok true")
        row = good_row()
        row["invalid_reasons"] = ["wrong_scene"]
        self.assertProblem(row, "valid must be true exactly when")
        row = good_row()
        row["fps"] = 60.0
        self.assertProblem(row, "not the median")
        row = good_row()
        row["system"]["other_cpu_max_pct"] = 40.0
        self.assertProblem(row, "background CPU")
        row = good_row()
        row["machine_id"] = "0123456789"
        self.assertProblem(row, "machine_id does not match")

    def test_invalid_row_conforms(self):
        row = good_row()
        row.update(valid=False, invalid_reasons=["wrong_scene", "error"], scene_ok=False, scene_diff=80.1,
                   error="wrong scene (diff 80.1): not measured", fps=None, mean_ms=None, p50_ms=None,
                   p90_ms=None, p99_ms=None, max_ms=None, windows_fps=[])
        self.assertEqual(bench.validate_row(row), [])

    def test_private_paths_are_refused(self):
        row = good_row()
        row["memory"] = {"dump": "C:\\Users\\somebody\\dumps"}
        self.assertProblem(row, "row.memory.dump: looks like a home directory")
        row["memory"] = {"dump": "/home/somebody/x"}
        self.assertProblem(row, "home directory")

    def test_validate_file(self):
        with tempfile.TemporaryDirectory() as folder:
            machine = bench.machine_id(HARDWARE)
            path = os.path.join(folder, f"2026-11-11-{machine}.jsonl")
            with open(path, "w") as file:
                file.write(json.dumps(good_row()) + "\n\n" + json.dumps(good_row()) + "\n")
            self.assertEqual(bench.validate_file(path), ([], 2))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bench.main(["validate", path]), 0)
            with open(path, "a") as file:
                file.write("{broken\n")
            problems, count = bench.validate_file(path)
            self.assertEqual(count, 3)
            self.assertIn(":4: not JSON", problems[0])
            other = os.path.join(folder, "2026-11-11-0000000000.jsonl")
            with open(other, "w") as file:
                file.write(json.dumps(good_row()) + "\n")
            self.assertIn("file name", bench.validate_file(other)[0][0])
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(bench.main(["validate", other]), 1)


class SanitizeTest(unittest.TestCase):
    def test_names_and_paths(self):
        clean = bench.make_sanitizer(user="Yousof", home="C:\\Users\\Yousof", host="DESKTOP-ABC123")
        row = clean({"a": "C:\\Users\\Yousof\\AppData\\Roaming\\lichii\\dump",
                     "b": ["c:/users/yousof/x", "D:\\games\\yousof\\save on desktop-abc123"],
                     "c": {"C:\\Users\\Other Person\\y": 5, "n": 1.5, "t": True, "z": None},
                     "d": "Yousofian /home/zed/q /Users/mac/q"})
        self.assertEqual(row["a"], "~\\AppData\\Roaming\\lichii\\dump")
        self.assertEqual(row["b"], ["~/x", "D:\\games\\<user>\\save on <host>"])
        self.assertEqual(row["c"], {"~ Person\\y": 5, "n": 1.5, "t": True, "z": None})
        self.assertEqual(row["d"], "Yousofian ~/q ~/q")
        self.assertEqual(bench.private_strings(row), [])
        self.assertNotIn("yousof", json.dumps(row).lower().replace("yousofian", ""))

    def test_unusual_home(self):
        clean = bench.make_sanitizer(user="me", home="/srv/people/me", host="")
        self.assertEqual(clean("/srv/people/me/roms and /srv/people/other"), "~/roms and /srv/people/other")

    def test_machine_id(self):
        first = bench.machine_id(HARDWARE)
        self.assertRegex(first, r"^[0-9a-f]{10}$")
        same = dict(HARDWARE, cpu=" example cpu 9 ", ram_gb=64, os="other")
        self.assertEqual(bench.machine_id(same), first)
        self.assertNotEqual(bench.machine_id(dict(HARDWARE, gpus=[{"name": "Other GPU"}])), first)

    def test_windows_hardware_parsing(self):
        raw = {"cpu": "Example  CPU 9 ", "cores": 8, "threads": 16, "ram_bytes": 34 * 2**30, "os": "Windows 11",
               "os_version": "10.0.26340", "os_build": "26340", "has_battery": True, "battery_pct": 80,
               "battery_status": 1, "power_online": False,
               "power_plan": "Power Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)",
               "gpus": {"name": "Example GPU", "driver_version": "1.2.3", "width": 2880, "height": 1800,
                        "refresh_hz": 120}}
        hardware = bench.parse_windows_hardware(raw)
        self.assertEqual(hardware["cpu"], "Example CPU 9")
        self.assertEqual(hardware["gpus"], [{"name": "Example GPU", "driver_version": "1.2.3"}])
        self.assertEqual((hardware["display"], hardware["power_source"], hardware["power_plan"], hardware["ram_gb"]),
                         ("2880x1800@120", "battery", "High performance", 34.0))
        desktop = bench.parse_windows_hardware({"cpu": "x", "gpus": [], "has_battery": False, "power_online": None})
        self.assertEqual(desktop["power_source"], "AC")
        errors = []
        bench._check_fields(hardware, bench.HARDWARE_FIELDS, "hardware", errors)
        self.assertEqual(errors, [])


class FakeProcess:
    def __init__(self, emulator):
        self.emulator = emulator
        self.pid = os.getpid()
        self.exited = False

    def poll(self):
        return 0 if self.exited else None

    def kill(self):
        self.exited = True
        self.emulator.close()


class FakeMeter:
    reading = {"other_cpu_pct": 2.0, "emulator_cpu_pct": 30.0}

    def __init__(self, pid):
        pass

    def start(self):
        pass

    def stop(self):
        return dict(self.reading)


class RunTest(unittest.TestCase):
    """The whole run against a fake emulator: nothing is launched."""

    INI = b"[Renderer]\r\nbackend\\default=true\r\nbackend=1\r\n\r\n[UI]\r\nfullscreen\\default=false\r\nfullscreen=true\r\n"

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = folder.name
        self.user = os.path.join(self.folder, "userdata")
        os.makedirs(os.path.join(self.user, "config"))
        self.ini = os.path.join(self.user, "config", "qt-config.ini")
        self.write(self.ini, self.INI)
        self.save = os.path.join(self.user, "nand", "user", "save")
        self.write(os.path.join(self.save, "slot", "mine.bin"), b"owner's progress")
        self.backup = os.path.join(self.folder, "backup")
        self.write(os.path.join(self.backup, "slot", "field.bin"), b"hyrule field")
        self.exe = os.path.join(self.folder, "lichii.exe")
        self.game = os.path.join(self.folder, "game.nup")
        self.write(self.exe, b"")
        self.write(self.game, b"")
        self.reference = os.path.join(self.folder, "ref.png")
        self.write(self.reference, solid(100))
        self.scene = os.path.join(self.folder, "scene.json")
        self.write(self.scene, json.dumps({
            "name": "test-scene", "title_id": "0400486024DEC000", "settle_s": 50,
            "input": [{"wait_s": 10, "buttons": ["A"], "frames": 6}, {"wait_s": 4, "buttons": ["A"], "frames": 6}],
        }).encode())
        self.variant = os.path.join(self.folder, "variant.json")
        self.write(self.variant, json.dumps({"name": "fast", "settings": {"Renderer": {"backend": 2}},
                                             "env": {"EDEN_TEST_SWITCH": 1}}).encode())
        self.out = os.path.join(self.folder, "results")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.emulator_options = {}
        self.launches = []
        self.stopped = []
        self.slept = []
        self.log = []
        test = self

        class TestDeps(bench.Deps):
            sleep = staticmethod(self.slept.append)
            running = staticmethod(lambda image: False)
            hardware = staticmethod(lambda: json.loads(json.dumps(HARDWARE)))
            cpu_meter = FakeMeter
            free_ram = staticmethod(lambda: 12.0)
            log = staticmethod(self.log.append)

            @staticmethod
            def launch(exe, game, env):
                emulator = FakeEmulator(port=test.port, **test.emulator_options)
                test.addCleanup(emulator.close)
                with open(test.ini, "rb") as file:
                    ini = file.read()
                test.launches.append({"exe": exe, "game": game, "env": env, "ini": ini, "emulator": emulator,
                                      "saves": sorted(os.listdir(os.path.join(test.save, "slot")))})
                return FakeProcess(emulator)

            @staticmethod
            def stop(process):
                test.stopped.append(process)
                if process is not None:
                    process.kill()

        self.deps = TestDeps

    @staticmethod
    def write(path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as file:
            file.write(data)

    def run_bench(self, *extra):
        args = bench.build_parser().parse_args([
            "run", "--exe", self.exe, "--game", self.game, "--scene", self.scene, "--user-dir", self.user,
            "--save-backup", self.backup, "--reference", self.reference, "--out", self.out,
            "--port", str(self.port), *extra])
        code = bench.command_run(args, self.deps)
        files = [name for name in os.listdir(self.out) if name.endswith(".jsonl")] if os.path.isdir(self.out) else []
        rows = []
        for name in files:
            rows += [row for _, row, _ in bench.read_rows(os.path.join(self.out, name))]
        return code, rows

    def assert_left_clean(self):
        with open(self.ini, "rb") as file:
            self.assertEqual(file.read(), self.INI)
        self.assertEqual(os.listdir(os.path.join(self.user, "config")), ["qt-config.ini"])
        self.assertEqual(os.listdir(os.path.join(self.save, "slot")), ["mine.bin"])
        self.assertFalse(os.path.exists(self.save + ".bench-keep"))
        self.assertTrue(all(process is None or process.exited for process in self.stopped))
        self.assertFalse(bench.DebugClient(port=self.port).listening())

    def test_valid_run(self):
        self.emulator_options = {"methods": ("power", "memory")}
        code, rows = self.run_bench("--variant", self.variant, "--sustain-min", "0.5")
        self.assertEqual(code, 0, self.log)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual(bench.validate_row(row), [])
        self.assertTrue(row["valid"])
        self.assertEqual((row["fps"], row["windows_fps"], row["p50_ms"]), (55.0, [50.0, 60.0, 55.0], 18.18))
        self.assertEqual((row["scene_ok"], row["scene_diff"]), (True, 0.0))
        self.assertEqual((row["variant"], row["variant_settings"], row["variant_env"]),
                         ("fast", {"Renderer": {"backend": "2"}}, {"EDEN_TEST_SWITCH": "1"}))
        self.assertEqual(row["emulator"], {"exe": "lichii.exe", "title": "Test", "program_id": "0400486024DEC000",
                                           "build": "nx2-test"})
        self.assertEqual(row["sustained"]["fps_min"], 50.0)
        self.assertEqual(len(row["sustained"]["samples"]), 3)
        self.assertEqual(row["sustained"]["fps_avg"], 55.0)
        self.assertEqual(row["power"], {"package_w": 30.0, "cpu_w": 9.0, "gpu_w": 15.0})
        self.assertEqual(row["watts"], 30.0)
        self.assertEqual(row["memory"], {"working_set_mb": 4096, "dump": "~\\dumps"})
        self.assertIsNone(row["gpu_costs"])
        self.assertEqual(row["system"]["other_cpu_pct"], 2.0)
        self.assertEqual(row["machine_id"], bench.machine_id(HARDWARE))
        self.assertNotIn(self.folder, json.dumps(row))
        self.assertTrue(os.path.isfile(os.path.join(self.out, row["screenshot"])))

        launch = self.launches[0]
        self.assertEqual(launch["env"][bench.DEBUG_ENV], f"127.0.0.1:{self.port}")
        self.assertEqual(launch["env"]["EDEN_TEST_SWITCH"], "1")
        self.assertIn(b"[Renderer]\r\nbackend\\default=false\r\nbackend=2\r\n", launch["ini"])
        self.assertIn(b"fullscreen\\default=false\r\nfullscreen=false\r\n", launch["ini"])
        self.assertEqual(launch["saves"], ["field.bin"])
        self.assertEqual([seconds for seconds in self.slept if seconds > 2], [10.0, 4.0, 50.0])
        methods = [request["method"] for request in launch["emulator"].requests]
        self.assertEqual(methods.count("input"), 2)
        self.assertLess(methods.index("input"), methods.index("screenshot"))
        self.assertLess(methods.index("screenshot"), methods.index("frame_times"))
        self.assertEqual(methods.count("frame_times"), 6)
        self.assertEqual(methods.count("screenshot"), 2)
        self.assert_left_clean()
        name = os.listdir(self.out)
        self.assertIn(f"{row['timestamp_utc'][:4]}", "".join(name))
        self.assertEqual(bench.validate_file(os.path.join(self.out, [n for n in name if n.endswith(".jsonl")][0]))[0], [])

    def test_wrong_scene_invalidates_the_row(self):
        self.emulator_options = {"shot": solid(230)}
        code, rows = self.run_bench()
        self.assertEqual(code, 2)
        row = rows[0]
        self.assertFalse(row["valid"])
        self.assertIn("wrong_scene", row["invalid_reasons"])
        self.assertEqual((row["scene_ok"], row["scene_diff"], row["fps"], row["variant"]), (False, 130.0, None, "baseline"))
        self.assertIn("wrong scene", row["error"])
        self.assertEqual(bench.validate_row(row), [])
        methods = [request["method"] for request in self.launches[0]["emulator"].requests]
        self.assertNotIn("frame_times", methods)
        self.assert_left_clean()

    def test_scene_that_changes_while_measuring(self):
        self.emulator_options = {}
        original = bench.measure_window
        calls = []

        def measure(client, seconds, refresh_hz):
            calls.append(1)
            if len(calls) == 3:
                self.launches[0]["emulator"].shot = solid(0)
            return original(client, seconds, refresh_hz)

        bench.measure_window = measure
        self.addCleanup(setattr, bench, "measure_window", original)
        code, rows = self.run_bench()
        self.assertEqual(code, 2)
        self.assertEqual((rows[0]["scene_ok"], rows[0]["invalid_reasons"], rows[0]["fps"]), (False, ["wrong_scene"], 55.0))
        self.assertEqual(bench.validate_row(rows[0]), [])

    def test_missing_reference_and_recording_one(self):
        os.remove(self.reference)
        code, rows = self.run_bench()
        self.assertEqual((code, rows[0]["invalid_reasons"], rows[0]["scene_ok"]), (2, ["scene_unverified"], None))
        code, rows = self.run_bench("--record-reference")
        self.assertEqual(rows[-1]["invalid_reasons"], ["reference_recorded"])
        with open(self.reference, "rb") as file:
            self.assertEqual(file.read(), solid(100))
        code, rows = self.run_bench()
        self.assertEqual((code, rows[-1]["valid"]), (0, True))
        self.assertEqual(len(rows), 3)
        self.assert_left_clean()

    def test_background_cpu_is_flagged(self):
        class Busy(FakeMeter):
            reading = {"other_cpu_pct": 23.5, "emulator_cpu_pct": 30.0}
        self.deps.cpu_meter = Busy
        code, rows = self.run_bench()
        self.assertEqual((code, rows[0]["invalid_reasons"], rows[0]["system"]["other_cpu_max_pct"]),
                         (2, ["background_cpu"], 23.5))
        self.assertEqual(rows[0]["fps"], 55.0)
        self.assertEqual(bench.validate_row(rows[0]), [])

    def test_emulator_failure_still_cleans_up(self):
        self.emulator_options = {"fps": ("broken",)}
        code, rows = self.run_bench("--variant", self.variant)
        self.assertEqual(code, 2)
        self.assertEqual(rows[0]["invalid_reasons"], ["error"])
        self.assertIn("frame_times", rows[0]["error"])
        self.assertEqual(len(self.stopped), 1)
        self.assert_left_clean()

    def test_crash_in_the_middle_still_cleans_up(self):
        def explode(*_):
            raise KeyboardInterrupt
        original = bench.play_input_script
        bench.play_input_script = explode
        self.addCleanup(setattr, bench, "play_input_script", original)
        with self.assertRaises(KeyboardInterrupt):
            self.run_bench("--variant", self.variant)
        self.assertEqual(len(self.stopped), 1)
        self.assert_left_clean()

    def test_refuses_when_the_emulator_runs(self):
        self.deps.running = staticmethod(lambda image: image == "lichii.exe")
        with self.assertRaisesRegex(bench.BenchError, "already running"):
            self.run_bench()
        self.assertEqual(self.launches, [])
        self.assert_left_clean()

    def test_refuses_when_the_port_is_taken(self):
        squatter = FakeEmulator(port=self.port)
        self.addCleanup(squatter.close)
        code, rows = self.run_bench()
        self.assertEqual(code, 2)
        self.assertIn("already listens", rows[0]["error"])
        self.assertEqual(self.launches, [])
        squatter.close()

    def test_memory_watchdog_stops_the_emulator(self):
        self.deps.free_ram = staticmethod(lambda: 1.2)
        code, rows = self.run_bench()
        self.assertEqual(code, 2)
        self.assertIn("memory_watchdog", rows[0]["invalid_reasons"])
        self.assertEqual(rows[0]["system"]["ram_free_min_gb"], 1.2)
        self.assert_left_clean()

    def test_missing_inputs_are_refused_before_anything_is_touched(self):
        os.remove(self.exe)
        with self.assertRaisesRegex(bench.BenchError, "emulator not found"):
            self.run_bench()
        self.assert_left_clean()

    def test_dry_run_prints_the_plan_and_touches_nothing(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code, rows = self.run_bench("--dry-run", "--variant", self.variant, "--sustain-min", "5")
        text = output.getvalue()
        self.assertEqual((code, rows, self.launches), (0, [], []))
        self.assertFalse(os.path.exists(self.out))
        for expected in ("variant fast:", "set [Renderer] backend=2", "set [UI] fullscreen=false",
                         "EDEN_TEST_SWITCH=1", "wait 10 s, hold A for 6 frames", "settle 50 s",
                         "measure 3 windows of 15 s", "for 5 min", "restore the saves"):
            self.assertIn(expected, text)
        self.assert_left_clean()


class SystemTest(unittest.TestCase):
    def test_cpu_meter_on_this_process(self):
        meter = bench.CpuMeter(os.getpid())
        meter.start()
        total = 0
        for value in range(3_000_000):
            total += value
        reading = meter.stop()
        if reading is None:
            self.skipTest("this host does not report CPU times")
        self.assertTrue(0 <= reading["other_cpu_pct"] <= 100)
        self.assertTrue(0 < reading["emulator_cpu_pct"] <= 100)

    def test_free_ram(self):
        free = bench.free_ram_gb()
        if free is None:
            self.skipTest("this host does not report free memory")
        self.assertGreater(free, 0)

    def test_process_running(self):
        self.assertFalse(bench.process_running("no-such-emulator-xyz.exe"))

    def test_stop_process_always_ends_it(self):
        child = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(120)"])
        bench.stop_process(child, grace_s=2)
        self.assertIsNotNone(child.poll())
        bench.stop_process(child)
        bench.stop_process(None)

    def test_watchdog(self):
        class Victim:
            killed = False

            def kill(self):
                self.killed = True
        victim = Victim()
        levels = iter([8.0, 5.0, 1.5])
        dog = bench.MemoryWatchdog(victim, 2.0, lambda: next(levels), interval_s=0.01)
        dog.start()
        dog.join(timeout=5)
        self.assertTrue(dog.fired and victim.killed)
        self.assertEqual(dog.lowest, 1.5)
        calm = bench.MemoryWatchdog(Victim(), 2.0, lambda: 9.0, interval_s=0.01)
        calm.start()
        calm.stop()
        self.assertFalse(calm.fired)


class TableTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = folder.name

    def write(self, name, rows):
        path = os.path.join(self.folder, name)
        with open(path, "w") as file:
            file.writelines(json.dumps(row) + "\n" for row in rows)
        return path

    def test_table(self):
        old = dict(good_row(), fps=40.0, timestamp_utc="2026-11-10T12:00:00Z")
        bad = dict(good_row(), valid=False, invalid_reasons=["wrong_scene"], scene_ok=False,
                   timestamp_utc="2026-11-12T12:00:00Z", fps=99.0)
        sustained = dict(good_row(), variant="mesh", watts=28.4, sustained={
            "minutes": 5, "interval_s": 10, "samples": [], "fps_min": 31.0, "fps_avg": 44.25, "fps_first": 55.0,
            "fps_last": 31.0})
        self.write("2026-11-11-x.jsonl", [old, good_row(), bad, sustained])
        text = bench.render_table(self.folder)
        self.assertIn("## Machine `" + bench.machine_id(HARDWARE) + "`", text)
        self.assertIn("Example CPU 9; Example GPU (driver 1.2.3); 31.4 GB RAM", text)
        self.assertIn("| oot-hyrule-field | baseline | 55.0 | 18.2 | 27.3 | - | - | yes | yes | 2026-11-11 |", text)
        self.assertIn("| oot-hyrule-field | mesh | 55.0 | 18.2 | 27.3 | 31.0 / 44.2 | 28.4 | yes | yes | 2026-11-11 |", text)
        self.assertNotIn("99.0", text)
        everything = bench.render_table(self.folder, every=True)
        self.assertIn("| 99.0 | 18.2 | 27.3 | - | - | NO | INVALID: wrong_scene |", everything)
        self.assertIn("| 40.0 |", everything)

    def test_empty_table_is_the_committed_skeleton(self):
        text = bench.render_table(self.folder)
        self.assertIn("No rows yet", text)
        with open(os.path.join(HERE, "RESULTS.md"), encoding="utf-8") as file:
            committed = file.read()
        if "## Machine" not in committed:
            self.assertEqual(committed, text)

    def test_compare(self):
        other = json.loads(json.dumps(HARDWARE))
        other["gpus"][0]["name"] = "Other GPU"
        fast = dict(good_row(), hardware=other, machine_id=bench.machine_id(other), fps=66.0, watts=45.0)
        only_b = dict(fast, variant="extra")
        text = bench.render_compare(self.write("a.jsonl", [good_row()]), self.write("b.jsonl", [fast, only_b]))
        self.assertIn("**A**: `a.jsonl`, machine `" + bench.machine_id(HARDWARE), text)
        self.assertIn("| oot-hyrule-field | baseline | 55.0 | 66.0 | +20.0% | 18.2 / 18.2 | 27.3 / 27.3 | - | - | "
                      "- / 45.0 | yes / yes |", text)
        self.assertIn("| oot-hyrule-field | extra | - | 66.0 | - |", text)


class ShippedFilesTest(unittest.TestCase):
    def test_scene_and_variants_load(self):
        for name in os.listdir(os.path.join(HERE, "variants")):
            variant = bench.load_json(os.path.join(HERE, "variants", name), "variant")
            self.assertRegex(variant["name"], r"^[A-Za-z0-9._-]+$")
            self.assertIsInstance(variant.get("settings", {}), dict)
            self.assertIsInstance(variant.get("env", {}), dict)
        scene = bench.load_json(os.path.join(HERE, "scenes", "oot-hyrule-field.json"), "scene")
        self.assertEqual(scene["reference"], "reference/oot-hyrule-field.png")


if __name__ == "__main__":
    unittest.main()
