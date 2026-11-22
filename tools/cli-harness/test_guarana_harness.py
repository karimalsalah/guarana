#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the guarana-cli JSON harness (standard-library unittest, no emulator needed).

Run:  py -3.13 -m unittest discover -s tools/cli-harness -v
Covers: envelope shape, RPC client against a fake TCP debug server (framing, retry, errors),
warm-pipelines log parsing, nx2_switches.txt parsing, screenshot decode/save, launch argv/env
builder, and end-to-end main() JSON output with a mocked guarana-cli subprocess.
"""

import base64
import contextlib
import io
import json
import os
import socket
import subprocess
import tempfile
import threading
import unittest
from unittest import mock

import guarana_harness as h

# A minimal valid 1x1 PNG (signature + IHDR + IDAT + IEND), used as fake screenshot payload.
TINY_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="
)

WARM_SAMPLE = """Config <Info> ... WriteToIni noise
warm: 0400486024DEC000: precache 4515/4515 — 4515/4515 pipelines built (29.1 pipelines/s)
warm: done after 155.0 s: 4515 cached graphics pipelines, 0 already built for this driver, 0 left
"""


class FakeDebugServer:
    """Threaded TCP server speaking the nx2 newline-JSON protocol (one request/connection)."""

    def __init__(self, responses):
        self.responses = responses  # method -> result dict, or Exception to raise
        self.requests = []
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(8)
        self.port = self.sock.getsockname()[1]
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            with conn:
                data = b""
                while b"\n" not in data:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                if not data:
                    continue
                request = json.loads(data.split(b"\n", 1)[0])
                self.requests.append(request)
                method = request.get("method")
                reply = {"id": request.get("id")}
                if method in self.responses:
                    value = self.responses[method]
                    if isinstance(value, Exception):
                        reply["error"] = {"message": str(value)}
                    else:
                        reply["result"] = value
                else:
                    reply["error"] = {"message": f"unknown method {method}"}
                conn.sendall((json.dumps(reply) + "\n").encode("utf-8"))

    @property
    def address(self):
        return f"127.0.0.1:{self.port}"

    def close(self):
        self.sock.close()


class EnvelopeTests(unittest.TestCase):
    def test_ok_envelope_shape(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = h.emit("status", {"fps": 60.0})
        self.assertEqual(code, 0)
        env = json.loads(out.getvalue())
        self.assertEqual(env, {"ok": True, "command": "status", "data": {"fps": 60.0},
                               "error": None, "exit_code": 0})

    def test_error_envelope_shape(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = h.fail("bench", "rpc_unreachable", "down", exit_code=2)
        self.assertEqual(code, 2)
        env = json.loads(out.getvalue())
        self.assertFalse(env["ok"])
        self.assertEqual(env["error"], {"kind": "rpc_unreachable", "message": "down"})
        self.assertEqual(env["exit_code"], 2)


class RpcClientTests(unittest.TestCase):
    def test_status_roundtrip_and_framing(self):
        server = FakeDebugServer({"status": {"title": "OoT", "fps": 59.9, "scm_rev": "abc123"}})
        try:
            result = h.rpc_call(server.address, "status", {"fps_window_ms": 500})
            self.assertEqual(result["title"], "OoT")
            self.assertEqual(server.requests[0]["method"], "status")
            self.assertEqual(server.requests[0]["params"], {"fps_window_ms": 500})
            self.assertIn("id", server.requests[0])
        finally:
            server.close()

    def test_unknown_method_is_rpc_error(self):
        server = FakeDebugServer({})
        try:
            with self.assertRaises(h.HarnessError) as ctx:
                h.rpc_call(server.address, "switches")  # field name, NOT a method
            self.assertEqual(ctx.exception.kind, "rpc_error")
            self.assertIn("unknown method", ctx.exception.message)
        finally:
            server.close()

    def test_unreachable_retries_then_fails(self):
        # Bind then close to get a almost-certainly-free port.
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        with self.assertRaises(h.HarnessError) as ctx:
            h.rpc_call(f"127.0.0.1:{port}", "status", retries=2, backoff_s=0.01)
        self.assertEqual(ctx.exception.kind, "rpc_unreachable")

    def test_parse_server_forms(self):
        self.assertEqual(h.parse_server("6545"), ("127.0.0.1", 6545))
        self.assertEqual(h.parse_server("127.0.0.1:7000"), ("127.0.0.1", 7000))
        with self.assertRaises(h.HarnessError):
            h.parse_server("not-a-port")


class WarmParseTests(unittest.TestCase):
    def test_full_progress_and_final(self):
        data = h.parse_warm_output(WARM_SAMPLE, 0)
        self.assertEqual(data["title_id"], "0400486024DEC000")
        self.assertEqual(data["pipelines_cached"], 4515)
        self.assertEqual(data["pipelines_built"], 4515)
        self.assertEqual(data["pipelines_left"], 0)
        self.assertEqual(data["elapsed_s"], 155.0)
        self.assertEqual(data["outcome"], "done")
        self.assertEqual(data["exit_code"], 0)
        self.assertAlmostEqual(data["build_rate_per_s"], 29.1)

    def test_exit_code_semantics(self):
        self.assertEqual(h.parse_warm_output("", 3)["outcome"], "run_again")
        self.assertEqual(h.parse_warm_output("", 4)["outcome"], "incomplete")
        self.assertEqual(h.parse_warm_output("", 1)["outcome"], "failed")

    def test_incomplete_final_line(self):
        line = ("warm: incomplete after 40.0 s: 4515 cached graphics pipelines, "
                "1200 already built for this driver, 3315 left\n")
        data = h.parse_warm_output(line, 4)
        self.assertEqual(data["outcome"], "incomplete")
        self.assertEqual(data["pipelines_already_built"], 1200)
        self.assertEqual(data["pipelines_left"], 3315)


class SwitchesFileTests(unittest.TestCase):
    def test_comments_blanks_and_values(self):
        text = "# launcher-managed block\nCADENCE=3\n\nSHOWN_RATE=120\nFLUID_INTERP=0 # note\n"
        self.assertEqual(h.parse_switches_file(text),
                         {"CADENCE": "3", "SHOWN_RATE": "120", "FLUID_INTERP": "0 # note"})


class ScreenshotTests(unittest.TestCase):
    def test_decode_and_save(self):
        server = FakeDebugServer({"screenshot": {
            "width": 1, "height": 1, "png_base64": base64.b64encode(TINY_PNG).decode()}})
        try:
            with tempfile.TemporaryDirectory() as tmp:
                out = os.path.join(tmp, "shot.png")
                argv = ["--server", server.address, "screenshot", "--out", out]
                stdout = io.StringIO()
                with contextlib.redirect_stdout(stdout):
                    code = h.main(argv)
                self.assertEqual(code, 0)
                env = json.loads(stdout.getvalue())
                self.assertTrue(env["ok"])
                self.assertEqual(env["data"]["bytes"], len(TINY_PNG))
                self.assertEqual(env["data"]["width"], 1)
                self.assertNotIn("png_base64", env["data"])  # no base64 spam by default
                with open(out, "rb") as handle:
                    self.assertEqual(handle.read(), TINY_PNG)
        finally:
            server.close()


class LaunchBuilderTests(unittest.TestCase):
    def test_argv_and_env(self):
        args = h.build_parser().parse_args(
            ["--server", "127.0.0.1:6545", "launch", "--game", "C:/games/oot.xci",
             "--fullscreen", "--user", "2", "--gdb-port", "24689"])
        argv, env = h.build_launch("C:/lb/build-1120/bin/guarana-cli.exe", args)
        self.assertEqual(argv, ["C:/lb/build-1120/bin/guarana-cli.exe", "-g",
                                "C:/games/oot.xci", "-f", "-u", "2", "-d", "24689"])
        # The env value is the PORT (not "1") — the RPC only listens once emulation boots.
        self.assertEqual(env["EDEN_NX2_DEBUG_SERVER"], "6545")


class EndToEndJsonTests(unittest.TestCase):
    """main() with a mocked guarana-cli binary: stdout must be exactly one JSON object."""

    def _run_main(self, argv, fake_proc):
        stdout = io.StringIO()
        with mock.patch.object(h.subprocess, "run", return_value=fake_proc), \
                contextlib.redirect_stdout(stdout):
            code = h.main(argv)
        lines = [ln for ln in stdout.getvalue().splitlines() if ln.strip()]
        self.assertEqual(len(lines), 1, "stdout must carry exactly one JSON line")
        return code, json.loads(lines[0])

    def test_warm_pipelines_json(self):
        fake = subprocess.CompletedProcess(args=[], returncode=0, stdout=WARM_SAMPLE, stderr="")
        code, env = self._run_main(
            ["--cli", __file__, "warm-pipelines", "--title-id", "0400486024DEC000"], fake)
        self.assertEqual(code, 0)
        self.assertEqual(env["command"], "warm-pipelines")
        self.assertTrue(env["ok"])
        self.assertEqual(env["data"]["pipelines_cached"], 4515)
        self.assertEqual(env["data"]["elapsed_s"], 155.0)

    def test_warm_bad_title_id(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = h.main(["--cli", __file__, "warm-pipelines", "--title-id", "zzz"])
        self.assertEqual(code, 1)
        env = json.loads(stdout.getvalue())
        self.assertEqual(env["error"]["kind"], "bad_title_id")

    def test_nup_pack_json(self):
        fake = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="",
            stderr="\r12 / 40 MiB, 3 / 9 files\nWrote C:/dumps/oot.nup in 3.2 s\n")
        code, env = self._run_main(
            ["--cli", __file__, "nup-pack", "--dump", "C:/dumps/oot"], fake)
        self.assertEqual(code, 0)
        self.assertEqual(env["data"]["archive"], "C:/dumps/oot.nup")
        self.assertEqual(env["data"]["elapsed_s"], 3.2)

    def test_version_json(self):
        fake = subprocess.CompletedProcess(
            args=[], returncode=0, stdout="guarana-cli 20.7.0\n", stderr="")
        code, env = self._run_main(["--cli", __file__, "version"], fake)
        self.assertEqual(code, 0)
        self.assertEqual(env["data"]["version"], "guarana-cli 20.7.0")

    def test_version_log_noise_filtered(self):
        # Real guarana-cli -v output shape: log spam + the version inside ApplyLaunchParams.
        sample = ("[   0.018594] Config <Info> frontend_common/config.cpp:107:WriteToIni: x\n"
                  "[   0.022494] Frontend <Info> core/launch_params.cpp:248:ApplyLaunchParams: "
                  "Eden merge/upstream-20261120 58afd454f6-merge/upstream-20261120\n"
                  "[   0.024603] Config <Info> frontend_common/config.cpp:107:WriteToIni: y\n")
        version, _ = h.parse_version_output(sample)
        self.assertEqual(version, "Eden merge/upstream-20261120 58afd454f6-merge/upstream-20261120")

    def test_rpc_unreachable_envelope(self):
        probe = socket.socket()
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
        probe.close()
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = h.main(["--server", f"127.0.0.1:{port}", "--timeout", "0.2", "status"])
        self.assertEqual(code, 1)
        env = json.loads(stdout.getvalue())
        self.assertFalse(env["ok"])
        self.assertEqual(env["error"]["kind"], "rpc_unreachable")


if __name__ == "__main__":
    unittest.main()
