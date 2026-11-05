"""schema 4 → 5 迁移：双槽转换、dataEpoch 保持、新指标不伪造。"""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StatisticsSchemaMigrationTest(unittest.TestCase):
    def test_v4_slots_migrate_to_v5_without_losing_history(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要本机 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / 'migration'
            build = subprocess.run(
                [compiler, '-std=c++17', '-I', str(ROOT / 'tests/native_support'),
                 str(ROOT / 'tests/native_support/statistics_migration_harness.cpp'),
                 '-o', str(executable)],
                text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            row = json.loads(run.stdout.strip().splitlines()[-1])

        # 落盘格式升级，但对旧 App 暴露的兼容字段保持 4。
        self.assertEqual(row['schema'], 7)
        self.assertEqual(row['compat'], 4)
        # 旧数据完整保留。
        self.assertEqual(row['daily'], 2)
        self.assertEqual(row['events'], 3)
        self.assertEqual(row['nextEventId'], 99)
        self.assertEqual(row['runtimeBucket6'], 1800)
        self.assertEqual(row['eventKind0'], 2)
        self.assertEqual(row['eventReserved0'], 120)
        # 两个槽位都已经是 schema 5。
        self.assertEqual(row['slotASchema'], 7)
        self.assertEqual(row['slotBSchema'], 7)
        self.assertGreaterEqual(row['generation'], 9)
        # 无法还原的指标一律填 0 并清覆盖位，只保留 runtime + activityCount。
        self.assertEqual(row['coverage'], 0b00000011)
        self.assertEqual(row['segmentCount'], 0)
        self.assertEqual(row['spanSeconds'], 0)
        self.assertEqual(row['toiletSamples'], 0)
        # 旧事件没有结束原因，必须是"未知"而不是"已完成"。
        self.assertEqual(row['eventEndReason0'], 0)
        # 重启不更换数据批次。
        self.assertTrue(row['epochStable'])
        # 旧接口视图保持不变。
        today = row['today']
        self.assertEqual(today['runtimeSeconds'], 120)
        self.assertEqual(today['activityCount'], 3)
        self.assertEqual(today['statisticsSchema'], 4)


if __name__ == '__main__':
    unittest.main()
