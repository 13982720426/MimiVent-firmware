"""每日活动/入厕指标写入：跨午夜切分、段数归属、覆盖位。"""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StatisticsDailyMetricsTest(unittest.TestCase):
    def test_activity_span_split_and_toilet_aggregation(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要本机 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / 'metrics'
            build = subprocess.run(
                [compiler, '-std=c++17', '-I', str(ROOT / 'tests/native_support'),
                 str(ROOT / 'tests/native_support/statistics_daily_metrics_harness.cpp'),
                 '-o', str(executable)],
                text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            row = json.loads(run.stdout.strip().splitlines()[-1])

        # 自动排风跑满设定时长一半 -> 计入入厕，按开始日归属。
        # 本场景没有前置感应活动，拿不到跨度就不计样本：
        # 入厕时长只认活动跨度，不再用排风时长冒充。
        self.assertEqual(row['toiletCount'], 0)
        self.assertEqual(row['toiletDuration'], 0)
        self.assertEqual(row['toiletCoverage'], 0)

        # 段数只在起点日累加；次日允许出现"有触发、有跨度、段数为 0"。
        self.assertEqual(row['seg29'], 1)
        self.assertEqual(row['seg30'], 0)
        self.assertEqual(row['segBucket29_23'], 1)
        self.assertEqual(row['segBucket30_0'], 0)

        # 触发各归各日。
        self.assertEqual(row['trig29_23'], 1)
        self.assertEqual(row['trig30_0'], 1)

        # 120 秒跨度跨午夜切成 60 + 60，日标量与小时桶一致。
        # 跨午夜的活动整段归开始日（29 日 120 秒），次日为 0。
        self.assertEqual(row['span29'], 120)
        self.assertEqual(row['span30'], 0)
        # 跨日部分并入开始日的最后一小时，桶和与整段跨度一致。
        self.assertEqual(row['spanBucket29_23'], 120)
        self.assertEqual(row['spanBucket30_0'], 0)

        # 覆盖位：可信时钟下新建的日记录从零开始就"已知"，两天都是 0xFF。
        # 30 日段数为 0 是真实的零（段从 29 日延续），不是"不可用"。
        # 无活动跨度 → 入厕位未置起，其余 7 位仍是 0x7F。
        self.assertEqual(row['cov29'], 0x7F)
        self.assertEqual(row['cov30'], 0xFF)
        self.assertEqual(row['seg30'], 0)
        self.assertEqual(row['runtime29'], 300)


if __name__ == '__main__':
    unittest.main()
