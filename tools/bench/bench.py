#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Cross-machine benchmark for Lichii, driven through its debug server (docs/nx2/debug-mcp.md).

  python -I tools/bench/bench.py run --exe <lichii> --game <game> --scene scenes/<scene>.json
  python -I tools/bench/bench.py table results/
  python -I tools/bench/bench.py compare a.jsonl b.jsonl
  python -I tools/bench/bench.py validate results/<date>-<machine>.jsonl

Standard library only; Pillow is used for the scene check when it is installed. See README.md.
"""

import argparse
import base64
import datetime
import getpass
import glob
import hashlib
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
BASELINE_VARIANT = {"name": "baseline", "settings": {}, "env": {}}
IS_WINDOWS = sys.platform == "win32"


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


def ini_apply_text(text, settings):
    """Applies {section: {key: value}} to ini text, keeping its line endings."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(newline)
    for section, values in settings.items():
        lines = ini_set(lines, section, values)
    return newline.join(lines)


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

    def apply(self, settings):
        if not os.path.isfile(self.path):
            raise BenchError(f"settings file not found: {self.path} (run the emulator once, or "
                             "pass --user-dir / --ini)")
        self.recover()
        shutil.copyfile(self.path, self.backup)
        self.active = True
        with open(self.path, "rb") as file:
            text = file.read().decode("utf-8", "surrogateescape")
        text = ini_apply_text(text, settings)
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
    if not isinstance(result, dict) or not result.get("available", True):
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


def stop_process(process, grace_s=10):
    """Closes the emulator's window, then terminates it, then kills it. Always returns."""
    if process is None or process.poll() is not None:
        return
    for action in (lambda: close_windows(process.pid), process.terminate, process.kill):
        try:
            action()
            process.wait(timeout=grace_s)
            return
        except (OSError, subprocess.TimeoutExpired):
            continue


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
# Hardware description

