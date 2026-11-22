#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-machine benchmark for Guarana, driven through its debug server (docs/nx2/debug-mcp.md).

  python -I tools/bench/bench.py run --exe <guarana> --game <game> --scene scenes/<scene>.json
  python -I tools/bench/bench.py suite suites/<suite>.json --exe <guarana> --game <game>
  python -I tools/bench/bench.py table results/
  python -I tools/bench/bench.py compare a.jsonl b.jsonl
  python -I tools/bench/bench.py validate results/<date>-<machine>.jsonl

Standard library only; Pillow is used for the scene check when it is installed. See README.md.
"""

import argparse
import base64
import collections
import copy
import csv
import datetime
import difflib
import getpass
import glob
import hashlib
import importlib
import json
import math
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

SCHEMA_VERSION = 1
HERE = os.path.dirname(os.path.abspath(__file__))
DEBUG_ENV = "EDEN_NX2_DEBUG_SERVER"
DEFAULT_PORT = 6545
THUMB = (32, 18)
OPTIONAL_METHODS = ("power", "memory", "gpu_costs")
WINDOWED_SETTINGS = {"UI": {"fullscreen": False}}
FULLSCREEN_SETTINGS = {"UI": {"fullscreen": True}}
# Set only for --pacing runs (or a variant's "pacing": true): the presenter then logs the pacing
# lines the row's "pacing" is read from. It is more than logging: with FLUID it also adds GPU
# timestamps to every presenter copy, so it changes the measured path and is off by default.
SHOWN_STATS_ENV = "EDEN_NX2_SHOWN_STATS"
# [Renderer] fullscreen_mode values (common/settings_enums.h FullscreenMode).
FULLSCREEN_MODES = {"0": "borderless", "1": "exclusive"}
# The picture check: a screenshot is black when this share of its 32x18 cells is near black.
PICTURE_DARK_PCT = 85
# The cells of the 32x18 grid the check looks at: columns 6-25, rows 4-13, where a HUD does not draw.
PICTURE_CENTER = (range(6, 26), range(4, 14))
BASELINE_VARIANT = {"name": "baseline", "settings": {}, "env": {}}
IS_WINDOWS = sys.platform == "win32"
# Boot suites (a suite's "boot" block): the state of the title's shader cache, and of the driver
# cache folders given with --driver-cache-dir, when a run starts. cold: none; second: what the same
# variant's cold run of the same repeat left; warm: a copy of the owner's cache as it was when the
# suite started.
CACHE_STATES = ("cold", "second", "warm")
NX2_TOOLS = os.path.join(os.path.dirname(HERE), "nx2")
# A boot run closes the emulator and waits this long for it to save its caches and exit.
BOOT_CLOSE_GRACE_S = 90
# vk_pipeline_cache.cpp: the last boot ended while building cached pipelines, so this one builds none.
BOOT_GUARD_TEXT = "building none at boot this time"
# vk_pipeline_cache.cpp SerializeVulkanPipelineCache: a save of the driver pipeline cache.
DRIVER_CACHE_SAVED_TEXT = "Vulkan driver pipelines cached at"
# vk_pipeline_cache.cpp: the driver keeps its own cache (NVIDIA, AMD), so the emulator saves none.
DRIVER_KEEPS_OWN_CACHE_TEXT = "Driver keeps its own pipeline cache, not saving a Vulkan one"
FIRST_FRAME_PHASE = "frontend: first guest frame presented"
GAME_SHOWN_PHASE = "frontend: loading screen hidden, game shown"
GUEST_RELEASED_PHASE = "guest: GPU started, guest cores released"
INSTANCE_CREATED_PHASE = "vulkan: instance created"


class BenchError(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# qt-config.ini editing

def ini_format(value):
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def ini_section_bounds(lines, section):
    """(header index, end index) of [section], or None. The end excludes the next header."""
    header = f"[{section}]"
    for start, line in enumerate(lines):
        if line.strip() == header:
            end = start + 1
            while end < len(lines) and not lines[end].startswith("["):
                end += 1
            return start, end
    return None


def ini_set(lines, section, values):
    """Sets key=value and key\\default=false inside one section; None resets a key to default.

    Returns a new list of lines. A missing section is appended."""
    lines = list(lines)
    bounds = ini_section_bounds(lines, section)
    if bounds is None:
        while lines and lines[-1] == "":
            lines.pop()
        if lines:
            lines.append("")
        lines.append(f"[{section}]")
        lines.append("")
        bounds = (len(lines) - 2, len(lines) - 1)
    start, end = bounds
    for key, value in values.items():
        key_line = default_line = -1
        for index in range(start + 1, end):
            if lines[index].startswith(f"{key}="):
                key_line = index
            elif lines[index].startswith(f"{key}\\default="):
                default_line = index
        if value is None:
            if default_line >= 0:
                lines[default_line] = f"{key}\\default=true"
            continue
        if default_line >= 0:
            lines[default_line] = f"{key}\\default=false"
        if key_line >= 0:
            lines[key_line] = f"{key}={ini_format(value)}"
            if default_line < 0:
                lines.insert(key_line, f"{key}\\default=false")
                end += 1
        else:
            fresh = [f"{key}={ini_format(value)}"]
            if default_line < 0:
                fresh.insert(0, f"{key}\\default=false")
            lines[start + 1:start + 1] = fresh
            end += len(fresh)
    return lines


def ini_set_addons(lines, title_id, off):
    """Rewrites [DisabledAddOns] so that exactly these add-ons of one title are disabled."""
    lines = list(lines)
    bounds = ini_section_bounds(lines, "DisabledAddOns")
    if bounds is None:
        while lines and lines[-1] == "":
            lines.pop()
        lines += ["", "[DisabledAddOns]"] if lines else ["[DisabledAddOns]"]
        bounds = (len(lines) - 1, len(lines))
    block = ["size=0"]
    if off:
        block = ["size=1", "1\\title_id\\default=false", f"1\\title_id={int(title_id, 16)}",
                 f"1\\disabled\\size={len(off)}"]
        for index, name in enumerate(off, 1):
            block += [f"1\\disabled\\{index}\\d\\default=false", f"1\\disabled\\{index}\\d={name}"]
    lines[bounds[0] + 1:bounds[1]] = block + [""]
    return lines


def ini_apply_text(text, settings, addons=None):
    """Applies {section: {key: value}} and (title id, disabled add-ons) to ini text, keeping its
    line endings."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    for section, values in settings.items():
        lines = ini_set(lines, section, values)
    if addons is not None:
        lines = ini_set_addons(lines, *addons)
    return newline.join(lines)


def ini_value(text, section, key):
    """The value of key in [section] of ini text; None when it is missing or marked default."""
    lines = text.splitlines()
    bounds = ini_section_bounds(lines, section)
    if bounds is None:
        return None
    value = None
    for line in lines[bounds[0] + 1:bounds[1]]:
        if line.strip() == f"{key}\\default=true":
            return None
        if line.startswith(f"{key}="):
            value = line[len(key) + 1:].strip()
    return value


def merge_settings(*layers):
    merged = {}
    for layer in layers:
        for section, values in (layer or {}).items():
            merged.setdefault(section, {}).update(values)
    return merged


class IniGuard:
    """Edits an ini file and puts the original bytes back on restore().

    The original is kept next to the file while it is modified, so a run that was killed is
    repaired by the next one."""

    def __init__(self, path):
        self.path = path
        self.backup = path + ".bench-backup"
        self.active = False

    def recover(self):
        if os.path.isfile(self.backup):
            shutil.copyfile(self.backup, self.path)
            os.remove(self.backup)
            return True
        return False

    def apply(self, settings, base=None, addons=None):
        """base: an ini whose content replaces the live one before the settings are applied."""
        if not os.path.isfile(self.path):
            raise BenchError(f"settings file not found: {self.path} (run the emulator once, or "
                             "pass --user-dir / --ini)")
        self.recover()
        shutil.copyfile(self.path, self.backup)
        self.active = True
        with open(base or self.path, "rb") as file:
            text = file.read().decode("utf-8", "surrogateescape")
        text = ini_apply_text(text, settings, addons)
        with open(self.path, "wb") as file:
            file.write(text.encode("utf-8", "surrogateescape"))

    def restore(self):
        if self.active:
            self.recover()
            self.active = False


# ---------------------------------------------------------------------------------------------
# PNG decoding and the scene check

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def _unfilter(kind, line, previous, bpp):
    if kind == 0:
        return line
    out = bytearray(line)
    size = len(out)
    if kind == 1:
        for i in range(bpp, size):
            out[i] = (out[i] + out[i - bpp]) & 255
    elif kind == 2:
        for i in range(size):
            out[i] = (out[i] + previous[i]) & 255
    elif kind == 3:
        for i in range(size):
            left = out[i - bpp] if i >= bpp else 0
            out[i] = (out[i] + ((left + previous[i]) >> 1)) & 255
    elif kind == 4:
        for i in range(size):
            a = out[i - bpp] if i >= bpp else 0
            b = previous[i]
            c = previous[i - bpp] if i >= bpp else 0
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            out[i] = (out[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    else:
        raise BenchError(f"PNG: unknown filter {kind}")
    return out


def decode_png(data):
    """(width, height, RGB bytes) of a non-interlaced 8 or 16 bit PNG."""
    if data[:8] != PNG_MAGIC:
        raise BenchError("not a PNG file")
    position = 8
    header = palette = None
    compressed = []
    while position + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[position:position + 8])
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            compressed.append(body)
        elif kind == b"IEND":
            break
    if header is None:
        raise BenchError("PNG: no IHDR")
    width, height, depth, colour, _, _, interlace = header
    if interlace or depth not in (8, 16) or colour not in PNG_CHANNELS:
        raise BenchError(f"PNG: unsupported format (depth {depth}, colour type {colour}, "
                         f"interlace {interlace}); install Pillow or use an 8-bit RGB PNG")
    step = depth // 8
    bpp = PNG_CHANNELS[colour] * step
    stride = width * bpp
    raw = zlib.decompress(b"".join(compressed))
    if len(raw) < (stride + 1) * height:
        raise BenchError("PNG: truncated image data")
    rgb = bytearray(width * height * 3)
    previous = bytearray(stride)
    for y in range(height):
        offset = y * (stride + 1)
        line = _unfilter(raw[offset], raw[offset + 1:offset + 1 + stride], previous, bpp)
        previous = line
        target = y * width * 3
        if colour == 3:
            if palette is None:
                raise BenchError("PNG: no palette")
            for x in range(width):
                rgb[target + 3 * x:target + 3 * x + 3] = palette[3 * line[x]:3 * line[x] + 3]
        elif colour in (0, 4):
            grey = line[0::bpp]
            for channel in range(3):
                rgb[target + channel:target + 3 * width:3] = grey
        else:
            for channel in range(3):
                rgb[target + channel:target + 3 * width:3] = line[channel * step::bpp]
    return width, height, bytes(rgb)


def box_thumbnail(width, height, rgb, size=THUMB):
    """Box-averaged downscale to size; a flat list of R, G, B values."""
    out = []
    for ty in range(size[1]):
        y0 = ty * height // size[1]
        y1 = max(y0 + 1, (ty + 1) * height // size[1])
        for tx in range(size[0]):
            x0 = tx * width // size[0]
            x1 = max(x0 + 1, (tx + 1) * width // size[0])
            count = (x1 - x0) * (y1 - y0)
            for channel in range(3):
                total = 0
                for y in range(y0, y1):
                    row = y * width * 3
                    total += sum(rgb[row + x0 * 3 + channel:row + x1 * 3:3])
                out.append(total / count)
    return out


def thumbnail(data, size=THUMB):
    try:
        from PIL import Image
    except ImportError:
        return box_thumbnail(*decode_png(data), size)
    import io
    with Image.open(io.BytesIO(data)) as image:
        return list(image.convert("RGB").resize(size, Image.BOX).tobytes())


def image_diff(png_a, png_b):
    """Mean absolute difference (0-255) of two PNGs, both shrunk to 32x18."""
    a, b = thumbnail(png_a), thumbnail(png_b)
    return round(sum(abs(x - y) for x, y in zip(a, b)) / len(a), 1)


def picture_limits(scene_threshold, dark_pct=PICTURE_DARK_PCT):
    """The picture check's limits, from the scene check's own arithmetic.

    The scene check calls two 32x18 downscales different scenes when their mean absolute
    difference reaches scene_threshold. Against an all-black frame that difference is the
    picture's mean: a picture with a mean under scene_threshold is, by that measure, a black
    screen. A cell is near black when even its brightest channel is under half that limit."""
    return {"dark_level": round(scene_threshold / 2, 1), "dark_pct": dark_pct, "mean": scene_threshold}


def picture_stats(png, limits):
    """Mean and spread (standard deviation) of the cell brightness of a 32x18 downscale, the
    share of near-black cells, and whether the picture is black by the limits.

    The verdict uses only the central cells (PICTURE_CENTER): at this downscale every HUD edge
    lights a whole cell, so a full-size HUD over a black frame lights a fifth of the whole grid
    but none of its centre."""
    values = thumbnail(png)
    cells = [values[index:index + 3] for index in range(0, len(values), 3)]
    brightness = [sum(cell) / 3 for cell in cells]
    mean = sum(brightness) / len(brightness)
    spread = math.sqrt(sum((value - mean) ** 2 for value in brightness) / len(brightness))
    dark = sum(1 for cell in cells if max(cell) < limits["dark_level"]) * 100 / len(cells)
    center = [row * THUMB[0] + column for row in PICTURE_CENTER[1] for column in PICTURE_CENTER[0]]
    center_dark = sum(1 for index in center if max(cells[index]) < limits["dark_level"]) * 100 / len(center)
    center_mean = sum(brightness[index] for index in center) / len(center)
    return {"mean": round(mean, 1), "spread": round(spread, 1), "dark_pct": round(dark, 1),
            "center_dark_pct": round(center_dark, 1), "center_mean": round(center_mean, 1),
            "black": center_dark >= limits["dark_pct"] and center_mean < limits["mean"]}


# ---------------------------------------------------------------------------------------------
# Debug server client

class DebugError(Exception):
    pass


class DebugClient:
    """One JSON line per request, one connection per request (the emulator may restart)."""

    def __init__(self, host="127.0.0.1", port=DEFAULT_PORT):
        self.host = host
        self.port = port

    def listening(self):
        try:
            socket.create_connection((self.host, self.port), timeout=2).close()
            return True
        except OSError:
            return False

    def call(self, method, params=None, timeout=30):
        deadline = time.monotonic() + timeout
        try:
            with socket.create_connection((self.host, self.port), timeout=5) as sock:
                sock.sendall(json.dumps({"id": 1, "method": method,
                                         "params": params or {}}).encode() + b"\n")
                buffer = b""
                while b"\n" not in buffer:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise socket.timeout()
                    sock.settimeout(remaining)
                    chunk = sock.recv(1 << 20)
                    if not chunk:
                        raise DebugError(f"{method}: the emulator closed the connection")
                    buffer += chunk
        except socket.timeout as error:
            raise DebugError(f"{method}: no answer within {timeout} s") from error
        except OSError as error:
            raise DebugError(f"{method}: {error}") from error
        try:
            response = json.loads(buffer.partition(b"\n")[0])
        except ValueError as error:
            raise DebugError(f"{method}: invalid answer") from error
        if "error" in response:
            raise DebugError(f"{method}: {response['error']}")
        return response.get("result")

    def try_call(self, method, params=None, timeout=20):
        """The result, or None when the server lacks the method or it fails."""
        try:
            return self.call(method, params, timeout)
        except DebugError:
            return None


def play_input_script(client, steps, sleep=time.sleep):
    """Runs [{wait_s, buttons, left_stick, frames}]: wait, then hold the input for frames."""
    for step in steps:
        if step.get("wait_s"):
            sleep(float(step["wait_s"]))
        params = {key: step[key] for key in ("buttons", "left_stick", "right_stick") if step.get(key)}
        if not params:
            continue
        params["frames"] = int(step.get("frames", 6))
        params["timeout_s"] = int(step.get("timeout_s", 30))
        client.call("input", params, params["timeout_s"] + 10)


def take_screenshot(client, height=540):
    """PNG bytes of the next presented frame."""
    result = client.call("screenshot", {"save": False, "inline": True, "height": height}, 40) or {}
    if result.get("png_base64"):
        return base64.b64decode(result["png_base64"])
    for field in ("path", "file", "saved"):
        path = result.get(field)
        if isinstance(path, str) and os.path.isfile(path):
            with open(path, "rb") as file:
                data = file.read()
            try:
                os.remove(path)
            except OSError:
                pass
            return data
    raise DebugError("screenshot: no image in the answer")


WINDOW_FIELDS = ("fps", "mean_ms", "p50_ms", "p90_ms", "p99_ms", "max_ms", "stutters", "vsync_buckets")


def measure_window(client, seconds, refresh_hz):
    result = client.call("frame_times", {"seconds": seconds, "refresh_hz": refresh_hz},
                         seconds + 30) or {}
    window = {field: result.get(field) for field in WINDOW_FIELDS}
    for field in WINDOW_FIELDS[:6]:
        if not isinstance(window[field], (int, float)):
            raise DebugError(f"frame_times: no {field} in the answer")
        window[field] = round(window[field], 2)
    return window


def median_window(windows):
    """The window with the median fps."""
    return sorted(windows, key=lambda window: window["fps"])[len(windows) // 2]


def read_power(client, available):
    if not available:
        return None
    result = client.try_call("power", {}, 20)
    if not isinstance(result, dict) or not result.get("available", True) or not result.get("valid", True):
        return None
    reading = {key: round(result[key], 2) for key in ("package_w", "cpu_w", "gpu_w")
               if isinstance(result.get(key), (int, float))}
    return reading or None


def mean_power(readings):
    readings = [reading for reading in readings if reading]
    if not readings:
        return None
    out = {}
    for key in ("package_w", "cpu_w", "gpu_w"):
        values = [reading[key] for reading in readings if key in reading]
        if values:
            out[key] = round(sum(values) / len(values), 2)
    return out


# ---------------------------------------------------------------------------------------------
# System: CPU use by other processes, free RAM, processes

if IS_WINDOWS:
    import ctypes
    from ctypes import wintypes

    def _filetime(value):
        return (value.dwHighDateTime << 32) | value.dwLowDateTime

    def system_cpu_times():
        """(busy, total) CPU time of the whole machine, any unit."""
        idle, kernel, user = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
        if not ctypes.windll.kernel32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel),
                                                     ctypes.byref(user)):
            return None
        total = _filetime(kernel) + _filetime(user)
        return total - _filetime(idle), total

    def process_cpu_time(pid):
        kernel32 = ctypes.windll.kernel32
        kernel32.OpenProcess.restype = wintypes.HANDLE
        handle = kernel32.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            if not kernel32.GetProcessTimes(wintypes.HANDLE(handle),
                                            *[ctypes.byref(item) for item in times]):
                return None
            return _filetime(times[2]) + _filetime(times[3])
        finally:
            kernel32.CloseHandle(wintypes.HANDLE(handle))

    def free_ram_gb():
        class Status(ctypes.Structure):
            _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD)] + [
                (name, ctypes.c_ulonglong) for name in
                ("total", "available", "page_total", "page_available", "virtual_total",
                 "virtual_available", "extended")]
        status = Status()
        status.length = ctypes.sizeof(Status)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
            return None
        return status.available / 2**30

    def close_windows(pid):
        """Asks the process's windows to close, like a click on the close button."""
        user32 = ctypes.windll.user32
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

        def visit(window, _):
            owner = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if owner.value == pid and user32.IsWindowVisible(window):
                user32.PostMessageW(window, 0x0010, 0, 0)
            return True

        user32.EnumWindows(callback_type(visit), 0)

    def window_client_size(pid):
        """'WxH' of the client area of the process's largest visible window, or None."""
        user32 = ctypes.windll.user32
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
        best = [0, 0]

        def visit(window, _):
            owner = wintypes.DWORD()
            rect = wintypes.RECT()
            user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if (owner.value == pid and user32.IsWindowVisible(window)
                    and user32.GetClientRect(window, ctypes.byref(rect))
                    and rect.right * rect.bottom > best[0] * best[1]):
                best[:] = [rect.right, rect.bottom]
            return True

        user32.EnumWindows(callback_type(visit), 0)
        return f"{best[0]}x{best[1]}" if best[0] else None

    def process_memory(pid):
        """Working set and private bytes with their peaks, in MiB, or None."""
        class Counters(ctypes.Structure):
            _fields_ = [("size", wintypes.DWORD), ("page_faults", wintypes.DWORD)] + [
                (name, ctypes.c_size_t) for name in
                ("working_set_peak", "working_set", "paged_pool_peak", "paged_pool", "nonpaged_pool_peak",
                 "nonpaged_pool", "commit", "commit_peak", "private")]
        kernel32 = ctypes.windll.kernel32
        kernel32.OpenProcess.restype = wintypes.HANDLE
        handle = kernel32.OpenProcess(0x1000, False, pid)
        if not handle:
            return None
        try:
            counters = Counters()
            counters.size = ctypes.sizeof(Counters)
            if not kernel32.K32GetProcessMemoryInfo(wintypes.HANDLE(handle), ctypes.byref(counters),
                                                    counters.size):
                return None
            return {"working_set_mb": round(counters.working_set / 2**20),
                    "working_set_peak_mb": round(counters.working_set_peak / 2**20),
                    "private_mb": round(counters.private / 2**20),
                    "private_peak_mb": round(counters.commit_peak / 2**20)}
        finally:
            kernel32.CloseHandle(wintypes.HANDLE(handle))
