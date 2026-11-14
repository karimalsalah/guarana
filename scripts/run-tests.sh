#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and runs the unit tests the way the linux-gcc job of .github/workflows/build-check.yml
# does (GCC, Release, no PGO, BUILD_TESTING and LICHII_TESTS on, update checker on) in build/tests,
# through scripts/build-linux.sh, then runs the Python tool tests. Exits 1 when anything fails.
#
#   bash scripts/run-tests.sh [options] [-- extra cmake arguments]
#
#   --config Release|Debug  (default Release; Debug builds in build/tests-debug)
#   --jobs N                parallel build and test jobs
#   --filter REGEX          only the ctest tests whose name matches (ctest -R)
#   --build-dir DIR         the test build directory (default build/tests)
#   --pgo                   build with the release's clang PGO flags instead of GCC
#   --march ARCH|none       -march for the test build (default none, as CI)
#   --no-build              run the tests of the existing test build
#   --python-only           only the Python tool tests: no C++ toolchain needed
#   --no-python             skip the Python tool tests
#   --no-install            passed to build-linux.sh: install nothing
#   --reconfigure           passed to build-linux.sh: configure from scratch
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
value() { [ -n "${2-}" ] || die "$1 needs a value"; printf '%s' "$2"; }

config=Release jobs="" filter="" build_dir="" pgo=0 march=none no_build=0 python_only=0 no_python=0
pass=() extra=()
while [ $# -gt 0 ]; do
  case $1 in
    --config) config=$(value "$1" "${2-}"); shift 2 ;;
    --jobs|-j) jobs=$(value "$1" "${2-}"); shift 2 ;;
    --filter) filter=$(value "$1" "${2-}"); shift 2 ;;
    --build-dir) build_dir=$(value "$1" "${2-}"); shift 2 ;;
    --pgo) pgo=1; shift ;;
    --march) march=$(value "$1" "${2-}"); shift 2 ;;
    --no-build) no_build=1; shift ;;
    --python-only) python_only=1; shift ;;
    --no-python) no_python=1; shift ;;
    --no-install|--reconfigure) pass+=("$1"); shift ;;
    -h|--help) awk 'NR > 2 && /^#/ { sub(/^# ?/, ""); print; next } NR > 2 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    --) shift; extra+=("$@"); break ;;
    -D*|-U*) extra+=("$1"); shift ;;
    *) die "unknown option $1 (see --help)" ;;
  esac
done
[ $python_only = 0 ] || [ $no_python = 0 ] || die "--python-only and --no-python leave nothing to run"
if [ -z "$build_dir" ]; then
  if [ "$config" = Debug ]; then build_dir=build/tests-debug; else build_dir=build/tests; fi
fi
case $build_dir in /*) ;; *) build_dir=$root/$build_dir ;; esac
[ -n "$jobs" ] || jobs=$(nproc 2>/dev/null || echo 2)

failures=()

if [ $python_only = 0 ]; then
  if [ $no_build = 0 ]; then
    args=(--config "$config" --tests --build-dir "$build_dir" --march "$march" --jobs "$jobs" "${pass[@]}")
    [ $pgo = 1 ] || args+=(--no-pgo)
    # build-check.yml configures its test build with the update checker on.
    bash "$root/scripts/build-linux.sh" "${args[@]}" -- -DENABLE_UPDATE_CHECKER=ON "${extra[@]}" ||
      die "the test build failed; see above"
  fi
  cache=$build_dir/CMakeCache.txt
  [ -f "$cache" ] || die "no build in $build_dir: run without --no-build"
  grep -q '^LICHII_TESTS:BOOL=ON$' "$cache" || die "$build_dir was configured without LICHII_TESTS: run without --no-build"
  cmake_bin=$(sed -n 's/^CMAKE_COMMAND:INTERNAL=//p' "$cache")
  ctest_bin=$(dirname "$cmake_bin")/ctest
  [ -x "$ctest_bin" ] || ctest_bin=ctest
  # Qt test programs need a display; without one, render offscreen (CI's runners have none either).
  if [ -z "${DISPLAY-}${WAYLAND_DISPLAY-}" ]; then export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen}; fi
  ctest_cmd=("$ctest_bin" --test-dir "$build_dir" --output-on-failure --no-tests=error --parallel "$jobs")
  [ -z "$filter" ] || ctest_cmd+=(-R "$filter")
  printf '==> %s\n' "${ctest_cmd[*]}" >&2
  "${ctest_cmd[@]}" || failures+=(ctest)
fi

run_python() {
  # run_python <name> <command...>
  local name=$1
  shift
  printf '==> %s\n' "$*" >&2
  (cd "$root" && "$@") || failures+=("$name")
}

if [ $no_python = 0 ]; then
  python=$(command -v python3 || command -v python || true)
  [ -n "$python" ] || die "python3 not found"
  # The Python tests build-check.yml runs, and the bench tests (AGENTS.md, "Verify a change").
  run_python test_debug_mcp "$python" tools/nx2/test_debug_mcp.py
  run_python test_cache_metrics "$python" tools/nx2/test_cache_metrics.py
  run_python test_cache_bench "$python" tools/nx2/test_cache_bench.py
  run_python test_bench "$python" -I tools/bench/test_bench.py
  cxx=${CXX:-g++}
  if command -v "$cxx" >/dev/null 2>&1; then
    run_python test_thread_worker_wake "$python" tools/nx2/test_thread_worker_wake.py
    run_python thread_worker_wake "$python" -I tools/nx2/thread_worker_wake.py --cxx "$cxx"
    run_python test_switches env CXX="$cxx" "$python" tools/nx2/test_switches.py
  else
    printf '    warning: %s not found: skipping the tests that compile a C++ probe\n' "$cxx" >&2
  fi
fi

printf '\n'
if [ ${#failures[@]} -gt 0 ]; then
  printf 'FAILED: %s\n' "${failures[*]}"
  exit 1
fi
printf 'All tests passed.\n'
