#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class BuildChannelTests(unittest.TestCase):
    def test_gradle_receives_the_channel_independently_of_the_updater(self):
        for env, flags, nightly, updater in [
            ({}, [], False, False),
            ({"NIGHTLY": "true"}, [], True, False),
            ({}, ["-n"], True, False),
            ({}, ["-r"], False, True),
            ({}, ["-n", "-r"], True, True),
        ]:
            with self.subTest(env=env, flags=flags), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                android = root / "src/android"
                android.mkdir(parents=True)
                (root / "artifacts").mkdir()
                shutil.copyfile(Path(__file__).resolve().parents[3] / ".ci/android/build.sh", root / "build.sh")
                (android / "gradlew").write_text(
                    '#!/bin/sh\nprintf "%s\\n" "$@" > gradle-args\n', encoding="utf-8", newline="\n"
                )
                subprocess.run(
                    ["sh", "build.sh", "-t", "standard", "-b", "Release", *flags],
                    cwd=root, check=True, capture_output=True,
                    env={**os.environ, "ANDROID_KEYSTORE_B64": "", "DEVEL": "true", "NIGHTLY": "false", **env},
                )
                args = (android / "gradle-args").read_text(encoding="utf-8").splitlines()
                self.assertIn(f"-Pnightly={str(nightly).lower()}", args)
                native = next(arg for arg in args if arg.startswith("-PLICHII_ANDROID_ARGS="))
                self.assertEqual("-DENABLE_UPDATE_CHECKER=ON" in native.split(), updater)


if __name__ == "__main__":
    unittest.main()
