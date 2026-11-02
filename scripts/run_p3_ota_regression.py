#!/usr/bin/env python3
"""P3 动态工作区真机回归：复用既有 Agent 上传器，只校验，绝不安装。"""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import time


async def write_data(client, frame, uuid):
    """验证器独立发送数据，避免依赖旧 Agent 中漏导入 OTA_DATA 的实现。"""
    characteristic = client.services.get_characteristic(uuid)
    if characteristic is None:
        raise RuntimeError('缺少 OTA 数据特征')
    maximum = characteristic.max_write_without_response_size
    if not maximum or len(frame) > maximum:
        raise RuntimeError('OTA 数据帧超过平台无响应写上限')
    await client.write_gatt_char(characteristic, frame, response=False)


async def run(args):
    sys.path.insert(0, str(args.agent_root))
    from mimivent_agent.client import BleSession
    from mimivent_agent.ota import OtaService
    from mimivent_agent.protocol import OTA_DATA

    class VerifyOnlySession(BleSession):
        async def write_ota_data(self, frame):
            await write_data(self.connected(), frame, OTA_DATA)

        async def ota_command(self, command, *pos, **kw):
            if command.startswith('INSTALL'):
                raise RuntimeError('诊断脚本禁止提交安装')
            # 既有上传器的超时从写成功后开始；系统配对卡在 write 时也必须有上限。
            async with asyncio.timeout(max(120, kw.get('timeout', 25) + 15)):
                return await super().ota_command(command, *pos, **kw)

    ble = VerifyOnlySession()
    service = OtaService(ble)
    results = []
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2))

    async def wait_transfer(pause=False):
        started = time.monotonic()
        last_progress = -1
        while service.busy:
            if service.sent // 65536 != last_progress:
                last_progress = service.sent // 65536
                print(f'{service.phase}: {service.sent} bytes', flush=True)
            if pause and service.sent >= 65536:
                service._pause_requested = True
            if time.monotonic() - started > 900:
                raise TimeoutError('单次上传超过 15 分钟')
            await asyncio.sleep(0.1)
        await service.task
        print(f'phase={service.phase} error={service.error}', flush=True)
        if service.error:
            raise RuntimeError(service.error)

    try:
        await ble.connect(args.device)
        await asyncio.sleep(3)  # 诊断固件主动请求加密，给系统完成协商的时间。
        initial = (await ble.ota_command('QUERY', ['IDLE', 'PAUSED', 'VERIFIED', 'INSTALLED', 'ERROR'], allow_error=True))['raw']
        if initial != 'IDLE':
            raise RuntimeError('保留已有任务，拒绝覆盖：' + initial)
        if args.signature_failure_package:
            before = (await ble.ota_command('INFO', ['INFO']))['raw']
            await service.call('ota.upload', {'confirm': True, 'filePath': str(args.signature_failure_package)})
            await wait_transfer()
            query = (await ble.ota_command('QUERY', ['PAUSED', 'ERROR', 'VERIFIED'], allow_error=True))['raw']
            fields = query.split('|')
            if (service.phase != 'paused' or not query.endswith('|ERROR:SIGNATURE') or
                    len(fields) < 9 or fields[5] != service.selection['taskId'] or
                    fields[6] != service._device_id):
                raise RuntimeError('坏签名未按预期拒绝：' + query)
            after = (await ble.ota_command('INFO', ['INFO']))['raw']
            if before != after:
                raise RuntimeError('坏签名测试前后运行镜像身份发生变化')
            abandoned = (await ble.ota_command('ABANDON|' + fields[5] + '|9', ['IDLE', 'ERROR'], allow_error=True))['raw']
            if abandoned != 'IDLE':
                raise RuntimeError('坏签名任务清理失败：' + abandoned)
            results.append({'signatureFailure': query, 'runningImageUnchanged': True, 'abandoned': abandoned})
            save()
            service = OtaService(ble)
        for package in args.packages:
            start = time.monotonic()
            await service.call('ota.upload', {'confirm': True, 'filePath': str(package)})
            await wait_transfer(pause=True)
            if service.phase != 'paused':
                raise RuntimeError('未进入暂停阶段')
            await ble.disconnect()
            await asyncio.sleep(2)
            await ble.connect(args.device)
            await asyncio.sleep(3)
            paused = (await ble.ota_command('QUERY', ['PAUSED', 'ERROR'], allow_error=True))['raw']
            if not paused.startswith('PAUSED|') or int(paused.split('|')[1]) <= 0:
                raise RuntimeError('断连后缺少有效续传检查点：' + paused)
            await service.call('ota.resume', {'confirm': True})
            await wait_transfer()
            if service.phase != 'readyToInstall':
                raise RuntimeError('未通过完整校验：' + str(service._last_status))
            verified = (await ble.ota_command('QUERY', ['VERIFIED', 'ERROR'], allow_error=True))['raw']
            fields = verified.split('|')
            if (len(fields) != 5 or fields[0] != 'VERIFIED' or
                    fields[2] != service.selection['imageSha256'] or
                    fields[3] != service.selection['taskId'] or fields[4] != service._device_id):
                raise RuntimeError('校验任务身份不匹配：' + verified)
            abandoned = (await ble.ota_command('ABANDON|' + fields[3] + '|9', ['IDLE', 'ERROR'], allow_error=True))['raw']
            if abandoned != 'IDLE':
                raise RuntimeError('测试任务清理失败：' + abandoned)
            results.append({'package': str(package), 'packageBytes': service.selection['packageLength'],
                            'paused': paused, 'verified': verified, 'abandoned': abandoned,
                            'elapsedSeconds': round(time.monotonic() - start, 2)})
            save()
            service = OtaService(ble)
        print('PASS：暂停、断连续传、完整校验和清理；未提交安装', flush=True)
    except Exception as error:
        results.append({'error': repr(error), 'phase': service.phase, 'sentBytes': service.sent})
        save()
        raise
    finally:
        await service.close()
        await ble.disconnect()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent-root', type=Path, required=True, help='既有 tools/agent 目录')
    parser.add_argument('--device', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--signature-failure-package', type=Path, help='可选：预先上传仅签名损坏的测试包，必须被设备拒绝')
    parser.add_argument('packages', nargs='+', type=Path)
    asyncio.run(run(parser.parse_args()))
