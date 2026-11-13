"""验证 MQTT 状态序号只随业务状态变化，并按 RFC 1982 规则比较。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WifiStateRevisionTest(unittest.TestCase):
    def test_state_change_sequence_and_wraparound(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        harness = r'''
#include <cassert>
#include <cstdint>
#include "WifiStateRevision.h"
int main() {
  WifiStateRevision revision;
  assert(revision.sequence() == 0);
  assert(!revision.observe(0x12345678));
  assert(revision.sequence() == 0);
  assert(!revision.observe(0x12345678));
  assert(revision.observe(0x12345679));
  assert(revision.sequence() == 1);
  assert(WifiStateRevision::isNewer(0, UINT32_MAX));
  assert(!WifiStateRevision::isNewer(UINT32_MAX, 0));
  assert(!WifiStateRevision::isNewer(7, 7));
  assert(!WifiStateRevision::isNewer(0x80000000u, 0));
  assert(!WifiStateRevision::isNewer(0, 0x80000000u));
  WifiStateRevision wrapping(UINT32_MAX);
  assert(!wrapping.observe(1));
  assert(wrapping.observe(2));
  assert(wrapping.sequence() == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'test.cpp'
            binary = Path(directory) / 'test'
            source.write_text(harness)
            subprocess.run([
                compiler, '-std=c++17', '-fsanitize=address,undefined',
                '-I', str(ROOT), str(source), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_mqtt_publishes_versioned_full_snapshot_with_heartbeat(self):
        source = (ROOT / 'WifiMqttTransport.cpp').read_text()
        self.assertIn('constexpr uint32_t kPublishIntervalMs = 5000;', source)
        self.assertIn('constexpr uint32_t kStateChangeCheckMs = 250;', source)
        for field in ('"schemaVersion"', '"deviceId"', '"bootId"',
                      '"seq"', '"sampledUptimeMs"', '"state"'):
            self.assertIn(field, source)
        self.assertIn('"fanRunMs"', source)
        self.assertIn('"remainingMs"', source)
        self.assertIn('stateRevision.observe(fingerprint)', source)


if __name__ == '__main__':
    unittest.main()