else:
    def system_cpu_times():
        try:
            with open("/proc/stat") as file:
                fields = [int(value) for value in file.readline().split()[1:]]
        except (OSError, ValueError):
            return None
        total = sum(fields[:8])
        return total - fields[3] - fields[4], total

    def process_cpu_time(pid):
        try:
            with open(f"/proc/{pid}/stat") as file:
                fields = file.read().rpartition(")")[2].split()
            return int(fields[11]) + int(fields[12])
        except (OSError, ValueError, IndexError):
            return None

    def free_ram_gb():
        try:
            with open("/proc/meminfo") as file:
                for line in file:
                    if line.startswith("MemAvailable:"):
                        return int(line.split()[1]) / 2**20
        except (OSError, ValueError):
            pass
        return None

    def close_windows(pid):
        pass

    def window_client_size(pid):
        return None

    def process_memory(pid):
        status = dict(re.findall(r"^(VmRSS|VmHWM|VmData):\s*(\d+)", _read_text(f"/proc/{pid}/status"), re.M))
        if "VmRSS" not in status:
            return None
        return {"working_set_mb": round(int(status["VmRSS"]) / 1024),
                "working_set_peak_mb": round(int(status.get("VmHWM", status["VmRSS"])) / 1024),
                "private_mb": round(int(status["VmData"]) / 1024) if "VmData" in status else None,
                "private_peak_mb": None}


class CpuMeter:
    """CPU use between start() and stop(), as shares of the whole machine's capacity."""

    def __init__(self, pid):
        self.pid = pid
        self.before = None

    def snapshot(self):
        system = system_cpu_times()
        process = process_cpu_time(self.pid)
        return None if system is None or process is None else (system[0], system[1], process)

    def start(self):
        self.before = self.snapshot()

    def stop(self):
        """{other_cpu_pct, emulator_cpu_pct}, or None when the host cannot tell."""
        after = self.snapshot()
        if self.before is None or after is None or after[1] <= self.before[1]:
            return None
        busy, total, process = (after[i] - self.before[i] for i in range(3))
        return {"other_cpu_pct": round(max(busy - process, 0) * 100 / total, 1),
                "emulator_cpu_pct": round(process * 100 / total, 1)}


def process_running(image_name):
    """Whether a process with this executable name runs."""
    try:
        if IS_WINDOWS:
            done = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
                                  capture_output=True, text=True, timeout=20)
            return image_name.lower() in done.stdout.lower()
        done = subprocess.run(["pgrep", "-x", image_name[:15]], capture_output=True, timeout=20)
        return done.returncode == 0
    except (OSError, subprocess.SubprocessError) as error:
        raise BenchError(f"cannot tell whether {image_name} runs: {error}") from error


def launch_emulator(exe, game, env):
    return subprocess.Popen([exe, game], cwd=os.path.dirname(exe) or None, env=env,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)


def stop_process(process, grace_s=10, close_grace_s=5):
    """Closes the emulator's window, then terminates it, then kills it. Always returns.

    A window that answers the close with a confirmation dialog leaves the process alive; it is
    terminated after close_grace_s."""
    if process is None or process.poll() is not None:
        return
    for action, wait_s in ((lambda: close_windows(process.pid), close_grace_s), (process.terminate, grace_s),
                           (process.kill, grace_s)):
        try:
            action()
            process.wait(timeout=wait_s)
            return
        except (OSError, subprocess.TimeoutExpired):
            continue


def wait_gone(process, sleep, timeout_s=30):
    """Whether the process has exited; waits up to timeout_s for it."""
    waited = 0.0
    while process is not None and process.poll() is None:
        if waited >= timeout_s:
            return False
        sleep(0.5)
        waited += 0.5
    return True


class MemoryWatchdog(threading.Thread):
    """Kills the emulator when free RAM falls under the floor."""

    def __init__(self, process, floor_gb, free_ram=free_ram_gb, interval_s=3):
        super().__init__(daemon=True)
        self.process = process
        self.floor_gb = floor_gb
        self.free_ram = free_ram
        self.interval_s = interval_s
        self.fired = False
        self.lowest = None
        self.done = threading.Event()

    def run(self):
        while not self.done.is_set():
            free = self.free_ram()
            if free is not None:
                self.lowest = free if self.lowest is None else min(self.lowest, free)
                if free < self.floor_gb:
                    self.fired = True
                    try:
                        self.process.kill()
                    except OSError:
                        pass
                    return
            self.done.wait(self.interval_s)

    def stop(self):
        self.done.set()
        if self.is_alive():
            self.join(timeout=10)


# ---------------------------------------------------------------------------------------------
# Host tools: processor power settings, energy counters, GPU telemetry

def run_command(command, timeout=60):
    """(exit code, stdout) of a host tool; a tool that cannot run gives (1, reason)."""
    try:
        done = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=timeout,
                              stdin=subprocess.DEVNULL)
    except (OSError, subprocess.SubprocessError) as error:
        return 1, str(error)
    return done.returncode, done.stdout


def parse_power_policy(items):
    """{NAME: value} from ['NAME=VALUE', 'A=1,B=2'] or a dict; names are powercfg aliases."""
    if isinstance(items, dict):
        pairs = list(items.items())
    else:
        pairs = [tuple(part.split("=", 1)) if "=" in part else (part, "")
                 for item in items or [] for part in str(item).split(",") if part.strip()]
    policy = {}
    for name, value in pairs:
        name = str(name).strip().upper()
        try:
            if not re.fullmatch(r"[A-Z][A-Z0-9_]*", name) or isinstance(value, bool):
                raise ValueError
            policy[name] = int(str(value).strip(), 0)
        except ValueError:
            raise BenchError(f"power policy: expected NAME=NUMBER, got {name}={value}") from None
        if policy[name] < 0:
            raise BenchError(f"power policy: {name} is negative")
    return policy


GUID_PATTERN = re.compile(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}")


def parse_powercfg_query(text):
    """{alias: (AC value, DC value)} from `powercfg /qh`. Reads the layout, not the (localized)
    labels: a block starts at a GUID line, the alias follows, the last two hex values are AC, DC."""
    settings = {}
    alias, values = None, []

    def close():
        if alias and len(values) >= 2:
            settings[alias] = (values[-2], values[-1])

    fresh = False
    for line in text.splitlines():
        if GUID_PATTERN.search(line):
            close()
            alias, values, fresh = None, [], True
            continue
        if fresh:
            fresh = False
            match = re.search(r":\s*([A-Z][A-Z0-9_]*)\s*$", line)
            alias = match.group(1) if match else None
            continue
        match = re.search(r":\s*0x([0-9a-fA-F]{1,8})\s*$", line)
        if match:
            values.append(int(match.group(1), 16))
    close()
    return settings


class PowerPolicy:
    """Sets processor power settings of the active plan (AC values) and puts the old ones back.

    The old values are also kept in restore_file while they are changed, so recover() repairs
    what a killed run left."""

    def __init__(self, values, run, restore_file=None):
        self.values = dict(values or {})
        self.run = run
        self.restore_file = restore_file
        self.previous = {}

    def _powercfg(self, *arguments):
        code, output = self.run(["powercfg", *arguments], 30)
        if code != 0:
            raise BenchError(f"powercfg {' '.join(arguments)} failed: {output.strip()[:200]}")
        return output

    def _set(self, values):
        for name, value in values.items():
            self._powercfg("/setacvalueindex", "SCHEME_CURRENT", "SUB_PROCESSOR", name, str(value))
        self._powercfg("/setactive", "SCHEME_CURRENT")

    def read(self):
        return parse_powercfg_query(self._powercfg("/qh", "SCHEME_CURRENT", "SUB_PROCESSOR"))

    def recover(self):
        """Applies and removes the restore file; the values put back, or None."""
        if not self.restore_file or not os.path.isfile(self.restore_file):
            return None
        try:
            with open(self.restore_file, encoding="utf-8") as file:
                previous = parse_power_policy(json.load(file)["previous"])
        except (OSError, ValueError, KeyError, TypeError) as error:
            raise BenchError(f"unreadable power restore file {self.restore_file}: {error}") from error
        self._set(previous)
        os.remove(self.restore_file)
        return previous

    def apply(self):
        """{applied, previous, skipped}; skipped are names this machine does not have."""
        self.recover()
        current = self.read()
        if not current:
            raise BenchError("powercfg reported no processor power settings")
        applied = {name: value for name, value in self.values.items() if name in current}
        previous = {name: current[name][0] for name in applied}
        if self.restore_file:
            os.makedirs(os.path.dirname(self.restore_file), exist_ok=True)
            with open(self.restore_file, "w", encoding="utf-8") as file:
                json.dump({"previous": previous}, file)
        self.previous = previous
        self._set(applied)
        return {"applied": applied, "previous": previous,
                "skipped": sorted(name for name in self.values if name not in current)}

    def restore(self):
        if not self.previous:
            return
        self._set(self.previous)
        self.previous = {}
        if self.restore_file and os.path.isfile(self.restore_file):
            os.remove(self.restore_file)


ENERGY_COUNTERS = (("package_w", "rapl_package0_pkg"), ("cpu_w", "rapl_package0_pp0"),
                   ("gpu_w", "rapl_package0_pp1"))


def parse_typeperf(text):
    """Mean {package_w, cpu_w, gpu_w} of typeperf's CSV of the Energy Meter counters (milliwatts)."""
    columns, values = {}, collections.defaultdict(list)
    for fields in csv.reader(text.splitlines()):
        if len(fields) < 2:
            continue
        if fields[0].startswith("(PDH"):
            columns = {index: key for index, field in enumerate(fields)
                       for key, instance in ENERGY_COUNTERS if f"({instance})" in field.lower()}
            continue
        for index, key in columns.items():
            try:
                values[key].append(float(fields[index]))
            except (ValueError, IndexError):
                pass
    return {key: round(sum(items) / len(items) / 1000, 2) for key, items in values.items()}


class EnergyMeter:
    """Package, core and integrated GPU power from Windows' Energy Meter counters."""

    def __init__(self, run):
        self.run = run
        self.dead = False

    def read(self):
        if self.dead:
            return None
        _, output = self.run(["typeperf", *(f"\\Energy Meter({instance})\\Power" for _, instance in ENERGY_COUNTERS),
                              "-si", "1", "-sc", "2"], 30)
        reading = parse_typeperf(output)
        self.dead = not reading
        return reading or None


class PowerReader:
    """Watts from the debug server's `power`, else from the host's energy counters."""

    def __init__(self, client, has_method, run):
        self.client = client
        self.has_method = has_method
        self.meter = EnergyMeter(run)
        self.source = None

    def read(self):
        reading = read_power(self.client, self.has_method)
        if reading:
            self.source = "debug_server"
            return reading
        reading = self.meter.read()
        if reading:
            self.source = "energy_meter"
        return reading


def parse_gpu_telemetry(text):
    """{gpu_mhz, gpu_telemetry_w, gpu_limited_by} from lines 'HH:MM:SS  MHz  Watts ... LimitedBy'."""
    clocks, watts, limits = [], [], []
    for line in text.splitlines():
        match = re.match(r"\s*\d\d:\d\d:\d\d\s+(\d+)\s+(\d+(?:\.\d+)?)(.*)$", line)
        if not match:
            continue
        clocks.append(int(match.group(1)))
        watts.append(float(match.group(2)))
        rest = match.group(3).strip()
        if rest:  # columns set apart by two spaces or a tab may hold spaces themselves
            aligned = re.search(r"\S(\s{2,}|\t)\S", line)
            limits.append(re.split(r"\s{2,}|\t", rest)[-1] if aligned else rest.split()[-1])
    if not clocks:
        return {}
    reading = {"gpu_mhz": round(sum(clocks) / len(clocks)), "gpu_telemetry_w": round(sum(watts) / len(watts), 1)}
    if limits:
        reading["gpu_limited_by"] = collections.Counter(limits).most_common(1)[0][0]
    return reading


def read_gpu_telemetry(script, run):
    if not script:
        return {}
    _, output = run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script,
                     "-Samples", "3", "-IntervalMs", "700"], 60)
    return parse_gpu_telemetry(output)


# ---------------------------------------------------------------------------------------------
# Tracer lines in the emulator log (EDEN_DKB_FRAME_TRACE=1)

TRACE_KINDS = ("gpu_work", "gpu_pass", "gpu_frame")
TRACE_FRAME_KEYS = ("renderer_frame", "gpu_frame_span_ms", "gpu_submission_sum_ms", "gpu_busy_ms",
                    "measured_submissions", "dropped_submissions")
TRACE_WORK = re.compile(r"(s6:[0-9a-f]{16}|s1:[0-9a-f]{16}\+s5:[0-9a-f]{16})(?:#(\S))?="
                        r"([\d.]+)ms/(\d+)x\(max ([\d.]+)ms")
TRACE_PASS = re.compile(r"(\d+x\d+/\S+?)=([\d.]+)ms/(\d+)x\(max ([\d.]+)ms\)")


def _trace_numbers(text):
    return {key: float(value) if "." in value else int(value)
            for key, value in re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)(?=\s|$)", text)}


