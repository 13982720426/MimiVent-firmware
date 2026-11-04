"""验证感应段边界及小时桶拆分。"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class StatisticsActivityCoreTest(unittest.TestCase):
    def test_grouping_and_bucket_boundaries(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if compiler is None:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'activity'
            subprocess.run([compiler, '-std=c++17', '-I', str(ROOT / 'tests/native_support'), '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/native_support/statistics_activity_core_harness.cpp'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
