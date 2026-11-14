#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds Guarana on Linux from a fresh clone. It installs what is missing (the apt packages of
# .github/workflows/release-binaries.yml; clang and lld from apt.llvm.org for the PGO build; CMake
# and Qt 6.8.3 through pip under .deps/), configures build/release with the release flags of the
# linux-x86_64-pgo job (clang with eglantine.profdata) plus -march=x86-64-v3, and builds guarana and
# guarana-cli (lichii and eden-cli in a tree from before the rename). A second run reuses everything and rebuilds only what changed.
#
#   bash scripts/build-linux.sh [options] [-- extra cmake arguments]
#
#   --config Release|Debug  Debug has no PGO and builds in build/debug (default Release)
#   --tests                 also configure the unit tests and build every target
#   --no-pgo                GCC without the profile, as the linux-x86_64 job; build/release-nopgo
#   --jobs N                parallel jobs (default: CPUs, at most one per 2 GB of RAM)
#   --march ARCH|none       -march value (default x86-64-v3 on x86-64, none on aarch64)
#   --build-dir DIR         build directory, relative to the repository root unless absolute
#   --qt-dir DIR            a Qt 6.8 gcc_64 (or gcc_arm64) folder instead of searching
#   --target NAME           target to build, repeatable (default lichii lichii-cmd; all with --tests)
#   --reconfigure           configure from scratch (cmake --fresh); objects are kept
#   --no-install            install nothing; stop and say what is missing
#   --dry-run               check the prerequisites and print the commands; change nothing
#
# CC and CXX choose another compiler. CI tests GCC 13 (--no-pgo) and clang-$PGO_LLVM_VERSION (PGO).
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
deps=$root/.deps
qt_version=6.8.3     # release-binaries.yml and build-check.yml install this Qt
cmake_pin=4.4.4      # build-check.yml pip-installs this CMake; Ubuntu 24.04 has 3.28
min_cmake=3.31       # cmake_minimum_required in CMakeLists.txt

# The workflow's "Install Linux dependencies" list, as it is.
apt_packages=(
  autoconf cmake g++ gcc git glslang-tools libglu1-mesa-dev
  libpulse-dev libtool libudev-dev libxkbcommon-x11-0
  mesa-common-dev nasm ninja-build qt6-base-private-dev
  qt6-charts-dev qt6-tools-dev libavfilter-dev libavcodec-dev
  libswscale-dev pkg-config zlib1g-dev libva-dev libvdpau-dev
  spirv-tools spirv-headers libusb-1.0-0-dev
  libboost-dev libboost-fiber-dev libboost-context-dev
  libopus-dev libasound2-dev ccache llvm python3-zstandard
  libxcb-cursor0 libxcb-icccm4 libxcb-image0
  libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1
)
# Not in the list because CI's runner image has them: the venv for .deps/ and curl for apt.llvm.org.
apt_extra=(python3-venv curl ca-certificates)

step() { printf '==> %s\n' "$*" >&2; }
info() { printf '    %s\n' "$*" >&2; }
warn() { printf '    warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
value() { [ -n "${2-}" ] || die "$1 needs a value"; printf '%s' "$2"; }
# version_ge A B: A >= B
version_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n1)" = "$2" ]; }

config=Release tests=OFF pgo=1 jobs="" march=default build_dir="" qt_dir="" reconfigure=0 install=1 dry_run=0
targets=() extra=()
while [ $# -gt 0 ]; do
  case $1 in
    --config) config=$(value "$1" "${2-}"); shift 2 ;;
    --config=*) config=${1#*=}; shift ;;
    --tests) tests=ON; shift ;;
    --no-pgo) pgo=0; shift ;;
    --jobs|-j) jobs=$(value "$1" "${2-}"); shift 2 ;;
    --jobs=*) jobs=${1#*=}; shift ;;
    --march) march=$(value "$1" "${2-}"); shift 2 ;;
    --march=*) march=${1#*=}; shift ;;
    --build-dir) build_dir=$(value "$1" "${2-}"); shift 2 ;;
    --build-dir=*) build_dir=${1#*=}; shift ;;
    --qt-dir) qt_dir=$(value "$1" "${2-}"); shift 2 ;;
    --qt-dir=*) qt_dir=${1#*=}; shift ;;
    --target) targets+=("$(value "$1" "${2-}")"); shift 2 ;;
    --target=*) targets+=("${1#*=}"); shift ;;
    --reconfigure) reconfigure=1; shift ;;
    --no-install) install=0; shift ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) awk 'NR > 2 && /^#/ { sub(/^# ?/, ""); print; next } NR > 2 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    --) shift; extra+=("$@"); break ;;
    -D*|-U*) extra+=("$1"); shift ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
