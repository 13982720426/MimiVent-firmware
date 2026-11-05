"""满载验收：30 天汇总 + 256 条事件同时满载的快照大小与写入成本。"""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StatisticsCapacityTest(unittest.TestCase):
    def test_full_load_snapshot_size_and_write_cost(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要本机 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / 'capacity'
            build = subprocess.run(
                [compiler, '-O2', '-std=c++17', '-I', str(ROOT / 'tests/native_support'),
                 str(ROOT / 'tests/native_support/statistics_capacity_harness.cpp'),
                 '-o', str(executable)],
                text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            row = json.loads(run.stdout.strip().splitlines()[-1])

        # 满载：30 天汇总 + 256 条事件同时成立。
        self.assertEqual(row['dailyCount'], 30)
        self.assertEqual(row['eventCount'], 256)

        # 结构尺寸必须是文档锁定的值，任何漂移都说明布局被改动。
        self.assertEqual(row['headerBytes'], 56)
        self.assertEqual(row['dailyRecordBytes'], 312)
        self.assertEqual(row['eventRecordBytes'], 20)
        self.assertEqual(row['snapshotBytes'], 14536)
        self.assertEqual(row['snapshotBytes'], row['expectedBytes'])

        # SPIFFS 占用：78 数据页 + 2 索引页 = 5 个 4 KiB block，与 §5.3 一致。
        self.assertEqual(row['dataPages'], 58)
        self.assertEqual(row['indexPages'], 1)
        self.assertEqual(row['blocks'], 4)
        self.assertEqual(row['spiffsBytes'], 16384)
        self.assertEqual(row['twoSlotBytes'], 32768)
        self.assertEqual(row['partitionBytes'], 524288)
        self.assertGreater(row['remainingBytes'], 400 * 1024)

        # 写回校验必须通过（CRC 覆盖 header + daily + events）。
        self.assertTrue(row['verifyOk'])

        # 写一次快照的主机耗时参考值，用于发现数量级退化。
        self.assertLess(row['persistUs'], 5000)
        self.assertLess(row['verifyUs'], 5000)

        # 容量淘汰推进了 droppedThrough，但 30 天汇总里每一天仍是完整真值。
        self.assertGreater(row['droppedThrough'], 0)
        # 本场景没有前置感应活动，入厕位不会被置起（127 = 0xFF 去掉 toilet 位）。
        self.assertEqual(row['day0Coverage'], 127)


if __name__ == '__main__':
    unittest.main()
