"""验证 WiFi OTA 工作区的分配失败、生命周期及真实解压器的释放/重建。"""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]


class OtaWorkspaceNativeTest(unittest.TestCase):
    def test_allocation_failure_resize_and_release(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        source = r'''
#include "OtaTransferWorkspace.h"
#include <assert.h>
static bool fail = false;
static int allocations = 0, releases = 0, calls = 0, failAt = 0;
void* allocate(size_t count) { ++calls; if (fail || calls == failAt || count > 20 * 1024) return nullptr; ++allocations; return malloc(count); }
void release(void* memory) { ++releases; free(memory); }
int main() {
  {
    OtaTransferWorkspace workspace(allocate, release);
    assert(!workspace.reserve(0));
    assert(!workspace.reserve(SIZE_MAX));
    assert(workspace.reserve(16384));
    assert(workspace.output() && workspace.input() && workspace.output() != workspace.input());
    workspace.input()[16383] = 17;
    workspace.output()[0] = 29;
    assert(workspace.input()[16383] == 17);
    assert(workspace.reserve(16384) && allocations == 2);
    fail = true;
    assert(!workspace.reserve(4096));
    assert(workspace.input() == nullptr && workspace.output() == nullptr);
    assert(workspace.capacity() == 0 && releases == 2);
    fail = false;
    failAt = calls + 2;
    assert(!workspace.reserve(4096));
    assert(workspace.input() == nullptr && workspace.output() == nullptr);
    assert(allocations == releases);
    failAt = 0;
    for (int i = 0; i < 100; ++i) {
      assert(workspace.reserve(4096));
      workspace.input()[4095] = 41;
      workspace.output()[4095] = 53;
      workspace.reset();
      workspace.reset();
    }
    assert(workspace.reserve(16384));
  }
  assert(allocations == releases);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(source)
            subprocess.run([compiler, '-std=c++11', '-I', str(ROOT),
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_dynamic_inflater_round_trip_after_release(self):
        cc = shutil.which('clang') or shutil.which('cc')
        cxx = shutil.which('clang++') or shutil.which('c++')
        if not cc or not cxx:
            self.skipTest('需要 C/C++ 编译器')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'esp_timer.h').write_text('''#include <chrono>
#include <stdint.h>
inline int64_t esp_timer_get_time() {
return std::chrono::duration_cast<std::chrono::microseconds>(
std::chrono::steady_clock::now().time_since_epoch()).count();
}
''')
            (path / 'wrapper.cpp').write_text('''#include "OtaCompression.h"
extern "C" bool decode(const uint8_t* in, size_t len, uint8_t* out, size_t cap, size_t* result) {
bool ok = otaInflateBlock(in, len, out, cap, result);
otaCompressionRelease(); otaCompressionRelease(); return ok;
}
''')
            subprocess.run([cc, '-fPIC', '-DNDEBUG', '-I', str(ROOT), '-c',
                            str(ROOT / 'miniz_tinfl.c'), '-o', str(path / 'miniz.o')], check=True)
            subprocess.run([cxx, '-std=c++11', '-shared', '-fPIC', '-DMIMIVENT_OTA_DYNAMIC_WORKSPACE=1',
                            '-DNDEBUG', '-I', str(path), '-I', str(ROOT),
                            str(ROOT / 'OtaCompression.cpp'), str(path / 'wrapper.cpp'),
                            str(path / 'miniz.o'), '-o', str(path / 'decode.so')], check=True)
            library = ctypes.CDLL(str(path / 'decode.so'))
            decode = library.decode
            decode.restype = ctypes.c_bool
            decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                               ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
            for length in (4096, 16384, 4096, 16384):
                raw = bytes(range(256)) * (length // 256)
                compressor = zlib.compressobj(wbits=-15)
                compressed = compressor.compress(raw) + compressor.flush()
                output = ctypes.create_string_buffer(length + 16)
                ctypes.memset(output, 0xA5, len(output))
                count = ctypes.c_size_t()
                self.assertTrue(decode(compressed, len(compressed), output, length, ctypes.byref(count)))
                self.assertEqual(count.value, length)
                self.assertEqual(output.raw[:length], raw)
                self.assertEqual(output.raw[length:], bytes([0xA5]) * 16)
                self.assertFalse(decode(compressed, len(compressed), output, 100, ctypes.byref(count)))
                self.assertFalse(decode(compressed, 1, output, length, ctypes.byref(count)))


if __name__ == '__main__':
    unittest.main()