def parse_trace(lines):
    """Summary of the last `DKB gpu_work`, `gpu_pass` and `gpu_frame` lines, or None without any."""
    last = {}
    for line in lines:
        at = line.find("DKB gpu_")
        if at >= 0:
            kind, _, rest = line[at + 4:].partition(" ")
            # The frame trace also logs one `DKB gpu_pass <ms>ms submission=...` line per render
            # pass (upstream's pass trace); the summary read here starts with its frame.
            if kind in TRACE_KINDS and (kind != "gpu_pass" or rest.startswith("renderer_frame=")):
                last[kind] = line[at:].strip()
    if not last:
        return None
    trace = {}
    if "gpu_work" in last:
        head, _, tail = last["gpu_work"].partition(" slowest:")
        trace["work"] = _trace_numbers(head)
        trace["work"]["top"] = [
            {"shader": match[1], **({"kind": match[2]} if match[2] else {}), "ms": float(match[3]),
             "count": int(match[4]), "max_ms": float(match[5])}
            for match in list(TRACE_WORK.finditer(tail))[:8]]
    if "gpu_pass" in last:
        head, _, tail = last["gpu_pass"].partition(" top:")
        trace["pass"] = _trace_numbers(head)
        trace["pass"]["top"] = [{"target": match[1], "ms": float(match[2]), "count": int(match[3]),
                                 "max_ms": float(match[4])} for match in list(TRACE_PASS.finditer(tail))[:8]]
    if "gpu_frame" in last:
        numbers = _trace_numbers(last["gpu_frame"])
        trace["frame"] = {key: numbers[key] for key in TRACE_FRAME_KEYS if key in numbers}
    return trace


def collect_log(source, target):
    """Copies the emulator log; (whether there was one, parse_trace() of it)."""
    if not source or not os.path.isfile(source):
        return False, None
    os.makedirs(os.path.dirname(target), exist_ok=True)
    shutil.copyfile(source, target)
    with open(target, encoding="utf-8", errors="replace") as file:
        return True, parse_trace(file)


# ---------------------------------------------------------------------------------------------
# Presenter lines in the emulator log (EDEN_NX2_SHOWN_STATS=1; formats from
# video_core/renderer_vulkan/vk_present_manager.cpp and vk_swapchain.cpp)

NUMBER_TEXT = r"(\d+(?:\.\d+)?)"
# FluidLogStats, first of its four lines every 10 s.
FLUID_HEAD = re.compile(r"Fluid presenter \(10 s, (.+?) at " + NUMBER_TEXT + r" Hz, a slot every (\d+) "
                        r"refreshes\): (\d+) slots, (\d+) shown on their slot, (\d+) missed")
# Its second line.
FLUID_RECEIVED = re.compile(r"Fluid presenter \(10 s\): .*; (\d+) frames received, (\d+) dropped unshown")
# NoteShownFrame, every 10 s, with or without the FLUID presenter.
SHOWN_FRAMES = re.compile(r"Shown frames: " + NUMBER_TEXT + r" per second, interval p5 " + NUMBER_TEXT
                          + r" p50 " + NUMBER_TEXT + r" p95 " + NUMBER_TEXT + r" p99 " + NUMBER_TEXT
                          + r" max " + NUMBER_TEXT + r" ms, (\d+)% within 2 ms of the median")
# Swapchain::Create, at every (re)creation.
SWAPCHAIN = re.compile(r"Swapchain: (\S+), (\d+) images \([^)]*\), (\d+)x(\d+) ")
# The log line's prefix (common/logging.cpp): seconds since the emulator started.
LOG_TIME = re.compile(r"\[\s*(\d+\.\d+)\]")


def _number(text):
    return float(text) if "." in text else int(text)


def _log_time(line):
    """Seconds since the emulator started, from the log line's "[  123.456789]" prefix; None without one."""
    match = LOG_TIME.match(line)
    return float(match[1]) if match else None


def parse_presenter(lines):
    """(pacing, swapchain) of a log: the last FLUID presenter report and the last "Shown frames"
    line, and the swapchain that was in effect at the last of those reports (the last swapchain
    creation when there is no report: the window can shrink while the emulator shuts down, and
    the swapchain made then is not the one that was measured). Each is None when the log has no
    such line. pacing's head_at_s and shown_at_s say when each report was logged."""
    head = received = shown = current = reported = None
    head_at = shown_at = None
    for line in lines:
        if "Fluid presenter (10 s" in line:
            match = FLUID_HEAD.search(line)
            if match:
                head, received, head_at, reported = match, None, _log_time(line), current
            elif head is not None:
                received = FLUID_RECEIVED.search(line) or received
        elif "Shown frames: " in line:
            match = SHOWN_FRAMES.search(line)
            if match:
                shown, shown_at, reported = match, _log_time(line), current
        elif "Swapchain: " in line:
            current = SWAPCHAIN.search(line) or current
    swapchain = reported or current
    pacing = {}
    if head:
        pacing.update({"clock": head[1], "display_hz": float(head[2]), "slot_refreshes": int(head[3]),
                       "slots": int(head[4]), "on_slot": int(head[5]), "missed": int(head[6])})
        if head_at is not None:
            pacing["head_at_s"] = head_at
        if received:
            pacing.update({"real_received": int(received[1]), "dropped_unshown": int(received[2])})
    if shown:
        keys = ("shown_fps", "interval_p5_ms", "interval_p50_ms", "interval_p95_ms", "interval_p99_ms",
                "interval_max_ms", "within_2ms_pct")
        pacing.update({key: _number(value) for key, value in zip(keys, shown.groups())})
        if shown_at is not None:
            pacing["shown_at_s"] = shown_at
    if swapchain:
        swapchain = {"present_mode": swapchain[1], "images": int(swapchain[2]),
                     "size": f"{swapchain[3]}x{swapchain[4]}"}
    return pacing or None, swapchain


def read_presenter(path):
    if not path or not os.path.isfile(path):
        return None, None
    with open(path, encoding="utf-8", errors="replace") as file:
        return parse_presenter(file)


# ---------------------------------------------------------------------------------------------
# Hardware description

WINDOWS_HW_SCRIPT = r"""
$ErrorActionPreference = 'SilentlyContinue'
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object { @{ name = $_.Name; driver_version = $_.DriverVersion;
    pnp = $_.PNPDeviceID; width = $_.CurrentHorizontalResolution; height = $_.CurrentVerticalResolution;
    refresh_hz = $_.CurrentRefreshRate } })
$cs = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$battery = Get-CimInstance Win32_Battery | Select-Object -First 1
$status = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus | Select-Object -First 1
@{ cpu = $cpu.Name; cores = $cpu.NumberOfCores; threads = $cpu.NumberOfLogicalProcessors; gpus = $gpus;
   ram_bytes = $cs.TotalPhysicalMemory; os = $os.Caption; os_version = $os.Version; os_build = $os.BuildNumber;
   has_battery = [bool]$battery; battery_pct = $battery.EstimatedChargeRemaining; battery_status = $battery.BatteryStatus;
   power_online = $(if ($status) { [bool]$status.PowerOnline } else { $null });
   power_plan = ((powercfg /getactivescheme) -join ' ') } | ConvertTo-Json -Depth 4 -Compress
"""


PCI_ID = re.compile(r"^[0-9A-F]{4}:[0-9A-F]{4}$")


def gpu_entry(name, driver_version, pci=None):
    """One hardware.gpus item: the name, the driver version and, when known, the PCI vendor and device
    ID as "8086:7D51" (the GPU family the engine's presets match on needs it, src/common/device_profile.cpp)."""
    entry = {"name": name, "driver_version": driver_version}
    if pci:
        entry["pci"] = pci
    return entry


def pci_of_pnp(pnp):
    """"8086:7D51" from a Windows PNP device ID (PCI\\VEN_8086&DEV_7D51&SUBSYS_...), or None."""
    match = re.search(r"(?i)\bVEN_([0-9a-f]{4})&DEV_([0-9a-f]{4})", str(pnp or ""))
    return f"{match.group(1)}:{match.group(2)}".upper() if match else None


def lspci_gpus(text):
    """hardware.gpus from `lspci -nn`, with the names plain `lspci` prints (machine_id hashes them)."""
    gpus = []
    for line in text.splitlines():
        if not re.search(r"VGA compatible|3D controller|Display controller", line) or ": " not in line:
            continue
        name = line.split(": ", 1)[1]
        ids = re.search(r"\s*\[([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\](?=\s*(?:\(rev [0-9a-fA-F]+\))?\s*$)", name)
        if ids:
            name = name[:ids.start()] + name[ids.end():]
        gpus.append(gpu_entry(name.strip(), None, f"{ids.group(1)}:{ids.group(2)}".upper() if ids else None))
    return gpus


def parse_windows_hardware(raw):
    """The row's hardware object from the JSON WINDOWS_HW_SCRIPT prints."""
    gpus = raw.get("gpus") or []
    if isinstance(gpus, dict):
        gpus = [gpus]
    display = next((f"{gpu['width']}x{gpu['height']}@{gpu.get('refresh_hz') or '?'}" for gpu in gpus
                    if gpu.get("width") and gpu.get("height")), None)
    if raw.get("power_online") is not None:
        power = "AC" if raw["power_online"] else "battery"
    elif raw.get("has_battery"):
        power = "AC" if raw.get("battery_status") == 2 else "battery" if raw.get("battery_status") == 1 else "unknown"
    else:
        power = "AC"
    plan = re.search(r"\(([^)]*)\)\s*$", raw.get("power_plan") or "")
    return {
        "cpu": " ".join((raw.get("cpu") or "unknown").split()),
        "cpu_cores": raw.get("cores"),
        "cpu_threads": raw.get("threads"),
        "gpus": [gpu_entry(gpu.get("name") or "unknown", gpu.get("driver_version"), pci_of_pnp(gpu.get("pnp")))
                 for gpu in gpus],
        "ram_gb": round(raw["ram_bytes"] / 2**30, 1) if raw.get("ram_bytes") else None,
        "os": " ".join(filter(None, [raw.get("os"), raw.get("os_version")])) or "Windows",
        "os_build": str(raw["os_build"]) if raw.get("os_build") else None,
        "display": display,
        "power_source": power,
        "battery_pct": raw.get("battery_pct"),
        "power_plan": plan.group(1) if plan else None,
    }


def _read_text(path):
    try:
        with open(path, errors="replace") as file:
            return file.read()
    except OSError:
        return ""


def _tool_output(command):
    try:
        return subprocess.run(command, capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def generic_hardware():
    """Best effort on Linux and macOS; unknown values are None."""
    cpu = re.search(r"model name\s*:\s*(.+)", _read_text("/proc/cpuinfo"))
    cpu = cpu.group(1) if cpu else _tool_output(["sysctl", "-n", "machdep.cpu.brand_string"]).strip()
    memory = re.search(r"MemTotal:\s*(\d+)", _read_text("/proc/meminfo"))
    gpus = lspci_gpus(_tool_output(["lspci", "-nn"]))
    power = "unknown"
    for supply in glob.glob("/sys/class/power_supply/*"):
        if _read_text(os.path.join(supply, "type")).strip() == "Mains":
            power = "AC" if _read_text(os.path.join(supply, "online")).strip() == "1" else "battery"
    return {
        "cpu": " ".join((cpu or platform.processor() or "unknown").split()),
        "cpu_cores": None,
        "cpu_threads": os.cpu_count(),
        "gpus": gpus or [{"name": "unknown", "driver_version": None}],
        "ram_gb": round(int(memory.group(1)) / 2**20, 1) if memory else None,
        "os": f"{platform.system()} {platform.release()}",
        "os_build": platform.version() or None,
        "display": None,
        "power_source": power,
        "battery_pct": None,
        "power_plan": None,
    }


def collect_hardware():
    if IS_WINDOWS:
        encoded = base64.b64encode(WINDOWS_HW_SCRIPT.encode("utf-16-le")).decode()
        try:
            done = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                                  capture_output=True, text=True, timeout=60)
            return parse_windows_hardware(json.loads(done.stdout[done.stdout.index("{"):]))
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
    return generic_hardware()


def machine_id(hardware):
    """Short hash of the CPU and GPU names; nothing else identifies the machine."""
    names = [hardware.get("cpu") or ""] + sorted(gpu.get("name") or "" for gpu in hardware.get("gpus") or [])
    return hashlib.sha256("|".join(name.strip().lower() for name in names).encode()).hexdigest()[:10]


# ---------------------------------------------------------------------------------------------
# Sanitization

PATH_SEPARATORS = "[" + re.escape("\\") + "/]+"
HOME_PATTERNS = (re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+[^\\/\s\"'<>|]+", re.IGNORECASE),
                 re.compile(r"/(?:home|Users)/[^/\s\"'<>|]+"))


def make_sanitizer(user=None, home=None, host=None):
    """A function that removes the user name, home directory and host name from row data."""
    try:
        user = getpass.getuser() if user is None else user
    except (OSError, KeyError):
        user = ""
    home = os.path.expanduser("~") if home is None else home
    host = platform.node() if host is None else host
    home_pattern = None
    if home and home not in ("~", "/", "\\"):
        parts = [re.escape(part) for part in re.split(PATH_SEPARATORS, home) if part]
        prefix = PATH_SEPARATORS if home.startswith(("/", "\\")) else ""
        home_pattern = re.compile(prefix + PATH_SEPARATORS.join(parts), re.IGNORECASE)
    names = [(re.compile(rf"(?<![A-Za-z0-9]){re.escape(name)}(?![A-Za-z0-9])", re.IGNORECASE), token)
             for name, token in ((user, "<user>"), (host, "<host>")) if name and len(name) >= 2]

    def clean_text(text):
        if home_pattern:
            text = home_pattern.sub(lambda _: "~", text)
        for pattern in HOME_PATTERNS:
            text = pattern.sub("~", text)
        for pattern, token in names:
            text = pattern.sub(token, text)
        return text

    def clean(value):
        if isinstance(value, str):
            return clean_text(value)
        if isinstance(value, list):
            return [clean(item) for item in value]
        if isinstance(value, dict):
            return {clean_text(key) if isinstance(key, str) else key: clean(item)
                    for key, item in value.items()}
        return value

    return clean


def private_strings(value, where="row"):
    """Paths of strings that still look like a home directory."""
    found = []
    if isinstance(value, str):
        if any(pattern.search(value) for pattern in HOME_PATTERNS):
            found.append(where)
    elif isinstance(value, list):
        for index, item in enumerate(value):
            found += private_strings(item, f"{where}[{index}]")
    elif isinstance(value, dict):
        for key, item in value.items():
            found += private_strings(key, f"{where}.{key}") + private_strings(item, f"{where}.{key}")
    return found


# ---------------------------------------------------------------------------------------------
# Row schema

NUMBER = (int, float)
OPTIONAL = type(None)
# field: (accepted types, required)
ROW_FIELDS = {
    "schema_version": ((int,), True),
    "timestamp_utc": ((str,), True),
    "machine_id": ((str,), True),
    "scene": ((str,), True),
    "title_id": ((str,), True),
    "variant": ((str,), True),
    "variant_settings": ((dict,), True),
    "variant_env": ((dict,), True),
    "emulator": ((dict,), True),
    "game": ((str,), True),
    "hardware": ((dict,), True),
    "valid": ((bool,), True),
    "invalid_reasons": ((list,), True),
    "warnings": ((list,), True),
    "error": ((str, OPTIONAL), True),
    "scene_ok": ((bool, OPTIONAL), True),
    "scene_diff": (NUMBER + (OPTIONAL,), True),
    "fps": (NUMBER + (OPTIONAL,), True),
    "mean_ms": (NUMBER + (OPTIONAL,), True),
    "p50_ms": (NUMBER + (OPTIONAL,), True),
    "p90_ms": (NUMBER + (OPTIONAL,), True),
    "p99_ms": (NUMBER + (OPTIONAL,), True),
    "max_ms": (NUMBER + (OPTIONAL,), True),
    "stutters": (NUMBER + (OPTIONAL,), False),
    "vsync_buckets": ((dict, list, OPTIONAL), False),
    "windows_fps": ((list,), True),
    "window_s": (NUMBER, True),
    "sustained": ((dict, OPTIONAL), True),
    "watts": (NUMBER + (OPTIONAL,), True),
    "power": ((dict, OPTIONAL), True),
    "memory": ((dict, OPTIONAL), False),
    "gpu_costs": ((dict, list, OPTIONAL), False),
    "system": ((dict,), True),
    "screenshot": ((str, OPTIONAL), False),
    # Added after the first rows were recorded: all optional.
    "id": ((str,), False),
    "suite": ((str, OPTIONAL), False),
    "probe": ((bool,), False),
    "base_ini": ((str, OPTIONAL), False),
    "mods": ((list, OPTIONAL), False),
    "addons_off": ((list, OPTIONAL), False),
    "reject": ((list,), False),
    "live": ((list,), False),
    "power_policy": ((dict, OPTIONAL), False),
    "gpu_mhz": (NUMBER + (OPTIONAL,), False),
    "gpu_limited_by": ((str, OPTIONAL), False),
    "gpu_telemetry_w": (NUMBER + (OPTIONAL,), False),
    "late": ((dict, OPTIONAL), False),
    "window": ((str, OPTIONAL), False),
    "process_memory": ((dict, OPTIONAL), False),
    "log": ((str, OPTIONAL), False),
    "trace": ((dict, OPTIONAL), False),
    "fullscreen": ((bool, OPTIONAL), False),
    "fullscreen_requested": ((bool, OPTIONAL), False),
    "shown_stats": ((bool, OPTIONAL), False),
    "output": ((dict, OPTIONAL), False),
    "pacing": ((dict, OPTIONAL), False),
    "picture": ((dict, OPTIONAL), False),
    # Boot suites only.
    "cache_state": ((str, OPTIONAL), False),
    "repeat": ((int, OPTIONAL), False),
    "boot": ((dict, OPTIONAL), False),
}
OUTPUT_FIELDS = {
    "size": ((str, OPTIONAL), True),
    "size_from": ((str, OPTIONAL), True),
    "present_mode": ((str, OPTIONAL), False),
    "images": ((int, OPTIONAL), False),
    "fullscreen_mode": ((str, OPTIONAL), False),
    "fullscreen_mode_from": ((str, OPTIONAL), False),
}
PACING_FIELDS = {
    "head_at_s": (NUMBER, False),
    "shown_at_s": (NUMBER, False),
    "clock": ((str,), False),
    "display_hz": (NUMBER, False),
    "slot_refreshes": ((int,), False),
    "slots": ((int,), False),
    "on_slot": ((int,), False),
    "missed": ((int,), False),
    "real_received": ((int,), False),
    "dropped_unshown": ((int,), False),
    "shown_fps": (NUMBER, False),
    "interval_p5_ms": (NUMBER, False),
    "interval_p50_ms": (NUMBER, False),
    "interval_p95_ms": (NUMBER, False),
    "interval_p99_ms": (NUMBER, False),
    "interval_max_ms": (NUMBER, False),
    "within_2ms_pct": (NUMBER, False),
}
PICTURE_FIELDS = {
    "limits": ((dict,), True),
    "shots": ((list,), True),
    "black": ((bool,), True),
}
HARDWARE_FIELDS = {
    "cpu": ((str,), True),
    "cpu_cores": ((int, OPTIONAL), False),
    "cpu_threads": ((int, OPTIONAL), False),
    "gpus": ((list,), True),
    "ram_gb": (NUMBER + (OPTIONAL,), True),
    "os": ((str,), True),
    "os_build": ((str, OPTIONAL), True),
    "display": ((str, OPTIONAL), True),
    "power_source": ((str,), True),
    "battery_pct": (NUMBER + (OPTIONAL,), False),
    "power_plan": ((str, OPTIONAL), False),
}
SUSTAINED_FIELDS = {
    "minutes": (NUMBER, True),
    "interval_s": (NUMBER, True),
    "samples": ((list,), True),
    "fps_min": (NUMBER + (OPTIONAL,), True),
    "fps_avg": (NUMBER + (OPTIONAL,), True),
    "fps_first": (NUMBER + (OPTIONAL,), True),
    "fps_last": (NUMBER + (OPTIONAL,), True),
    "watts_avg": (NUMBER + (OPTIONAL,), False),
}
SYSTEM_FIELDS = {
    "other_cpu_pct": (NUMBER + (OPTIONAL,), True),
    "other_cpu_max_pct": (NUMBER + (OPTIONAL,), True),
    "other_cpu_limit_pct": (NUMBER, True),
    "emulator_cpu_pct": (NUMBER + (OPTIONAL,), False),
    "ram_free_min_gb": (NUMBER + (OPTIONAL,), False),
}