case $config in Release|Debug) ;; *) die "--config must be Release or Debug, not $config" ;; esac
[ -z "$jobs" ] || [[ $jobs =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive number"

as_root() {
  if [ "$(id -u)" -eq 0 ]; then "$@"
  elif command -v sudo >/dev/null 2>&1; then sudo "$@"
  else die "root is needed for: $*"; fi
}

venv_python() {
  # .deps/venv, created on first use.
  if [ ! -x "$deps/venv/bin/python" ]; then
    command -v python3 >/dev/null 2>&1 || die "python3 is needed to install the missing tools into .deps/"
    mkdir -p "$deps"
    [ -f "$deps/.gitignore" ] || printf '*\n' > "$deps/.gitignore"
    info "creating .deps/venv"
    python3 -m venv "$deps/venv" >&2 || die "python3 -m venv failed: install python3-venv"
  fi
  printf '%s\n' "$deps/venv/bin/python"
}

pip_install() {
  local python
  python=$(venv_python)
  info "pip install $* into .deps/venv"
  "$python" -m pip install --disable-pip-version-check --quiet "$@" >&2
}

tool_version() { { "$1" --version 2>/dev/null || true; } | head -n1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -n1 || true; }

qt_version_of() {
  # The Qt version of a prefix, or nothing when it is not a Qt 6 prefix.
  [ -f "$1/lib/cmake/Qt6/Qt6Config.cmake" ] || return 0
  sed -n 's/^QT_VERSION *= *\([0-9][0-9.]*\).*/\1/p' "$1/mkspecs/qconfig.pri" 2>/dev/null | head -n1 || true
}

default_jobs() {
  local cpus mem_gb by_mem
  cpus=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
  mem_gb=$(awk '/^MemTotal:/ { printf "%d", $2 / 1048576 }' /proc/meminfo 2>/dev/null || echo 0)
  by_mem=$((mem_gb / 2))
  [ "$by_mem" -ge 1 ] || by_mem=1
  if [ "$mem_gb" -gt 0 ] && [ "$by_mem" -lt "$cpus" ]; then echo "$by_mem"; else echo "$cpus"; fi
}

case $(uname -m) in
  x86_64|amd64) qt_host=linux qt_arch=linux_gcc_64 qt_subdir=gcc_64 ;;
  aarch64|arm64) qt_host=linux_arm64 qt_arch=linux_gcc_arm64 qt_subdir=gcc_arm64 ;;
  *) die "unsupported machine $(uname -m): the release builds are for x86_64 and aarch64" ;;
esac
if [ "$march" = default ]; then
  if [ "$qt_subdir" = gcc_64 ]; then march=x86-64-v3; else march=none; fi
fi
use_pgo=0
if [ "$config" = Release ] && [ "$pgo" = 1 ]; then use_pgo=1; fi
# The clang major the PGO job uses, from the workflow so the two cannot drift apart.
pgo_llvm=${PGO_LLVM_VERSION:-$(sed -n 's/^ *PGO_LLVM_VERSION: *"\{0,1\}\([0-9][0-9]*\)"\{0,1\}.*/\1/p' \
  "$root/.github/workflows/release-binaries.yml" 2>/dev/null | head -n1)}
