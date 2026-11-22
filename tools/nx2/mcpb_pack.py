#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Packs the debug MCP adapter as an MCP Bundle (.mcpb) for the MCP registry.

  python -I tools/nx2/mcpb_pack.py [--version 20.2.0] [--out-dir build/mcpb] [--update-server-json]

The bundle holds manifest.json, server/debug_mcp.py and server/frame_dump_index.py (capture_frame
runs it). The same sources and version give the same bytes on any machine: the entries are stored
in a fixed order with a fixed date, and line endings become LF. It prints the bundle's path and
SHA-256. --update-server-json writes the version, the release download URL and the SHA-256 into
tools/nx2/server.json, the file `mcp-publisher publish` sends. The publish steps are in
docs/agents/README.md. Standard library only.
"""

import argparse
import hashlib
import io
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SERVER_JSON = os.path.join(HERE, "server.json")
NAME = "guarana-debug-mcp"
REPOSITORY = "https://github.com/karimalsalah/guarana"
SOURCES = ("debug_mcp.py", "frame_dump_index.py")
DATE = (2026, 1, 1, 0, 0, 0)
SEMVER = re.compile(r"^\d+\.\d+\.\d+$")


def bundle_name(version):
    return f"{NAME}-{version}.mcpb"


def download_url(version):
    """Where the release workflow's assets live; the registry needs "mcp" in it, and the name has it."""
    return f"{REPOSITORY}/releases/download/v{version}/{bundle_name(version)}"


def tools():
    sys.path.insert(0, HERE)
    try:
        import debug_mcp
    finally:
        sys.path.remove(HERE)
    return [{"name": tool["name"], "description": tool["description"]} for tool in debug_mcp.TOOLS]


def manifest(version):
    script = "${__dirname}/server/debug_mcp.py"
    return {
        "manifest_version": "0.3",
        "name": NAME,
        "display_name": "Guarana emulator debug",
        "version": version,
        "description": "Frame times, screenshots, GPU costs, logs and controller input from a running Guarana "
                       "emulator started with EDEN_NX2_DEBUG_SERVER.",
        "author": {"name": "Guarana contributors", "url": REPOSITORY},
        "repository": {"type": "git", "url": REPOSITORY + ".git"},
        "homepage": "https://karimalsalah.github.io/guarana/agents.html",
        "documentation": f"{REPOSITORY}/blob/HEAD/docs/nx2/debug-mcp.md",
        "support": f"{REPOSITORY}/issues",
        "license": "GPL-3.0-or-later",
        "keywords": ["emulator", "debugging", "frame-pacing", "vulkan", "gpu"],
        "server": {
            "type": "python",
            "entry_point": "server/debug_mcp.py",
            "mcp_config": {
                "command": "python",
                "args": ["-X", "utf8", "-I", script, "--server", "${user_config.server}"],
                "platform_overrides": {"darwin": {"command": "python3"}, "linux": {"command": "python3"}},
            },
        },
        "user_config": {
            "server": {
                "type": "string",
                "title": "Debug server address",
                "description": "host:port of the emulator's EDEN_NX2_DEBUG_SERVER. It listens on loopback only.",
                "default": "127.0.0.1:6545",
                "required": False,
            },
        },
        "tools": tools(),
        "compatibility": {"platforms": ["win32", "linux", "darwin"], "runtimes": {"python": ">=3.10"}},
    }


def build(version):
    """The bundle's bytes."""
    if not SEMVER.match(version):
        raise SystemExit(f"version {version!r} is not X.Y.Z")
    entries = [("manifest.json", (json.dumps(manifest(version), indent=2, ensure_ascii=False) + "\n").encode())]
    for name in SOURCES:
        with open(os.path.join(HERE, name), "rb") as source:
            entries.append(("server/" + name, source.read().replace(b"\r\n", b"\n")))
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_STORED) as bundle:
        for name, data in entries:
            info = zipfile.ZipInfo(name, DATE)
            info.create_system = 3  # the same on every host; ZipInfo picks 0 on Windows
            info.external_attr = 0o100644 << 16
            bundle.writestr(info, data)
    return buffer.getvalue()


def update_server_json(version, digest, path=SERVER_JSON):
    with open(path, encoding="utf-8") as handle:
        server = json.load(handle)
    server["version"] = version
    package = server["packages"][0]
    package["identifier"] = download_url(version)
    package["version"] = version
    package["fileSha256"] = digest
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(server, indent=2, ensure_ascii=False) + "\n")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    with open(os.path.join(ROOT, "VERSION"), encoding="utf-8") as handle:
        current = handle.read().strip()
    parser.add_argument("--version", default=current, help=f"bundle version, X.Y.Z (default {current}, from VERSION)")
    parser.add_argument("--out-dir", default=os.path.join(ROOT, "build", "mcpb"), help="where the .mcpb goes")
    parser.add_argument("--update-server-json", action="store_true", help="write version, URL and SHA-256 into server.json")
    args = parser.parse_args(argv)
    data = build(args.version)
    digest = hashlib.sha256(data).hexdigest()
    os.makedirs(args.out_dir, exist_ok=True)
    out = os.path.join(args.out_dir, bundle_name(args.version))
    with open(out, "wb") as handle:
        handle.write(data)
    print(f"{out}\nsha256 {digest}\nupload as {download_url(args.version)}")
    if args.update_server_json:
        update_server_json(args.version, digest)
        print(f"updated {os.path.relpath(SERVER_JSON, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