def _check_fields(value, fields, where, errors):
    for name, (types, required) in fields.items():
        if name not in value:
            if required:
                errors.append(f"{where}: missing {name}")
        elif not isinstance(value[name], types) or (isinstance(value[name], bool) and bool not in types):
            errors.append(f"{where}: {name} has type {type(value[name]).__name__}")


def validate_row(row):
    """Problems with one result row; empty when it conforms to the schema."""
    if not isinstance(row, dict):
        return ["row is not an object"]
    errors = []
    _check_fields(row, ROW_FIELDS, "row", errors)
    if errors:
        return errors
    if row["schema_version"] != SCHEMA_VERSION:
        errors.append(f"schema_version is {row['schema_version']}, expected {SCHEMA_VERSION}")
    if not re.fullmatch(r"[0-9a-f]{10}", row["machine_id"]):
        errors.append("machine_id is not 10 hex digits")
    if not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", row["timestamp_utc"]):
        errors.append("timestamp_utc is not YYYY-MM-DDThh:mm:ssZ")
    if not re.fullmatch(r"[0-9A-Fa-f]{16}", row["title_id"]):
        errors.append("title_id is not 16 hex digits")
    _check_fields(row["hardware"], HARDWARE_FIELDS, "hardware", errors)
    _check_fields(row["system"], SYSTEM_FIELDS, "system", errors)
    if isinstance(row["hardware"].get("gpus"), list):
        for gpu in row["hardware"]["gpus"]:
            if not isinstance(gpu, dict) or not isinstance(gpu.get("name"), str):
                errors.append("hardware: every gpu needs a name")
            elif "pci" in gpu and not (isinstance(gpu["pci"], str) and PCI_ID.match(gpu["pci"])):
                errors.append("hardware: a gpu's pci is VVVV:DDDD in hex")
        if not errors and machine_id(row["hardware"]) != row["machine_id"]:
            errors.append("machine_id does not match the CPU and GPU names")
    if row["sustained"] is not None:
        _check_fields(row["sustained"], SUSTAINED_FIELDS, "sustained", errors)
    if any(not isinstance(reason, str) for reason in row["invalid_reasons"] + row["warnings"]):
        errors.append("invalid_reasons and warnings hold strings")
    if row["valid"] != (not row["invalid_reasons"]):
        errors.append("valid must be true exactly when invalid_reasons is empty")
    if row["valid"]:
        if row["scene_ok"] is not True:
            errors.append("a valid row needs scene_ok true")
        if row["error"] is not None:
            errors.append("a valid row has no error")
        if not isinstance(row["fps"], NUMBER) or row["fps"] <= 0:
            errors.append("a valid row needs fps > 0")
        if len(row["windows_fps"]) != 3:
            errors.append("a valid row has three measurement windows")
        elif isinstance(row["fps"], NUMBER) and sorted(row["windows_fps"])[1] != row["fps"]:
            errors.append("fps is not the median of windows_fps")
        limit, peak = row["system"].get("other_cpu_limit_pct"), row["system"].get("other_cpu_max_pct")
        if isinstance(peak, NUMBER) and isinstance(limit, NUMBER) and peak > limit:
            errors.append("a valid row cannot exceed the background CPU limit")
    if row["scene_ok"] is False and "wrong_scene" not in row["invalid_reasons"]:
        errors.append("scene_ok false requires the wrong_scene reason")
    if bool(row.get("reject")) != bool(row.get("probe")):
        errors.append("probe must be true exactly when shaders were rejected")
    if any(not isinstance(key, str) or not re.fullmatch(r"[0-9a-f]{16}", key) for key in row.get("reject") or []):
        errors.append("reject holds 16-digit lower-case hex hashes")
    if row.get("log") and re.search(r"[\\/]", row["log"].replace("logs/", "", 1)):
        errors.append("log is a name under logs/, not a path")
    if row.get("output") is not None:
        _check_fields(row["output"], OUTPUT_FIELDS, "output", errors)
        size = row["output"].get("size")
        if isinstance(size, str) and not re.fullmatch(r"\d+x\d+", size):
            errors.append("output: size is WxH")
        if row["output"].get("size_from") not in (None, "swapchain", "window"):
            errors.append("output: size_from is swapchain or window")
    # A fullscreen row measures another path than a windowed one (the screen output step runs at
    # the display's size): it is only comparable when it says what size that was.
    if row["valid"] and row.get("fullscreen") and not (row.get("output") or {}).get("size"):
        errors.append("a valid fullscreen row needs output.size")
    if row.get("pacing") is not None:
        _check_fields(row["pacing"], PACING_FIELDS, "pacing", errors)
    if row.get("cache_state") is not None:
        if row["cache_state"] not in CACHE_STATES:
            errors.append(f"cache_state is one of {', '.join(CACHE_STATES)}")
        if row["valid"] and not isinstance((row.get("boot") or {}).get("first_frame_ms"), NUMBER):
            errors.append("a valid boot row needs boot.first_frame_ms")
    if row.get("picture") is not None:
        _check_fields(row["picture"], PICTURE_FIELDS, "picture", errors)
        if row["picture"].get("black") is True and "black_picture" not in row["invalid_reasons"]:
            errors.append("a black picture requires the black_picture reason")
    errors += [f"{where}: looks like a home directory path" for where in private_strings(row)]
    return errors


def read_rows(path):
    """[(line number, row or None, error or None)] of a .jsonl file."""
    rows = []
    with open(path, encoding="utf-8") as file:
        for number, line in enumerate(file, 1):
            if not line.strip():
                continue
            try:
                rows.append((number, json.loads(line), None))
            except ValueError as error:
                rows.append((number, None, f"not JSON: {error}"))
    return rows


def validate_file(path):
    problems = []
    name = re.fullmatch(r"\d{4}-\d\d-\d\d-([0-9a-f]{10})\.jsonl", os.path.basename(path))
    rows = read_rows(path)
    if not rows:
        problems.append(f"{path}: no rows")
    for number, row, error in rows:
        errors = [error] if error else validate_row(row)
        if not errors and name and row["machine_id"] != name.group(1):
            errors.append("machine_id differs from the one in the file name")
        problems += [f"{path}:{number}: {message}" for message in errors]
    return problems, len(rows)


def result_files(paths):
    """The paths, with a folder replaced by the .jsonl files in it."""
    files = []
    for path in paths:
        files += sorted(glob.glob(os.path.join(path, "*.jsonl"))) if os.path.isdir(path) else [path]
    return files


# ---------------------------------------------------------------------------------------------
# The run

class Deps:
    """What a run touches outside this process; tests replace these."""
    sleep = staticmethod(time.sleep)
    launch = staticmethod(launch_emulator)
    stop = staticmethod(stop_process)
    running = staticmethod(process_running)
    hardware = staticmethod(collect_hardware)
    cpu_meter = CpuMeter
    free_ram = staticmethod(free_ram_gb)
    run_command = staticmethod(run_command)
    window_size = staticmethod(window_client_size)
    process_memory = staticmethod(process_memory)
    log = staticmethod(lambda message: print(message, flush=True))


def expand(path):
    return os.path.abspath(os.path.expanduser(os.path.expandvars(path))) if path else path


def load_json(path, what):
    try:
        with open(path, encoding="utf-8") as file:
            value = json.load(file)
    except (OSError, ValueError) as error:
        raise BenchError(f"{what} {path}: {error}") from error
    if not isinstance(value, dict):
        raise BenchError(f"{what} {path}: not a JSON object")
    return value


def find_user_dir(exe):
    """A portable 'user' folder next to the emulator, else the per-user data folder: guarana, or
    lichii where only that exists (the emulator copies it to guarana on its first start)."""
    candidates = [os.path.join(os.path.dirname(exe), "user")]
    if IS_WINDOWS:
        base = os.environ.get("APPDATA", "")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    candidates += [os.path.join(base, "guarana"), os.path.join(base, "lichii")]
    return next((path for path in candidates if os.path.isdir(path)), None)


def default_state_dir():
    """Where the power restore file lives: per user, shared by bench.py and play.py."""
    base = os.environ.get("LOCALAPPDATA") or os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
    return os.path.join(base, "lichii-bench")


def check_variant(variant, cli_mods, cli_policy, cli_fullscreen=False, cli_pacing=False):
    """The variant with its lists checked and the command line's mods, power policy,
    fullscreen and pacing choices filled in."""
    variant = dict(variant)
    name = str(variant.get("name", ""))
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name):
        raise BenchError("every variant needs a name of letters, digits, '.', '_' or '-'")
    for key, cli_value in (("fullscreen", cli_fullscreen), ("pacing", cli_pacing)):
        variant.setdefault(key, bool(cli_value))
        if not isinstance(variant[key], bool):
            raise BenchError(f"variant {name}: {key} is true or false")
    for key in ("mods", "addons_off", "reject", "live"):
        if variant.get(key) is not None and not isinstance(variant[key], list):
            raise BenchError(f"variant {name}: {key} is a list")
    for key in ("mods", "addons_off"):
        for item in variant.get(key) or []:
            if not isinstance(item, str) or not item.strip() or re.search(r'[\\/:*?"<>|\r\n]', item) \
                    or item.strip(". ") == "":
                raise BenchError(f"variant {name}: {key} holds plain names, got {item!r}")
    variant["reject"] = [str(key).lower() for key in variant.get("reject") or []]
    if any(not re.fullmatch(r"[0-9a-f]{16}", key) for key in variant["reject"]):
        raise BenchError(f"variant {name}: reject holds full 16-digit hex shader hashes")
    variant["live"] = variant.get("live") or []
    for call in variant["live"]:
        if not isinstance(call, dict) or not isinstance(call.get("method"), str) \
                or not isinstance(call.get("params") or {}, dict):
            raise BenchError(f"variant {name}: live holds {{method, params}} objects")
    if "mods" not in variant:
        variant["mods"] = cli_mods
    variant["power_policy"] = {**cli_policy, **parse_power_policy(variant.get("power_policy") or {})}
    try:
        variant["late_s"] = float(variant.get("late_s") or 0)
        variant["live_settle_s"] = float(variant.get("live_settle_s", 20 if variant["reject"] else 3))
    except (TypeError, ValueError):
        raise BenchError(f"variant {name}: late_s and live_settle_s are numbers") from None
    return variant