pgo_llvm=${pgo_llvm:-23}
[ -n "$jobs" ] || jobs=$(default_jobs)
if [ -n "$build_dir" ]; then
  case $build_dir in /*) ;; *) build_dir=$root/$build_dir ;; esac
else
  build_name=$(printf '%s' "$config" | tr '[:upper:]' '[:lower:]')
  if [ "$config" = Release ] && [ $use_pgo = 0 ]; then build_name=$build_name-nopgo; fi
  build_dir=$root/build/$build_name
fi

step "System packages"
if command -v dpkg-query >/dev/null 2>&1 && command -v apt-get >/dev/null 2>&1; then
  missing=()
  for pkg in "${apt_packages[@]}" "${apt_extra[@]}"; do
    dpkg-query -W -f='${db:Status-Status}' "$pkg" 2>/dev/null | grep -qx installed || missing+=("$pkg")
  done
  if [ ${#missing[@]} -eq 0 ]; then
    info "all $((${#apt_packages[@]} + ${#apt_extra[@]})) installed"
  elif [ $dry_run = 1 ]; then
    info "would install: ${missing[*]}"
  elif [ $install = 0 ]; then
    die "missing packages: ${missing[*]} (sudo apt-get install ${missing[*]})"
  else
    info "installing: ${missing[*]}"
    as_root apt-get update
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
  fi
else
  warn "not a Debian or Ubuntu system: install the equivalents of these packages (docs/Deps.md): ${apt_packages[*]}"
fi

step "CMake $min_cmake or newer, and Ninja"
cmake_bin="" ninja_bin=""
for candidate in "$(command -v cmake 2>/dev/null || true)" "$deps/venv/bin/cmake"; do
  [ -n "$candidate" ] && [ -x "$candidate" ] || continue
  version=$(tool_version "$candidate")
  if [ -n "$version" ] && version_ge "$version" "$min_cmake"; then cmake_bin=$candidate; break; fi
done
for candidate in "$(command -v ninja 2>/dev/null || true)" "$deps/venv/bin/ninja"; do
  if [ -n "$candidate" ] && [ -x "$candidate" ]; then ninja_bin=$candidate; break; fi
done
wants=()
[ -n "$cmake_bin" ] || wants+=("cmake==$cmake_pin")
[ -n "$ninja_bin" ] || wants+=(ninja)
if [ ${#wants[@]} -gt 0 ]; then
  if [ $dry_run = 1 ]; then info "would pip install ${wants[*]} into .deps/venv"
  elif [ $install = 0 ]; then die "missing: ${wants[*]} (pip install ${wants[*]})"
  else pip_install "${wants[@]}"; fi
  [ -n "$cmake_bin" ] || cmake_bin=$deps/venv/bin/cmake
  [ -n "$ninja_bin" ] || ninja_bin=$deps/venv/bin/ninja
fi
for tool in "$cmake_bin" "$ninja_bin"; do
  if [ -x "$tool" ]; then info "$tool ($(tool_version "$tool"))"; else info "$tool (not installed yet)"; fi
done

step "Qt $qt_version ($qt_subdir)"
qt=""
if [ -n "$qt_dir" ]; then
  version=$(qt_version_of "$qt_dir")
  [ -n "$version" ] || die "--qt-dir $qt_dir is not a Qt 6 folder (no lib/cmake/Qt6)"
  case $version in 6.8.*) ;; *) warn "--qt-dir holds Qt $version; CI builds with Qt $qt_version" ;; esac
  qt=$qt_dir
else
  candidates=()
  [ -z "${QT_ROOT_DIR-}" ] || candidates+=("$QT_ROOT_DIR")
  [ -z "${Qt6_DIR-}" ] || candidates+=("$(dirname "$(dirname "$(dirname "$Qt6_DIR")")")")
  candidates+=("$deps/qt/$qt_version/$qt_subdir")
  # The Qt online installer's default location.
  while IFS= read -r dir; do candidates+=("$dir"); done < <(ls -d "$HOME"/Qt/6.8.*/"$qt_subdir" 2>/dev/null | sort -rV)
  for dir in "${candidates[@]}"; do
    case $(qt_version_of "$dir") in 6.8.*) qt=$dir; break ;; esac
  done
  if [ -z "$qt" ]; then
    qt=$deps/qt/$qt_version/$qt_subdir
    if [ $dry_run = 1 ]; then info "would install Qt $qt_version ($qt_arch, with Qt Charts) into .deps/qt with aqtinstall"
    elif [ $install = 0 ]; then die "no Qt 6.8 found: pass --qt-dir, set QT_ROOT_DIR, or run without --no-install"
    else
      pip_install 'aqtinstall>=3.2,<4'
      info "installing Qt $qt_version ($qt_arch, with Qt Charts, as CI) into .deps/qt"
      # aqtinstall writes aqtinstall.log into the current folder.
      (cd "$deps" && "$deps/venv/bin/python" -m aqt install-qt "$qt_host" desktop "$qt_version" "$qt_arch" \
        -m qtcharts --outputdir "$deps/qt") >&2
      [ -n "$(qt_version_of "$qt")" ] || die "aqtinstall did not produce a Qt in $qt"
    fi
  fi
