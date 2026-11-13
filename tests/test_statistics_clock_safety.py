"""时钟映射不得跨活动跳变，来源优先级与回拨标志稳定。"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StatisticsClockSafetyTest(unittest.TestCase):
    def test_clock_safety_and_source_policy(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("需要本机 C++ 编译器")
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / "clock-safety"
            build = subprocess.run(
                [compiler, "-std=c++17", "-I", str(ROOT / "tests/native_support"),
                 str(ROOT / "tests/native_support/statistics_clock_safety_harness.cpp"),
                 "-o", str(executable)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