class Plan:
    """Everything a run needs, resolved from the arguments and the scene file."""

    def __init__(self, args, variants=None):
        self.exe = expand(args.exe)
        self.game = expand(args.game)
        self.scene_path = expand(args.scene)
        self.scene = load_json(self.scene_path, "scene")
        scene = self.scene
        for key in ("name", "title_id", "input"):
            if key not in scene:
                raise BenchError(f"scene {self.scene_path}: missing {key}")
        if not re.fullmatch(r"[0-9A-Fa-f]{16}", str(scene["title_id"])):
            raise BenchError(f"scene {self.scene_path}: title_id is not 16 hex digits")
        if variants is None:
            variants = [load_json(expand(path), "variant") for path in args.variant or []]
        cli_policy = parse_power_policy(args.power_policy)
        self.variants = [check_variant(variant, args.mod, cli_policy, args.fullscreen, getattr(args, "pacing", False))
                         for variant in variants or [BASELINE_VARIANT]]
        # A boot suite runs a variant once per cache state and repeat.
        runs = [(variant["name"], variant.get("cache_state"), variant.get("repeat")) for variant in self.variants]
        if len(set(runs)) != len(runs):
            raise BenchError("variant names must differ")
        self.base_ini = expand(args.base_ini)
        self.mod_library = expand(args.mod_library)
        self.gpu_telemetry = expand(args.gpu_telemetry)
        self.power_restore = os.path.join(expand(args.state_dir) or default_state_dir(), "power-restore.json")
        self.suite = None
        # A boot suite's options (load_boot_options) and, while it runs, its BootSession.
        self.boot = None
        self.boot_session = None
        saves = scene.get("saves") or {}
        self.user_dir = expand(args.user_dir or saves.get("user_dir")) or find_user_dir(self.exe)
        self.mod_dir = os.path.join(self.user_dir, "load", scene["title_id"].upper()) if self.user_dir else None
        self.log_file = os.path.join(self.user_dir, "log", "eden_log.txt") if self.user_dir else None
        self.ini = expand(args.ini) or (os.path.join(self.user_dir, "config", "qt-config.ini")
                                        if self.user_dir else None)
        self.save_backup = expand(args.save_backup or saves.get("backup_dir"))
        self.save_dir = (os.path.join(self.user_dir, *saves.get("save_subdir", "nand/user/save").split("/"))
                         if self.user_dir and self.save_backup else None)
        reference = args.reference or scene.get("reference")
        if reference and not args.reference:
            reference = os.path.join(os.path.dirname(self.scene_path), os.path.expandvars(reference))
        self.reference = expand(reference)
        self.record_reference = args.record_reference
        self.threshold = float(scene.get("scene_threshold", 30))
        self.picture_limits = picture_limits(self.threshold, float(scene.get("picture_dark_pct", PICTURE_DARK_PCT)))
        boot = scene.get("boot") or {}
        self.min_presents = int(boot.get("min_presents", 30))
        self.server_timeout_s = float(boot.get("server_timeout_s", 120))
        self.presents_timeout_s = float(boot.get("presents_timeout_s", 180))
        self.settle_s = float(scene.get("settle_s", 50) if args.settle_s is None else args.settle_s)
        self.window_s = float(scene.get("window_s", 15) if args.window_s is None else args.window_s)
        self.refresh_hz = float(scene.get("refresh_hz", 60))
        self.shot_height = int(scene.get("screenshot_height", 540))
        self.sustain_min = float(args.sustain_min)
        self.sustain_interval_s = float(args.sustain_interval_s)
        self.port = args.port
        self.out = expand(args.out)
        self.ram_floor_gb = args.ram_floor_gb
        self.cpu_limit_pct = args.max_other_cpu_pct
        if not 0 < self.window_s <= 60 or not 0 < self.sustain_interval_s <= 60:
            raise BenchError("measurement windows are between 0 and 60 s")

    def settings(self, variant):
        """Windowed unless the variant runs fullscreen; then [UI] fullscreen is turned on, unless the
        scene's or the variant's own settings turn it off."""
        if variant["fullscreen"]:
            return merge_settings(FULLSCREEN_SETTINGS, self.scene.get("settings"), variant.get("settings"))
        return merge_settings(WINDOWED_SETTINGS, self.scene.get("settings"), variant.get("settings"),
                              WINDOWED_SETTINGS)

    def check(self):
        """Problems that stop a run before anything is touched."""
        problems = []
        if not os.path.isfile(self.exe):
            problems.append(f"emulator not found: {self.exe}")
        if not os.path.exists(self.game):
            problems.append(f"game not found: {self.game}")
        if not self.ini or not os.path.isfile(self.ini):
            problems.append(f"settings file not found: {self.ini} (pass --user-dir or --ini)")
        if self.save_backup and not (os.path.isdir(self.save_backup) and os.listdir(self.save_backup)):
            problems.append(f"save backup is missing or empty: {self.save_backup}")
        if self.save_backup and not self.user_dir:
            problems.append("a save backup needs --user-dir")
        if self.record_reference and not self.reference:
            problems.append("--record-reference needs a reference path")
        if self.base_ini and not os.path.isfile(self.base_ini):
            problems.append(f"base ini not found: {self.base_ini}")
        if self.gpu_telemetry and not os.path.isfile(self.gpu_telemetry):
            problems.append(f"GPU telemetry script not found: {self.gpu_telemetry}")
        mods = sorted({mod for variant in self.variants for mod in variant["mods"] or []})
        if any(variant["mods"] is not None for variant in self.variants):
            if not self.user_dir:
                problems.append("mods need --user-dir")
            if mods and not self.mod_library:
                problems.append("mods need --mod-library")
        if self.mod_library:
            problems += [f"mod not found in the library: {os.path.join(self.mod_library, mod)}" for mod in mods
                         if not os.path.isdir(os.path.join(self.mod_library, mod))]
        if self.boot and not self.user_dir:
            problems.append("a boot suite moves the title's shader cache: it needs the emulator's user folder "
                            "(--user-dir)")
        return problems

    def describe(self):
        lines = [f"emulator    {self.exe}", f"game        {self.game}",
                 f"scene       {self.scene['name']} ({self.scene['title_id']})",
                 f"settings    {self.ini}",
                 f"saves       " + (f"{self.save_backup} -> {self.save_dir} before every variant; the current "
                                    "saves are set aside and put back at the end"
                                    if self.save_dir else "not restored (no backup dir given)"),
                 f"reference   {self.reference or 'none: rows will be invalid (scene_unverified)'}"
                 + ("" if not self.reference or os.path.isfile(self.reference) or self.record_reference
                    else "  [missing: rows will be invalid]")
                 + ("  [will be recorded by this run]" if self.record_reference else ""),
                 f"results     {os.path.join(self.out, '<date>-<machine-id>.jsonl')}",
                 f"limits      stop under {self.ram_floor_gb} GB free RAM; flag over "
                 f"{self.cpu_limit_pct}% CPU used by other processes"]
        described = set()
        for variant in self.variants:
            if variant["name"] in described:
                continue
            described.add(variant["name"])
            lines.append(f"variant {variant['name']}:")
            step = 1
            for text in self.steps(variant):
                lines.append(f"  {step:2}. {text}")
                step += 1
        if self.boot:
            lines += self.describe_boot()
        return "\n".join(lines)

    def describe_boot(self):
        """The caches a boot suite moves, the order of its runs and a rough length."""
        title = f"{int(self.scene['title_id'], 16):016x}"
        folders = [os.path.join(self.user_dir or "<user folder>", "cache", "shader", title)] + self.boot["driver_dirs"]
        lines = ["caches      backed up once to " + os.path.join(self.boot["work_dir"], "<date-time>", "backup")
                 + " (sha256 manifest); before every run moved aside (renamed to <folder>.bench-aside) and "
                 "replaced by the run's start state; after it moved into the work folder, the original renamed "
                 "back and checked:"]
        lines += [f"              {folder}" for folder in folders]
        if not self.boot["driver_dirs"]:
            lines.append("              (no --driver-cache-dir: the GPU driver keeps its own cache, so cold runs are "
                         "cold for the emulator only)")
        lines.append("states      cold: none; second: what the same variant's cold run of the same repeat left; "
                     "warm: a copy of the backup (your cache as it was when the suite started)")
        total = sum(boot_run_seconds(self, variant["cache_state"]) for variant in self.variants)
        lines.append(f"runs        {len(self.variants)}, about {total / 3600:.1f} h (a rough estimate):")
        for index, variant in enumerate(self.variants, 1):
            lines.append(f"  {index:3}. repeat {variant['repeat'] + 1} {variant['cache_state']:6} {variant['name']}")
        return lines

    def steps(self, variant):
        steps = [f"refuse to run if {os.path.basename(self.exe)} is running or port {self.port} is in use"]
        if self.save_dir:
            steps.append("restore the saves from the backup")
        if variant.get("cache_state"):
            steps.append("move the caches aside and put in the run's start state (cold, second or warm: see caches "
                         "below)")
        if variant["mods"] is not None:
            steps.append(f"move the mod folders in {self.mod_dir} aside and copy in: "
                         + (", ".join(variant["mods"]) or "none"))
        if self.base_ini:
            steps.append(f"replace the settings file with {self.base_ini}")
        for section, values in self.settings(variant).items():
            steps.append(f"set [{section}] " + ", ".join(f"{key}={ini_format(value) if value is not None else '<default>'}"
                                                         for key, value in values.items()))
        if variant.get("addons_off") is not None:
            steps.append("set [DisabledAddOns] to: " + (", ".join(variant["addons_off"]) or "none"))
        if variant["power_policy"]:
            steps.append("powercfg (AC, restored afterwards): "
                         + " ".join(f"{name}={value}" for name, value in variant["power_policy"].items()))
        env = {DEBUG_ENV: f"127.0.0.1:{self.port}", **({SHOWN_STATS_ENV: "1"} if variant["pacing"] else {}),
               **(variant.get("env") or {})}
        steps.append("start the emulator " + ("fullscreen ([UI] fullscreen on unless the scene's or the variant's "
                                              "settings turn it off)" if variant["fullscreen"] else "windowed")
                     + " with " + " ".join(f"{key}={value}" for key, value in env.items()))
        steps.append(f"wait for the debug server ({self.server_timeout_s:g} s) and {self.min_presents} presents "
                     f"({self.presents_timeout_s:g} s)")
        for item in self.scene["input"]:
            held = "+".join(item.get("buttons") or []) or ("stick" if item.get("left_stick") else "")
            steps.append(f"wait {item.get('wait_s', 0):g} s" + (f", hold {held} for {item.get('frames', 6)} frames"
                                                                if held else ""))
        steps.append(f"settle {self.settle_s:g} s")
        limits = self.picture_limits
        steps.append(f"screenshot, compare with the reference (limit {self.threshold:g}); refuse a black picture "
                     f"({limits['dark_pct']:g}% of its centre under {limits['dark_level']:g} and a centre mean under "
                     f"{limits['mean']:g})")
        for call in variant["live"]:
            steps.append(f"call {call['method']} {json.dumps(call.get('params') or {})}")
        if variant["reject"]:
            steps.append("reject shaders " + ", ".join(variant["reject"]) + " (probe row; restored before exit)")
        if variant["live"] or variant["reject"]:
            steps.append(f"wait {variant['live_settle_s']:g} s")
        steps.append(f"measure 3 windows of {self.window_s:g} s, keep the median; watts after each "
                     "(debug server, else energy counters)")
        if self.gpu_telemetry:
            steps.append(f"GPU clock and limit from {self.gpu_telemetry}")
        if self.sustain_min > 0:
            steps.append(f"sustained: one sample every {self.sustain_interval_s:g} s for {self.sustain_min:g} min")
        if variant["late_s"]:
            steps.append(f"wait {variant['late_s']:g} s, measure one more window")
        steps.append("read memory and gpu_costs when the server has them; window size, process memory")
        if variant.get("cache_state"):
            steps.append("boot_timing at the first frames, and with the marks 'scene' (after the scene check) and "
                         "'end' (here)")
        steps.append("screenshot again, save it" + ("" if variant["reject"] else ", compare"))
        if variant.get("cache_state"):
            steps.append(f"close the emulator and wait up to {self.boot['close_grace_s']:g} s for it to save its "
                         "caches and exit; then put the original caches back")
        steps.append("stop the emulator; once it is gone restore the settings file"
                     + (", the mod folders" if variant["mods"] is not None else "")
                     + (", the power settings" if variant["power_policy"] else "")
                     + "; copy the log, read its pacing and swapchain lines, append the row")
        return steps


class SaveGuard:
    """Replaces the save folder from a backup per variant and puts the owner's saves back at the end."""

    def __init__(self, backup, target):
        self.backup = backup
        self.target = target
        self.kept = target + ".bench-keep" if target else None

    def begin(self):
        if not self.target:
            return
        if os.path.isdir(self.kept):
            # A killed run left the owner's saves aside.
            shutil.rmtree(self.target, ignore_errors=True)
            os.rename(self.kept, self.target)
        if os.path.isdir(self.target):
            os.rename(self.target, self.kept)

    def restore_scene(self):
        if not self.target:
            return
        if os.path.isdir(self.target):
            shutil.rmtree(self.target)
        shutil.copytree(self.backup, self.target)

    def end(self):
        if self.target and os.path.isdir(self.kept):
            shutil.rmtree(self.target, ignore_errors=True)
            os.rename(self.kept, self.target)


class ModGuard:
    """Leaves exactly the named mod folders in the title's mod directory; the owner's are moved
    aside and put back by restore(), or by recover() after a killed run."""

    def __init__(self, library, target):
        self.library = library
        self.target = target
        self.kept = target + ".bench-keep" if target else None
        self.active = False

    def recover(self):
        if not self.kept or not os.path.isdir(self.kept):
            return False
        shutil.rmtree(self.target, ignore_errors=True)
        os.rename(self.kept, self.target)
        return True

    def apply(self, mods):
        self.recover()
        os.makedirs(self.target, exist_ok=True)
        os.rename(self.target, self.kept)
        self.active = True
        os.makedirs(self.target)
        for mod in mods:
            shutil.copytree(os.path.join(self.library, mod), os.path.join(self.target, mod))

    def restore(self):
        if self.active:
            self.recover()
            self.active = False


def same_title(expected, reported):
    try:
        return int(str(reported), 16) == int(expected, 16)
    except ValueError:
        return True


def run_sustained(client, plan, power, gpu, meter, deps):
    """power() and gpu() give one reading each; both run after a sample's window."""
    count = max(1, math.ceil(plan.sustain_min * 60 / plan.sustain_interval_s))
    samples = []
    started = time.monotonic()
    for index in range(count):
        meter.start()
        window = measure_window(client, plan.sustain_interval_s, plan.refresh_hz)
        cpu = meter.stop() or {}
        sample = {"t_s": round(time.monotonic() - started, 1), "fps": window["fps"],
                  "p50_ms": window["p50_ms"], "p99_ms": window["p99_ms"],
                  "other_cpu_pct": cpu.get("other_cpu_pct")}
        sample.update(power() or {})
        sample.update(gpu())
        samples.append(sample)
        if index % 6 == 5:
            deps.log(f"    sustained {sample['t_s']:.0f} s: {sample['fps']} fps")
    fps = [sample["fps"] for sample in samples]
    watts = [sample["package_w"] for sample in samples if "package_w" in sample]
    clocks = [sample["gpu_mhz"] for sample in samples if "gpu_mhz" in sample]
    sustained = {"minutes": plan.sustain_min, "interval_s": plan.sustain_interval_s, "samples": samples,
                 "fps_min": min(fps), "fps_avg": round(sum(fps) / len(fps), 2),
                 "fps_first": fps[0], "fps_last": fps[-1],
                 "watts_avg": round(sum(watts) / len(watts), 2) if watts else None}
    if clocks:
        sustained["gpu_mhz_min"] = min(clocks)
    return sustained


def new_row(plan, variant, hardware):
    return {
        "schema_version": SCHEMA_VERSION,
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "machine_id": machine_id(hardware),
        "scene": plan.scene["name"],
        "title_id": plan.scene["title_id"],
        "variant": variant["name"],
        "variant_settings": {section: {key: value if value is None else ini_format(value)
                                       for key, value in values.items()}
                             for section, values in (variant.get("settings") or {}).items()},
        "variant_env": {key: str(value) for key, value in (variant.get("env") or {}).items()},
        "emulator": {"exe": os.path.basename(plan.exe)},
        "game": os.path.basename(plan.game),
        "hardware": hardware,
        "valid": False, "invalid_reasons": [], "warnings": [], "error": None,
        "scene_ok": None, "scene_diff": None,
        "fps": None, "mean_ms": None, "p50_ms": None, "p90_ms": None, "p99_ms": None, "max_ms": None,
        "stutters": None, "vsync_buckets": None,
        "windows_fps": [], "window_s": plan.window_s,
        "sustained": None, "watts": None, "power": None, "memory": None, "gpu_costs": None,
        "system": {"other_cpu_pct": None, "other_cpu_max_pct": None,
                   "other_cpu_limit_pct": plan.cpu_limit_pct, "emulator_cpu_pct": None,
                   "ram_free_min_gb": None},
        "screenshot": None,
        "suite": plan.suite,
        "probe": bool(variant["reject"]),
        "base_ini": os.path.basename(plan.base_ini) if plan.base_ini else None,
        "mods": variant["mods"],
        "addons_off": variant.get("addons_off"),
        "reject": variant["reject"],
        "live": variant["live"],
        "power_policy": None,
        "gpu_mhz": None, "gpu_limited_by": None, "gpu_telemetry_w": None,
        "late": None, "window": None, "process_memory": None, "log": None, "trace": None,
        "fullscreen": None, "fullscreen_requested": variant["fullscreen"], "shown_stats": None,
        "output": None, "pacing": None,
        "picture": {"limits": dict(plan.picture_limits), "shots": [], "black": False},
    }


def check_scene(row, plan, png, reference):
    """Updates scene_ok / scene_diff with one more screenshot; False when the scene is wrong."""
    if reference is None:
        return True
    diff = image_diff(png, reference)
    row["scene_diff"] = diff if row["scene_diff"] is None else max(row["scene_diff"], diff)
    row["scene_ok"] = row["scene_diff"] < plan.threshold
    return row["scene_ok"]


def check_picture(row, png):
    """Adds one screenshot to the row's picture check, which needs no reference; False when the
    picture is black: near black almost everywhere, at most a HUD drawn."""
    picture = row["picture"]
    stats = picture_stats(png, picture["limits"])
    black = stats.pop("black")
    picture["shots"].append(stats)
    picture["black"] = picture["black"] or black
    return not black


def black_message(row):
    stats = row["picture"]["shots"][-1]
    drawn = round(100 - stats["dark_pct"], 1)
    return (f"black picture: {stats['center_dark_pct']:g}% of its centre near black, centre mean "
            f"{stats['center_mean']:g}" + (f"; only {drawn:g}% of the whole is drawn, as a HUD would be"
                                            if drawn else "; nothing is drawn"))


def run_each(*actions):
    """Runs every action even when one fails, then raises the first failure."""
    failure = None
    for action in actions:
        try:
            action()
        except BaseException as error:  # the remaining restores must still run
            failure = failure or error
    if failure is not None:
        raise failure


