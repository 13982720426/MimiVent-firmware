"""防止设备端 HTTPS 服务和 TLS 服务专属构建依赖重新进入固件。"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DeviceHttpsRemovalTest(unittest.TestCase):
    def test_server_sources_are_retired(self):
        for name in ('WifiHttpsServer.cpp', 'WifiHttpsServer.h',
                     'WifiTlsResourceProbe.cpp', 'WifiTlsResourceProbe.h',
                     'WifiTlsHandshakeDeadline.cpp', 'WifiTlsIdleTracker.h'):
            self.assertFalse((ROOT / name).exists(), name)

    def test_production_sources_do_not_reference_https_server(self):
        files = ('MimiVent.ino', 'WifiMqttTransport.cpp',
                 'WifiProvisioningManager.cpp', 'OtaManager.cpp',
                 'AppConfig.h', 'scripts/build_firmware.sh')
        for name in files:
            source = (ROOT / name).read_text()
            self.assertNotIn('WifiHttpsServer', source, name)
            self.assertNotIn('MIMIVENT_HTTPS_ENABLED', source, name)
            self.assertNotIn('esp_tls_server_session_create', source, name)


if __name__ == '__main__':
    unittest.main()
