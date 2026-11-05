"""事件标志与淘汰语义：2038 边界、未校时淘汰、运行时长补回。"""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StatisticsFlagsTest(unittest.TestCase):
    def test_event_flags_and_head_only_eviction(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要本机 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / 'flags'
            build = subprocess.run(
                [compiler, '-std=c++17', '-I', str(ROOT / 'tests/native_support'),
                 str(ROOT / 'tests/native_support/statistics_flags_harness.cpp'),
                 '-o', str(executable)],
                text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            row = json.loads(run.stdout.strip().splitlines()[-1])

        # 2040 年的时间戳高位为 1，不得被识别成未校时（D41/Y2K38）。
        self.assertEqual(row['lateTimestamp'], 2208988800)
        self.assertFalse(row['lateUnsynced'])
        self.assertTrue(row['lateHasActivity'])

        # 未校时事件用 flags.bit0 标记，unixSeconds 只存 uptime 秒。
        self.assertTrue(row['anyUnsyncedFlag'])
        self.assertLess(row['firstUptime'], 1 << 31)

        # 未校时事件被淘汰不推进 droppedThrough（D38）。
        self.assertEqual(row['droppedAfterFlood'], 0)
        self.assertEqual(row['countAfterFlood'], 256)

        # 明细记录被淘汰后停止运行：必须补回记录，校时后时长仍能归账。
        self.assertTrue(row['fanRecordPresent'])
        self.assertTrue(row['fanRecordUnsynced'])
        self.assertEqual(row['runtimeAfterSync'], 60)


if __name__ == '__main__':
    unittest.main()