def run_variant(plan, variant, hardware, deps, row_id):
    """Runs one variant and returns its row. Leaves no emulator, modified ini, moved mods or
    changed power settings behind."""
    row = new_row(plan, variant, hardware)
    row["id"] = row_id
    shot_path = os.path.join(plan.out, "shots", row_id + ".png")
    reasons = row["invalid_reasons"]
    client = DebugClient(port=plan.port)
    guard = IniGuard(plan.ini)
    mods = ModGuard(plan.mod_library, plan.mod_dir)
    policy = PowerPolicy(variant["power_policy"], deps.run_command, plan.power_restore)
    reject = {"hashes": variant["reject"], "timeout_s": 20}
    process = watchdog = None
    rejected = False
    cpu_readings = []
    log_copy = None
    boot = bool(variant.get("cache_state"))
    boot_answers = []
    if boot:
        row["cache_state"], row["repeat"] = variant["cache_state"], variant["repeat"]
        row["boot"] = {"caches": {}}
    try:
        image = os.path.basename(plan.exe)
        if deps.running(image):
            raise BenchError(f"{image} is already running: close it first")
        if client.listening():
            raise BenchError(f"something already listens on port {plan.port}")
        SaveGuard(plan.save_backup, plan.save_dir).restore_scene()
        if variant["mods"] is not None:
            mods.apply(variant["mods"])
        addons = None if variant.get("addons_off") is None else (plan.scene["title_id"], variant["addons_off"])
        guard.apply(plan.settings(variant), plan.base_ini, addons)
        with open(plan.ini, encoding="utf-8", errors="replace") as file:
            ini = file.read()
        row["fullscreen"] = ini_value(ini, "UI", "fullscreen") == "true"
        if row["fullscreen"]:
            mode = ini_value(ini, "Renderer", "fullscreen_mode")
            # The default is Borderless on Windows and Exclusive elsewhere (common/settings.h).
            # A per-game config can still override the mode; the bench does not read those.
            row["output"] = {"size": None, "size_from": None,
                             "fullscreen_mode": (("borderless" if IS_WINDOWS else "exclusive") if mode is None
                                                 else FULLSCREEN_MODES.get(mode, mode)),
                             "fullscreen_mode_from": "default" if mode is None else "settings"}
        elif variant["fullscreen"]:
            row["warnings"].append("fullscreen_off_in_settings")
            deps.log("    fullscreen asked for, but the scene's or the variant's settings turn [UI] fullscreen off: "
                     "the run is windowed")
        if policy.values:
            row["power_policy"] = policy.apply()
            if row["power_policy"]["skipped"]:
                row["warnings"].append("power_policy_partial")
        env = dict(os.environ)
        env[DEBUG_ENV] = f"127.0.0.1:{plan.port}"
        if variant["pacing"]:
            env[SHOWN_STATS_ENV] = "1"
        env.update({key: str(value) for key, value in (variant.get("env") or {}).items()})
        # The switch adds GPU timestamps to FLUID presenter copies: rows say whether it was on.
        row["shown_stats"] = env.get(SHOWN_STATS_ENV, "") not in ("", "0")
        if boot:
            row["boot"]["caches"] = plan.boot_session.prepare(variant, row_id)
        process = deps.launch(plan.exe, plan.game, env)

        deadline = time.monotonic() + plan.server_timeout_s
        while not client.listening():
            if process.poll() is not None:
                raise BenchError("the emulator exited during boot")
            if time.monotonic() > deadline:
                raise BenchError(f"the debug server did not come up in {plan.server_timeout_s:g} s")
            deps.sleep(1)
        watchdog = MemoryWatchdog(process, plan.ram_floor_gb, deps.free_ram)
        watchdog.start()
        deadline = time.monotonic() + plan.presents_timeout_s
        while True:
            status = client.try_call("status", {}, 15) or {}
            if isinstance(status.get("presents"), NUMBER) and status["presents"] >= plan.min_presents:
                break
            if process.poll() is not None:
                raise BenchError("the emulator exited during boot")
            if time.monotonic() > deadline:
                raise BenchError(f"fewer than {plan.min_presents} presents after {plan.presents_timeout_s:g} s")
            deps.sleep(2)
        for key in ("title", "program_id", "build", "renderer"):
            if isinstance(status.get(key), (str, int)):
                row["emulator"][key] = status[key]
        if boot:
            boot_answers.append(client.try_call("boot_timing", {}, 20))
            row["boot"]["switch_defaults"] = switch_decisions(status)
        if "program_id" in status and not same_title(plan.scene["title_id"], status["program_id"]):
            reasons.append("wrong_title")
        available = {method: client.try_call(method, {}, 20) is not None for method in OPTIONAL_METHODS}
        power = PowerReader(client, available["power"], deps.run_command)

        def gpu():
            return read_gpu_telemetry(plan.gpu_telemetry, deps.run_command)

        deps.log("    input script")
        play_input_script(client, plan.scene["input"], deps.sleep)
        deps.sleep(plan.settle_s)

        reference = None
        first_shot = take_screenshot(client, plan.shot_height)
        if not check_picture(row, first_shot):
            # Also when recording: a black reference would let every broken run pass the scene check.
            with open(shot_path, "wb") as file:
                file.write(first_shot)
            row["screenshot"] = shot_path
            raise BenchError(black_message(row) + ": not measured"
                             + (", no reference written" if plan.record_reference else ""))
        if plan.record_reference:
            os.makedirs(os.path.dirname(plan.reference), exist_ok=True)
            with open(plan.reference, "wb") as file:
                file.write(first_shot)
            reasons.append("reference_recorded")
            deps.log(f"    reference written to {plan.reference}: look at it before trusting it")
        elif plan.reference and os.path.isfile(plan.reference):
            with open(plan.reference, "rb") as file:
                reference = file.read()
        else:
            reasons.append("scene_unverified")
        if not check_scene(row, plan, first_shot, reference):
            with open(shot_path, "wb") as file:
                file.write(first_shot)
            row["screenshot"] = shot_path
            raise BenchError(f"wrong scene (diff {row['scene_diff']}): not measured")
        deps.sleep(2)
        if boot:
            boot_answers.append(client.try_call("boot_timing", {"mark": "scene"}, 20))

        for call in variant["live"]:
            client.call(call["method"], call.get("params") or {}, 40)
        if variant["reject"]:
            rejected = True
            client.call("reject_shaders", dict(reject, reject=True), 40)
        if variant["live"] or variant["reject"]:
            deps.sleep(variant["live_settle_s"])

        meter = deps.cpu_meter(process.pid)
        windows, powers = [], []
        for _ in range(3):
            meter.start()
            windows.append(measure_window(client, plan.window_s, plan.refresh_hz))
            cpu_readings.append(meter.stop())
            powers.append(power.read())
        row.update(median_window(windows))
        row["windows_fps"] = [window["fps"] for window in windows]
        row["power"] = mean_power(powers)
        row["watts"] = (row["power"] or {}).get("package_w")
        if row["power"]:
            row["power"]["source"] = power.source
        row.update(gpu())
        deps.log(f"    {row['fps']} fps (windows {row['windows_fps']}), p50 {row['p50_ms']} ms, "
                 f"p99 {row['p99_ms']} ms")
        if plan.sustain_min > 0:
            row["sustained"] = run_sustained(client, plan, power.read, gpu, meter, deps)
            cpu_readings += [{"other_cpu_pct": sample["other_cpu_pct"]} for sample in row["sustained"]["samples"]
                             if sample["other_cpu_pct"] is not None]
        if variant["late_s"]:
            deps.sleep(variant["late_s"])
            meter.start()
            window = measure_window(client, plan.window_s, plan.refresh_hz)
            cpu_readings.append(meter.stop())
            row["late"] = {"after_s": variant["late_s"], "fps": window["fps"], "p50_ms": window["p50_ms"],
                           "p99_ms": window["p99_ms"], **(power.read() or {}), **gpu()}
        for method in ("memory", "gpu_costs"):
            if available[method]:
                row[method] = client.try_call(method, {}, 30)
        row["window"] = deps.window_size(process.pid)
        row["process_memory"] = deps.process_memory(process.pid)
        if boot:
            boot_answers.append(client.try_call("boot_timing", {"mark": "end"}, 20))

        last_shot = take_screenshot(client, plan.shot_height)
        with open(shot_path, "wb") as file:
            file.write(last_shot)
        row["screenshot"] = shot_path
        if not variant["reject"]:
            check_scene(row, plan, last_shot, reference)
            if not check_picture(row, last_shot):
                deps.log("    " + black_message(row))
    except Exception as error:  # every failure becomes an invalid row
        row["error"] = str(error) or type(error).__name__
    finally:
        def stop():
            if rejected and process.poll() is None \
                    and client.try_call("reject_shaders", dict(reject, reject=False), 40) is None:
                row["warnings"].append("reject_not_restored")
            if watchdog is not None:
                watchdog.stop()
            if boot:
                # It saves its caches on the way out: give it time to.
                deps.stop(process, close_grace_s=plan.boot["close_grace_s"])
            else:
                deps.stop(process)
            # The emulator writes its settings when it exits: restore only once it is gone.
            if not wait_gone(process, deps.sleep):
                row["warnings"].append("emulator_still_running")

        def restore_caches():
            if not boot:
                return
            if process is not None and process.poll() is None:
                raise BenchError("the emulator is still running, so its caches stay aside: close it, then run the "
                                 "suite again, which puts them back first")
            plan.boot_session.restore(variant)

        run_each(stop, restore_caches, guard.restore, mods.restore, policy.restore)

    if process is not None:
        log_name = f"logs/{row_id}.log.txt"
        log_copy = os.path.join(plan.out, *log_name.split("/"))
        copied, row["trace"] = collect_log(plan.log_file, log_copy)
        row["log"] = log_name if copied else None
        row["pacing"], swapchain = read_presenter(log_copy) if copied else (None, None)
        output = row["output"] or {"size": None, "size_from": None}
        if swapchain:
            output.update(swapchain, size_from="swapchain")
            # The window size was read while the game ran; a fullscreen window should match it.
            if row["fullscreen"] and row["window"] and swapchain["size"] != row["window"]:
                row["warnings"].append("output_size_not_window_size")
        elif row["window"]:
            output.update(size=row["window"], size_from="window")
        row["output"] = output if output["size"] or row["output"] else None
    if row["fullscreen"] and row["fps"] is not None and not (row["output"] or {}).get("size"):
        reasons.append("output_size_unknown")
    if watchdog is not None:
        if watchdog.lowest is not None:
            row["system"]["ram_free_min_gb"] = round(watchdog.lowest, 1)
        if watchdog.fired:
            reasons.append("memory_watchdog")
            row["error"] = f"free RAM fell under {plan.ram_floor_gb} GB: emulator stopped"
    cpu_readings = [reading for reading in cpu_readings if reading]
    others = [reading["other_cpu_pct"] for reading in cpu_readings]
    if others:
        row["system"]["other_cpu_pct"] = round(sum(others[:3]) / len(others[:3]), 1)
        row["system"]["other_cpu_max_pct"] = max(others)
        emulator = [reading["emulator_cpu_pct"] for reading in cpu_readings[:3] if "emulator_cpu_pct" in reading]
        row["system"]["emulator_cpu_pct"] = round(sum(emulator) / len(emulator), 1) if emulator else None
        if max(others) > plan.cpu_limit_pct:
            reasons.append("background_cpu")
    elif row["fps"] is not None:
        row["warnings"].append("background_cpu_not_measured")
    if row["scene_ok"] is False:
        reasons.append("wrong_scene")
    if row["picture"]["black"]:
        reasons.append("black_picture")
    if row["error"] is not None:
        reasons.append("error")
    if hardware.get("power_source") == "battery":
        row["warnings"].append("on_battery")
    if row["fps"] is not None and row["power"] is None:
        row["warnings"].append("no_power_reading")
    if boot:
        row["boot"].update(boot_summary(*(boot_answers + [None, None, None])[:3]))
        finish_boot_row(row, variant, log_copy if row["log"] else None, plan.boot_session)
    row["invalid_reasons"] = list(dict.fromkeys(reasons))
    row["valid"] = not row["invalid_reasons"]
    return row


def run_plans(plans, deps, dry_run=False):
    """Runs every variant of every plan. Returns (exit code, rows)."""
    if dry_run:
        for plan in plans:
            print(plan.describe())
            for problem in plan.check():
                print(f"PROBLEM: {problem}")
        return 0, []
    problems = list(dict.fromkeys(problem for plan in plans for problem in plan.check()))
    if problems:
        raise BenchError("; ".join(problems))
    first = plans[0]
    image = os.path.basename(first.exe)
    if deps.running(image):
        raise BenchError(f"{image} is already running: close it first (one instance only)")
    for plan in plans:
        if IniGuard(plan.ini).recover():
            deps.log("restored the settings file an interrupted run had left modified")
        if ModGuard(plan.mod_library, plan.mod_dir).recover():
            deps.log("restored the mod folders an interrupted run had left aside")
    if PowerPolicy({}, deps.run_command, first.power_restore).recover():
        deps.log("restored the power settings an interrupted run had left changed")
    for plan in plans:
        if plan.boot:
            plan.boot_session = BootSession(plan)
            deps.log(f"copying the caches a boot run moves to {plan.boot_session.work}")
            for message in plan.boot_session.begin():
                deps.log(message)
    deps.log("collecting hardware information")
    hardware = deps.hardware()
    sanitize = make_sanitizer()
    hardware = sanitize(hardware)
    machine = machine_id(hardware)
    stamp = datetime.datetime.now()
    os.makedirs(os.path.join(first.out, "shots"), exist_ok=True)
    results = os.path.join(first.out, f"{stamp:%Y-%m-%d}-{machine}.jsonl")
    rows = []
    for plan in plans:
        saves = SaveGuard(plan.save_backup, plan.save_dir)
        saves.begin()
        try:
            for variant in plan.variants:
                state = (f", {variant['cache_state']} cache, repeat {variant['repeat'] + 1}"
                         if variant.get("cache_state") else "")
                deps.log(f"variant {variant['name']}{state}")
                suffix = f"-{variant['cache_state']}-r{variant['repeat'] + 1}" if variant.get("cache_state") else ""
                row_id = (f"{stamp:%Y-%m-%d}-{machine}-{plan.scene['name']}-{variant['name']}{suffix}-"
                          f"{datetime.datetime.now():%H%M%S}")
                row = run_variant(plan, variant, hardware, deps, row_id)
                if row["screenshot"]:
                    row["screenshot"] = f"shots/{row_id}.png"
                row = sanitize(row)
                problems = validate_row(row)
                if problems:
                    row["valid"] = False
                    row["invalid_reasons"].append("schema")
                    deps.log("    schema problems: " + "; ".join(problems))
                with open(results, "a", encoding="utf-8", newline="\n") as file:
                    file.write(json.dumps(row, separators=(",", ":")) + "\n")
                rows.append(row)
                deps.log(f"    {'VALID' if row['valid'] else 'INVALID (' + ', '.join(row['invalid_reasons']) + ')'}"
                         + (" (probe)" if row["probe"] else "") + (f": {row['error']}" if row["error"] else ""))
            if plan.boot_session:
                plan.boot_session.finish()
        finally:
            saves.end()
    deps.log(f"rows appended to {results}")
    return (0 if all(row["valid"] for row in rows) else 2), rows


def command_run(args, deps=Deps):
    return run_plans([Plan(args)], deps, args.dry_run)[0]


# ---------------------------------------------------------------------------------------------
# Boot suites: launch to gameplay from a cold, second or warm cache (a suite's "boot" block)

def nx2_tool(name):
    """A module of tools/nx2 (standard library only), imported when a boot suite first needs it."""
    if NX2_TOOLS not in sys.path:
        sys.path.insert(0, NX2_TOOLS)
    return importlib.import_module(name)


def check_driver_cache_dir(path):
    """The absolute path of a GPU driver's cache folder the bench may move aside. It must exist and
    its own name must say cache; never a drive, the home folder, or a path that names saves or keys."""
    path = os.path.abspath(expand(path))
    name = os.path.basename(path.rstrip("\\/"))
    home = os.path.abspath(os.path.expanduser("~"))
    parts = {part.lower() for part in re.split(r"[\\/]+", path) if part}
    if not os.path.isdir(path):
        raise BenchError(f"--driver-cache-dir {path}: not a folder")
    if "cache" not in name.lower():
        raise BenchError(f"--driver-cache-dir {path}: the folder's own name must say cache (a GPU driver's "
                         "shader cache folder)")
    if os.path.dirname(path) == path or os.path.normcase(path) in {os.path.normcase(home),
                                                                   os.path.normcase(os.path.dirname(home))}:
        raise BenchError(f"--driver-cache-dir {path}: refusing a drive or the home folder")
    if parts & {"nand", "sdmc", "save", "saves", "keys"}:
        raise BenchError(f"--driver-cache-dir {path}: refusing a path that names saves or keys")
    return path


def load_boot_options(suite, path, args):
    """The suite's "boot" block with the command line's choices applied, or None for a suite without
    one: {states, repeats, close_grace_s, driver_dirs, work_dir, keep_caches}."""
    boot = suite.get("boot")
    if boot is None:
        return None
    if not isinstance(boot, dict):
        raise BenchError(f"suite {path}: boot is an object")
    states = boot.get("cache_states", list(CACHE_STATES))
    if not isinstance(states, list) or not states or any(state not in CACHE_STATES for state in states):
        raise BenchError(f"suite {path}: boot.cache_states lists some of {', '.join(CACHE_STATES)}")
    chosen = getattr(args, "cache_states", None)
    if chosen:
        unknown = [state for state in chosen if state not in CACHE_STATES]
        if unknown:
            raise BenchError(f"--cache-states: unknown {', '.join(unknown)} (known: {', '.join(CACHE_STATES)})")
        states = [state for state in CACHE_STATES if state in chosen]
    try:
        chosen_repeats = getattr(args, "repeats", None)
        repeats = int(boot.get("repeats", 3) if chosen_repeats is None else chosen_repeats)
        close_grace_s = float(boot.get("close_grace_s", BOOT_CLOSE_GRACE_S))
    except (TypeError, ValueError):
        raise BenchError(f"suite {path}: boot.repeats and boot.close_grace_s are numbers") from None
    if repeats < 1:
        raise BenchError("a boot suite runs at least once (--repeats 1)")
    return {"states": [state for state in CACHE_STATES if state in states], "repeats": repeats,
            "close_grace_s": close_grace_s,
            "driver_dirs": [check_driver_cache_dir(item) for item in getattr(args, "driver_cache_dir", None) or []],
            "work_dir": expand(getattr(args, "cache_work", None)) or os.path.join(default_state_dir(), "boot-caches"),
            "keep_caches": bool(getattr(args, "keep_caches", False)), "baseline": None}


def expand_boot_variants(variants, boot):
    """Every variant once per cache state and repeat: per repeat, per cache state, the variants that
    run in it, in the suite's order on even repeats and reversed on odd ones, so that a variant and
    the baseline take turns going first. A variant's "cache_states" picks some of the suite's."""
    states = {}
    for variant in variants:
        mine = variant.get("cache_states", boot["states"])
        if not isinstance(mine, list) or any(state not in CACHE_STATES for state in mine):
            raise BenchError(f"variant {variant.get('name')}: cache_states lists some of {', '.join(CACHE_STATES)}")
        mine = [state for state in boot["states"] if state in mine]
        if "second" in mine and "cold" not in mine:
            raise BenchError(f"variant {variant.get('name')}: a second run starts from what the same variant's "
                             "cold run left, so it needs cold too")
        states[variant.get("name")] = mine
    expanded = []
    for repeat in range(boot["repeats"]):
        for state in boot["states"]:
            group = [variant for variant in variants if state in states[variant.get("name")]]
            if repeat % 2:
                group.reverse()
            for variant in group:
                entry = {key: value for key, value in variant.items() if key != "cache_states"}
                expanded.append({**entry, "cache_state": state, "repeat": repeat})
    return expanded


