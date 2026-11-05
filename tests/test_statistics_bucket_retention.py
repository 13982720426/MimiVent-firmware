"""主机执行真实统计存储实现；仅替换时钟、Flash 文件和 NVS 适配。"""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class StatisticsBucketRetentionTest(unittest.TestCase):
    def test_retention_midnight_restart_and_schema_reset(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler:
            self.skipTest('需要本机 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            executable = Path(folder) / 'stats'
            build = subprocess.run([compiler, '-std=c++17', '-I', str(ROOT/'tests/native_support'),
                                    str(ROOT/'tests/native_support/statistics_harness.cpp'), '-o', str(executable)],
                                   text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            rows = dict((name, json.loads(payload)) for name,payload in
                        (line.split('\t',1) for line in run.stdout.splitlines()))
            for name,expected in [('today',5460),('yesterday',1740),('threeDays',7200),('sevenDays',7200),('restored',7200),('active_today',5460),('unsynced',60)]:
                self.assertEqual(rows[name]['runtimeSeconds'], expected, name)
                self.assertEqual(sum(rows[name]['bucketSeconds']), expected, name)
            self.assertEqual(rows['today']['bucketSeconds'][:2], [3600,1860])
            self.assertEqual(rows['yesterday']['bucketSeconds'][-1],1740)
            segment = rows['activity_segment']
            self.assertEqual(len(segment['events']),1)
            self.assertEqual(segment['activityBucketSeconds'][23],60)
            self.assertEqual(segment['events'][0]['triggerCount'],3)
            self.assertEqual(segment['events'][0]['durationSeconds'],60)
            self.assertEqual(sum(rows['activity_segment_restored']['activityBucketSeconds']),60)
            self.assertEqual(
                sum(day['runtimeSeconds'] for day in rows['active_threeDays']['days']),
                7200)
            self.assertTrue(rows['events']['eventsTruncated'])
            self.assertEqual(rows['reset']['runtimeSeconds'],0)
