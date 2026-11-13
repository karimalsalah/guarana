#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs once after the dev container is created: makes the ccache volume writable and prints what to
# run next.
set -euo pipefail

ccache_dir=${CCACHE_DIR:-$HOME/.cache/ccache}
mkdir -p "$ccache_dir" 2>/dev/null || true
if [ ! -w "$ccache_dir" ]; then sudo chown -R "$(id -u):$(id -g)" "$ccache_dir" || true; fi

# CMake reads the commit through git, which refuses a checkout another user owns.
git config --global --add safe.directory "$PWD" 2>/dev/null || true

clang_line=$("clang-${PGO_LLVM_VERSION:-23}" --version 2>/dev/null | head -n1 || true)
cat <<EOF

Guarana dev container
  GCC $(gcc -dumpfullversion 2>/dev/null || gcc -dumpversion), ${clang_line:-no clang for the PGO build}
  $(cmake --version | head -n1), Qt in ${QT_ROOT_DIR:-?}

Next steps:
  bash scripts/run-tests.sh                 unit and tool tests, built as CI's build-check (GCC, Release)
  bash scripts/run-tests.sh --python-only   only the Python tool tests, in seconds
  bash scripts/build-linux.sh               release build (clang PGO, x86-64-v3): build/release/bin/guarana
  bash scripts/build-linux.sh --no-pgo      GCC release build: build/release-nopgo/bin/guarana

The first C++ build takes a while on 4 cores; ccache keeps the next ones short. Games do not run here
(no GPU): use the container to build and test, and a PC or a pull request's test build to play.
Agents: read AGENTS.md first. git config core.hooksPath hooks turns on the whitespace pre-commit hook.
EOF
