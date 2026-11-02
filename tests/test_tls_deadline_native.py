"""验证实际握手 deadline 包装器的前置 IO 限制、错误和成功清理路径。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TlsDeadlineTest(unittest.TestCase):
    def test_io_policy_and_ownership(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / 'lwip').mkdir()
            (path / 'esp_tls.h').write_text('''#pragma once
struct esp_tls_cfg_server_t {};
struct esp_tls_t {};
''')
            (path / 'lwip/sockets.h').write_text('''#pragma once
#include <stddef.h>
#include <sys/time.h>
#define SOL_SOCKET 1
#define SO_RCVTIMEO 2
#define SO_SNDTIMEO 3
#define SHUT_RDWR 4
extern "C" int setsockopt(int, int, int, const void*, size_t);
extern "C" int shutdown(int, int);
''')
            (path / 'test.cpp').write_text('''#include <assert.h>
#include <vector>
#include "esp_tls.h"
#include "lwip/sockets.h"
struct Call { int option; long seconds; };
static std::vector<Call> calls;
static int failAt = 0, result = 0, realCalls = 0, shutdownCalls = 0;
extern "C" int setsockopt(int fd, int level, int option, const void* value, size_t size) {
 assert(fd == 7 && level == SOL_SOCKET && size == sizeof(timeval));
 calls.push_back({option, static_cast<const timeval*>(value)->tv_sec});
 return static_cast<int>(calls.size()) == failAt ? -1 : 0;
}
extern "C" int shutdown(int fd, int how) {
 assert(fd == 7 && how == SHUT_RDWR); ++shutdownCalls; return 0;
}
extern "C" int __real_esp_tls_server_session_create(esp_tls_cfg_server_t*, int fd, esp_tls_t*) {
 assert(fd == 7 && calls.size() == 2);
 assert(calls[0].option == SO_RCVTIMEO && calls[0].seconds == 1);
 assert(calls[1].option == SO_SNDTIMEO && calls[1].seconds == 1);
 ++realCalls; return result;
}
extern "C" int __wrap_esp_tls_server_session_create(esp_tls_cfg_server_t*, int, esp_tls_t*);
void reset(int fail, int status) { calls.clear(); failAt = fail; result = status; realCalls = shutdownCalls = 0; }
int main() {
 esp_tls_cfg_server_t config; esp_tls_t tls;
 for (int fail : {1, 2}) {
  reset(fail, 0);
  assert(__wrap_esp_tls_server_session_create(&config, 7, &tls) == -1);
  assert(realCalls == 0 && shutdownCalls == 0);
 }
 reset(0, -17);
 assert(__wrap_esp_tls_server_session_create(&config, 7, &tls) == -17);
 assert(calls.size() == 2 && shutdownCalls == 0);
 reset(0, 0);
 assert(__wrap_esp_tls_server_session_create(&config, 7, &tls) == 0);
 assert(realCalls == 1 && calls.size() == 4 && shutdownCalls == 0);
 assert(calls[2].option == SO_RCVTIMEO && calls[2].seconds == 5);
 assert(calls[3].option == SO_SNDTIMEO && calls[3].seconds == 5);
 for (int fail : {3, 4}) {
  reset(fail, 0);
  assert(__wrap_esp_tls_server_session_create(&config, 7, &tls) == 0);
  assert(realCalls == 1 && shutdownCalls == 1);
 }
}
''')
            subprocess.run([compiler, '-std=c++17', '-DMIMIVENT_ENABLE_WIFI=1',
                            '-DMIMIVENT_WIFI_TLS_PROBE=1', '-I', str(path),
                            '-I', str(ROOT / 'tests/native_support'), '-I', str(ROOT),
                            str(ROOT / 'WifiTlsHandshakeDeadline.cpp'), str(path / 'test.cpp'),
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_idle_generation_and_activity(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / 'idle.cpp').write_text('''#include "WifiTlsIdleTracker.h"
#include <assert.h>
int main() {
 WifiTlsIdleTracker tracker;
 WifiTlsIdleTracker::Work work{}, next{};
 assert(!tracker.schedule(20000, 15000, &work));
 tracker.opened(7, 100);
 assert(!tracker.schedule(15099, 15000, &work));
 assert(tracker.schedule(15100, 15000, &work));
 assert(!tracker.schedule(15101, 15000, &next));
 assert(tracker.valid(work, 15101, 15000));
 tracker.touch(7, 15102);
 assert(!tracker.valid(work, 15102, 15000));
 tracker.completed();
 assert(tracker.schedule(30102, 15000, &work));
 tracker.closed(7);
 tracker.opened(7, 0); // 同一个 fd 已属于新连接。
 assert(!tracker.valid(work, 30102, 15000));
 tracker.completed();
 assert(tracker.schedule(30102, 15000, &next));
 assert(tracker.valid(next, 30102, 15000));
 tracker.closed(99); // 其他连接的关闭不能影响本连接。
 assert(tracker.valid(next, 30102, 15000));
 tracker.closed(7);
 assert(!tracker.valid(next, 30102, 15000));
 tracker.completed();
 tracker.opened(9, UINT32_MAX - 1000);
 assert(!tracker.schedule(400, 1500, &work));
 assert(tracker.schedule(500, 1500, &work));
 assert(tracker.valid(work, 600, 1500));
}
''')
            subprocess.run([compiler, '-std=c++17', '-I', str(ROOT),
                            str(path / 'idle.cpp'), '-o', str(path / 'idle')], check=True)
            subprocess.run([str(path / 'idle')], check=True)


if __name__ == '__main__':
    unittest.main()