fi
if [ -n "$(qt_version_of "$qt")" ]; then info "$qt (Qt $(qt_version_of "$qt"))"; else info "$qt (not installed yet)"; fi

step "Compiler"
cflags="" ldflags=""
if [ $use_pgo = 1 ]; then
  cc=${CC:-clang-$pgo_llvm} cxx=${CXX:-clang++-$pgo_llvm}
  if [ -z "${CC-}" ] && { ! command -v "$cc" >/dev/null 2>&1 || [ ! -x "/usr/lib/llvm-$pgo_llvm/bin/ld.lld" ]; }; then
    # eglantine.profdata is read only by an LLVM at least as new as the one that wrote it.
    if [ $dry_run = 1 ]; then info "would install clang-$pgo_llvm and lld-$pgo_llvm from apt.llvm.org"
    elif [ $install = 0 ] || ! command -v apt-get >/dev/null 2>&1; then
      die "the PGO build needs clang-$pgo_llvm and lld-$pgo_llvm: install them, or pass --no-pgo for a GCC build"
    else
      codename=$(. /etc/os-release && printf '%s' "${VERSION_CODENAME-}")
      [ -n "$codename" ] || die "cannot tell this distribution's codename for apt.llvm.org; install clang-$pgo_llvm yourself or pass --no-pgo"
      info "adding apt.llvm.org (llvm-toolchain-$codename-$pgo_llvm) and installing clang-$pgo_llvm and lld-$pgo_llvm"
      curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | as_root tee /etc/apt/trusted.gpg.d/apt.llvm.org.asc >/dev/null
      printf 'deb https://apt.llvm.org/%s/ llvm-toolchain-%s-%s main\n' "$codename" "$codename" "$pgo_llvm" |
        as_root tee /etc/apt/sources.list.d/llvm.list >/dev/null
      as_root apt-get update
      as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "clang-$pgo_llvm" "lld-$pgo_llvm"
    fi
  fi
  if [ -z "${CC-}" ]; then
    # As the workflow: lld-$pgo_llvm keeps ld.lld in /usr/lib/llvm-$pgo_llvm/bin only.
    ldflags="-B/usr/lib/llvm-$pgo_llvm/bin -fuse-ld=lld"
  else
    warn "CC=$CC: the PGO build is tested with clang-$pgo_llvm only"
  fi
  profile=$root/eglantine.profdata
  [ -s "$profile" ] || die "$profile is missing or empty; pass --no-pgo"
  case $profile in *" "*) profile_flag="-fprofile-use='$profile'" ;; *) profile_flag="-fprofile-use=$profile" ;; esac
  cflags="$profile_flag -Wno-backend-plugin -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date"
else
  cc=${CC:-gcc} cxx=${CXX:-g++}
  [ -z "${CC-}${CXX-}" ] || warn "CC/CXX set: CI tests this build with GCC 13"
fi
[ "$march" = none ] || cflags="-march=$march${cflags:+ $cflags}"
if command -v "$cc" >/dev/null 2>&1; then info "$cc ($("$cc" --version 2>/dev/null | head -n1))"
elif [ $dry_run = 1 ]; then info "$cc (not installed yet)"
else die "compiler $cc not found"; fi
if command -v ccache >/dev/null 2>&1; then use_ccache=ON; info "ccache"; else use_ccache=OFF; fi

