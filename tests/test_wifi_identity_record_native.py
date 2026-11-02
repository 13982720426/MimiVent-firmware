"""身份双槽记录的完整性、半写和未知格式保护。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class IdentityRecordTest(unittest.TestCase):
    def test_corruption_partial_write_and_future_version(self):
        compiler = shutil.which('clang++') or shutil.which('c++')
        if not compiler:
            self.skipTest('需要 C++ 编译器')
        source = r'''
#include "WifiIdentityRecord.h"
#include <assert.h>
using namespace WifiIdentityRecord;
int main() {
 uint8_t a[kMaximum]{}, b[kMaximum]{};
 a[16] = 1; a[kHeader] = 0x30;
 const size_t n = kHeader + 450 + 121;
 assert(seal(a, n, 1, 1, 450, 121));
 memcpy(b, a, n); assert(seal(b, n, 2, 1, 450, 121));
 assert(select(a, n, b, n) == 1);
 for (size_t cut = 0; cut < n; ++cut) assert(select(a, n, b, cut) == 0);
 for (size_t pos = 8; pos < n; ++pos) {
   b[pos] ^= 1; assert(inspect(b, n) == State::kCorrupt);
   assert(select(a, n, b, n) == 0); b[pos] ^= 1;
 }
 put16(b + 4, 2); assert(inspect(b, n) == State::kUnsupported);
 assert(select(a, n, b, n) == -1);
 assert(inspect(nullptr, 0) == State::kCorrupt);
 assert(!seal(a, n, 0, 1, 450, 121));
 assert(!seal(a, n, 1, 0, 450, 121));
 assert(!seal(a, n, 1, 1, 1025, 121));
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.cpp').write_text(source)
            subprocess.run([compiler, '-std=c++11', '-fsanitize=address,undefined',
                            '-I', str(ROOT), str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
