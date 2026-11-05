import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class ResizeMigrationTest(unittest.TestCase):
    def test_keeps_newest_and_checks_discarded_bytes(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            target = pathlib.Path(folder) / 'resize'
            subprocess.run([compiler, '-std=c++17', '-I', str(ROOT / 'tests/native_support'),
                            str(ROOT / 'tests/native_support/statistics_resize_migration_harness.cpp'),
                            '-o', str(target)], check=True)
            subprocess.run([str(target)], check=True)