# The flags of the Linux configure step in release-binaries.yml. Local builds leave the update
# checker off (they have no build-N tag) and add -march.
configure=(
  -S "$root" -B "$build_dir" -G Ninja
  "-DCMAKE_BUILD_TYPE=$config"
  "-DBUILD_TESTING=$tests" "-DLICHII_TESTS=$tests"
  -DLICHII_CMD=ON -DLICHII_ROOM=OFF -DLICHII_ROOM_STANDALONE=OFF
  -DLICHII_USE_BUNDLED_QT=OFF -DENABLE_QT_TRANSLATION=ON
  "-DCMAKE_PREFIX_PATH=$qt" "-DQt6_DIR=$qt/lib/cmake/Qt6"
  -DLICHII_USE_BUNDLED_SDL3=ON
  -DVulkanHeaders_FORCE_BUNDLED=ON -DVulkanUtilityLibraries_FORCE_BUNDLED=ON
  -DENABLE_UPDATE_CHECKER=OFF -DUSE_DISCORD_PRESENCE=OFF
  "-DUSE_CCACHE=$use_ccache"
  "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_CXX_COMPILER=$cxx"
  "-DCMAKE_C_FLAGS=$cflags" "-DCMAKE_CXX_FLAGS=$cflags"
  "-DCMAKE_EXE_LINKER_FLAGS=$ldflags"
  "-DCMAKE_MAKE_PROGRAM=$ninja_bin"
  "${extra[@]}"
)
build=("$cmake_bin" --build "$build_dir" --parallel "$jobs")
if [ ${#targets[@]} -gt 0 ]; then build+=(--target "${targets[@]}")
elif [ "$tests" = OFF ]; then build+=(--target lichii lichii-cmd); fi

describe=$config
[ $use_pgo = 0 ] || describe="$describe, PGO (clang-$pgo_llvm)"
[ $use_pgo = 1 ] || describe="$describe, $cc"
[ "$march" = none ] || describe="$describe, -march=$march"
[ "$tests" = OFF ] || describe="$describe, tests"

stamp=$build_dir/.configure-args
stamp_text=$(printf '%s\n' "${configure[@]}")
previous=""
[ ! -f "$stamp" ] || previous=$(cat "$stamp")
fresh=$reconfigure
if [ -n "$previous" ] && [ "$fresh" = 0 ]; then
  # A different compiler or Qt in an existing cache leaves stale results behind: start over.
  pattern='^-D(CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|CMAKE_PREFIX_PATH|Qt6_DIR)='
  if [ "$(grep -E "$pattern" <<< "$previous" || true)" != "$(grep -E "$pattern" <<< "$stamp_text" || true)" ]; then fresh=1; fi
fi
need_configure=0
if [ "$fresh" = 1 ] || [ "$previous" != "$stamp_text" ] || [ ! -f "$build_dir/build.ninja" ]; then need_configure=1; fi
configure_cmd=("$cmake_bin")
[ "$fresh" = 0 ] || configure_cmd+=(--fresh)
configure_cmd+=("${configure[@]}")

if [ $dry_run = 1 ]; then
  step "Dry run: $describe in $build_dir"
  if [ $need_configure = 1 ]; then printf '%q ' "${configure_cmd[@]}"; printf '\n'; else echo "(configuration unchanged: no configure step)"; fi
  printf '%q ' "${build[@]}"; printf '\n'
  exit 0
fi

if [ $need_configure = 1 ]; then
  step "Configure $build_dir ($describe)"
  [ -d "$root/.cache/cpm" ] || info "the first configure downloads the dependencies into .cache/cpm; that takes a while"
  mkdir -p "$build_dir"
  rm -f "$stamp"
  "${configure_cmd[@]}" || die "configure failed; the CMake error is above ($build_dir/CMakeFiles/CMakeConfigureLog.yaml has the details)"
  printf '%s\n' "$stamp_text" > "$stamp"
else
  step "Configure $build_dir (unchanged, skipped)"
fi

step "Build with $jobs jobs"
"${build[@]}" || die "the build failed; the first error is above"

# clangd finds build/compile_commands.json on its own.
case $build_dir in "$root"/build/*) cp -f "$build_dir/compile_commands.json" "$root/build/compile_commands.json" 2>/dev/null || true ;; esac

printf '\nBuilt (%s) in %d min:\n' "$describe" $((SECONDS / 60))
for exe in guarana guarana-cli lichii eden-cli; do
  if [ -x "$build_dir/bin/$exe" ]; then printf '  %s\n' "$build_dir/bin/$exe"; fi
done
printf 'Tests: bash scripts/run-tests.sh\n'
