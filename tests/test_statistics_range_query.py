"""范围查询：选桶、coveredSeconds、coverage、days、detail、错误码。"""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run_harness():
    compiler = shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise unittest.SkipTest('需要本机 C++ 编译器')
    with tempfile.TemporaryDirectory() as folder:
        executable = Path(folder) / 'range'
        build = subprocess.run(
            [compiler, '-std=c++17', '-I', str(ROOT / 'tests/native_support'),
             str(ROOT / 'tests/native_support/statistics_range_harness.cpp'),
             '-o', str(executable)],
            text=True, capture_output=True)
        assert build.returncode == 0, build.stderr
        run = subprocess.run([str(executable)], text=True, capture_output=True)
        assert run.returncode == 0, run.stderr
    rows = {}
    for line in run.stdout.splitlines():
        name, error, body = line.split('\t')
        rows[name] = (int(error), json.loads(body) if body else None)
    return rows


class StatisticsRangeQueryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rows = run_harness()

    def test_single_day_buckets_and_detail(self):
        error, data = self.rows['single']
        self.assertEqual(error, 0)
        self.assertEqual(data['apiVersion'], 2)
        self.assertEqual(data['storageSchema'], 7)
        self.assertEqual(data['statisticsSchema'], 4)
        self.assertEqual((data['dayCount'], data['bucketHours'], data['bucketCount']), (1, 1, 24))
        self.assertNotIn('days', data)
        last = data['buckets'][23]
        self.assertEqual((last['start'], last['end'], last['cov']),
                         (1793286000, 1793289600, 3600))
        self.assertEqual(last['rt'], 300)
        self.assertEqual(last['span'], 120)
        self.assertEqual(last['trig'], 1)
        self.assertEqual(last['seg'], 1)
        # 无活动跨度 → 入厕位不置起，桶覆盖位是 127。
        self.assertEqual(last['bits'], 127)
        # 无前置感应活动 → 不计入厕样本（旧行为会用排风时长 300 秒冒充）。
        self.assertIsNone(data['summary']['toiletAverageSeconds'])
        self.assertEqual(data['detail']['status'], 'COMPLETE')
        self.assertTrue(data['detail']['complete'])
        self.assertFalse(data['detail']['timeUntrusted'])
        self.assertFalse(data['coverage']['toilet'])

    def test_multi_day_uses_hour_buckets_with_days_array(self):
        error, data = self.rows['twoDays']
        self.assertEqual(error, 0)
        self.assertEqual((data['dayCount'], data['bucketHours'], data['bucketCount']), (2, 2, 24))
        self.assertEqual(len(data['buckets']), 24)
        self.assertEqual(len(data['days']), 2)
        self.assertNotIn('detail', data)
        # 跨午夜的 120 秒跨度被切成两天各 60 秒。
        # 2h 桶下午夜边界落在第 11/12 根柱。
        # 跨午夜整段归开始日：第 11 根柱拿到完整 120 秒，第 12 根没有跨度字段。
        self.assertEqual(data['buckets'][11]['span'], 120)
        self.assertEqual(data['buckets'][12]['span'], 0)
        self.assertEqual(data['buckets'][12]['seg'], 0)
        self.assertEqual(data['days'][1]['act'], 1)
        self.assertEqual(data['days'][1]['seg'], 0)
        self.assertEqual(data['summary']['activitySpanSeconds'], 120)
        self.assertEqual(data['summary']['activitySegmentCount'], 1)

    def test_seven_days_use_day_buckets(self):
        error, data = self.rows['sevenDays']
        self.assertEqual(error, 0)
        self.assertEqual((data['dayCount'], data['bucketHours'], data['bucketCount']), (7, 12, 14))
        self.assertEqual(len(data['buckets']), 14)
        self.assertEqual(len(data['days']), 7)

    def test_partial_bucket_coverage(self):
        error, data = self.rows['todayPartial']
        self.assertEqual(error, 0)
        self.assertEqual(data['bucketCount'], 24)
        self.assertEqual(data['buckets'][23]['cov'], 0)
        self.assertEqual(data['buckets'][22]['cov'], 3600)

    def test_events_endpoint_shares_detail_judgement(self):
        error, data = self.rows['eventsSingle']
        self.assertEqual(error, 0)
        self.assertEqual(data['detailStatus'], 'COMPLETE')
        self.assertTrue(data['detailComplete'])
        self.assertTrue(data['withinRetention'])
        self.assertFalse(data['capacityLimited'])
        self.assertFalse(data['timeUntrusted'])
        self.assertEqual(data['storedEventCount'], 2)
        self.assertEqual(data['eventCapacity'], 256)
        self.assertEqual(data['total'], 2)          # 本页匹配数，不是存储总数
        self.assertFalse(data['eventsTruncated'])
        self.assertEqual(data['oldestAvailableAt'], 1793287800)
        self.assertEqual(data['newestAvailableAt'], 1793289540)

        _error, multi = self.rows['eventsMulti']
        self.assertEqual(multi['detailStatus'], 'MULTI_DAY')
        self.assertFalse(multi['detailComplete'])

    def test_unsynced_returns_pending_instead_of_zero(self):
        error, data = self.rows['unsynced']
        self.assertEqual(error, 0)
        self.assertFalse(data['timeSynced'])
        self.assertEqual(data['buckets'], [])
        for key, value in data['summary'].items():
            self.assertIsNone(value, key)          # 不得输出 0 冒充真实数据
        for key, value in data['coverage'].items():
            self.assertFalse(value, key)
        self.assertEqual(data['pendingUnsynced'], {
            'runtimeSeconds': 60, 'activityCount': 1, 'automaticCount': 0})

    def test_error_codes(self):
        self.assertEqual(self.rows['badTz'][0], 4)          # TIMEZONE_MISMATCH
        self.assertEqual(self.rows['notMidnight'][0], 1)    # INVALID_RANGE
        self.assertEqual(self.rows['tooLarge'][0], 2)       # RANGE_TOO_LARGE
        self.assertEqual(self.rows['outOfRetention'][0], 3) # RANGE_OUT_OF_RETENTION


if __name__ == '__main__':
    unittest.main()
