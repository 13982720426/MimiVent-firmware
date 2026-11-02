"""验证真机测试驱动的数据发送与平台上限，不需要 BLE 硬件。"""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location('ota_driver', Path(__file__).resolve().parents[1] / 'scripts/run_p3_ota_regression.py')
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


class DriverTest(unittest.IsolatedAsyncioTestCase):
    async def test_writes_without_response_and_rejects_oversized_frame(self):
        characteristic = SimpleNamespace(max_write_without_response_size=244)
        writes = []
        async def write(*args, **kwargs):
            writes.append((args, kwargs))
        client = SimpleNamespace(services=SimpleNamespace(get_characteristic=lambda uuid: characteristic), write_gatt_char=write)
        await driver.write_data(client, bytes(244), 'data-uuid')
        self.assertEqual(writes, [((characteristic, bytes(244)), {'response': False})])
        with self.assertRaises(RuntimeError):
            await driver.write_data(client, bytes(245), 'data-uuid')
        characteristic.max_write_without_response_size = 0
        with self.assertRaises(RuntimeError):
            await driver.write_data(client, bytes(9), 'data-uuid')
        self.assertEqual(len(writes), 1)

    async def test_missing_characteristic_fails_before_write(self):
        client = SimpleNamespace(services=SimpleNamespace(get_characteristic=lambda uuid: None))
        with self.assertRaises(RuntimeError):
            await driver.write_data(client, bytes(9), 'data-uuid')
