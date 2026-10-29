import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class OtaControlCommandBudgetTest(unittest.TestCase):
    def test_resume_keeps_sequential_erase_mode(self):
        source = (ROOT / "OtaManager.cpp").read_text()
        self.assertIn("esp_ota_resume(targetPartition, OTA_WITH_SEQUENTIAL_WRITES,", source)

    def test_maximum_supported_begin_commands_fit_firmware_limit(self):
        header = (ROOT / "OtaBleAdapter.h").read_text()
        match = re.search(r"kControlBytes\s*=\s*(\d+)", header)
        self.assertIsNotNone(match, "kControlBytes must be declared in OtaBleAdapter.h")
        control_limit = int(match.group(1))

        task_id = "t" * 32
        version = "123456789.123.1"  # 15 字节，语义版本格式的合法上限
        digest = "a" * 64
        maximum_size = "1769472"
        begin2 = (
            f"BEGIN2|{task_id}|{version}|{maximum_size}|{digest}|"
            f"{maximum_size}|{digest}|1"
        )
        begin_delta = (
            f"BEGIN_DELTA|{task_id}|{version}|{maximum_size}|{digest}|"
            f"{maximum_size}|{digest}|{version}|{maximum_size}|{digest}|1"
        )

        self.assertLessEqual(len(begin2), control_limit)
        self.assertLessEqual(len(begin_delta), control_limit)
        self.assertEqual(len(begin2), 203)
        self.assertEqual(len(begin_delta), 297)

    def test_versions_are_limited_to_the_wire_budget(self):
        source = (ROOT / "OtaManager.cpp").read_text()
        self.assertRegex(
            source,
            r"bool isSemanticVersion\(const String& candidate\)\s*\{[\s\S]*?"
            r"candidate\.length\(\)\s*>\s*15",
        )


if __name__ == "__main__":
    unittest.main()
