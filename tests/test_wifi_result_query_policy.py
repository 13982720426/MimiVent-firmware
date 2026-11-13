"""检查 MQTT 历史查询只读、受绑定校验且不占用控制租约。"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ResultQueryPolicyTest(unittest.TestCase):
    def test_history_query_does_not_acquire_lease(self):
        source = (ROOT / 'WifiMqttTransport.cpp').read_text()
        start = source.index('void handleQuery(')
        end = source.index('String freshnessProof(', start)
        handler = source[start:end]
        self.assertIn('request.method = "GET"', handler)
        self.assertIn('wifiBindingAuthorize(token, millis())', handler)
        self.assertIn('"/api/events"', handler)
        self.assertNotIn('wifiControlLeaseAcquire', handler)
