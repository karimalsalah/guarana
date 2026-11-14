#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import base64
import contextlib
import io
import json
import os
import re
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
import play  # noqa: E402

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
        if method == "reject_shaders" and "reject_shaders" in self.methods:
            if params.get("reject"):
                self.shot = solid(0)  # the rejected shader drew most of the picture
            return {"result": {"applied": True, "rejected": params["hashes"] if params.get("reject") else []}}
        if method in ("set_shading_rate", "power_set") and method in self.methods:
            return {"result": dict(params)}
        if method == "gpu_costs" and "gpu_costs" in self.methods:
            self.costs = params.get("enable", getattr(self, "costs", False))
            return {"result": {"enabled": self.costs, "frame": 7, "work": [{"hash": "78134ce8125d5959", "ms": 15.1}]}}
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
        # On Linux close() alone does not wake the accept() in serve(), and the port answers one
        # more connection: shut the socket down first. Windows refuses that for a listener, and
        # its close() does wake accept().
        try:
            self.listener.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
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
        clean = bench.make_sanitizer(user="Alice", home="C:\\Users\\Alice", host="DESKTOP-ABC123")
        row = clean({"a": "C:\\Users\\Alice\\AppData\\Roaming\\lichii\\dump",
                     "b": ["c:/users/alice/x", "D:\\games\\alice\\save on desktop-abc123"],
                     "c": {"C:\\Users\\Other Person\\y": 5, "n": 1.5, "t": True, "z": None},
                     "d": "Aliceian /home/zed/q /Users/mac/q"})
        self.assertEqual(row["a"], "~\\AppData\\Roaming\\lichii\\dump")
        self.assertEqual(row["b"], ["~/x", "D:\\games\\<user>\\save on <host>"])
        self.assertEqual(row["c"], {"~ Person\\y": 5, "n": 1.5, "t": True, "z": None})
        self.assertEqual(row["d"], "Aliceian ~/q ~/q")
        self.assertEqual(bench.private_strings(row), [])
        self.assertNotIn("alice", json.dumps(row).lower().replace("aliceian", ""))

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
    """linger: how many more poll() calls report it alive after kill(), like an emulator that is
    still writing its settings. at_exit runs when it is finally gone."""

    def __init__(self, emulator, linger=0, at_exit=None):
        self.emulator = emulator
        self.pid = os.getpid()
        self.exited = False
        self.killed = False
        self.linger = linger
        self.at_exit = at_exit

    def poll(self):
        if self.killed and not self.exited:
            if self.linger > 0:
                self.linger -= 1
            else:
                self.exited = True
                if self.at_exit:
                    self.at_exit()
        return 0 if self.exited else None

    def kill(self):
        self.killed = True
        self.emulator.close()
        self.poll()


QUERY_HEAD = """Power Scheme GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Balanced)
  GUID Alias: SCHEME_BALANCED
  Subgroup GUID: 54533251-82be-4824-96c1-47b60b740d00  (Processor power management)
    GUID Alias: SUB_PROCESSOR
    Power Setting GUID: 06cadf0e-64ed-448a-8927-ce7bf90eb35d  (A setting without an alias)
      Minimum Possible Setting: 0x00000000
      Maximum Possible Setting: 0x00000064
    Current AC Power Setting Index: 0x0000003c
    Current DC Power Setting Index: 0x0000003c
"""
QUERY_PERCENT = """
    Power Setting GUID: 893dee8e-2bef-41e0-89c6-b55d0929964c  (Some processor state)
      GUID Alias: {name}
      Minimum Possible Setting: 0x00000000
      Maximum Possible Setting: 0x00000064
      Possible Settings increment: 0x00000001
      Possible Settings units: %
    Current AC Power Setting Index: 0x{ac:08x}
    Current DC Power Setting Index: 0x{dc:08x}
"""
QUERY_INDEX = """
    Power Setting GUID: be337238-0d82-4146-a960-4f3749d470c7  (Processor performance boost mode)
      GUID Alias: {name}
      Possible Setting Index: 000
      Possible Setting Friendly Name: Disabled
      Possible Setting Index: 002
      Possible Setting Friendly Name: Aggressive
    Current AC Power Setting Index: 0x{ac:08x}
    Current DC Power Setting Index: 0x{dc:08x}
"""
TELEMETRY = """Time      MHz   Watts  Temp  LimitedBy
09:15:01  2250  14.5   71    None
09:15:02  2150  15.5   72    Power
09:15:03  2200  15.0   72    Power
"""
TYPEPERF = """
"(PDH-CSV 4.0)","\\\\SOMEHOST\\Energy Meter(rapl_package0_pkg)\\Power","\\\\SOMEHOST\\Energy Meter(rapl_package0_pp0)\\Power","\\\\SOMEHOST\\Energy Meter(rapl_package0_pp1)\\Power"
"11/11/2026 21:39:39.769","27000.000000","9000.500000"," "
"11/11/2026 21:39:40.784","29000.000000","9001.500000","15250.000000"
Exiting, please wait...
The command completed successfully.
"""


class FakeHost:
    """Stands in for powercfg, typeperf, the GPU telemetry script, the process survey and
    taskkill. Any other command fails the test: nothing real is ever run."""

    def __init__(self):
        self.power = {"PERFBOOSTMODE": [2, 1], "PROCTHROTTLEMIN": [100, 5], "PROCTHROTTLEMIN1": [100, 5],
                      "PROCTHROTTLEMAX": [100, 100]}
        self.commands = []
        self.energy = True
        self.fail = None
        self.activated = 0
        self.survey = {"processes": [], "power_online": True, "battery_pct": 100}

    def query(self):
        return QUERY_HEAD + "".join((QUERY_INDEX if name == "PERFBOOSTMODE" else QUERY_PERCENT).format(
            name=name, ac=ac, dc=dc) for name, (ac, dc) in self.power.items())

    def ran(self, tool):
        return [command for command in self.commands if command[0] == tool]

    def __call__(self, command, timeout=60):
        self.commands.append(list(command))
        if self.fail and self.fail in " ".join(command):
            return 1, "Access is denied."
        tool = command[0]
        if tool == "powercfg" and command[1:] == ["/qh", "SCHEME_CURRENT", "SUB_PROCESSOR"]:
            return 0, self.query()
        if tool == "powercfg" and command[1:4] == ["/setacvalueindex", "SCHEME_CURRENT", "SUB_PROCESSOR"]:
            self.power[command[4]][0] = int(command[5])
            return 0, ""
        if tool == "powercfg" and command[1:] == ["/setactive", "SCHEME_CURRENT"]:
            self.activated += 1
            return 0, ""
        if tool == "typeperf":
            return (0, TYPEPERF) if self.energy else (1, "Error: No valid counters.")
        if tool == "powershell" and "-File" in command:
            return 0, TELEMETRY
        if tool == "powershell" and "-EncodedCommand" in command:
            return 0, "noise before\n" + json.dumps(self.survey)
        if tool == "taskkill":
            return 0, ""
        raise AssertionError(f"unexpected command: {command}")

    def ac(self):
        return {name: values[0] for name, values in self.power.items()}


class FakeMeter:
    reading = {"other_cpu_pct": 2.0, "emulator_cpu_pct": 30.0}

    def __init__(self, pid):
        pass

    def start(self):
        pass

    def stop(self):
        return dict(self.reading)


class RunCase(unittest.TestCase):
    """A user folder, a scene and a fake emulator: nothing is launched and no host tool runs."""

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
        self.process_options = {}
        self.log_text = None
        self.host = FakeHost()
        self.state = os.path.join(self.folder, "state")
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
            run_command = staticmethod(self.host)
            window_size = staticmethod(lambda pid: "1280x761")
            process_memory = staticmethod(lambda pid: {"working_set_mb": 3900, "working_set_peak_mb": 4100,
                                                       "private_mb": 5200, "private_peak_mb": 5300})
            log = staticmethod(self.log.append)

            @staticmethod
            def launch(exe, game, env):
                emulator = FakeEmulator(port=test.port, **test.emulator_options)
                test.addCleanup(emulator.close)
                with open(test.ini, "rb") as file:
                    ini = file.read()
                mods = os.path.join(test.user, "load", "0400486024DEC000")
                test.launches.append({"exe": exe, "game": game, "env": env, "ini": ini, "emulator": emulator,
                                      "saves": sorted(os.listdir(os.path.join(test.save, "slot"))),
                                      "mods": sorted(os.listdir(mods)) if os.path.isdir(mods) else None,
                                      "power": test.host.ac()})
                if test.log_text is not None:
                    test.write(os.path.join(test.user, "log", "eden_log.txt"), test.log_text.encode())
                return FakeProcess(emulator, **test.process_options)

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
            "--port", str(self.port), "--state-dir", self.state, *extra])
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
        self.assertEqual(self.host.ac(), FakeHost().ac())
        self.assertFalse(os.path.exists(os.path.join(self.state, "power-restore.json")))
        load = os.path.join(self.user, "load")
        if os.path.isdir(load):
            self.assertEqual(os.listdir(load), ["0400486024DEC000"])
            self.assertEqual(sorted(os.listdir(os.path.join(load, "0400486024DEC000"))), ["Owner Mod"])


class RunTest(RunCase):
    """The whole run against a fake emulator."""

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
        self.assertEqual(row["power"], {"package_w": 30.0, "cpu_w": 9.0, "gpu_w": 15.0, "source": "debug_server"})
        self.assertEqual(self.host.commands, [])
        self.assertEqual((row["probe"], row["mods"], row["power_policy"], row["log"], row["trace"]),
                         (False, None, None, None, None))
        self.assertEqual((row["window"], row["process_memory"]["private_peak_mb"]), ("1280x761", 5300))
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
        # The picture turned black: also refused without a reference by the picture check.
        self.assertEqual((rows[0]["scene_ok"], rows[0]["invalid_reasons"], rows[0]["fps"]),
                         (False, ["wrong_scene", "black_picture"], 55.0))
        self.assertEqual([shot["dark_pct"] for shot in rows[0]["picture"]["shots"]], [0.0, 100.0])
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
        bench.stop_process(child, grace_s=2, close_grace_s=1)
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