WINDOWS_HW_SCRIPT = r"""
$ErrorActionPreference = 'SilentlyContinue'
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$gpus = @(Get-CimInstance Win32_VideoController | ForEach-Object { @{ name = $_.Name; driver_version = $_.DriverVersion;
    width = $_.CurrentHorizontalResolution; height = $_.CurrentVerticalResolution; refresh_hz = $_.CurrentRefreshRate } })
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
        "gpus": [{"name": gpu.get("name") or "unknown", "driver_version": gpu.get("driver_version")}
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
    gpus = [{"name": line.split(": ", 1)[1].strip(), "driver_version": None}
            for line in _tool_output(["lspci"]).splitlines()
            if re.search(r"VGA compatible|3D controller|Display controller", line) and ": " in line]
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
    """A portable 'user' folder next to the emulator, else the per-user data folder."""
    candidates = [os.path.join(os.path.dirname(exe), "user")]
    if IS_WINDOWS:
        candidates.append(os.path.join(os.environ.get("APPDATA", ""), "lichii"))
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
        candidates.append(os.path.join(base, "lichii"))
    return next((path for path in candidates if os.path.isdir(path)), None)


class Plan:
    """Everything a run needs, resolved from the arguments and the scene file."""

    def __init__(self, args):
        self.exe = expand(args.exe)
        self.game = expand(args.game)
        self.scene_path = expand(args.scene)
        self.scene = load_json(self.scene_path, "scene")
        scene = self.scene
        for key in ("name", "title_id", "input"):
            if key not in scene:
                raise BenchError(f"scene {self.scene_path}: missing {key}")
        self.variants = [load_json(expand(path), "variant") for path in args.variant or []] or [BASELINE_VARIANT]
        for variant in self.variants:
            if not re.fullmatch(r"[A-Za-z0-9._-]+", str(variant.get("name", ""))):
                raise BenchError("every variant needs a name of letters, digits, '.', '_' or '-'")
        saves = scene.get("saves") or {}
        self.user_dir = expand(args.user_dir or saves.get("user_dir")) or find_user_dir(self.exe)
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
        for variant in self.variants:
            lines.append(f"variant {variant['name']}:")
            step = 1
            for text in self.steps(variant):
                lines.append(f"  {step:2}. {text}")
                step += 1
        return "\n".join(lines)

    def steps(self, variant):
        steps = [f"refuse to run if {os.path.basename(self.exe)} is running or port {self.port} is in use"]
        if self.save_dir:
            steps.append("restore the saves from the backup")
        for section, values in self.settings(variant).items():
            steps.append(f"set [{section}] " + ", ".join(f"{key}={ini_format(value) if value is not None else '<default>'}"
                                                         for key, value in values.items()))
        env = {DEBUG_ENV: f"127.0.0.1:{self.port}", **(variant.get("env") or {})}
        steps.append("start the emulator windowed with " + " ".join(f"{key}={value}" for key, value in env.items()))
        steps.append(f"wait for the debug server ({self.server_timeout_s:g} s) and {self.min_presents} presents "
                     f"({self.presents_timeout_s:g} s)")
        for item in self.scene["input"]:
            held = "+".join(item.get("buttons") or []) or ("stick" if item.get("left_stick") else "")
            steps.append(f"wait {item.get('wait_s', 0):g} s" + (f", hold {held} for {item.get('frames', 6)} frames"
                                                                if held else ""))
        steps.append(f"settle {self.settle_s:g} s")
        steps.append(f"screenshot, compare with the reference (limit {self.threshold:g})")
        steps.append(f"measure 3 windows of {self.window_s:g} s, keep the median")
        if self.sustain_min > 0:
            steps.append(f"sustained: one sample every {self.sustain_interval_s:g} s for {self.sustain_min:g} min")
        steps.append("read power, memory and gpu_costs when the server has them")
        steps.append("screenshot again, compare, save it")
        steps.append("stop the emulator, restore the settings file, append the row")
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


def same_title(expected, reported):
    try:
        return int(str(reported), 16) == int(expected, 16)
    except ValueError:
        return True


def run_sustained(client, plan, has_power, meter, deps):
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
        power = read_power(client, has_power)
        if power:
            sample.update(power)
        samples.append(sample)
        if index % 6 == 5:
            deps.log(f"    sustained {sample['t_s']:.0f} s: {sample['fps']} fps")
    fps = [sample["fps"] for sample in samples]
    watts = [sample["package_w"] for sample in samples if "package_w" in sample]
    return {"minutes": plan.sustain_min, "interval_s": plan.sustain_interval_s, "samples": samples,
            "fps_min": min(fps), "fps_avg": round(sum(fps) / len(fps), 2),
            "fps_first": fps[0], "fps_last": fps[-1],
            "watts_avg": round(sum(watts) / len(watts), 2) if watts else None}


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
    }


def check_scene(row, plan, png, reference):
    """Updates scene_ok / scene_diff with one more screenshot; False when the scene is wrong."""
    if reference is None:
        return True
    diff = image_diff(png, reference)
    row["scene_diff"] = diff if row["scene_diff"] is None else max(row["scene_diff"], diff)
    row["scene_ok"] = row["scene_diff"] < plan.threshold
    return row["scene_ok"]


def run_variant(plan, variant, hardware, deps, shot_path):
    """Runs one variant and returns its row. Leaves no emulator and no modified ini behind."""
    row = new_row(plan, variant, hardware)
    reasons = row["invalid_reasons"]
    client = DebugClient(port=plan.port)
    guard = IniGuard(plan.ini)
    process = watchdog = None
    cpu_readings = []
    try:
        image = os.path.basename(plan.exe)
        if deps.running(image):
            raise BenchError(f"{image} is already running: close it first")
        if client.listening():
            raise BenchError(f"something already listens on port {plan.port}")
        SaveGuard(plan.save_backup, plan.save_dir).restore_scene()
        guard.apply(plan.settings(variant))
        env = dict(os.environ)
        env[DEBUG_ENV] = f"127.0.0.1:{plan.port}"
        env.update({key: str(value) for key, value in (variant.get("env") or {}).items()})
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
        if "program_id" in status and not same_title(plan.scene["title_id"], status["program_id"]):
            reasons.append("wrong_title")
        available = {method: client.try_call(method, {}, 20) is not None for method in OPTIONAL_METHODS}

        deps.log("    input script")
        play_input_script(client, plan.scene["input"], deps.sleep)
        deps.sleep(plan.settle_s)

        reference = None
        first_shot = take_screenshot(client, plan.shot_height)
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

        meter = deps.cpu_meter(process.pid)
        windows, powers = [], []
        for _ in range(3):
            meter.start()
            windows.append(measure_window(client, plan.window_s, plan.refresh_hz))
            cpu_readings.append(meter.stop())
            powers.append(read_power(client, available["power"]))
        row.update(median_window(windows))
        row["windows_fps"] = [window["fps"] for window in windows]
        row["power"] = mean_power(powers)
        row["watts"] = (row["power"] or {}).get("package_w")
        deps.log(f"    {row['fps']} fps (windows {row['windows_fps']}), p50 {row['p50_ms']} ms, "
                 f"p99 {row['p99_ms']} ms")
        if plan.sustain_min > 0:
            row["sustained"] = run_sustained(client, plan, available["power"], meter, deps)
            cpu_readings += [{"other_cpu_pct": sample["other_cpu_pct"]} for sample in row["sustained"]["samples"]
                             if sample["other_cpu_pct"] is not None]
        for method in ("memory", "gpu_costs"):
            if available[method]:
                row[method] = client.try_call(method, {}, 30)

        last_shot = take_screenshot(client, plan.shot_height)
        with open(shot_path, "wb") as file:
            file.write(last_shot)
        row["screenshot"] = shot_path
        check_scene(row, plan, last_shot, reference)
    except Exception as error:  # every failure becomes an invalid row
        row["error"] = str(error) or type(error).__name__
    finally:
        if watchdog is not None:
            watchdog.stop()
        try:
            deps.stop(process)
        finally:
            guard.restore()

    if watchdog is not None:
        if watchdog.lowest is not None:
            row["system"]["ram_free_min_gb"] = round(watchdog.lowest, 1)
        if watchdog.fired:
            reasons.append("memory_watchdog")
            row["error"] = f"free RAM fell under {plan.ram_floor_gb} GB: emulator stopped"
    others = [reading["other_cpu_pct"] for reading in cpu_readings if reading]
    if others:
        row["system"]["other_cpu_pct"] = round(sum(others[:3]) / len(others[:3]), 1)
        row["system"]["other_cpu_max_pct"] = max(others)
        emulator = [reading["emulator_cpu_pct"] for reading in cpu_readings[:3] if reading]
        row["system"]["emulator_cpu_pct"] = round(sum(emulator) / len(emulator), 1) if emulator else None
        if max(others) > plan.cpu_limit_pct:
            reasons.append("background_cpu")
    elif row["fps"] is not None:
        row["warnings"].append("background_cpu_not_measured")
    if row["scene_ok"] is False:
        reasons.append("wrong_scene")
    if row["error"] is not None:
        reasons.append("error")
    if hardware.get("power_source") == "battery":
        row["warnings"].append("on_battery")
    if row["fps"] is not None and row["power"] is None:
        row["warnings"].append("no_power_reading")
    row["invalid_reasons"] = list(dict.fromkeys(reasons))
    row["valid"] = not row["invalid_reasons"]
    return row


def command_run(args, deps=Deps):
    plan = Plan(args)
    if args.dry_run:
        print(plan.describe())
        for problem in plan.check():
            print(f"PROBLEM: {problem}")
        return 0
    problems = plan.check()
    if problems:
        raise BenchError("; ".join(problems))
    image = os.path.basename(plan.exe)
    if deps.running(image):
        raise BenchError(f"{image} is already running: close it first (one instance only)")
    if IniGuard(plan.ini).recover():
        deps.log("restored the settings file an interrupted run had left modified")
    deps.log("collecting hardware information")
    hardware = deps.hardware()
    sanitize = make_sanitizer()
    hardware = sanitize(hardware)
    machine = machine_id(hardware)
    stamp = datetime.datetime.now()
    os.makedirs(os.path.join(plan.out, "shots"), exist_ok=True)
    results = os.path.join(plan.out, f"{stamp:%Y-%m-%d}-{machine}.jsonl")
    saves = SaveGuard(plan.save_backup, plan.save_dir)
    all_valid = True
    saves.begin()
    try:
        for variant in plan.variants:
            deps.log(f"variant {variant['name']}")
            shot_name = f"{stamp:%Y-%m-%d}-{machine}-{plan.scene['name']}-{variant['name']}-{datetime.datetime.now():%H%M%S}.png"
            row = run_variant(plan, variant, hardware, deps, os.path.join(plan.out, "shots", shot_name))
            if row["screenshot"]:
                row["screenshot"] = f"shots/{shot_name}"
            row = sanitize(row)
            problems = validate_row(row)
            if problems:
                row["valid"] = False
                row["invalid_reasons"].append("schema")
                deps.log("    schema problems: " + "; ".join(problems))
            with open(results, "a", encoding="utf-8", newline="\n") as file:
                file.write(json.dumps(row, separators=(",", ":")) + "\n")
            all_valid = all_valid and row["valid"]
            deps.log(f"    {'VALID' if row['valid'] else 'INVALID (' + ', '.join(row['invalid_reasons']) + ')'}"
                     + (f": {row['error']}" if row["error"] else ""))
    finally:
        saves.end()
    deps.log(f"rows appended to {results}")
    return 0 if all_valid else 2


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


def latest_rows(rows):
    """One row per (scene, variant): the newest valid one, else the newest."""
    chosen = {}
    for row in sorted(rows, key=lambda item: item.get("timestamp_utc", "")):
        key = (row.get("scene"), row.get("variant"))
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


def render_table(directory, every=False):
    lines = ["# Lichii benchmark results", "",
             "Generated by `python -I tools/bench/bench.py table tools/bench/results --out tools/bench/RESULTS.md`; "
             "do not edit by hand.",
             "fps is the median of three windows. Rows marked INVALID (wrong scene, busy machine, error) "
             "are not comparable.", ""]
    machines = {}
    for path in sorted(glob.glob(os.path.join(directory, "*.jsonl"))):
        for row in load_rows(path):
            machines.setdefault(row.get("machine_id", "?"), []).append(row)
    if not machines:
        lines += ["No rows yet. See [README.md](README.md) for how to add yours.", "", TABLE_HEADER]
    for machine, rows in sorted(machines.items()):
        rows.sort(key=lambda row: row.get("timestamp_utc", ""))
        lines += [f"## Machine `{machine}`", "", hardware_line(rows[-1].get("hardware") or {}), "", TABLE_HEADER]
        shown = rows if every else sorted(latest_rows(rows).values(),
                                          key=lambda row: (row.get("scene"), row.get("variant")))
        for row in shown:
            lines.append("| " + " | ".join([
                _cell(row.get("scene")), _cell(row.get("variant")), _cell(row.get("fps")), _cell(row.get("p50_ms")),
                _cell(row.get("p99_ms")), _sustained(row), _cell(row.get("watts")), _cell(row.get("scene_ok")),
                _status(row), _cell((row.get("timestamp_utc") or "")[:10])]) + " |")
        lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def render_compare(path_a, path_b):
    sides = [latest_rows(load_rows(path)) for path in (path_a, path_b)]
    lines = []
    for label, path, rows in zip("AB", (path_a, path_b), sides):
        first = next(iter(rows.values()), {})
        lines.append(f"- **{label}**: `{os.path.basename(path)}`, machine `{first.get('machine_id', '?')}`: "
                     f"{hardware_line(first.get('hardware') or {})}")
    lines += ["",
              "| scene | variant | fps A | fps B | B vs A | p50 ms A / B | p99 ms A / B | "
              "sustained min / avg fps A | sustained min / avg fps B | watts A / B | scene_ok A / B |",
              "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for key in sorted(set(sides[0]) | set(sides[1]), key=lambda item: tuple(map(str, item))):
        a, b = sides[0].get(key, {}), sides[1].get(key, {})
        delta = "-"
        if a.get("valid") and b.get("valid") and a.get("fps") and b.get("fps"):
            delta = f"{(b['fps'] / a['fps'] - 1) * 100:+.1f}%"
        lines.append("| " + " | ".join([
            _cell(key[0]), _cell(key[1]), _cell(a.get("fps")), _cell(b.get("fps")), delta,
            f"{_cell(a.get('p50_ms'))} / {_cell(b.get('p50_ms'))}",
            f"{_cell(a.get('p99_ms'))} / {_cell(b.get('p99_ms'))}",
            _sustained(a), _sustained(b), f"{_cell(a.get('watts'))} / {_cell(b.get('watts'))}",
            f"{_cell(a.get('scene_ok'))} / {_cell(b.get('scene_ok'))}"]) + " |")
    lines += ["", "B vs A is given only when both rows are valid."]
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run", help="run a scene with one or more variants and append result rows")
    run.add_argument("--exe", required=True, help="the emulator executable")
    run.add_argument("--game", required=True, help="the game file")
    run.add_argument("--scene", required=True, help="scene file (scenes/*.json)")
    run.add_argument("--variant", action="append", help="variant file (variants/*.json); repeatable; "
                     "default: baseline, no overrides")
    run.add_argument("--sustain-min", type=float, default=0, help="also sample for this many minutes")
    run.add_argument("--sustain-interval-s", type=float, default=10)
    run.add_argument("--out", default=os.path.join(HERE, "results"))
    run.add_argument("--user-dir", help="the emulator's user data folder (default: 'user' next to the "
                     "emulator, else the per-user one)")
    run.add_argument("--ini", help="settings file (default: <user-dir>/config/qt-config.ini)")
    run.add_argument("--save-backup", help="folder copied over <user-dir>/nand/user/save before every variant")
    run.add_argument("--reference", help="reference screenshot of the scene (default: the scene file's)")
    run.add_argument("--record-reference", action="store_true",
                     help="write this run's screenshot as the reference; its rows are invalid")
    run.add_argument("--port", type=int, default=DEFAULT_PORT)
    run.add_argument("--window-s", type=float, help="measurement window (default: the scene's, else 15)")
    run.add_argument("--settle-s", type=float, help="settle time (default: the scene's, else 50)")
    run.add_argument("--ram-floor-gb", type=float, default=2.0)
    run.add_argument("--max-other-cpu-pct", type=float, default=10.0,
                     help="rows are invalid when other processes use more of the machine's CPU than this")
    run.add_argument("--dry-run", action="store_true", help="print the plan and touch nothing")
    compare = commands.add_parser("compare", help="markdown table of two result files")
    compare.add_argument("a")
    compare.add_argument("b")
    table = commands.add_parser("table", help="markdown results page of every .jsonl in a folder")
    table.add_argument("directory")
    table.add_argument("--all", action="store_true", help="every row, not the latest per variant")
    table.add_argument("--out", help="write here instead of stdout")
    validate = commands.add_parser("validate", help="schema check of result files")
    validate.add_argument("files", nargs="+")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.command == "run":
            return command_run(args)
        if args.command == "compare":
            sys.stdout.write(render_compare(args.a, args.b))
        elif args.command == "table":
            text = render_table(args.directory, args.all)
            if args.out:
                with open(args.out, "w", encoding="utf-8", newline="\n") as file:
                    file.write(text)
            else:
                sys.stdout.write(text)
        elif args.command == "validate":
            failed = False
            for path in args.files:
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
