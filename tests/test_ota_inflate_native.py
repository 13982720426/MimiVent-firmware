"""验证固件使用的 miniz 实现及专属链接符号。"""
import ctypes
import shutil
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class OtaInflateNativeTest(unittest.TestCase):
    def test_namespaced_decoder_and_block_contents(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if compiler is None:
            self.skipTest('需要本地 C 编译器')
        with tempfile.TemporaryDirectory() as folder:
            library = Path(folder) / 'inflate.so'
            subprocess.run([compiler, '-shared', '-fPIC', '-I', str(ROOT),
                            str(ROOT / 'miniz_tinfl.c'), '-o', str(library)],
                           check=True, capture_output=True)
            decoder = ctypes.CDLL(str(library))
            with self.assertRaises(AttributeError):
                getattr(decoder, 'tinfl_decompress')
            inflate = decoder.mimivent_tinfl_decompress_mem_to_mem
            inflate.restype = ctypes.c_size_t
            inflate.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
            blocks = [bytes(16384), bytes(range(256)) * 64,
                      bytes((i * 73 + i // 31) % 256 for i in range(16384)),
                      b'MimiVent OTA' * 17]
            for raw in blocks:
                compressor = zlib.compressobj(wbits=-15)
                compressed = compressor.compress(raw) + compressor.flush()
                output = ctypes.create_string_buffer(len(raw) + 32)
                ctypes.memset(output, 0xA5, len(output))
                length = inflate(output, len(raw), compressed, len(compressed), 0)
                self.assertEqual(length, len(raw))
                self.assertEqual(output.raw[:length], raw)
                self.assertEqual(output.raw[length:], bytes([0xA5]) * 32)


if __name__ == '__main__':
    unittest.main()