class BootSession:
    """The caches a boot suite moves. The title's shader cache folder (and each --driver-cache-dir) is
    copied once with a sha256 manifest, moved aside before every run and put back after it. The
    driver folders are not compared with their manifest afterwards: other programs on the same GPU
    write them too. What a cold run left is kept until the same variant's second run of that repeat
    has copied it."""

    def __init__(self, plan):
        self.cache_bench = nx2_tool("cache_bench")
        self.keep = plan.boot["keep_caches"]
        self.work = os.path.join(plan.boot["work_dir"], datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
        backup = os.path.join(self.work, "backup")
        title = plan.scene["title_id"]
        driver_dirs = plan.boot["driver_dirs"]
        roots = [self.cache_bench.shader_root(plan.user_dir)] + [os.path.dirname(path) for path in driver_dirs]
        self.dirs = [self.cache_bench.CacheDir(f"shader-{title.lower()}",
                                               self.cache_bench.title_dir(plan.user_dir, title), backup, roots)]
        self.dirs += [self.cache_bench.CacheDir(f"driver-{index}", path, backup, roots, strict=False)
                      for index, path in enumerate(driver_dirs)]
        self.cold = {}
        # The cold runs whose caches a second run of the same variant and repeat copies; the
        # others' are deleted at once unless --keep-caches.
        self.seconds = {(variant["name"], variant["repeat"]) for variant in plan.variants
                        if variant.get("cache_state") == "second"}
        self.prepared = []
        self.after = None

    def recover(self):
        """Puts back the originals an interrupted suite left aside; what was in their place goes to
        the work folder. Returns what it did."""
        done = []
        for directory in self.dirs:
            if directory.aside.exists():
                if directory.path.exists():
                    keep = os.path.join(self.work, "recovered", directory.key)
                    os.makedirs(os.path.dirname(keep), exist_ok=True)
                    shutil.move(str(directory.path), keep)
                os.replace(directory.aside, directory.path)
                done.append(f"put back {directory.path}, which an interrupted run had left aside")
        return done

    def begin(self):
        """recover(), then the backups the warm runs copy from."""
        done = self.recover()
        try:
            for directory in self.dirs:
                directory.back_up()
        except self.cache_bench.BenchError as error:
            raise BenchError(str(error)) from None
        return done

    def prepare(self, variant, row_id):
        """Moves the caches aside and puts in what the run's cache state starts from. Returns
        {folder key: present or absent} as the run starts."""
        state = variant["cache_state"]
        self.after = os.path.join(self.work, "runs", row_id, "after")
        source = None
        if state == "second":
            cold = self.cold.get((variant["name"], variant["repeat"])) or {}
            if not cold.get("after"):
                raise BenchError("no caches from this variant's cold run of this repeat to start from "
                                 "(the cold run failed or left none)")
            source = cold["after"]
        try:
            for directory in self.dirs:
                directory.check()
        except self.cache_bench.BenchError as error:
            raise BenchError(str(error)) from None
        start = {}
        for directory in self.dirs:
            self.prepared.append(directory)
            directory.prepare(state, os.path.join(source, directory.key) if source else None)
            start[directory.key] = "present" if directory.path.exists() else "absent"
        return start

    def restore(self, variant):
        """Once the emulator is gone: moves what the run wrote out of the way and puts every original
        back, all of them even when one fails. The cold run's folders are kept for its second run."""
        failure = None
        for directory in self.prepared:
            try:
                directory.restore(self.after)
            except (OSError, self.cache_bench.BenchError) as error:
                failure = failure or error
        self.prepared = []
        key = (variant["name"], variant["repeat"])
        if variant["cache_state"] == "cold" and (key in self.seconds or self.keep):
            self.cold[key] = {"after": self.after if os.path.isdir(self.after) else None, "usable": None}
        elif not self.keep:
            # The run's own folder goes with what it held.
            shutil.rmtree(os.path.dirname(self.after), ignore_errors=True)
            if variant["cache_state"] == "second" and (self.cold.get(key) or {}).get("after"):
                shutil.rmtree(os.path.dirname(self.cold[key]["after"]), ignore_errors=True)
                self.cold[key]["after"] = None
        if failure is not None:
            raise BenchError(f"the caches were not all put back ({failure}); run the suite again, which puts "
                             f"back what is aside before anything else, or rename the {self.cache_bench.ASIDE_SUFFIX} "
                             "folders back by hand")

    def cold_usable(self, variant):
        """Whether the same variant's cold run of this repeat is valid and saved the driver pipeline
        cache after its first frame, so that what it left is a whole session's caches (None: unknown)."""
        return (self.cold.get((variant["name"], variant["repeat"])) or {}).get("usable")

    def note_cold(self, variant, usable):
        self.cold.setdefault((variant["name"], variant["repeat"]), {"after": None})["usable"] = usable

    def finish(self):
        """After the last run: the backup copies go unless --keep-caches (their manifests stay)."""
        if not self.keep:
            for directory in self.dirs:
                shutil.rmtree(directory.backup, ignore_errors=True)


def _rounded(value):
    if isinstance(value, float):
        return round(value, 1)
    if isinstance(value, dict):
        return {key: _rounded(item) for key, item in value.items()}
    return value


def boot_summary(first, scene, end):
    """The row's boot numbers from the debug server's boot_timing answers (any may be None: an older
    build): phases since the process started, the first frame, and the held draws and pipeline builds
    from the first frame to the "scene" and "end" marks."""
    latest = next((item for item in (end, scene, first) if isinstance(item, dict)), None)
    if latest is None:
        return {"first_frame_ms": None}
    phases = [phase for phase in latest.get("phases") or [] if isinstance(phase, dict)
              and isinstance(phase.get("phase"), str) and isinstance(phase.get("at_ms"), NUMBER)]

    def at(name):
        return next((round(phase["at_ms"], 1) for phase in phases if phase["phase"] == name), None)

    split = latest.get("split") or {}
    first_frame = split.get("first_frame") or {}
    marks = {mark.get("label"): mark for mark in split.get("marks") or [] if isinstance(mark, dict)}
    return {
        "first_frame_ms": (round(first_frame["at_ms"], 1) if isinstance(first_frame.get("at_ms"), NUMBER)
                           else at(FIRST_FRAME_PHASE)),
        "game_shown_ms": at(GAME_SHOWN_PHASE),
        "guest_released_ms": at(GUEST_RELEASED_PHASE),
        "instances_created": sum(1 for phase in phases if phase["phase"] == INSTANCE_CREATED_PHASE),
        "phases": [[phase["phase"], round(phase["at_ms"], 1),
                    round(phase["took_ms"], 1) if isinstance(phase.get("took_ms"), NUMBER) else None]
                   for phase in phases],
        "at_first_frame": _rounded(first_frame.get("totals")),
        "to_scene": _rounded((marks.get("scene") or {}).get("since_first_frame")),
        "to_end": _rounded((marks.get("end") or {}).get("since_first_frame")),
    }


def switch_decisions(status):
    """[name, value, decision] of each switch the game's defaults resolved at boot (status.resolution)."""
    resolution = (status or {}).get("resolution") or {}
    return [[str(item.get("name")), str(item.get("value")), str(item.get("decision"))]
            for item in resolution.get("switches") or [] if isinstance(item, dict)] or None


def finish_boot_row(row, variant, log_copy, session):
    """What the copied log of a boot run says, with the checks that the run started from the cache
    state it claims: a cold run loaded no records, the boot guard did not skip the boot builds, the
    emulator used the cache folder the bench prepared. Called once every other reason is known: a
    cold run that is invalid, or did not save the driver pipeline cache after its first frame, makes
    the same variant's second run of that repeat invalid."""
    boot = row["boot"]
    reasons = row["invalid_reasons"]
    state = variant["cache_state"]
    if boot.get("first_frame_ms") is None:
        reasons.append("no_boot_timing")
    if state == "warm" and boot["caches"].get(session.dirs[0].key) == "absent":
        row["warnings"].append("warm_cache_empty")
    if state == "second" and session.cold_usable(variant) is False:
        reasons.append("second_source_incomplete")
    boot["driver_cache_saved_at_exit"] = None
    try:
        read_boot_log(row, variant, log_copy, session)
    finally:
        if state == "cold":
            # None: the emulator saves no driver cache of its own on this driver (nothing to wait for)
            session.note_cold(variant, boot["driver_cache_saved_at_exit"] is not False and not reasons)


def read_boot_log(row, variant, log_copy, session):
    boot = row["boot"]
    reasons = row["invalid_reasons"]
    lines = None
    if log_copy:
        try:
            metrics = nx2_tool("cache_metrics").parse_log(log_copy)
            with open(log_copy, encoding="utf-8", errors="replace") as file:
                lines = file.read().splitlines()
        except (OSError, ValueError):
            lines = None
    if lines is None:
        reasons.append("no_log")
        return
    keys = ("boot_to_first_frame_s", "loading_shaders_s", "records_loaded", "pipelines_queued_at_boot",
            "runtime_builds", "runtime_build_gpu_thread_ms", "frame_ms", "frames_over_33ms", "frames_over_100ms")
    boot["log"] = {key: metrics.get(key) for key in keys}
    boot["log"]["stutter_by_group"] = (metrics.get("stutter") or {}).get("by_group")
    first = next((index for index, line in enumerate(lines) if FIRST_FRAME_PHASE in line), None)
    boot["guard_tripped"] = any(BOOT_GUARD_TEXT in line for line in lines)
    if any(DRIVER_KEEPS_OWN_CACHE_TEXT in line for line in lines):
        boot["driver_cache_saved_at_exit"] = None
    else:
        boot["driver_cache_saved_at_exit"] = first is not None and any(
            DRIVER_CACHE_SAVED_TEXT in line for line in lines[first:])
    boot["cache_dir_ok"] = session.cache_bench.check_cache_dir(log_copy, session.dirs[0].path)
    if boot["guard_tripped"]:
        reasons.append("boot_guard")
    if boot["cache_dir_ok"] is False:
        reasons.append("cache_dir_mismatch")
    records = [value for value in (metrics.get("records_loaded") or {}).values() if isinstance(value, int)]
    if variant["cache_state"] == "cold":
        if not records:
            row["warnings"].append("cold_unverified")
        elif sum(records):
            reasons.append("cache_not_cold")


def boot_run_seconds(plan, state):
    """A rough length of one boot run, for the plan's estimate: boot, the scene's input script,
    settling, the three windows, and closing."""
    boot_s = {"cold": 90, "second": 60, "warm": 60}[state]
    script_s = sum(float(item.get("wait_s", 0)) for item in plan.scene["input"])
    return boot_s + script_s + plan.settle_s + 3 * plan.window_s + 15 + plan.boot["close_grace_s"] / 3


def _mean(values):
    values = [value for value in values if isinstance(value, NUMBER) and not isinstance(value, bool)]
    return sum(values) / len(values) if values else None


def _seconds(ms_values):
    values = [value / 1000 for value in ms_values if isinstance(value, NUMBER)]
    if not values:
        return "-"
    mean = sum(values) / len(values)
    return f"{mean:.1f}" + (f" ({min(values):.1f}-{max(values):.1f})" if len(values) > 1 else "")


def render_boot_suite(rows, baseline):
    """Markdown tables of a boot suite, one per cache state: the valid runs of every variant against
    the baseline's in the same state."""
    groups = collections.OrderedDict()
    for row in rows:
        groups.setdefault((row.get("cache_state"), row["variant"]), []).append(row)
    lines = []
    for state in CACHE_STATES:
        keys = [key for key in groups if key[0] == state]
        if not keys:
            continue
        base = [row for row in groups.get((state, baseline), []) if row["valid"]]
        base_first = _mean([(row.get("boot") or {}).get("first_frame_ms") for row in base])
        lines += [f"### {state}", "",
                  "| variant | valid runs | first frame s | vs baseline | game shown s | Vulkan instances | "
                  "held draws to scene | held ms to scene | frames over 100 ms | fps |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
        for key in keys:
            valid = [row for row in groups[key] if row["valid"]]
            boots = [row.get("boot") or {} for row in valid]
            first = _mean([boot.get("first_frame_ms") for boot in boots])
            if key[1] == baseline:
                delta = "baseline"
            elif first is not None and base_first is not None:
                delta = f"{(first - base_first) / 1000:+.1f} s"
            else:
                delta = "-"
            scene = [boot.get("to_scene") or {} for boot in boots]
            lines.append("| " + " | ".join([
                _cell(key[1]), f"{len(valid)} of {len(groups[key])}",
                _seconds([boot.get("first_frame_ms") for boot in boots]), delta,
                _seconds([boot.get("game_shown_ms") for boot in boots]),
                _cell(_mean([boot.get("instances_created") for boot in boots])),
                _cell(_mean([item.get("held_draws") for item in scene])),
                _cell(_mean([item.get("held_draw_ms") for item in scene])),
                _cell(_mean([(boot.get("log") or {}).get("frames_over_100ms") for boot in boots])),
                _cell(_mean([row.get("fps") for row in valid]))]) + " |")
        lines.append("")
    lines += ["Means of the valid runs; first frame and game shown are seconds since the process started, with "
              "the range over the repeats. Held draws waited for their pipeline to be built, from the first frame "
              "to the scene check. A difference counts only when every repeat shows it: read the rows."]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------
# Suites

SUITE_OPTIONS = ("sustain_min", "sustain_interval_s", "window_s", "settle_s", "ram_floor_gb", "max_other_cpu_pct")


def merge_variant(base, variant):
    """variant on top of base: env, settings and power_policy merge per key, the rest replaces."""
    merged = {**base, **variant}
    for key in ("env", "power_policy"):
        if base.get(key) or variant.get(key):
            merged[key] = {**(base.get(key) or {}), **(variant.get(key) or {})}
    merged["settings"] = merge_settings(base.get("settings"), variant.get("settings"))
    return merged


def variant_files(folder, directory):
    """The variant files in a suite run's "variants_dir" (relative to the suite file), by name."""
    if not directory:
        return []
    path = os.path.join(folder, os.path.expandvars(str(directory)))
    if not os.path.isdir(path):
        raise BenchError(f"variants_dir {path}: not a folder")
    return sorted(glob.glob(os.path.join(path, "*.json")))


def load_suite(path, args, defaults):
    """(plans, {(scene, variant): variant to compare with}) of a suite file.

    A suite is {name, scene, options, base, baseline, paired: {suffix, env}, boot, runs: [{scene,
    options, base, baseline, variants: [variant object or file name], variants_dir}]}. Paths are
    relative to the suite file; variants_dir adds every .json in that folder, by name, after the
    variants listed. A variant with "paired": true also runs with the paired env, as
    <name><suffix>. Options given on the command line win over the suite's. A suite with a boot
    block runs its single run's variants once per cache state and repeat (expand_boot_variants)."""
    path = expand(path)
    suite = load_json(path, "suite")
    folder = os.path.dirname(path)
    name = str(suite.get("name") or os.path.splitext(os.path.basename(path))[0])
    paired = suite.get("paired") or {}
    suffix = str(paired.get("suffix", "-paired"))
    runs = suite.get("runs")
    if not isinstance(runs, list) or not runs:
        raise BenchError(f"suite {path}: runs is a non-empty list")
    boot = load_boot_options(suite, path, args)
    if boot and len(runs) != 1:
        raise BenchError(f"suite {path}: a boot suite has one run (one scene)")
    only = getattr(args, "only_variants", None)
    plans, compare_to = [], {}
    for run in runs:
        run_args = copy.copy(args)
        scene = run.get("scene") or suite.get("scene")
        if not args.scene:
            if not scene:
                raise BenchError(f"suite {path}: no scene")
            run_args.scene = os.path.join(folder, os.path.expandvars(scene))
        for key, value in {**(suite.get("options") or {}), **(run.get("options") or {})}.items():
            if key not in SUITE_OPTIONS:
                raise BenchError(f"suite {path}: unknown option {key} (known: {', '.join(SUITE_OPTIONS)})")
            if getattr(args, key) == defaults[key]:
                setattr(run_args, key, value)
        base = merge_variant(suite.get("base") or {}, run.get("base") or {})
        variants, baselines = [], {}
        baseline = run.get("baseline") or suite.get("baseline")
        for entry in list(run.get("variants") or []) + variant_files(folder, run.get("variants_dir")):
            if isinstance(entry, str):
                entry = load_json(os.path.join(folder, os.path.expandvars(entry)), "variant")
            if not isinstance(entry, dict):
                raise BenchError(f"suite {path}: a variant is an object or a file name")
            variant = merge_variant(base, entry)
            twin = variant.pop("paired", False) and paired.get("env")
            variants.append(variant)
            baseline = baseline or variant.get("name")
            baselines[variant.get("name")] = baseline
            if twin:
                variants.append(merge_variant(variant, {"name": f"{variant.get('name')}{suffix}", "env": paired["env"]}))
                baselines[variants[-1]["name"]] = baseline + suffix
        if not variants:
            raise BenchError(f"suite {path}: a run without variants")
        if only:
            unknown = sorted(set(only) - {variant.get("name") for variant in variants})
            if unknown:
                raise BenchError(f"--only-variants: no variant named {', '.join(unknown)}")
            variants = [variant for variant in variants if variant.get("name") in only
                        or variant.get("name") == baseline]
        if boot:
            variants = expand_boot_variants(variants, boot)
        plan = Plan(run_args, variants)
        plan.suite = name
        if boot:
            plan.boot = dict(boot, baseline=baseline)
        names = {variant["name"] for variant in plan.variants}
        for variant, target in baselines.items():
            compare_to[(plan.scene["name"], variant)] = target if target in names else None
        plans.append(plan)
    return plans, compare_to


def render_suite(rows, compare_to):
    """Markdown table of one suite run: every variant against its baseline."""
    by_key = collections.defaultdict(dict)
    for row in rows:
        by_key[row["scene"], row["variant"]][_display(row)] = row
    display, pacing = optional_columns(rows)
    lines = ["| scene | variant | " + ("display | " if display else "") + "fps | vs baseline | p50 ms | p99 ms | "
             + ("shown fps | within 2 ms of p50 | " if pacing else "") + "watts | GPU MHz | limited by | "
             "late fps | valid |",
             "|---|---|" + ("---|" if display else "") + "---:|---:|---:|---:|" + ("---:|---:|" if pacing else "")
             + "---:|---:|---|---:|---|"]
    for row in rows:
        key = (row["scene"], row["variant"])
        target = compare_to.get(key)
        candidates = by_key.get((key[0], target), {})
        base = candidates.get(_display(row))
        delta = "-"
        if target == key[1]:
            delta = "baseline"
        elif candidates and not base:
            delta = f"not compared: {target} ran {', '.join(candidates)}"
        elif base and not _output_size(row):
            delta = "not compared: output size unknown"
        elif base and (change := _fps_change(base, row)):
            delta = f"{change} vs {target}"
        lines.append("| " + " | ".join(
            [_cell(key[0]), _cell(key[1])] + ([_display(row)] if display else [])
            + [_cell(row["fps"]), delta, _cell(row["p50_ms"]), _cell(row["p99_ms"])]
            + ([_cell(_pacing(row, "shown_fps")), _pct(_pacing(row, "within_2ms_pct"))] if pacing else [])
            + [_cell(row["watts"]), _cell(row.get("gpu_mhz")), _cell(row.get("gpu_limited_by")),
               _cell((row.get("late") or {}).get("fps")),
               _status(row) + (" (probe)" if row.get("probe") else "")]) + " |")
    lines += ["", "A percentage requires two verified, valid rows at the same display mode and known output size. "
              "Traced variants are compared with the traced baseline: tracing itself costs time."] + (
              [MIXED_NOTE] if display else [])
    return "\n".join(lines) + "\n"


def command_suite(args, defaults, deps=Deps):
    plans, compare_to = load_suite(args.suite, args, defaults)
    code, rows = run_plans(plans, deps, args.dry_run)
    if rows:
        boot = plans[0].boot
        sys.stdout.write("\n" + (render_boot_suite(rows, boot["baseline"]) if boot else render_suite(rows, compare_to)))
    return code


# ---------------------------------------------------------------------------------------------
# Tables

def _cell(value, digits=1):
    if value is None:
        return "-"
    if isinstance(value, bool):
        return "yes" if value else "NO"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value).replace("|", "/")


def _sustained(row):
    sustained = row.get("sustained")
    if not sustained:
        return "-"
    return f"{_cell(sustained.get('fps_min'))} / {_cell(sustained.get('fps_avg'))}"


def _status(row):
    return "yes" if row.get("valid") else "INVALID: " + ", ".join(row.get("invalid_reasons") or ["?"])


def display_mode(row):
    """'fullscreen' or 'windowed'. Rows from before the fullscreen option ran windowed; a row that
    failed before the settings were read says what was asked for."""
    fullscreen = row.get("fullscreen")
    return "fullscreen" if (row.get("fullscreen_requested") if fullscreen is None else fullscreen) else "windowed"


def _output_size(row):
    """The measured output size; older rows may record only the window's client size."""
    size = (row.get("output") or {}).get("size") or row.get("window")
    return size if isinstance(size, str) and re.fullmatch(r"[1-9]\d*x[1-9]\d*", size) else None


def _display(row):
    size = _output_size(row)
    return display_mode(row) + (f" {size}" if size else "")


def _fps_change(a, b):
    if (a.get("valid") is True and b.get("valid") is True and a.get("scene_ok") is True and b.get("scene_ok") is True
            and a.get("fps") and b.get("fps") and _output_size(a) and _display(a) == _display(b)):
        return f"{(b['fps'] / a['fps'] - 1) * 100:+.1f}%"
    return None


def _pacing(row, key):
    return (row.get("pacing") or {}).get(key)


def _on_slot(row):
    slots = _pacing(row, "slots")
    return f"{_pacing(row, 'on_slot')} / {slots}" if slots is not None else "-"


def _pct(value):
    return "-" if value is None else f"{_cell(value)}%"


PACING_COLUMNS = (
    ("shown fps", lambda row: _cell(_pacing(row, "shown_fps"))),
    ("shown interval p50 / p99 ms",
     lambda row: f"{_cell(_pacing(row, 'interval_p50_ms'))} / {_cell(_pacing(row, 'interval_p99_ms'))}"),
    ("within 2 ms of p50", lambda row: _pct(_pacing(row, "within_2ms_pct"))),
    ("on their slot", _on_slot),
)
MIXED_NOTE = ("Windowed and fullscreen runs measure different paths, and the output size changes how many "
              "pixels the presenter handles. Different modes or sizes are listed apart and never compared.")


def optional_columns(rows):
    """(display column shown, pacing columns shown): each only when some row has it, so pages of
    rows without them stay as they were."""
    display = any(display_mode(row) == "fullscreen" for row in rows) or len({_display(row) for row in rows}) > 1
    return display, any(row.get("pacing") for row in rows)


def variant_label(row):
    """The variant's name, with the cache state of a boot suite's row."""
    state = row.get("cache_state")
    return f"{row.get('variant')} ({state})" if state else row.get("variant")


def latest_rows(rows):
    """One row per scene, variant, display mode and output size: the newest valid one, else the
    newest. A boot suite's rows count per cache state."""
    chosen = {}
    for row in sorted(rows, key=lambda item: item.get("timestamp_utc", "")):
        key = (row.get("scene"), variant_label(row), _display(row))
        if key not in chosen or row.get("valid") or not chosen[key].get("valid"):
            chosen[key] = row
    return chosen


def hardware_line(hardware):
    gpus = ", ".join(f"{gpu.get('name')} (driver {gpu.get('driver_version') or '?'})"
                     for gpu in hardware.get("gpus") or [])
    return (f"{hardware.get('cpu')}; {gpus}; {_cell(hardware.get('ram_gb'))} GB RAM; {hardware.get('os')} "
            f"build {hardware.get('os_build') or '?'}; display {hardware.get('display') or '?'}; "
            f"{hardware.get('power_source')}")


def load_rows(path):
    return [row for _, row, error in read_rows(path) if error is None and isinstance(row, dict)]


TABLE_HEADER = ("| scene | variant | fps | p50 ms | p99 ms | sustained min / avg fps | watts | scene_ok | valid | date |\n"
                "|---|---|---:|---:|---:|---:|---:|---|---|---|")
TABLE_COMMAND = "python -I tools/bench/bench.py table tools/bench/results --out tools/bench/RESULTS.md"


def table_header(display=False, pacing=False):
    """TABLE_HEADER, with the display column and the pacing columns when asked for."""
    names = ["scene", "variant"] + (["display"] if display else []) + [
        "fps", "p50 ms", "p99 ms", "sustained min / avg fps", "watts"] + (
        [name for name, _ in PACING_COLUMNS] if pacing else []) + ["scene_ok", "valid", "date"]
    rule = ["---" if name in ("scene", "variant", "display", "scene_ok", "valid", "date") else "---:"
            for name in names]
    return "| " + " | ".join(names) + " |\n|" + "|".join(rule) + "|"


def render_table(directory, every=False):
    lines = ["# Guarana benchmark results", "",
             f"Generated by `{TABLE_COMMAND}`; do not edit by hand.",
             "fps is the median of three windows. Rows marked INVALID (wrong scene, busy machine, error) "
             "are not comparable.", ""]
    machines = {}
    for path in sorted(glob.glob(os.path.join(directory, "*.jsonl"))):
        for row in load_rows(path):
            if not row.get("probe"):  # rows with rejected shaders are experiments, not benchmarks
                machines.setdefault(row.get("machine_id", "?"), []).append(row)
    display, pacing = optional_columns([row for rows in machines.values() for row in rows])
    if display:
        lines[4:4] = ["", MIXED_NOTE]
    if pacing:
        lines[-1:-1] = ["", "The shown-frame columns come from the emulator's own log (`bench.py run --pacing`, "
                        "`EDEN_NX2_SHOWN_STATS`), generated frames included: the last complete 10 s report in "
                        "the copied log; it is not aligned with the measurement windows and can include the steps "
                        "after them. Compare them only with rows that also ran with `--pacing`."]
    header = table_header(display, pacing)
    if not machines:
        lines += ["No rows yet. See [README.md](README.md) for how to add yours.", "", header]
    for machine, rows in sorted(machines.items()):
        rows.sort(key=lambda row: row.get("timestamp_utc", ""))
        lines += [f"## Machine `{machine}`", "", hardware_line(rows[-1].get("hardware") or {}), "", header]
        shown = rows if every else sorted(latest_rows(rows).values(),
                                          key=lambda row: (row.get("scene"), variant_label(row), _display(row)))
        for row in shown:
            lines.append("| " + " | ".join(
                [_cell(row.get("scene")), _cell(variant_label(row))] + ([_display(row)] if display else [])
                + [_cell(row.get("fps")), _cell(row.get("p50_ms")), _cell(row.get("p99_ms")), _sustained(row),
                   _cell(row.get("watts"))] + ([column(row) for _, column in PACING_COLUMNS] if pacing else [])
                + [_cell(row.get("scene_ok")), _status(row), _cell((row.get("timestamp_utc") or "")[:10])]) + " |")
        lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def table_diff(path, text):
    """None when the page at path is text; else a unified diff from the page to text (maybe empty when
    only line ends differ)."""
    with open(path, encoding="utf-8") as file:
        current = file.read()
    if current == text:
        return None
    return list(difflib.unified_diff(current.splitlines(), text.splitlines(), path, "generated", lineterm=""))


def render_compare(path_a, path_b):
    sides = [latest_rows(load_rows(path)) for path in (path_a, path_b)]
    lines = []
    for label, path, rows in zip("AB", (path_a, path_b), sides):
        first = next(iter(rows.values()), {})
        lines.append(f"- **{label}**: `{os.path.basename(path)}`, machine `{first.get('machine_id', '?')}`: "
                     f"{hardware_line(first.get('hardware') or {})}")
    display, pacing = optional_columns(list(sides[0].values()) + list(sides[1].values()))
    lines += ["",
              "| scene | variant | " + ("display | " if display else "") + "fps A | fps B | B vs A | p50 ms A / B | "
              "p99 ms A / B | sustained min / avg fps A | sustained min / avg fps B | watts A / B | "
              + ("shown fps A / B | within 2 ms of p50 A / B | " if pacing else "") + "scene_ok A / B |",
              "|---|---|" + ("---|" if display else "") + "---:|---:|---:|---:|---:|---:|---:|---:|"
              + ("---:|---:|" if pacing else "") + "---|"]
    for key in sorted(set(sides[0]) | set(sides[1]), key=lambda item: tuple(map(str, item))):
        a, b = sides[0].get(key, {}), sides[1].get(key, {})
        lines.append("| " + " | ".join(
            [_cell(key[0]), _cell(key[1])] + ([key[2]] if display else [])
            + [_cell(a.get("fps")), _cell(b.get("fps")), _fps_change(a, b) or "-",
               f"{_cell(a.get('p50_ms'))} / {_cell(b.get('p50_ms'))}",
               f"{_cell(a.get('p99_ms'))} / {_cell(b.get('p99_ms'))}",
               _sustained(a), _sustained(b), f"{_cell(a.get('watts'))} / {_cell(b.get('watts'))}"]
            + ([f"{_cell(_pacing(a, 'shown_fps'))} / {_cell(_pacing(b, 'shown_fps'))}",
                f"{_pct(_pacing(a, 'within_2ms_pct'))} / {_pct(_pacing(b, 'within_2ms_pct'))}"] if pacing else [])
            + [f"{_cell(a.get('scene_ok'))} / {_cell(b.get('scene_ok'))}"]) + " |")
    lines += ["", "B vs A requires two verified, valid rows at the same display mode and known output size. "
              "Across machines or sessions it describes the readings, not a switch's gain."] + (
              [MIXED_NOTE] if display else [])
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------

def add_run_arguments(run):
    run.add_argument("--exe", required=True, help="the emulator executable")
    run.add_argument("--game", required=True, help="the game file")
    run.add_argument("--sustain-min", type=float, default=0, help="also sample for this many minutes")
    run.add_argument("--sustain-interval-s", type=float, default=10)
    run.add_argument("--out", default=os.path.join(HERE, "results"))
    run.add_argument("--user-dir", help="the emulator's user data folder (default: 'user' next to the "
                     "emulator, else the per-user one)")
    run.add_argument("--ini", help="settings file (default: <user-dir>/config/qt-config.ini)")
    run.add_argument("--base-ini", help="settings file copied over the live one before every variant's "
                     "overrides; the live one is restored afterwards")
    run.add_argument("--save-backup", help="folder copied over <user-dir>/nand/user/save before every variant")
    run.add_argument("--mod-library", help="folder that holds the mod folders variants name")
    run.add_argument("--mod", action="append", metavar="FOLDER", help="mod folder (from --mod-library) for "
                     "variants without a 'mods' list; repeatable. The other mods of the title are moved aside "
                     "for the run")
    run.add_argument("--power-policy", action="append", metavar="NAME=VALUE", help="processor power setting "
                     "(powercfg alias, AC value) applied during every variant and restored afterwards, "
                     "e.g. PERFBOOSTMODE=0; repeatable")
    run.add_argument("--gpu-telemetry", metavar="PS1", help="PowerShell script printing 'HH:MM:SS  MHz  Watts "
                     "... LimitedBy' lines (called with -Samples 3 -IntervalMs 700)")
    run.add_argument("--state-dir", help="where the power restore file is kept (default: per user)")
    run.add_argument("--reference", help="reference screenshot of the scene (default: the scene file's)")
    run.add_argument("--record-reference", action="store_true",
                     help="write this run's screenshot as the reference; its rows are invalid")
    run.add_argument("--port", type=int, default=DEFAULT_PORT)
    run.add_argument("--window-s", type=float, help="measurement window (default: the scene's, else 15)")
    run.add_argument("--settle-s", type=float, help="settle time (default: the scene's, else 50)")
    run.add_argument("--ram-floor-gb", type=float, default=2.0)
    run.add_argument("--max-other-cpu-pct", type=float, default=10.0,
                     help="rows are invalid when other processes use more of the machine's CPU than this")
    run.add_argument("--fullscreen", action="store_true",
                     help="run fullscreen, as the player plays, for variants without a 'fullscreen' field: [UI] "
                     "fullscreen is turned on unless the scene's or the variant's settings turn it off. Rows "
                     "record the mode and output size; table and compare keep them apart from windowed rows")
    run.add_argument("--pacing", action="store_true",
                     help="set EDEN_NX2_SHOWN_STATS=1 for variants without a 'pacing' field and record the "
                     "presenter's pacing from the log in the row. Off by default: with FLUID the switch also adds "
                     "GPU timestamps to every presenter copy, so it changes the measured path")
    run.add_argument("--dry-run", action="store_true", help="print the plan and touch nothing")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="run a scene with one or more variants and append result rows")
    add_run_arguments(run)
    run.add_argument("--scene", required=True, help="scene file (scenes/*.json)")
    run.add_argument("--variant", action="append", help="variant file (variants/*.json); repeatable; "
                     "default: baseline, no overrides")
    suite = commands.add_parser("suite", help="run a suite file's scenes and variants, then print a comparison")
    suite.add_argument("suite", help="suite file (suites/*.json)")
    add_run_arguments(suite)
    suite.add_argument("--scene", help="scene file used instead of the suite's")
    names = lambda text: [item for item in text.split(",") if item]  # noqa: E731
    suite.add_argument("--only-variants", type=names, metavar="A,B",
                       help="run only these of the suite's variants, and its baseline")
    suite.add_argument("--cache-states", type=names, metavar="STATES",
                       help="boot suites: run only these cache states (cold, second, warm)")
    suite.add_argument("--repeats", type=int, help="boot suites: how many times the matrix runs (default: the "
                       "suite's)")
    suite.add_argument("--driver-cache-dir", action="append", metavar="DIR",
                       help="boot suites: a GPU driver's shader cache folder, moved aside like the emulator's so "
                       "that a cold run is cold for the driver too (repeatable; never guessed)")
    suite.add_argument("--cache-work", metavar="DIR", help="boot suites: where the cache backups and what the "
                       "runs left go (default: beside the power restore file)")
    suite.add_argument("--keep-caches", action="store_true",
                       help="boot suites: keep the backups and every run's caches")
    parser.suite_defaults = {key: suite.get_default(key) for key in SUITE_OPTIONS}
    compare = commands.add_parser("compare", help="markdown table of two result files")
    compare.add_argument("a")
    compare.add_argument("b")
    table = commands.add_parser("table", help="markdown results page of every .jsonl in a folder")
    table.add_argument("directory")
    table.add_argument("--all", action="store_true", help="every row, not the latest per variant")
    output = table.add_mutually_exclusive_group()
    output.add_argument("--out", help="write here instead of stdout")
    output.add_argument("--check", metavar="PAGE", help="write nothing; exit 1 when PAGE is not what the "
                        "table would be (RESULTS.md not regenerated, or edited by hand)")
    validate = commands.add_parser("validate", help="schema check of result files")
    validate.add_argument("files", nargs="+", help="result files, or folders whose .jsonl files are checked")
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        if args.command == "run":
            return command_run(args)
        if args.command == "suite":
            return command_suite(args, parser.suite_defaults)
        if args.command == "compare":
            sys.stdout.write(render_compare(args.a, args.b))
        elif args.command == "table":
            text = render_table(args.directory, args.all)
            if args.check:
                diff = table_diff(args.check, text)
                if diff is not None:
                    print(f"{args.check} is stale: it is not what `table` generates from {args.directory}. "
                          f"Regenerate it with `{TABLE_COMMAND}` and commit it with the rows.", file=sys.stderr)
                    print("\n".join(diff[:80] + (["..."] if len(diff) > 80 else [])), file=sys.stderr)
                    return 1
                print(f"{args.check} matches the rows in {args.directory}")
            elif args.out:
                with open(args.out, "w", encoding="utf-8", newline="\n") as file:
                    file.write(text)
            else:
                sys.stdout.write(text)
        elif args.command == "validate":
            failed = False
            files = result_files(args.files)
            if not files:
                print(f"no result files in {', '.join(args.files)}")
            for path in files:
                problems, count = validate_file(path)
                failed = failed or bool(problems)
                print("\n".join(problems) if problems else f"{path}: {count} row(s) ok")
            return 1 if failed else 0
    except (BenchError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
