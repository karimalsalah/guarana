# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ubuntu 24.04 with everything scripts/build-linux.sh and scripts/run-tests.sh need, so a GitHub
# Codespace or a local dev container builds and runs the unit tests without sudo: the apt packages
# of .github/workflows/release-binaries.yml, clang and lld for the PGO build, CMake 3.31 or newer
# and Qt 6.8.3. The configure step still downloads the CPM dependencies into .cache/cpm.
FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

# Keep these in step with .github/workflows/release-binaries.yml and build-check.yml.
ARG QT_VERSION=6.8.3
ARG CMAKE_VERSION=4.4.4
ARG PGO_LLVM_VERSION=23
ARG DEBIAN_FRONTEND=noninteractive

# The workflow's "Install Linux dependencies" list as it is, then what the container adds:
# python3-venv for the tool venv, python-is-python3 so that `python` (.mcp.json, the docs) works, gdb.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      autoconf cmake g++ gcc git glslang-tools libglu1-mesa-dev \
      libpulse-dev libtool libudev-dev libxkbcommon-x11-0 \
      mesa-common-dev nasm ninja-build qt6-base-private-dev \
      qt6-charts-dev qt6-tools-dev libavfilter-dev libavcodec-dev \
      libswscale-dev pkg-config zlib1g-dev libva-dev libvdpau-dev \
      spirv-tools spirv-headers libusb-1.0-0-dev \
      libboost-dev libboost-fiber-dev libboost-context-dev \
      libopus-dev libasound2-dev ccache llvm python3-zstandard \
      libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
      libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 \
      ca-certificates curl gdb python3-venv python-is-python3 \
 && rm -rf /var/lib/apt/lists/*

# clang for the PGO build, from the LLVM apt repository as in the workflow: eglantine.profdata is read
# only by an LLVM at least as new as the one that wrote it, and Ubuntu's own clang is older.
RUN curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
 && echo "deb https://apt.llvm.org/noble/ llvm-toolchain-noble-${PGO_LLVM_VERSION} main" > /etc/apt/sources.list.d/llvm.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends "clang-${PGO_LLVM_VERSION}" "lld-${PGO_LLVM_VERSION}" \
 && rm -rf /var/lib/apt/lists/*

# CMake (CMakeLists.txt needs 3.31, Ubuntu 24.04 has 3.28; build-check.yml pip-installs this one) and
# aqtinstall, in one venv.
RUN python3 -m venv /opt/tools \
 && /opt/tools/bin/pip install --no-cache-dir --disable-pip-version-check "cmake==${CMAKE_VERSION}" "aqtinstall>=3.2,<4" \
 && ln -s /opt/tools/bin/cmake /opt/tools/bin/ctest /opt/tools/bin/cpack /usr/local/bin/

# Qt with Qt Charts, as CI installs it (jurplel/install-qt-action runs aqtinstall too).
RUN set -eu; \
    case "$(dpkg --print-architecture)" in \
      arm64) host=linux_arm64; arch=linux_gcc_arm64; dir=gcc_arm64 ;; \
      *)     host=linux;       arch=linux_gcc_64;    dir=gcc_64 ;; \
    esac; \
    cd /tmp; \
    /opt/tools/bin/aqt install-qt "$host" desktop "${QT_VERSION}" "$arch" -m qtcharts --outputdir /opt/qt; \
    ln -s "/opt/qt/${QT_VERSION}/$dir" /opt/qt/current; \
    rm -f /tmp/aqtinstall.log; \
    test -f /opt/qt/current/lib/cmake/Qt6/Qt6Config.cmake

# scripts/build-linux.sh finds Qt through QT_ROOT_DIR, and takes the clang major from PGO_LLVM_VERSION.
ENV QT_ROOT_DIR=/opt/qt/current \
    PGO_LLVM_VERSION=${PGO_LLVM_VERSION} \
    CCACHE_DIR=/home/vscode/.cache/ccache

# devcontainer.json mounts a volume here; a new volume takes this directory's owner.
RUN mkdir -p /home/vscode/.cache/ccache && chown -R vscode:vscode /home/vscode/.cache
