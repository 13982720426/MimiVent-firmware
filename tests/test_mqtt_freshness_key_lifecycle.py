"""MQTT 重连不能使两阶段控制 challenge 失效。"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MqttFreshnessKeyLifecycleTest(unittest.TestCase):
    def test_key_is_generated_at_transport_initialization_not_each_reconnect(self):
        source = (ROOT / "WifiMqttTransport.cpp").read_text()
        connected = source.index("if (eventId == MQTT_EVENT_CONNECTED)")
        next_event = source.index("} else if(eventId", connected)
        connected_handler = source[connected:next_event]
        self.assertNotIn("esp_fill_random(freshnessKey", connected_handler)

        begin = source.index("void wifiMqttBegin()")
        begin_end = source.index("void wifiMqttSuspendForOta()", begin)
        initializer = source[begin:begin_end]
        self.assertIn("esp_fill_random(freshnessKey", initializer)


if __name__ == "__main__":
    unittest.main()
