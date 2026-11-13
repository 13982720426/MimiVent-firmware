import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class WifiCapabilitiesTest(unittest.TestCase):
    def test_https_disabled_build_advertises_mqtt_features_not_http_read(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("需要本机 C++ 编译器")
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "wifi_capabilities"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(ROOT / "tests/native_support/wifi_capabilities_harness.cpp"),
                 "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
