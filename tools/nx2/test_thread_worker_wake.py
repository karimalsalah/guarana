#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import contextlib
import importlib.util
import io
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER = os.path.join(HERE, "thread_worker_wake.py")

# Stands in for the compiler: writes a reproducer that ends as STUB_RESULT says, per mode.
STUB_COMPILER = """#!/bin/sh
[ -n "$STUB_BUILD_FAILS" ] && exit 1
while [ $# -gt 0 ]; do
    [ "$1" = -o ] && out=$2
    shift
done
cat > "$out" <<'EOF'
#!/bin/sh
case "$STUB_RESULT:$1" in
signal:cancel) kill -SEGV $$ ;;
odd:cancel) exit 3 ;;
lost:cancel) echo "lost wake"; exit 1 ;;
unsupported:*) exit 77 ;;
esac
echo woke
EOF
chmod +x "$out"
"""


class Platform(unittest.TestCase):
    # Runs on every platform: the runner sees a non-Linux sys.platform and stops before compiling.
    # CXX is set so that the --cxx default does not call shutil.which under the patched platform.
    def test_unsupported_off_linux(self):
        spec = importlib.util.spec_from_file_location("thread_worker_wake", RUNNER)
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        output = io.StringIO()
        with mock.patch.object(sys, "platform", "win32"), mock.patch.object(sys, "argv", [RUNNER]), \
                mock.patch.dict(os.environ, {"CXX": "c++"}), contextlib.redirect_stdout(output):
            self.assertEqual(runner.main(), 77)
        self.assertIn("unsupported: Linux only", output.getvalue())


@unittest.skipUnless(sys.platform.startswith("linux"),
                     "the runner and its /bin/sh stub compiler are Linux-only")
class RunnerExitCodes(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.compiler = os.path.join(self.work.name, "stub-cxx")
        with open(self.compiler, "w") as file:
            file.write(STUB_COMPILER)
        os.chmod(self.compiler, os.stat(self.compiler).st_mode | stat.S_IEXEC)

    def tearDown(self):
        self.work.cleanup()

    def run_runner(self, *arguments, **environment):
        env = dict(os.environ, **environment)
        return subprocess.run([sys.executable, "-I", RUNNER, "--cxx", self.compiler, *arguments],
                              capture_output=True, text=True, env=env, timeout=120)

    def test_both_cases_wake(self):
        self.assertEqual(self.run_runner(STUB_RESULT="woke").returncode, 0)

    def test_lost_wake(self):
        self.assertEqual(self.run_runner(STUB_RESULT="lost").returncode, 1)

    def test_signal_is_a_harness_failure(self):
        run = self.run_runner(STUB_RESULT="signal")
        self.assertEqual(run.returncode, 2)
        self.assertIn("killed by signal", run.stdout)

    def test_unexpected_exit_code_is_a_harness_failure(self):
        run = self.run_runner(STUB_RESULT="odd")
        self.assertEqual(run.returncode, 2)
        self.assertIn("unexpected exit code 3", run.stdout)

    def test_unsupported(self):
        self.assertEqual(self.run_runner(STUB_RESULT="unsupported").returncode, 77)

    def test_build_failure(self):
        self.assertEqual(self.run_runner(STUB_BUILD_FAILS="1").returncode, 2)

    def test_missing_compiler(self):
        missing = os.path.join(self.work.name, "missing-cxx")
        self.assertEqual(self.run_runner("--cxx", missing, STUB_RESULT="woke").returncode, 2)

    def test_unknown_revision(self):
        run = self.run_runner("--rev", "does-not-exist", STUB_RESULT="woke")
        self.assertEqual(run.returncode, 2)


if __name__ == "__main__":
    unittest.main()