def run_main(*argv):
    """(exit code, stdout and stderr) of bench.main(argv)."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = bench.main(list(argv))
    return code, out.getvalue()


class ContributedRowsTest(unittest.TestCase):
    """The checks .github/workflows/bench-rows.yml runs on a results pull request, on a copy of the folder."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.results = os.path.join(folder.name, "results")
        os.mkdir(self.results)
        self.page = os.path.join(folder.name, "RESULTS.md")
        self.rows = os.path.join(self.results, f"2026-11-11-{bench.machine_id(HARDWARE)}.jsonl")
        self.write([good_row(), dict(good_row(), variant="mesh")])

    def write(self, rows, regenerate=True):
        """Writes the contributor's rows file and, as CONTRIBUTING-bench.md asks, regenerates the page."""
        with open(self.rows, "w", encoding="utf-8", newline="\n") as file:
            file.writelines(json.dumps(row) + "\n" for row in rows)
        if regenerate:
            self.assertEqual(run_main("table", self.results, "--out", self.page), (0, ""))

    def validate(self):
        return run_main("validate", self.results)

    def check_page(self):
        return run_main("table", self.results, "--check", self.page)

    def test_valid_rows_and_their_page_pass(self):
        code, log = self.validate()
        self.assertEqual(code, 0, log)
        self.assertIn("2 row(s) ok", log)
        code, log = self.check_page()
        self.assertEqual(code, 0, log)
        self.assertIn("matches the rows", log)

    def test_edited_median_fails(self):
        self.write([good_row(), dict(good_row(), fps=60.0)])  # windows 50, 60, 55: the median is 55
        code, log = self.validate()
        self.assertEqual(code, 1)
        self.assertIn(":2: fps is not the median of windows_fps", log)
        self.assertEqual(self.check_page()[0], 0)

    def test_home_directory_path_fails(self):
        for exe in ("C:\\Users\\somebody\\guarana\\guarana.exe", "/home/somebody/guarana/guarana"):
            self.write([dict(good_row(), emulator={"exe": exe})])
            code, log = self.validate()
            self.assertEqual(code, 1, exe)
            self.assertIn(":1: row.emulator.exe: looks like a home directory path", log)

    def test_stale_page_fails(self):
        self.write([good_row(), dict(good_row(), variant="mesh"), dict(good_row(), variant="extra")],
                   regenerate=False)
        self.assertEqual(self.validate()[0], 0)
        code, log = self.check_page()
        self.assertEqual(code, 1)
        self.assertIn("RESULTS.md is stale", log)
        self.assertIn(bench.TABLE_COMMAND, log)
        self.assertIn("+| oot-hyrule-field | extra | 55.0 |", log)

    def test_hand_edited_page_fails(self):
        with open(self.page, "a", encoding="utf-8") as file:
            file.write("| oot-hyrule-field | baseline | 99.0 |\n")
        code, log = self.check_page()
        self.assertEqual(code, 1)
        self.assertIn("-| oot-hyrule-field | baseline | 99.0 |", log)

    def test_missing_page_and_empty_folder(self):
        os.remove(self.page)
        self.assertEqual(self.check_page()[0], 1)
        os.remove(self.rows)
        self.assertEqual(self.validate(), (0, f"no result files in {self.results}\n"))
        self.assertEqual(run_main("table", self.results, "--out", self.page)[0], 0)
        self.assertEqual(self.check_page()[0], 0)

    def test_check_and_out_exclude_each_other(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            bench.main(["table", self.results, "--out", self.page, "--check", self.page])


ROOT = os.path.dirname(os.path.dirname(HERE))


class WorkflowTest(unittest.TestCase):
    """What CI runs for the benchmark tool; the workflows themselves are proven by a run on GitHub."""

    def read(self, name):
        path = os.path.join(ROOT, ".github", "workflows", name)
        if not os.path.isfile(path):
            self.skipTest(f"no {name} next to this copy of the tool")
        with open(path, encoding="utf-8") as file:
            return file.read()

    def test_rows_workflow_runs_the_checks_read_only(self):
        text = self.read("bench-rows.yml")
        self.assertRegex(text, r"(?m)^permissions:\n  contents: read$")
        self.assertNotRegex(text, r":\s*write\b")
        self.assertNotIn("pull_request_target", text)
        self.assertNotIn("secrets.", text)
        self.assertNotIn("github.token", text)
        self.assertIn('      - "tools/bench/**"', text)
        for command in ("python3 -I tools/bench/bench.py validate tools/bench/results",
                        "python3 -I tools/bench/bench.py table tools/bench/results --check tools/bench/RESULTS.md",
                        "python3 -I tools/bench/test_bench.py"):
            self.assertIn(command, text)

    def test_results_only_pull_requests_skip_the_builds(self):
        text = self.read("build-check.yml")
        skipped = re.compile(re.search(r"grep -qvE '([^']+)'", text).group(1))
        for name in ("tools/bench/results/2026-11-11-0123456789.jsonl", "tools/bench/RESULTS.md"):
            self.assertTrue(skipped.fullmatch(name), name)
        for name in ("tools/bench/bench.py", "tools/bench/test_bench.py", "src/core/core.cpp"):
            self.assertFalse(skipped.fullmatch(name), name)
        self.assertIn("python3 -I tools/bench/test_bench.py", text)


class ShippedFilesTest(unittest.TestCase):
    def test_committed_rows_and_page_pass_the_ci_checks(self):
        results = os.path.join(HERE, "results")
        code, log = run_main("validate", results)
        self.assertEqual(code, 0, log)
        code, log = run_main("table", results, "--check", os.path.join(HERE, "RESULTS.md"))
        self.assertEqual(code, 0, log)

    def test_scene_and_variants_load(self):
        for name in os.listdir(os.path.join(HERE, "variants")):
            variant = bench.load_json(os.path.join(HERE, "variants", name), "variant")
            self.assertRegex(variant["name"], r"^[A-Za-z0-9._-]+$")
            self.assertIsInstance(variant.get("settings", {}), dict)
            self.assertIsInstance(variant.get("env", {}), dict)
        scene = bench.load_json(os.path.join(HERE, "scenes", "oot-hyrule-field.json"), "scene")
        self.assertEqual(scene["reference"], "reference/oot-hyrule-field.png")


class AddonsTest(unittest.TestCase):
    TEXT = "[UI]\r\nfullscreen=true\r\n\r\n[DisabledAddOns]\r\nsize=1\r\n1\\title_id=5\r\n1\\disabled\\size=0\r\n\r\n[Controls]\r\nx=1\r\n"

    def test_block_is_the_one_the_emulator_writes(self):
        text = bench.ini_apply_text(self.TEXT, {}, ("0400486024DEC000", ["Update", "DLC 1"]))
        self.assertEqual(text, "[UI]\r\nfullscreen=true\r\n\r\n[DisabledAddOns]\r\nsize=1\r\n"
                         "1\\title_id\\default=false\r\n1\\title_id=288309953924349952\r\n1\\disabled\\size=2\r\n"
                         "1\\disabled\\1\\d\\default=false\r\n1\\disabled\\1\\d=Update\r\n"
                         "1\\disabled\\2\\d\\default=false\r\n1\\disabled\\2\\d=DLC 1\r\n\r\n[Controls]\r\nx=1\r\n")
        self.assertEqual(bench.ini_apply_text(text, {}, ("0400486024DEC000", ["Update", "DLC 1"])), text)

    def test_empty_list_enables_everything(self):
        text = bench.ini_apply_text(self.TEXT, {}, ("0400486024DEC000", []))
        self.assertEqual(text, "[UI]\r\nfullscreen=true\r\n\r\n[DisabledAddOns]\r\nsize=0\r\n\r\n[Controls]\r\nx=1\r\n")
        self.assertEqual(bench.ini_apply_text(self.TEXT, {"UI": {"fullscreen": False}}),
                         self.TEXT.replace("fullscreen=true", "fullscreen\\default=false\r\nfullscreen=false"))

    def test_missing_and_last_section(self):
        self.assertEqual(bench.ini_apply_text("[UI]\nx=1\n", {}, ("0400486024DEC000", [])),
                         "[UI]\nx=1\n\n[DisabledAddOns]\nsize=0\n")
        self.assertEqual(bench.ini_apply_text("[DisabledAddOns]\nsize=3\njunk\n", {}, ("0400486024DEC000", [])),
                         "[DisabledAddOns]\nsize=0\n")

    def test_guard_with_a_base_file(self):
        with tempfile.TemporaryDirectory() as folder:
            live, base = os.path.join(folder, "qt-config.ini"), os.path.join(folder, "base.ini")
            with open(live, "wb") as file:
                file.write(b"[UI]\r\nmine=1\r\n")
            with open(base, "wb") as file:
                file.write(b"[UI]\r\nfullscreen=true\r\n\r\n[DisabledAddOns]\r\nsize=0\r\n")
            guard = bench.IniGuard(live)
            guard.apply({"UI": {"fullscreen": False}}, base, ("0400486024DEC000", ["Update"]))
            with open(live, "rb") as file:
                applied = file.read()
            self.assertNotIn(b"mine=1", applied)
            self.assertIn(b"fullscreen\\default=false\r\nfullscreen=false\r\n", applied)
            self.assertIn(b"1\\disabled\\1\\d=Update\r\n", applied)
            guard.restore()
            with open(live, "rb") as file:
                self.assertEqual(file.read(), b"[UI]\r\nmine=1\r\n")
            with open(base, "rb") as file:
                self.assertIn(b"size=0", file.read())


TRACE_LOG = """[  42.9] Render.Vulkan <Info> vk_scheduler.cpp:949:TraceGpuSubmission: DKB gpu_frame renderer_frame=2 last_submission_tick=12 gpu_frame_span_ms=61.804 gpu_submission_sum_ms=13.428 gpu_busy_ms=12.000 measured_submissions=5 dropped_submissions=0 valid_bits=36 period_ns=52.083332
[  42.9] Render.Vulkan <Info> vk_scheduler.cpp:771:ReportWorkTimes: DKB gpu_work renderer_frame=2 timed=1 dropped=0 timed_sum_ms=0.105 submission_sum_ms=3.872 untimed_ms=3.767 slowest: s1:16aff33d999f993f+s5:52402d8e8ab55263=0.105ms/1x(max 0.105ms size 6x1x0)
[  43.0] Frontend <Info> C:\\Users\\somebody\\lichii: an unrelated line
[  99.1] Render.Vulkan <Info> vk_scheduler.cpp:949:TraceGpuSubmission: DKB gpu_frame renderer_frame=3300 last_submission_tick=9000 gpu_frame_span_ms=24.500 gpu_submission_sum_ms=23.100 gpu_busy_ms=22.750 measured_submissions=6 dropped_submissions=0 valid_bits=36 period_ns=52.083332
[  99.1] Render.Vulkan <Info> vk_scheduler.cpp:771:ReportWorkTimes: DKB gpu_work renderer_frame=3300 timed=412 dropped=0 timed_sum_ms=21.900 submission_sum_ms=23.100 untimed_ms=1.200 slowest: s1:1917b5b970b5f601+s5:78134ce8125d5959#m=15.125ms/40x(max 2.500ms size 1920x1080x1) s6:5f43233f59d916f8=2.250ms/3x(max 1.000ms size 240x135x1)""" + "".join(
    f" s1:{index:016x}+s5:{index + 1:016x}=0.{9 - index}00ms/1x(max 0.{9 - index}00ms size 6x1x0)" for index in range(8)) + """
[  99.1] Render.Vulkan <Info> vk_scheduler.cpp:860:ReportPassTimes: DKB gpu_pass renderer_frame=3300 passes=31 timed=31 dropped=0 targets=9 pass_sum_ms=22.400 top: 1920x1080/B10G11R11_FLOAT/1/D32_FLOAT/1=16.500ms/4x(max 15.200ms) 2560x1440/A8B8G8R8_SRGB/1/-/1=0.107ms/1x(max 0.107ms)
"""


class HostToolsTest(unittest.TestCase):
    def test_power_policy_arguments(self):
        self.assertEqual(bench.parse_power_policy(["perfboostmode=0", "PROCTHROTTLEMIN=5, PROCTHROTTLEMIN1=0x5"]),
                         {"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5, "PROCTHROTTLEMIN1": 5})
        self.assertEqual(bench.parse_power_policy({"PROCTHROTTLEMAX": 99}), {"PROCTHROTTLEMAX": 99})
        self.assertEqual(bench.parse_power_policy(None), {})
        for bad in (["PERFBOOSTMODE"], ["PERFBOOSTMODE=off"], ["A B=1"], ["X=1 & calc"], ["X=-1"], {"X": True}):
            with self.assertRaises(bench.BenchError, msg=bad):
                bench.parse_power_policy(bad)

    def test_powercfg_query_parsing(self):
        settings = bench.parse_powercfg_query(FakeHost().query())
        self.assertEqual(settings, {"PERFBOOSTMODE": (2, 1), "PROCTHROTTLEMIN": (100, 5),
                                    "PROCTHROTTLEMIN1": (100, 5), "PROCTHROTTLEMAX": (100, 100)})
        # Another display language: only the layout is the same.
        german = FakeHost().query().replace("GUID Alias", "GUID-Alias").replace(
            "Current AC Power Setting Index", "Index der aktuellen Wechselstromeinstellung").replace(
            "Current DC Power Setting Index", "Index der aktuellen Gleichstromeinstellung")
        self.assertEqual(bench.parse_powercfg_query(german), settings)
        self.assertEqual(bench.parse_powercfg_query("Access is denied."), {})

    def test_power_policy_applies_and_restores(self):
        with tempfile.TemporaryDirectory() as folder:
            host = FakeHost()
            restore = os.path.join(folder, "state", "power-restore.json")
            policy = bench.PowerPolicy({"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5, "NOSUCHSETTING": 1}, host, restore)
            self.assertEqual(policy.apply(), {"applied": {"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5},
                                              "previous": {"PERFBOOSTMODE": 2, "PROCTHROTTLEMIN": 100},
                                              "skipped": ["NOSUCHSETTING"]})
            self.assertEqual(host.ac(), {"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5, "PROCTHROTTLEMIN1": 100,
                                         "PROCTHROTTLEMAX": 100})
            self.assertEqual(host.power["PROCTHROTTLEMIN"][1], 5)  # the battery value is not touched
            self.assertEqual(host.commands[0], ["powercfg", "/qh", "SCHEME_CURRENT", "SUB_PROCESSOR"])
            self.assertIn(["powercfg", "/setacvalueindex", "SCHEME_CURRENT", "SUB_PROCESSOR", "PROCTHROTTLEMIN", "5"],
                          host.commands)
            self.assertEqual(host.commands[-1], ["powercfg", "/setactive", "SCHEME_CURRENT"])
            with open(restore) as file:
                self.assertEqual(json.load(file), {"previous": {"PERFBOOSTMODE": 2, "PROCTHROTTLEMIN": 100}})
            policy.restore()
            self.assertEqual(host.ac(), FakeHost().ac())
            self.assertEqual(host.activated, 2)
            self.assertFalse(os.path.exists(restore))
            policy.restore()
            self.assertEqual(host.activated, 2)

    def test_power_policy_recovers_what_a_killed_run_left(self):
        with tempfile.TemporaryDirectory() as folder:
            host = FakeHost()
            restore = os.path.join(folder, "power-restore.json")
            bench.PowerPolicy({"PERFBOOSTMODE": 0}, host, restore).apply()  # never restored
            self.assertEqual(host.ac()["PERFBOOSTMODE"], 0)
            # The next run must not take the left-over 0 for the machine's own value.
            second = bench.PowerPolicy({"PERFBOOSTMODE": 1, "PROCTHROTTLEMAX": 80}, host, restore)
            self.assertEqual(second.apply()["previous"], {"PERFBOOSTMODE": 2, "PROCTHROTTLEMAX": 100})
            self.assertEqual(bench.PowerPolicy({}, host, restore).recover(), {"PERFBOOSTMODE": 2, "PROCTHROTTLEMAX": 100})
            self.assertEqual(host.ac(), FakeHost().ac())
            self.assertIsNone(bench.PowerPolicy({}, host, restore).recover())

    def test_power_policy_failure_is_reported_and_undone(self):
        with tempfile.TemporaryDirectory() as folder:
            host = FakeHost()
            host.fail = "PROCTHROTTLEMIN 5"
            policy = bench.PowerPolicy({"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5}, host,
                                       os.path.join(folder, "power-restore.json"))
            with self.assertRaisesRegex(bench.BenchError, "powercfg /setacvalueindex.*Access is denied"):
                policy.apply()
            self.assertEqual(host.ac()["PERFBOOSTMODE"], 0)
            host.fail = None
            policy.restore()
            self.assertEqual(host.ac(), FakeHost().ac())
            host.fail = "/qh"
            with self.assertRaisesRegex(bench.BenchError, "powercfg /qh"):
                bench.PowerPolicy({"PERFBOOSTMODE": 0}, host).apply()
            self.assertEqual(host.ac(), FakeHost().ac())

    def test_energy_counters(self):
        self.assertEqual(bench.parse_typeperf(TYPEPERF), {"package_w": 28.0, "cpu_w": 9.0, "gpu_w": 15.25})
        self.assertEqual(bench.parse_typeperf("Error: No valid counters.\n"), {})
        host = FakeHost()
        meter = bench.EnergyMeter(host)
        self.assertEqual(meter.read()["package_w"], 28.0)
        self.assertEqual(host.commands[0][:2], ["typeperf", "\\Energy Meter(rapl_package0_pkg)\\Power"])
        self.assertEqual(host.commands[0][-4:], ["-si", "1", "-sc", "2"])
        host.energy = False
        dead = bench.EnergyMeter(host)
        self.assertIsNone(dead.read())
        self.assertIsNone(dead.read())
        self.assertEqual(len(host.ran("typeperf")), 2)  # a machine without the counters is asked once

    def test_gpu_telemetry(self):
        self.assertEqual(bench.parse_gpu_telemetry(TELEMETRY),
                         {"gpu_mhz": 2200, "gpu_telemetry_w": 15.0, "gpu_limited_by": "Power"})
        self.assertEqual(bench.parse_gpu_telemetry("10:00:00 1800 9.5\n"), {"gpu_mhz": 1800, "gpu_telemetry_w": 9.5})
        self.assertEqual(bench.parse_gpu_telemetry("10:00:00  1800  9.5  Thermal Limit\n")["gpu_limited_by"],
                         "Thermal Limit")
        self.assertEqual(bench.parse_gpu_telemetry("the script failed\n"), {})
        host = FakeHost()
        self.assertEqual(bench.read_gpu_telemetry(None, host), {})
        self.assertEqual(bench.read_gpu_telemetry("C:\\tools\\gpu.ps1", host)["gpu_mhz"], 2200)
        self.assertEqual(host.commands, [["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                          "C:\\tools\\gpu.ps1", "-Samples", "3", "-IntervalMs", "700"]])

    def test_trace_summary_uses_the_last_lines(self):
        trace = bench.parse_trace(TRACE_LOG.splitlines())
        self.assertEqual(trace["frame"], {"renderer_frame": 3300, "gpu_frame_span_ms": 24.5,
                                          "gpu_submission_sum_ms": 23.1, "gpu_busy_ms": 22.75,
                                          "measured_submissions": 6, "dropped_submissions": 0})
        work = trace["work"]
        self.assertEqual((work["renderer_frame"], work["timed"], work["timed_sum_ms"], work["untimed_ms"]),
                         (3300, 412, 21.9, 1.2))
        self.assertEqual(len(work["top"]), 8)
        self.assertEqual(work["top"][0], {"shader": "s1:1917b5b970b5f601+s5:78134ce8125d5959", "kind": "m",
                                          "ms": 15.125, "count": 40, "max_ms": 2.5})
        self.assertEqual(work["top"][1], {"shader": "s6:5f43233f59d916f8", "ms": 2.25, "count": 3, "max_ms": 1.0})
        self.assertEqual((trace["pass"]["passes"], trace["pass"]["targets"], trace["pass"]["pass_sum_ms"]),
                         (31, 9, 22.4))
        self.assertEqual(trace["pass"]["top"], [
            {"target": "1920x1080/B10G11R11_FLOAT/1/D32_FLOAT/1", "ms": 16.5, "count": 4, "max_ms": 15.2},
            {"target": "2560x1440/A8B8G8R8_SRGB/1/-/1", "ms": 0.107, "count": 1, "max_ms": 0.107}])
        self.assertEqual(bench.private_strings(trace), [])
        self.assertIsNone(bench.parse_trace(["nothing traced", "DKB gpu_hist renderer_frame=1"]))
        old = bench.parse_trace(["DKB gpu_frame renderer_frame=2 gpu_frame_span_ms=61.804 measured_submissions=5"])
        self.assertEqual(old, {"frame": {"renderer_frame": 2, "gpu_frame_span_ms": 61.804, "measured_submissions": 5}})

    def test_collect_log(self):
        with tempfile.TemporaryDirectory() as folder:
            source = os.path.join(folder, "eden_log.txt")
            target = os.path.join(folder, "results", "logs", "row.log.txt")
            self.assertEqual(bench.collect_log(source, target), (False, None))
            with open(source, "wb") as file:
                file.write(TRACE_LOG.encode() + b"\xff odd byte\n")
            copied, trace = bench.collect_log(source, target)
            self.assertTrue(copied)
            self.assertEqual(trace["frame"]["renderer_frame"], 3300)
            with open(target, "rb") as file:
                self.assertTrue(file.read().endswith(b"\xff odd byte\n"))

    def test_this_process(self):
        memory = bench.process_memory(os.getpid())
        if memory is None:
            self.skipTest("this host does not report process memory")
        self.assertGreater(memory["working_set_mb"], 0)
        self.assertGreaterEqual(memory["working_set_peak_mb"], memory["working_set_mb"])
        size = bench.window_client_size(os.getpid())
        self.assertTrue(size is None or re.fullmatch(r"\d+x\d+", size))
        self.assertIsNone(bench.window_client_size(0x7ffffffe))

    def test_wait_gone(self):
        class Slow:
            polls = 0

            def poll(self):
                self.polls += 1
                return 0 if self.polls > 3 else None
        slept = []
        self.assertTrue(bench.wait_gone(Slow(), slept.append))
        self.assertEqual(slept, [0.5, 0.5, 0.5])
        self.assertTrue(bench.wait_gone(None, slept.append))
        stuck = Slow()
        stuck.polls = -1000
        self.assertFalse(bench.wait_gone(stuck, slept.append, timeout_s=2))

    def test_real_run_command_reports_a_missing_tool(self):
        code, output = bench.run_command(["no-such-tool-xyz-123"], 5)
        self.assertEqual(code, 1)
        self.assertTrue(output)


class HarnessRunTest(RunCase):
    """What replaces the PowerShell harness: base ini, mods, add-ons, power, telemetry, probes."""

    def setUp(self):
        super().setUp()
        self.library = os.path.join(self.folder, "mods")
        for mod in ("DualSense UI", "Arc Perf Aggressive", "Unused"):
            self.write(os.path.join(self.library, mod, "romfs", "file.bin"), mod.encode())
        self.write(os.path.join(self.user, "load", "0400486024DEC000", "Owner Mod", "x.bin"), b"owner's mod")
        self.base = os.path.join(self.folder, "base.ini")
        self.write(self.base, b"[Renderer]\r\nbackend\\default=true\r\nbackend=1\r\nfrom_base=1\r\n\r\n"
                              b"[DisabledAddOns]\r\nsize=0\r\n\r\n[UI]\r\nfullscreen=true\r\n")
        self.telemetry = os.path.join(self.folder, "gpu-telemetry.ps1")
        self.write(self.telemetry, b"")
        self.log_text = TRACE_LOG

    def variant_file(self, **fields):
        path = os.path.join(self.folder, f"{fields['name']}.json")
        self.write(path, json.dumps(fields).encode())
        return path

    def test_everything_at_once(self):
        self.emulator_options = {"methods": ("gpu_costs", "set_shading_rate", "power_set")}
        variant = self.variant_file(
            name="full", settings={"Renderer": {"backend": 2}}, env={"EDEN_NX2_VRS": "1"},
            mods=["DualSense UI", "Arc Perf Aggressive"], addons_off=["Update"],
            power_policy={"PROCTHROTTLEMIN": 5, "NOSUCHSETTING": 3}, late_s=150,
            live=[{"method": "set_shading_rate", "params": {"hashes": ["78134ce8125d5959"], "rate": "2x2"}},
                  {"method": "power_set", "params": {"spin_scale": 0.25}},
                  {"method": "gpu_costs", "params": {"enable": True}}])
        code, rows = self.run_bench("--variant", variant, "--base-ini", self.base, "--mod-library", self.library,
                                    "--power-policy", "PERFBOOSTMODE=0", "--power-policy", "PROCTHROTTLEMIN=50",
                                    "--gpu-telemetry", self.telemetry, "--sustain-min", "0.5")
        self.assertEqual(code, 0, self.log)
        row = rows[0]
        self.assertEqual(bench.validate_row(row), [])
        self.assertTrue(row["valid"])
        self.assertEqual(row["warnings"], ["power_policy_partial"])

        launch = self.launches[0]
        self.assertIn(b"from_base=1", launch["ini"])
        self.assertIn(b"backend\\default=false\r\nbackend=2\r\n", launch["ini"])
        self.assertIn(b"[DisabledAddOns]\r\nsize=1\r\n1\\title_id\\default=false\r\n1\\title_id=288309953924349952\r\n"
                      b"1\\disabled\\size=1\r\n1\\disabled\\1\\d\\default=false\r\n1\\disabled\\1\\d=Update\r\n\r\n[UI]",
                      launch["ini"])
        self.assertIn(b"fullscreen\\default=false\r\nfullscreen=false", launch["ini"])
        self.assertEqual(launch["mods"], ["Arc Perf Aggressive", "DualSense UI"])
        self.assertEqual(launch["power"], {"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5, "PROCTHROTTLEMIN1": 100,
                                           "PROCTHROTTLEMAX": 100})
        self.assertEqual(row["power_policy"], {"applied": {"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5},
                                               "previous": {"PERFBOOSTMODE": 2, "PROCTHROTTLEMIN": 100},
                                               "skipped": ["NOSUCHSETTING"]})
        self.assertEqual((row["base_ini"], row["mods"], row["addons_off"], row["probe"], row["reject"]),
                         ("base.ini", ["DualSense UI", "Arc Perf Aggressive"], ["Update"], False, []))
        self.assertEqual(row["live"][1], {"method": "power_set", "params": {"spin_scale": 0.25}})

        # No `power` method in this build: watts come from the energy counters.
        self.assertEqual(row["power"], {"package_w": 28.0, "cpu_w": 9.0, "gpu_w": 15.25, "source": "energy_meter"})
        self.assertEqual(row["watts"], 28.0)
        self.assertNotIn("no_power_reading", row["warnings"])
        self.assertEqual((row["gpu_mhz"], row["gpu_limited_by"], row["gpu_telemetry_w"]), (2200, "Power", 15.0))
        samples = row["sustained"]["samples"]
        self.assertEqual(len(samples), 3)
        for sample in samples:
            self.assertEqual((sample["package_w"], sample["gpu_w"], sample["gpu_mhz"], sample["gpu_limited_by"]),
                             (28.0, 15.25, 2200, "Power"))
        self.assertEqual((row["sustained"]["watts_avg"], row["sustained"]["gpu_mhz_min"]), (28.0, 2200))
        self.assertEqual(row["late"], {"after_s": 150.0, "fps": 50.0, "p50_ms": 20.0, "p99_ms": 30.0,
                                       "package_w": 28.0, "cpu_w": 9.0, "gpu_w": 15.25, "gpu_mhz": 2200,
                                       "gpu_telemetry_w": 15.0, "gpu_limited_by": "Power"})
        self.assertEqual([seconds for seconds in self.slept if seconds > 2], [10.0, 4.0, 50.0, 3.0, 150.0])
        self.assertEqual((row["window"], row["process_memory"]["working_set_peak_mb"]), ("1280x761", 4100))
        self.assertEqual(row["gpu_costs"]["enabled"], True)

        requests = launch["emulator"].requests
        methods = [request["method"] for request in requests]
        live = [request for request in requests if request["method"] in ("set_shading_rate", "power_set")]
        self.assertEqual(live[0]["params"], {"hashes": ["78134ce8125d5959"], "rate": "2x2"})
        self.assertLess(methods.index("screenshot"), methods.index("set_shading_rate"))
        self.assertLess(methods.index("power_set"), methods.index("frame_times"))
        self.assertEqual(methods.count("frame_times"), 7)

        self.assertEqual(row["id"], os.path.basename(row["screenshot"])[:-len(".png")])
        self.assertEqual(row["log"], f"logs/{row['id']}.log.txt")
        with open(os.path.join(self.out, "logs", f"{row['id']}.log.txt"), encoding="utf-8") as file:
            self.assertEqual(file.read(), TRACE_LOG)
        self.assertEqual(row["trace"]["work"]["top"][0]["ms"], 15.125)
        self.assertEqual(row["trace"]["frame"]["gpu_frame_span_ms"], 24.5)
        self.assertNotIn("somebody", json.dumps(row))
        self.assertNotIn(self.folder, json.dumps(row))
        self.assert_left_clean()

    def test_command_line_mods_apply_to_variants_without_a_list(self):
        none = self.variant_file(name="no-mods", mods=[])
        plain = self.variant_file(name="plain")
        code, rows = self.run_bench("--variant", plain, "--variant", none, "--mod-library", self.library,
                                    "--mod", "DualSense UI")
        self.assertEqual(code, 0, self.log)
        self.assertEqual([launch["mods"] for launch in self.launches], [["DualSense UI"], []])
        self.assertEqual([row["mods"] for row in rows], [["DualSense UI"], []])
        self.assertEqual([row["power_policy"] for row in rows], [None, None])
        self.assertEqual(self.host.ran("powercfg"), [])
        self.assert_left_clean()

    def test_without_mods_the_owners_stay_in_place(self):
        code, rows = self.run_bench()
        self.assertEqual((code, self.launches[0]["mods"], rows[0]["mods"]), (0, ["Owner Mod"], None))
        self.assert_left_clean()

    def test_probe_rejects_measures_and_restores(self):
        self.emulator_options = {"methods": ("reject_shaders",)}
        probe = self.variant_file(name="rej-78134ce8", reject=["78134CE8125D5959", "5f43233f59d916f8"])
        code, rows = self.run_bench("--variant", probe)
        self.assertEqual(code, 0, self.log)
        row = rows[0]
        # The picture changed after the rejection and the row is still valid: the scene was
        # checked before it and not after.
        self.assertEqual((row["valid"], row["probe"], row["scene_ok"], row["scene_diff"], row["fps"]),
                         (True, True, True, 0.0, 55.0))
        self.assertEqual(row["reject"], ["78134ce8125d5959", "5f43233f59d916f8"])
        self.assertEqual(bench.validate_row(row), [])
        requests = self.launches[0]["emulator"].requests
        methods = [request["method"] for request in requests]
        rejects = [request["params"] for request in requests if request["method"] == "reject_shaders"]
        self.assertEqual(rejects, [
            {"hashes": ["78134ce8125d5959", "5f43233f59d916f8"], "timeout_s": 20, "reject": True},
            {"hashes": ["78134ce8125d5959", "5f43233f59d916f8"], "timeout_s": 20, "reject": False}])
        first = methods.index("reject_shaders")
        self.assertLess(methods.index("screenshot"), first)
        self.assertLess(first, methods.index("frame_times"))
        self.assertEqual(methods[-1], "reject_shaders")
        self.assertIn(20.0, self.slept)
        with open(os.path.join(self.out, row["screenshot"]), "rb") as file:
            self.assertEqual(bench.image_diff(file.read(), solid(0)), 0)
        self.assert_left_clean()

        # table never counts a probe; rows of the same file that are not probes stay.
        code, rows = self.run_bench()
        table = bench.render_table(self.out, every=True)
        self.assertNotIn("rej-78134ce8", table)
        self.assertIn("| test-scene | baseline | 55.0 |", table)
        results = [name for name in os.listdir(self.out) if name.endswith(".jsonl")]
        self.assertEqual(bench.validate_file(os.path.join(self.out, results[0])), ([], 2))

    def test_probe_without_the_method_is_an_error_row(self):
        probe = self.variant_file(name="rej", reject=["78134ce8125d5959"])
        code, rows = self.run_bench("--variant", probe)
        self.assertEqual((code, rows[0]["invalid_reasons"], rows[0]["probe"]), (2, ["error"], True))
        self.assertIn("reject_shaders", rows[0]["error"])
        self.assertEqual(rows[0]["warnings"], ["reject_not_restored"])
        self.assert_left_clean()

    def test_failed_live_call_is_an_error_row(self):
        live = self.variant_file(name="live", live=[{"method": "set_shading_rate", "params": {"rate": "2x2"}}])
        code, rows = self.run_bench("--variant", live)
        self.assertEqual((code, rows[0]["invalid_reasons"], rows[0]["fps"]), (2, ["error"], None))
        self.assertIn("set_shading_rate", rows[0]["error"])
        self.assert_left_clean()

    def test_bad_variants_are_refused_before_anything_is_touched(self):
        for fields, message in (({"reject": ["78134ce8"]}, "16-digit"),
                                ({"mods": ["..\\elsewhere"]}, "plain names"),
                                ({"mods": "DualSense UI"}, "mods is a list"),
                                ({"live": ["gpu_costs"]}, "method, params"),
                                ({"power_policy": {"PERFBOOSTMODE": "fast"}}, "NAME=NUMBER"),
                                ({"late_s": "soon"}, "numbers")):
            with self.assertRaisesRegex(bench.BenchError, message, msg=fields):
                self.run_bench("--variant", self.variant_file(name="bad", **fields), "--mod-library", self.library)
        with self.assertRaisesRegex(bench.BenchError, "mod not found in the library"):
            self.run_bench("--variant", self.variant_file(name="bad", mods=["Missing Mod"]),
                           "--mod-library", self.library)
        with self.assertRaisesRegex(bench.BenchError, "mods need --mod-library"):
            self.run_bench("--mod", "DualSense UI")
        with self.assertRaisesRegex(bench.BenchError, "base ini not found"):
            self.run_bench("--base-ini", os.path.join(self.folder, "missing.ini"))
        with self.assertRaisesRegex(bench.BenchError, "telemetry script not found"):
            self.run_bench("--gpu-telemetry", os.path.join(self.folder, "missing.ps1"))
        with self.assertRaisesRegex(bench.BenchError, "names must differ"):
            self.run_bench("--variant", self.variant_file(name="twice"), "--variant", self.variant_file(name="twice"))
        self.assertEqual((self.launches, self.host.commands), ([], []))
        self.assert_left_clean()

    def test_power_policy_that_cannot_be_applied_is_an_error_row(self):
        self.host.fail = "PERFBOOSTMODE 0"
        code, rows = self.run_bench("--power-policy", "PROCTHROTTLEMIN=5,PERFBOOSTMODE=0")
        self.assertEqual((code, rows[0]["invalid_reasons"], self.launches), (2, ["error"], []))
        self.assertIn("powercfg /setacvalueindex", rows[0]["error"])
        self.assert_left_clean()
        # When even putting them back fails the run stops, and the restore file stays for the next start.
        self.host.fail = "/setactive"
        with self.assertRaisesRegex(bench.BenchError, "powercfg /setactive"):
            self.run_bench("--power-policy", "PERFBOOSTMODE=0")
        self.assertTrue(os.path.isfile(os.path.join(self.state, "power-restore.json")))
        self.host.fail = None
        self.assertEqual(self.run_bench()[0], 0)
        self.assert_left_clean()

    def test_crash_in_the_middle_restores_mods_and_power(self):
        def explode(*_):
            self.assertEqual(self.host.ac()["PERFBOOSTMODE"], 0)
            raise KeyboardInterrupt
        original = bench.play_input_script
        bench.play_input_script = explode
        self.addCleanup(setattr, bench, "play_input_script", original)
        with self.assertRaises(KeyboardInterrupt):
            self.run_bench("--mod-library", self.library, "--mod", "Unused", "--power-policy", "PERFBOOSTMODE=0",
                           "--base-ini", self.base)
        self.assertEqual(self.launches[0]["mods"], ["Unused"])
        self.assert_left_clean()

    def test_next_run_repairs_what_a_killed_run_left(self):
        mods = os.path.join(self.user, "load", "0400486024DEC000")
        bench.ModGuard(self.library, mods).apply(["Unused"])  # never restored
        bench.PowerPolicy({"PERFBOOSTMODE": 0}, self.host, os.path.join(self.state, "power-restore.json")).apply()
        self.assertEqual(os.listdir(mods), ["Unused"])
        code, rows = self.run_bench()
        self.assertEqual(code, 0, self.log)
        self.assertEqual((self.launches[0]["mods"], self.launches[0]["power"]["PERFBOOSTMODE"]), (["Owner Mod"], 2))
        self.assertIn("restored the mod folders an interrupted run had left aside", self.log)
        self.assertIn("restored the power settings an interrupted run had left changed", self.log)
        self.assert_left_clean()

    def test_mod_guard_when_the_title_has_no_mod_folder(self):
        target = os.path.join(self.folder, "elsewhere", "load", "0400486024DEC000")
        guard = bench.ModGuard(self.library, target)
        guard.apply(["DualSense UI"])
        self.assertEqual(os.listdir(target), ["DualSense UI"])
        guard.restore()
        self.assertEqual(os.listdir(target), [])
        self.assertEqual(os.listdir(os.path.dirname(target)), ["0400486024DEC000"])

    def test_settings_are_restored_only_once_the_emulator_is_gone(self):
        seen = {}

        def at_exit():
            with open(self.ini, "rb") as file:
                seen["ini"] = file.read()
            seen["power"] = self.host.ac()["PERFBOOSTMODE"]
            # The emulator writes its settings as it exits.
            with open(self.ini, "ab") as file:
                file.write(b"written_at_exit=1\r\n")
        self.process_options = {"linger": 3, "at_exit": at_exit}
        code, rows = self.run_bench("--variant", self.variant, "--power-policy", "PERFBOOSTMODE=0")
        self.assertEqual(code, 0, self.log)
        self.assertIn(b"backend=2", seen["ini"])
        self.assertEqual(seen["power"], 0)
        self.assertEqual(self.slept[-3:], [2, 0.5, 0.5])
        self.assertEqual(rows[0]["warnings"], [])
        self.assert_left_clean()

    def test_emulator_that_never_exits_is_reported(self):
        self.process_options = {"linger": 10**6}
        code, rows = self.run_bench()
        self.assertEqual((code, rows[0]["warnings"]), (0, ["emulator_still_running"]))
        self.launches[0]["emulator"].close()
        with open(self.ini, "rb") as file:
            self.assertEqual(file.read(), self.INI)

    def test_dry_run_lists_the_new_steps(self):
        variant = self.variant_file(name="full", mods=["DualSense UI"], addons_off=["Update"], late_s=150,
                                    reject=["78134ce8125d5959"], power_policy={"PROCTHROTTLEMIN": 5},
                                    live=[{"method": "gpu_costs", "params": {"enable": True}}])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code, rows = self.run_bench("--dry-run", "--variant", variant, "--base-ini", self.base,
                                        "--mod-library", self.library, "--power-policy", "PERFBOOSTMODE=0",
                                        "--gpu-telemetry", self.telemetry)
        text = output.getvalue()
        self.assertEqual((code, rows, self.launches, self.host.commands), (0, [], [], []))
        self.assertNotIn("PROBLEM", text)
        for expected in ("aside and copy in: DualSense UI", "replace the settings file with", "base.ini",
                         "set [DisabledAddOns] to: Update", "PERFBOOSTMODE=0 PROCTHROTTLEMIN=5",
                         'call gpu_costs {"enable": true}', "reject shaders 78134ce8125d5959 (probe row",
                         "wait 20 s", "gpu-telemetry.ps1", "wait 150 s, measure one more window",
                         "screenshot again, save it", "the mod folders, the power settings"):
            self.assertIn(expected, text)
        self.assert_left_clean()


class SuiteTest(RunCase):
    def suite_args(self, suite, *extra):
        parser = bench.build_parser()
        args = parser.parse_args([
            "suite", suite, "--exe", self.exe, "--game", self.game, "--user-dir", self.user,
            "--save-backup", self.backup, "--reference", self.reference, "--out", self.out,
            "--port", str(self.port), "--state-dir", self.state, *extra])
        return args, parser.suite_defaults

    def test_shipped_suite(self):
        args, defaults = self.suite_args(os.path.join(HERE, "suites", "igpu-switches.json"),
                                         "--power-policy", "PERFBOOSTMODE=0")
        plans, compare_to = bench.load_suite(args.suite, args, defaults)
        self.assertEqual(len(plans), 1)
        plan = plans[0]
        self.assertEqual((plan.suite, plan.scene["name"]), ("igpu-switches", "oot-hyrule-field"))
        env = {variant["name"]: variant.get("env") or {} for variant in plan.variants}
        trace = {"EDEN_DKB_FRAME_TRACE": "1"}
        vrs = {"EDEN_NX2_VRS": "1", "EDEN_NX2_VRS_HASHES": "78134ce8125d5959:2x2"}
        self.assertEqual(env, {
            "baseline": {}, "baseline-trace": trace,
            "vrs-2x2": vrs, "vrs-2x2-trace": {**vrs, **trace},
            "spin-scale-0.25": {"EDEN_NX2_SPIN_SCALE": "0.25"},
            "eco-background": {"EDEN_NX2_ECO_BACKGROUND": "1"},
            "low-memory": {"EDEN_NX2_LOW_MEMORY": "1"}})
        self.assertEqual(list(env)[:2], ["baseline", "baseline-trace"])
        self.assertEqual(compare_to[("oot-hyrule-field", "vrs-2x2")], "baseline")
        self.assertEqual(compare_to[("oot-hyrule-field", "vrs-2x2-trace")], "baseline-trace")
        self.assertEqual(compare_to[("oot-hyrule-field", "low-memory")], "baseline")
        self.assertTrue(all(variant["power_policy"] == {"PERFBOOSTMODE": 0} and not variant["reject"]
                            and "paired" not in variant for variant in plan.variants))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(bench.command_suite(args.__class__(**{**vars(args), "dry_run": True}), defaults,
                                                 self.deps), 0)
        self.assertIn("variant vrs-2x2-trace:", output.getvalue())
        self.assertEqual((self.launches, self.host.commands), ([], []))

    def test_shipped_presenter_and_locks_suite(self):
        args, defaults = self.suite_args(os.path.join(HERE, "suites", "presenter-and-locks.json"))
        plans, compare_to = bench.load_suite(args.suite, args, defaults)
        self.assertEqual(len(plans), 1)
        plan = plans[0]
        self.assertEqual((plan.suite, plan.scene["name"]), ("presenter-and-locks", "oot-kokiri-forest"))
        env = {variant["name"]: variant.get("env") or {} for variant in plan.variants}
        self.assertEqual(env, {
            "baseline": {},
            "present-spin": {"EDEN_NX2_PRESENT_SPIN": "1"},
            "gpl-optimize-cached": {"EDEN_NX2_GPL_OPTIMIZE_CACHED": "1"},
            "sched-pause": {"EDEN_NX2_SCHED_PAUSE": "1"},
            "syncpoint-split": {"EDEN_NX2_SYNCPOINT_SPLIT": "1"},
            "hybrid": {"EDEN_NX2_HYBRID": "1"},
            "idle-spin-0": {"EDEN_NX2_IDLE_SPIN_US": "0"}})
        self.assertEqual(list(env)[0], "baseline")
        self.assertTrue(all(variant["fullscreen"] and variant["pacing"] for variant in plan.variants))
        self.assertTrue(all(compare_to[("oot-kokiri-forest", name)] == "baseline" for name in env))
        # Every switch the suite turns on is a documented one.
        with open(os.path.join(HERE, "..", "..", "docs", "switches.md"), encoding="utf-8") as file:
            documented = file.read()
        for switches in env.values():
            for name in switches:
                self.assertIn(f"`{name}`", documented)

    def write_suite(self, **fields):
        path = os.path.join(self.folder, "suites", "mine.json")
        self.write(os.path.join(self.folder, "extra.json"), json.dumps(
            {"name": "from-file", "env": {"FILE": "1"}, "power_policy": {"PROCTHROTTLEMAX": 90}}).encode())
        suite = {"name": "mine", "scene": "../scene.json", "options": {"window_s": 5, "settle_s": 7},
                 "base": {"env": {"BASE": "1"}, "power_policy": {"PERFBOOSTMODE": 0}},
                 "paired": {"suffix": "-trace", "env": {"EDEN_DKB_FRAME_TRACE": "1"}},
                 "runs": [{"variants": [{"name": "baseline", "paired": True},
                                        {"name": "switch", "env": {"SWITCH": "1"}, "paired": True},
                                        "../extra.json"]},
                          {"options": {"window_s": 3}, "baseline": "other",
                           "variants": [{"name": "other", "env": {"BASE": "2"}}]}]}
        suite.update(fields)
        self.write(path, json.dumps(suite).encode())
        return path

    def test_suite_runs_and_prints_the_comparison(self):
        self.emulator_options = {"fps": (50.0, 60.0, 55.0, 44.0)}
        args, defaults = self.suite_args(self.write_suite())
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = bench.command_suite(args, defaults, self.deps)
        self.assertEqual(code, 0, self.log)
        rows = [row for _, row, _ in bench.read_rows(os.path.join(self.out, [
            name for name in os.listdir(self.out) if name.endswith(".jsonl")][0]))]
        self.assertEqual([row["variant"] for row in rows],
                         ["baseline", "baseline-trace", "switch", "switch-trace", "from-file", "other"])
        self.assertTrue(all(row["suite"] == "mine" and row["valid"] for row in rows))
        self.assertEqual([row["window_s"] for row in rows], [5.0] * 5 + [3.0])
        self.assertEqual([launch["env"].get("BASE") for launch in self.launches], ["1"] * 5 + ["2"])
        self.assertEqual(self.launches[3]["env"]["SWITCH"] + self.launches[3]["env"]["EDEN_DKB_FRAME_TRACE"], "11")
        self.assertNotIn("EDEN_DKB_FRAME_TRACE", self.launches[2]["env"])
        self.assertEqual(rows[4]["power_policy"]["applied"], {"PERFBOOSTMODE": 0, "PROCTHROTTLEMAX": 90})
        self.assertEqual(rows[0]["power_policy"]["applied"], {"PERFBOOSTMODE": 0})
        self.assertIn(7.0, self.slept)
        text = output.getvalue()
        self.assertIn("| scene | variant | fps | vs baseline |", text)
        self.assertIn("| test-scene | baseline | 55.0 | baseline | 18.2 | 27.3 | 28.0 | - | - | - | yes |", text)
        self.assertIn("| test-scene | switch | 55.0 | +0.0% vs baseline |", text)
        self.assertIn("| test-scene | switch-trace | 55.0 | +0.0% vs baseline-trace |", text)
        self.assertIn("| test-scene | other | 55.0 | baseline |", text)
        self.assert_left_clean()

    def test_comparison_table(self):
        rows = [dict(good_row(), variant="baseline", fps=50.0),
                dict(good_row(), variant="fast", fps=60.0, watts=27.5, gpu_mhz=2200, gpu_limited_by="Power",
                     late={"fps": 41.0}),
                dict(good_row(), variant="broken", fps=70.0, valid=False, invalid_reasons=["wrong_scene"]),
                dict(good_row(), variant="probe", fps=75.0, probe=True, reject=["78134ce8125d5959"]),
                dict(good_row(), variant="alone", fps=80.0)]
        compare_to = {("oot-hyrule-field", name): "baseline" for name in ("baseline", "fast", "broken", "probe")}
        text = bench.render_suite(rows, compare_to)
        self.assertIn("| oot-hyrule-field | baseline | 50.0 | baseline |", text)
        self.assertIn("| oot-hyrule-field | fast | 60.0 | +20.0% vs baseline | 18.2 | 27.3 | 27.5 | 2200 | Power | "
                      "41.0 | yes |", text)
        self.assertIn("| broken | 70.0 | - |", text)
        self.assertIn("INVALID: wrong_scene |", text)
        self.assertIn("| probe | 75.0 | +50.0% vs baseline |", text)
        self.assertIn("| yes (probe) |", text)
        self.assertIn("| alone | 80.0 | - |", text)

    def test_command_line_wins_over_suite_options(self):
        args, defaults = self.suite_args(self.write_suite(), "--window-s", "9", "--scene", self.scene)
        plans, _ = bench.load_suite(args.suite, args, defaults)
        self.assertEqual([(plan.window_s, plan.settle_s) for plan in plans], [(9.0, 7.0), (9.0, 7.0)])

    def test_bad_suites(self):
        for fields, message in (({"options": {"exe": "x"}}, "unknown option exe"),
                                ({"runs": []}, "non-empty list"),
                                ({"runs": [{"variants": []}]}, "without variants"),
                                ({"runs": [{"variants": [5]}]}, "object or a file name"),
                                ({"runs": [{"variants": [{"name": "a"}, {"name": "a"}]}]}, "names must differ"),
                                ({"scene": None}, "no scene")):
            args, defaults = self.suite_args(self.write_suite(**fields))
            with self.assertRaisesRegex(bench.BenchError, message, msg=fields):
                bench.load_suite(args.suite, args, defaults)

    def test_merge_variant(self):
        merged = bench.merge_variant(
            {"env": {"A": "1", "B": "1"}, "mods": ["x"], "settings": {"UI": {"a": 1}}, "power_policy": {"P": 1}},
            {"name": "v", "env": {"B": "2"}, "settings": {"UI": {"b": 2}}, "late_s": 5})
        self.assertEqual(merged, {"name": "v", "env": {"A": "1", "B": "2"}, "mods": ["x"], "late_s": 5,
                                  "settings": {"UI": {"a": 1, "b": 2}}, "power_policy": {"P": 1}})
        self.assertEqual(bench.merge_variant({}, {"name": "v"}), {"name": "v", "settings": {}})


class ProbeSchemaTest(unittest.TestCase):
    def test_probe_and_reject_go_together(self):
        row = dict(good_row(), probe=True, reject=["78134ce8125d5959"])
        self.assertEqual(bench.validate_row(row), [])
        self.assertTrue(any("probe" in problem for problem in bench.validate_row(dict(row, probe=False))))
        self.assertTrue(any("probe" in problem for problem in bench.validate_row(dict(good_row(), probe=True))))
        self.assertTrue(any("16-digit" in problem for problem in bench.validate_row(dict(row, reject=["78134ce8"]))))
        self.assertTrue(any("log" in problem
                            for problem in bench.validate_row(dict(good_row(), log="logs/../../x.log.txt"))))
        self.assertEqual(bench.validate_row(dict(good_row(), log="logs/2026-11-11-x.log.txt", probe=False,
                                                 reject=[], trace={"frame": {}}, power_policy=None)), [])


class FakeGame:
    def __init__(self, result=0):
        self.result = result
        self.waited = False

    def wait(self):
        self.waited = True
        if isinstance(self.result, BaseException):
            raise self.result
        return self.result


class PlayTest(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.folder = folder.name
        self.exe = os.path.join(self.folder, "lichii.exe")
        self.game = os.path.join(self.folder, "game.nup")
        for path in (self.exe, self.game):
            open(path, "wb").close()
        self.state = os.path.join(self.folder, "state")
        self.restore = os.path.join(self.state, "power-restore.json")
        self.host = FakeHost()
        self.host.survey = {"processes": [
            {"name": "chrome", "pid": 4242, "cpu_pct": 7.5, "ram_mb": 900, "gpu_mb": 1200},
            {"name": "dwm", "pid": 900, "cpu_pct": 9.0, "ram_mb": 100, "gpu_mb": 800},
            {"name": "notepad", "pid": 77, "cpu_pct": 0.5, "ram_mb": 20, "gpu_mb": 60}],
            "power_online": False, "battery_pct": 41}
        self.log = []
        self.launches = []
        self.running = set()
        self.game_result = 0
        test = self

        class TestDeps(play.Deps):
            run_command = staticmethod(self.host)
            running = staticmethod(lambda image: image in test.running)
            log = staticmethod(self.log.append)
            sleep = staticmethod(lambda seconds: None)

            @staticmethod
            def launch(exe, game, env):
                test.launches.append({"exe": exe, "game": game, "env": env, "power": test.host.ac(),
                                      "restore_file": os.path.isfile(test.restore)})
                return FakeGame(test.game_result)

        self.deps = TestDeps

    def profile(self, **fields):
        path = os.path.join(self.folder, "profile.json")
        with open(path, "w") as file:
            json.dump({"name": "test-profile", "env": {}, **fields}, file)
        return path

    def play(self, *extra):
        args = play.build_parser().parse_args(["--exe", self.exe, "--game", self.game, "--state-dir", self.state,
                                               *extra])
        return play.play(args, self.deps)

    def test_shipped_profile_has_no_switches_yet(self):
        profile = play.load_profile(play.DEFAULT_PROFILE)
        self.assertEqual(profile, {"name": "oot-intel-igpu", "env": {}, "power_policy": {}})
        with open(play.DEFAULT_PROFILE, encoding="utf-8") as file:
            self.assertIn("measured gain", json.load(file)["_comment_env"])

    def test_session_applies_the_profile_and_restores(self):
        self.game_result = 3
        code = self.play("--profile", self.profile(env={"EDEN_NX2_LOW_MEMORY": 1}, power_policy={"PERFBOOSTMODE": 0}),
                         "--power-policy", "PROCTHROTTLEMIN=5", "--power-policy", "NOSUCHSETTING=1")
        self.assertEqual(code, 3)
        launch = self.launches[0]
        self.assertEqual((launch["exe"], launch["game"]), (self.exe, self.game))
        self.assertEqual(launch["env"]["EDEN_NX2_LOW_MEMORY"], "1")
        self.assertNotIn(bench.DEBUG_ENV, {key for key in launch["env"] if key not in os.environ})
        self.assertEqual((launch["power"]["PERFBOOSTMODE"], launch["power"]["PROCTHROTTLEMIN"]), (0, 5))
        self.assertTrue(launch["restore_file"])
        self.assertEqual(self.host.ac(), FakeHost().ac())
        self.assertFalse(os.path.exists(self.restore))
        text = "\n".join(self.log)
        self.assertIn("PERFBOOSTMODE 2 -> 0, PROCTHROTTLEMIN 100 -> 5; not on this machine: NOSUCHSETTING", text)
        self.assertIn("environment: EDEN_NX2_LOW_MEMORY=1", text)
        self.assertIn("power settings restored", text)
        self.assertEqual(self.host.ran("taskkill"), [])

    def test_report_of_the_machine(self):
        self.play("--profile", self.profile())
        text = "\n".join(self.log)
        self.assertIn("top CPU: dwm 9%, chrome 7.5%, notepad 0.5%", text)
        self.assertIn("top GPU memory: chrome 1200 MB, dwm 800 MB, notepad 60 MB", text)
        self.assertIn("WARNING: chrome (pid 4242) uses 7.5% CPU and 1200 MB GPU memory", text)
        self.assertIn("--close chrome", text)
        self.assertNotIn("WARNING: dwm", text)
        self.assertNotIn("WARNING: notepad", text)
        self.assertIn("WARNING: on battery (41%)", text)
        self.assertEqual(self.host.ran("taskkill"), [])  # reported, not closed
        self.assertEqual(self.host.ran("powercfg"), [])  # no policy: power settings are not even read
        self.assertEqual(len(self.launches), 1)

    def test_quiet_machine_and_missing_survey(self):
        self.host.survey = {"processes": {"name": "notepad", "pid": 1, "cpu_pct": 1.0, "ram_mb": 5, "gpu_mb": 0},
                            "power_online": True, "battery_pct": 100}
        self.play("--profile", self.profile())
        self.assertIn("nothing heavy is running", self.log)
        self.assertFalse(any("WARNING" in line for line in self.log))
        self.assertEqual(play.describe_survey(None, "lichii.exe"),
                         ["could not list the running processes (needs Windows PowerShell): not checked"])
        self.assertIsNone(play.survey(lambda command, timeout: (1, "not recognized")))
        own = [{"name": "lichii", "pid": 5, "cpu_pct": 50.0, "gpu_mb": 3000}]
        self.assertEqual(play.heavy_processes(own, "lichii.exe"), [])
        self.assertEqual(play.heavy_processes(own, "other.exe"), own)

    def test_close_ends_only_the_named_processes(self):
        self.running = {"chrome.exe"}
        self.play("--profile", self.profile(), "--close", "chrome", "--close", "obs64.exe")
        self.assertEqual(self.host.ran("taskkill"), [["taskkill", "/IM", "chrome.exe"],
                                                     ["taskkill", "/F", "/IM", "chrome.exe"],
                                                     ["taskkill", "/IM", "obs64.exe"]])
        self.assertEqual(len(self.launches), 1)

    def test_dry_run_changes_nothing(self):
        self.running = {"lichii.exe"}
        code = self.play("--dry-run", "--close", "chrome", "--power-policy", "PERFBOOSTMODE=0,NOSUCHSETTING=1",
                         "--profile", self.profile(env={"A": "1"}))
        self.assertEqual((code, self.launches), (0, []))
        self.assertEqual(self.host.ac(), FakeHost().ac())
        self.assertEqual([command[:2] for command in self.host.commands if command[0] != "powershell"],
                         [["powercfg", "/qh"]])
        self.assertFalse(os.path.exists(self.state))
        text = "\n".join(self.log)
        for expected in ("would close chrome", "would set (AC, restored on exit): PERFBOOSTMODE 2 -> 0",
                         "not on this machine, skipped: NOSUCHSETTING", "would set environment: A=1",
                         f"would start {self.exe}", "WARNING: on battery",
                         "PROBLEM: lichii.exe is already running"):
            self.assertIn(expected, text)

    def test_power_left_by_a_killed_session_is_restored_at_the_next_start(self):
        bench.PowerPolicy({"PERFBOOSTMODE": 0, "PROCTHROTTLEMIN": 5}, self.host, self.restore).apply()
        self.assertEqual(self.play("--dry-run", "--profile", self.profile()), 0)
        self.assertEqual(self.host.ac()["PERFBOOSTMODE"], 0)  # a dry run does not even repair
        self.assertTrue(any("would restore the power settings" in line for line in self.log))
        self.play("--profile", self.profile())
        self.assertEqual(self.launches[0]["power"], FakeHost().ac())
        self.assertFalse(os.path.exists(self.restore))
        self.assertTrue(any("restored the power settings an earlier session left changed" in line
                            for line in self.log))

    def test_interrupt_and_launch_failure_restore_the_power_settings(self):
        self.game_result = KeyboardInterrupt()
        self.assertEqual(self.play("--profile", self.profile(), "--power-policy", "PERFBOOSTMODE=0"), 130)
        self.assertEqual(self.launches[0]["power"]["PERFBOOSTMODE"], 0)
        self.assertEqual(self.host.ac(), FakeHost().ac())

        def broken(exe, game, env):
            raise OSError("cannot start")
        self.deps.launch = staticmethod(broken)
        with self.assertRaises(OSError):
            self.play("--profile", self.profile(), "--power-policy", "PERFBOOSTMODE=0")
        self.assertEqual(self.host.ac(), FakeHost().ac())
        self.assertFalse(os.path.exists(self.restore))

    def test_refusals(self):
        self.running = {"lichii.exe"}
        with self.assertRaisesRegex(bench.BenchError, "already running"):
            self.play("--profile", self.profile())
        self.running = set()
        os.remove(self.game)
        with self.assertRaisesRegex(bench.BenchError, "game not found"):
            self.play("--profile", self.profile())
        with self.assertRaisesRegex(bench.BenchError, "profile"):
            self.play("--profile", os.path.join(self.folder, "missing.json"))
        self.assertEqual((self.launches, self.host.commands), ([], []))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(play.main(["--exe", self.exe, "--game", self.game, "--state-dir", self.state]), 1)
        self.assertIn("game not found", errors.getvalue())


# The presenter's lines as the emulator logs them with EDEN_NX2_SHOWN_STATS=1 (formats from
# vk_present_manager.cpp FluidLogStats and NoteShownFrame, vk_swapchain.cpp Swapchain::Create).
# Two reports: the row takes the last one.
LOG_HEAD = "] Render.Vulkan <Info> video_core/renderer_vulkan/"
PRESENTER_LOG = "".join(f"[{seconds:4d}.000000{LOG_HEAD}{line}\n" for seconds, line in (
    (2, "vk_swapchain.cpp:597:Create: Swapchain: VK_PRESENT_MODE_FIFO_KHR, 3 images (3 asked, surface min 2 max 8), "
        "1280x720 VK_FORMAT_B8G8R8A8_UNORM VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; surface present modes: "
        "VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR; vsync setting Fifo, frame generation off"),
    (4, "vk_swapchain.cpp:597:Create: Swapchain: VK_PRESENT_MODE_FIFO_KHR, 5 images (5 asked, surface min 2 max 8), "
        "2880x1800 VK_FORMAT_B8G8R8A8_UNORM VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; surface present modes: "
        "VK_PRESENT_MODE_FIFO_KHR; vsync setting Fifo, frame generation on"),
    (40, "vk_present_manager.cpp:1364:FluidLogStats: Fluid presenter (10 s, DXGI WaitForVBlank at 120.00 Hz, a slot "
         "every 1 refreshes): 1200 slots, 790 shown on their slot, 410 missed (300 with no frame ready: 200 scheduled "
         "but their GPU work not done, 100 with nothing scheduled; 100 with the last copy still queued, 10 slept "
         "through), 0 shown in arrival order, 500 copies done after their vblank"),
    (40, "vk_present_manager.cpp:1372:FluidLogStats: Fluid presenter (10 s): 61% of shown frames held one slot, held "
         "1/2/3/4+ refreshes 700/300/100/90; real frame age at display 30.2 ms; GPU work done 12.5 ms after recording "
         "(201 real frames); 205 frames received, 4 dropped unshown"),
    (40, "vk_present_manager.cpp:1379:FluidLogStats: Fluid presenter (10 s): 3 late or blocked frames shown a slot "
         "later instead of dropped, 0 shown frames predicted from an older real frame than the newest, 0 frames "
         "dropped as older than a real frame no prediction follows; real frames: 201 recorded, 0 not predictable, 0 "
         "with fewer predictions than slots for lack of a free frame, 0 ended by the prediction limit"),
    (41, "vk_present_manager.cpp:565:NoteShownFrame: Shown frames: 98.0 per second, interval p5 7.9 p50 8.3 p95 16.7 "
         "p99 25.0 max 41.6 ms, 24% within 2 ms of the median"),
    (50, "vk_present_manager.cpp:1364:FluidLogStats: Fluid presenter (10 s, DXGI WaitForVBlank at 119.88 Hz, a slot "
         "every 1 refreshes): 1199 slots, 1136 shown on their slot, 63 missed (40 with no frame ready: 30 scheduled "
         "but their GPU work not done, 10 with nothing scheduled; 20 with the last copy still queued, 3 slept "
         "through), 0 shown in arrival order, 0 copies done after their vblank"),
    (50, "vk_present_manager.cpp:1372:FluidLogStats: Fluid presenter (10 s): 88% of shown frames held one slot, held "
         "1/2/3/4+ refreshes 1000/120/10/2; real frame age at display 21.4 ms; GPU work done 9.8 ms after recording "
         "(230 real frames); 231 frames received, 1 dropped unshown"),
    (51, "vk_present_manager.cpp:565:NoteShownFrame: Shown frames: 112.4 per second, interval p5 7.9 p50 8.3 p95 9.1 "
         "p99 16.6 max 25.0 ms, 82% within 2 ms of the median"),
))
PACING = {"clock": "DXGI WaitForVBlank", "display_hz": 119.88, "slot_refreshes": 1, "slots": 1199, "on_slot": 1136,
          "missed": 63, "real_received": 231, "dropped_unshown": 1, "shown_fps": 112.4, "interval_p5_ms": 7.9,
          "interval_p50_ms": 8.3, "interval_p95_ms": 9.1, "interval_p99_ms": 16.6, "interval_max_ms": 25.0,
          "within_2ms_pct": 82, "head_at_s": 50.0, "shown_at_s": 51.0}


def hud_over(background, width=64, height=36):
    """A picture of one colour with a HUD: hearts at the top left, a map at the bottom right."""
    def pixel(x, y):
        if (x < 8 and y < 4) or (x >= 54 and y >= 28):
            return (230, 210, 120)
        return background(x, y) if callable(background) else background
    return make_png(width, height, pixel)


class PresenterLogTest(unittest.TestCase):
    def test_last_report_and_swapchain(self):
        pacing, swapchain = bench.parse_presenter(PRESENTER_LOG.splitlines())
        self.assertEqual(pacing, PACING)
        self.assertEqual(swapchain, {"present_mode": "VK_PRESENT_MODE_FIFO_KHR", "images": 5, "size": "2880x1800"})
        self.assertEqual(bench.private_strings(pacing), [])

    def test_partial_and_missing_lines(self):
        self.assertEqual(bench.parse_presenter(["nothing here", "Fluid presenter (10 s): no head"]), (None, None))
        # Without FLUID only the shown frames are logged.
        shown_only = [line for line in PRESENTER_LOG.splitlines() if "Shown frames" in line]
        self.assertEqual(set(bench.parse_presenter(shown_only)[0]),
                         {key for key in PACING if key.startswith(("shown", "interval", "within"))})
        # Without the log prefix the report is still read, only its time is left out.
        self.assertNotIn("shown_at_s", bench.parse_presenter([line.split("] ", 1)[1] for line in shown_only])[0])
        # A report cut off after its first line does not borrow the previous report's received count.
        lines = PRESENTER_LOG.splitlines()
        cut = lines[:-2]
        pacing = bench.parse_presenter(cut)[0]
        self.assertEqual((pacing["slots"], pacing["shown_fps"]), (1199, 98.0))
        self.assertNotIn("real_received", pacing)

    def test_read_presenter(self):
        self.assertEqual(bench.read_presenter(None), (None, None))
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "row.log.txt")
            with open(path, "wb") as file:
                file.write(PRESENTER_LOG.encode() + b"\xff odd byte\n")
            self.assertEqual(bench.read_presenter(path)[0], PACING)

    def test_swapchain_of_the_last_report(self):
        # The window shrinks while the emulator shuts down: a swapchain made after the last report
        # is not the one that was measured.
        later = PRESENTER_LOG.splitlines() + [PRESENTER_LOG.splitlines()[0].replace("[   2.", "[  60.")]
        self.assertEqual(bench.parse_presenter(later)[1]["size"], "2880x1800")
        # With no report the last swapchain is all there is.
        swapchains = [line for line in later if "Swapchain: " in line]
        self.assertEqual(bench.parse_presenter(swapchains)[1]["size"], "1280x720")


class PictureTest(unittest.TestCase):
    LIMITS = bench.picture_limits(30)

    def stats(self, png):
        return bench.picture_stats(png, self.LIMITS)

    def test_limits_follow_the_scene_threshold(self):
        self.assertEqual(self.LIMITS, {"dark_level": 15.0, "dark_pct": 85, "mean": 30})
        self.assertEqual(bench.picture_limits(40, 90), {"dark_level": 20.0, "dark_pct": 90, "mean": 40})

    def test_hud_over_black_is_black(self):
        stats = self.stats(hud_over((0, 0, 0)))
        self.assertTrue(stats["black"])
        self.assertGreater(stats["dark_pct"], 90)
        self.assertLess(stats["mean"], 30)
        self.assertGreater(stats["spread"], 30)  # the HUD alone gives the picture its spread
        self.assertTrue(self.stats(solid(0))["black"])
        self.assertTrue(self.stats(hud_over((6, 8, 12)))["black"])  # a clear colour just above black

    def test_lit_and_night_pictures_are_not(self):
        self.assertEqual(self.stats(solid(100)), {"mean": 100.0, "spread": 0.0, "dark_pct": 0.0, "center_dark_pct": 0.0,
                                                  "center_mean": 100.0, "black": False})
        self.assertFalse(self.stats(make_png(64, 36, gradient))["black"])
        # Night: a dark blue sky over dark green ground. Darker than the scene threshold on average,
        # but each cell has a channel at 15 or more, so it is not near black.
        night = hud_over(lambda x, y: (8, 16, 40) if y < 18 else (6, 17, 9))
        stats = self.stats(night)
        self.assertLess(stats["mean"], 30)
        self.assertLess(stats["dark_pct"], 10)
        self.assertFalse(stats["black"])
        # A dim room with a bright window covering a fifth of the picture: mostly dark, not black.
        window = make_png(64, 36, lambda x, y: (200, 190, 170) if 20 <= x < 40 and y < 24 else (4, 4, 4))
        self.assertFalse(self.stats(window)["black"])

    def test_full_size_hud_over_black_is_black(self):
        # A HUD the size of the game's at 960x540 (hearts, rupees, the button cluster, the map) lights
        # a fifth of the whole 32x18 grid, enough to defeat a whole-picture 85% rule, but none of its centre.
        def big_hud(background):
            boxes = ((20, 15, 190, 45), (20, 480, 120, 525), (700, 5, 950, 115), (780, 375, 950, 530))
            def pixel(x, y):
                if any(left <= x < right and top <= y < bottom for left, top, right, bottom in boxes):
                    return (230, 210, 120)
                return background(x, y) if callable(background) else background
            return make_png(960, 540, pixel)
        black = self.stats(big_hud((0, 0, 0)))
        self.assertLess(black["dark_pct"], self.LIMITS["dark_pct"])
        self.assertEqual(black["center_dark_pct"], 100.0)
        self.assertTrue(black["black"])
        # Night: a dark blue sky over ground just above black, under the same HUD, is not black.
        night = self.stats(big_hud(lambda x, y: (8, 10, 30) if y < 270 else (12, 14, 10)))
        self.assertEqual(night["center_dark_pct"], 50.0)
        self.assertFalse(night["black"])
        # A broken frame cleared to dark grey is not near black: only the scene check against a
        # reference catches it.
        self.assertFalse(self.stats(big_hud((20, 20, 20)))["black"])


class PacingRunTest(RunCase):
    """Fullscreen runs, the pacing lines of the log and the picture check in a whole run."""

    def variant_file(self, **fields):
        path = os.path.join(self.folder, f"{fields['name']}.json")
        self.write(path, json.dumps(fields).encode())
        return path

    def test_windowed_is_the_default(self):
        code, rows = self.run_bench()
        self.assertEqual(code, 0, self.log)
        row = rows[0]
        self.assertIn(b"fullscreen=false", self.launches[0]["ini"])
        # Without --pacing the presenter path is the one rows had before: no EDEN_NX2_SHOWN_STATS.
        self.assertNotIn(bench.SHOWN_STATS_ENV, self.launches[0]["env"])
        self.assertEqual((row["fullscreen"], row["fullscreen_requested"], row["shown_stats"], row["output"],
                          row["pacing"]), (False, False, False, {"size": "1280x761", "size_from": "window"}, None))
        self.assertEqual(row["picture"], {"limits": {"dark_level": 15.0, "dark_pct": 85.0, "mean": 30.0},
                                          "shots": [{"mean": 100.0, "spread": 0.0, "dark_pct": 0.0,
                                                     "center_dark_pct": 0.0, "center_mean": 100.0}] * 2,
                                          "black": False})
        self.assertEqual(bench.validate_row(row), [])
        self.assert_left_clean()

    def test_fullscreen_run_reads_the_log(self):
        self.log_text = PRESENTER_LOG
        code, rows = self.run_bench("--fullscreen", "--pacing", "--variant", self.variant)
        self.assertEqual(code, 0, self.log)
        row = rows[0]
        self.assertIn(b"fullscreen\\default=false\r\nfullscreen=true\r\n", self.launches[0]["ini"])
        self.assertNotIn(b"fullscreen=false", self.launches[0]["ini"])
        self.assertEqual(self.launches[0]["env"][bench.SHOWN_STATS_ENV], "1")
        self.assertNotIn(bench.SHOWN_STATS_ENV, row["variant_env"])
        self.assertTrue(row["fullscreen"] and row["shown_stats"])
        self.assertEqual(row["output"], {"size": "2880x1800", "size_from": "swapchain",
                                         "fullscreen_mode": "borderless" if bench.IS_WINDOWS else "exclusive",
                                         "fullscreen_mode_from": "default",
                                         "present_mode": "VK_PRESENT_MODE_FIFO_KHR", "images": 5})
        self.assertEqual(row["pacing"], PACING)
        # The fake's window is 1280x761: a fullscreen swapchain of another size is worth a warning.
        self.assertEqual(row["window"], "1280x761")
        self.assertIn("output_size_not_window_size", row["warnings"])
        self.assertTrue(row["valid"])
        self.assertEqual(bench.validate_row(row), [])
        self.assert_left_clean()

    def test_variant_field_wins_over_the_command_line(self):
        windowed = self.variant_file(name="windowed", fullscreen=False)
        exclusive = self.variant_file(name="exclusive", settings={"Renderer": {"fullscreen_mode": 1}})
        code, rows = self.run_bench("--fullscreen", "--variant", windowed, "--variant", exclusive)
        self.assertEqual(code, 0, self.log)
        self.assertEqual([row["fullscreen"] for row in rows], [False, True])
        self.assertEqual(rows[1]["output"], {"size": "1280x761", "size_from": "window", "fullscreen_mode": "exclusive",
                                             "fullscreen_mode_from": "settings"})
        with self.assertRaisesRegex(bench.BenchError, "fullscreen is true or false"):
            self.run_bench("--variant", self.variant_file(name="bad", fullscreen="yes"))
        self.assert_left_clean()

    def test_fullscreen_off_in_the_settings_is_a_windowed_row(self):
        variant = self.variant_file(name="asked", fullscreen=True, settings={"UI": {"fullscreen": False}})
        code, rows = self.run_bench("--variant", variant)
        self.assertEqual((code, rows[0]["fullscreen"], rows[0]["warnings"]),
                         (0, False, ["fullscreen_off_in_settings"]))
        self.assertTrue(any("the run is windowed" in line for line in self.log))

    def test_fullscreen_row_without_an_output_size_is_invalid(self):
        self.deps.window_size = staticmethod(lambda pid: None)
        code, rows = self.run_bench("--fullscreen")
        self.assertEqual((code, rows[0]["invalid_reasons"], rows[0]["output"]["size"]),
                         (2, ["output_size_unknown"], None))
        self.assertEqual(bench.validate_row(rows[0]), [])

    def test_black_picture_is_refused_without_a_reference(self):
        os.remove(self.reference)
        self.emulator_options = {"shot": hud_over((0, 0, 0))}
        code, rows = self.run_bench()
        row = rows[0]
        self.assertEqual((code, row["invalid_reasons"], row["fps"]), (2, ["black_picture", "error"], None))
        self.assertIn("black picture", row["error"])
        self.assertIn("as a HUD would be", row["error"])
        self.assertTrue(row["picture"]["black"])
        self.assertTrue(os.path.isfile(os.path.join(self.out, row["screenshot"])))
        self.assertNotIn("frame_times", [request["method"] for request in self.launches[0]["emulator"].requests])
        self.assertEqual(bench.validate_row(row), [])
        self.assert_left_clean()

    def test_black_picture_is_never_recorded_as_the_reference(self):
        os.remove(self.reference)
        self.emulator_options = {"shot": solid(0)}
        code, rows = self.run_bench("--record-reference")
        self.assertEqual((code, rows[0]["invalid_reasons"]), (2, ["black_picture", "error"]))
        self.assertIn("nothing is drawn: not measured, no reference written", rows[0]["error"])
        self.assertFalse(os.path.exists(self.reference))

    def test_dry_run(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.run_bench("--dry-run", "--fullscreen")
        text = output.getvalue()
        self.assertNotIn(bench.SHOWN_STATS_ENV, text)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.run_bench("--dry-run", "--fullscreen", "--pacing")
        text = output.getvalue()
        for expected in ("start the emulator fullscreen ([UI] fullscreen on unless",
                         f"{bench.SHOWN_STATS_ENV}=1",
                         "refuse a black picture (85% of its centre under 15 and a centre mean under 30)",
                         "read its pacing and swapchain lines"):
            self.assertIn(expected, text)
        self.assertNotIn("fullscreen=false", text)

    def test_suite_base_sets_fullscreen(self):
        path = os.path.join(self.folder, "suites", "full.json")
        self.write(path, json.dumps({"name": "full", "scene": "../scene.json", "base": {"fullscreen": True},
                                     "runs": [{"variants": [{"name": "baseline"},
                                                            {"name": "windowed", "fullscreen": False}]}]}).encode())
        parser = bench.build_parser()
        args = parser.parse_args(["suite", path, "--exe", self.exe, "--game", self.game, "--user-dir", self.user])
        plans, _ = bench.load_suite(args.suite, args, parser.suite_defaults)
        self.assertEqual([variant["fullscreen"] for variant in plans[0].variants], [True, False])


class PacingSchemaTest(unittest.TestCase):
    def problems(self, **fields):
        return bench.validate_row(dict(good_row(), **fields))

    def test_new_fields_are_optional_and_typed(self):
        self.assertEqual(self.problems(), [])
        self.assertEqual(self.problems(fullscreen=False, output=None, pacing=PACING, picture={
            "limits": bench.picture_limits(30), "shots": [], "black": False}), [])
        self.assertIn("pacing: shown_fps has type str", self.problems(pacing=dict(PACING, shown_fps="fast")))
        self.assertIn("output: size is WxH", self.problems(output={"size": "big", "size_from": "window"}))
        self.assertIn("output: size_from is swapchain or window", self.problems(output={"size": "1x1", "size_from": "x"}))

    def test_fullscreen_rows_say_their_size(self):
        self.assertIn("a valid fullscreen row needs output.size", self.problems(fullscreen=True))
        self.assertEqual(self.problems(fullscreen=True, output={"size": "2880x1800", "size_from": "swapchain"}), [])

    def test_black_picture_needs_its_reason(self):
        picture = {"limits": bench.picture_limits(30), "shots": [], "black": True}
        self.assertIn("a black picture requires the black_picture reason", self.problems(picture=picture))
        self.assertEqual(self.problems(picture=picture, valid=False, invalid_reasons=["black_picture"]), [])


class DisplayTableTest(unittest.TestCase):
    setUp = TableTest.setUp
    write = TableTest.write

    def fullscreen(self, **fields):
        return dict(good_row(), fullscreen=True, output={"size": "2880x1800", "size_from": "swapchain"}, **fields)

    def test_header_is_the_old_one_without_new_fields(self):
        self.assertEqual(bench.table_header(), bench.TABLE_HEADER)

    def test_rows_from_before_render_as_before(self):
        # Rows in the shape written before fullscreen, output, pacing and picture existed. The
        # expected page is what bench.py rendered for them before that change.
        rows = [good_row(), dict(good_row(), variant="fsr", valid=False, invalid_reasons=["wrong_scene"],
                                 scene_ok=False, fps=40.0, timestamp_utc="2026-11-12T12:00:00Z")]
        self.assertEqual([bench.validate_row(row) for row in rows], [[], []])
        self.write("2026-11-11-x.jsonl", rows)
        self.assertEqual(bench.render_table(self.folder), (
            "# Guarana benchmark results\n\n"
            f"Generated by `{bench.TABLE_COMMAND}`; do not edit by hand.\n"
            "fps is the median of three windows. Rows marked INVALID (wrong scene, busy machine, error) are not "
            "comparable.\n\n"
            f"## Machine `{bench.machine_id(HARDWARE)}`\n\n"
            "Example CPU 9; Example GPU (driver 1.2.3); 31.4 GB RAM; Example OS 11 build 26340; display "
            "2880x1800@120; AC\n\n"
            "| scene | variant | fps | p50 ms | p99 ms | sustained min / avg fps | watts | scene_ok | valid | date |\n"
            "|---|---|---:|---:|---:|---:|---:|---|---|---|\n"
            "| oot-hyrule-field | baseline | 55.0 | 18.2 | 27.3 | - | - | yes | yes | 2026-11-11 |\n"
            "| oot-hyrule-field | fsr | 40.0 | 18.2 | 27.3 | - | - | NO | INVALID: wrong_scene | 2026-11-12 |\n"))

    def test_failed_fullscreen_attempt_is_not_listed_as_windowed(self):
        failed = dict(good_row(), fullscreen=None, fullscreen_requested=True, valid=False, invalid_reasons=["error"])
        self.assertEqual(bench.display_mode(failed), "fullscreen")
        self.assertEqual(bench.display_mode(dict(good_row(), fullscreen=False, fullscreen_requested=True)), "windowed")

    def test_notes_are_paragraphs_of_their_own(self):
        self.write("2026-11-11-x.jsonl", [good_row(), self.fullscreen(fps=48.0, pacing=PACING)])
        text = bench.render_table(self.folder)
        self.assertIn("are not comparable.\n\n" + bench.MIXED_NOTE + "\n\nThe shown-frame columns", text)
        self.assertIn("ran with `--pacing`.\n\n## Machine", text)

    def test_windowed_and_fullscreen_rows_are_listed_apart(self):
        self.write("2026-11-11-x.jsonl", [good_row(), self.fullscreen(fps=48.0, pacing=PACING)])
        text = bench.render_table(self.folder)
        self.assertIn(bench.MIXED_NOTE, text)
        self.assertIn("| scene | variant | display | fps | p50 ms | p99 ms | sustained min / avg fps | watts | shown fps "
                      "| shown interval p50 / p99 ms | within 2 ms of p50 | on their slot | scene_ok | valid | date |",
                      text)
        self.assertIn("| oot-hyrule-field | baseline | fullscreen 2880x1800 | 48.0 | 18.2 | 27.3 | - | - | 112.4 | "
                      "8.3 / 16.6 | 82% | 1136 / 1199 | yes | yes | 2026-11-11 |", text)
        self.assertIn("| oot-hyrule-field | baseline | windowed | 55.0 | 18.2 | 27.3 | - | - | - | - / - | - | - | yes "
                      "| yes |", text)

    def test_pacing_columns_without_fullscreen_rows(self):
        self.write("2026-11-11-x.jsonl", [dict(good_row(), pacing=PACING)])
        text = bench.render_table(self.folder)
        self.assertNotIn("| display |", text)
        self.assertNotIn(bench.MIXED_NOTE, text)
        self.assertIn("| 112.4 | 8.3 / 16.6 | 82% | 1136 / 1199 |", text)

    def test_compare_never_crosses_display_modes(self):
        text = bench.render_compare(self.write("a.jsonl", [good_row()]),
                                    self.write("b.jsonl", [self.fullscreen(fps=66.0)]))
        self.assertIn("| oot-hyrule-field | baseline | fullscreen | - | 66.0 | - |", text)
        self.assertIn("| oot-hyrule-field | baseline | windowed | 55.0 | - | - |", text)
        self.assertIn(bench.MIXED_NOTE, text)

    def test_suite_says_when_it_does_not_compare(self):
        rows = [good_row(), self.fullscreen(variant="full", fps=60.0, pacing=PACING)]
        compare_to = {("oot-hyrule-field", "baseline"): "baseline", ("oot-hyrule-field", "full"): "baseline"}
        text = bench.render_suite(rows, compare_to)
        self.assertIn("| oot-hyrule-field | full | fullscreen 2880x1800 | 60.0 | not compared: baseline ran windowed | "
                      "18.2 | 27.3 | 112.4 | 82% |", text)
        self.assertIn(bench.MIXED_NOTE, text)


class KokiriSceneTest(unittest.TestCase):
    def test_scene_for_a_fresh_save(self):
        path = os.path.join(HERE, "scenes", "oot-kokiri-forest.json")
        scene = bench.load_json(path, "scene")
        self.assertEqual((scene["name"], scene["title_id"], scene["reference"]),
                         ("oot-kokiri-forest", "0400486024DEC000", "reference/oot-kokiri-forest.png"))
        self.assertEqual(scene["input"], [{"wait_s": 25, "buttons": ["A"], "frames": 6}])
        self.assertEqual((scene["settle_s"], scene["window_s"], scene["scene_threshold"]), (50, 15, 30))
        self.assertTrue(any("ten minutes" in note for note in scene["notes"]))
        self.assertEqual(bench.private_strings(scene), [])
        emulator = FakeEmulator()
        self.addCleanup(emulator.close)
        slept = []
        bench.play_input_script(bench.DebugClient(port=emulator.port), scene["input"], slept.append)
        self.assertEqual((slept, [request["params"]["buttons"] for request in emulator.requests]), ([25], [["A"]]))


if __name__ == "__main__":
    unittest.main()
