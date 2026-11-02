#!/usr/bin/env python3
"""P3-A 实验：模拟 BLE 只读客户端，同时握手 HTTPS 探针；不发送 token 或业务写命令。"""
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import ssl
import struct
import time
import zlib
from bleak import BleakClient

COMMAND = '6f9b0002-7d5a-4c3b-9a21-2f6d4d494d49'
RESPONSE = '6f9b0003-7d5a-4c3b-9a21-2f6d4d494d49'

async def run(args):
    metrics = {'ble': [], 'tls': [], 'errors': [], 'edgeCases': []}
    pending = None
    expected = 0
    parts = []
    length = crc = None
    pin = None
    def notify(_, frame):
        nonlocal length, crc
        if pending is None or pending.done(): return
        try:
            request, index = struct.unpack_from('<HH', frame)
            if request != expected: return
            if index != len(parts): raise ValueError('fragment sequence mismatch')
            header = 4
            if index == 0:
                version, flags, length, crc = struct.unpack_from('<BBHI', frame, 4)
                if version != 2 or flags or not 1 <= length <= 4096: raise ValueError('invalid response')
                header = 12
            parts.append(bytes(frame[header:]))
            body = b''.join(parts)
            if len(body) > length: raise ValueError('response length exceeded')
            if len(body) == length:
                if zlib.crc32(body) != crc: raise ValueError('response CRC mismatch')
                code, value, tail = body.decode().split('\n')
                if code != '200' or tail: raise ValueError('status response rejected')
                pending.set_result(json.loads(value))
        except Exception as error:
            pending.set_exception(error)

    async with BleakClient(args.device, timeout=20) as client:
        await client.start_notify(RESPONSE, notify)
        started = time.monotonic()
        async def ble_loop():
            nonlocal pending, expected, parts, length, crc
            while time.monotonic() - started < args.seconds:
                expected += 1
                parts = []
                length = crc = None
                pending = asyncio.get_running_loop().create_future()
                begin = time.monotonic()
                try:
                    packet = struct.pack('<H', expected) + b'GET\n/api/status\nproduction\n\n\n0\n0\n'
                    await client.write_gatt_char(COMMAND, packet, response=True)
                    result = await asyncio.wait_for(pending, 6)
                    metrics['ble'].append(round((time.monotonic()-begin)*1000, 2))
                    print('BLE status OK firmware=' + result.get('firmware', ''), flush=True)
                except Exception as error:
                    metrics['errors'].append('BLE: ' + repr(error))
                    print('BLE ERROR ' + repr(error), flush=True)
                    break
                await asyncio.sleep(0.5)
        async def tls_loop():
            nonlocal pin
            await asyncio.sleep(15)
            # 仅访问实验探针，不发送任何凭据；生产代码不得复用此未验证 CA 的上下文。
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            if args.edge_cases:
                try:
                    # TCP 建立但不发 ClientHello，等待服务端握手超时。
                    reader, writer = await asyncio.open_connection(args.host, 8443)
                    try:
                        await asyncio.sleep(12)
                        data = await asyncio.wait_for(reader.read(1024), 3)
                        if data: raise ValueError('unexpected data from stalled handshake')
                    finally:
                        writer.close()
                        try: await writer.wait_closed()
                        except Exception: pass
                    metrics['edgeCases'].append('stalled-handshake-closed')
                    print('EDGE stalled TLS handshake closed', flush=True)
                    # TLS 握手成功但不发送 HTTP 请求，等待 HTTP 接收超时。
                    reader, writer = await asyncio.wait_for(asyncio.open_connection(
                        args.host, 8443, ssl=context, server_hostname='probe'), 15)
                    try:
                        await asyncio.sleep(17)
                        data = await asyncio.wait_for(reader.read(1024), 3)
                        if data and not data.startswith(b'HTTP/1.1 408'):
                            raise ValueError('unexpected idle HTTP response')
                    finally:
                        writer.close()
                        try: await writer.wait_closed()
                        except Exception: pass
                    metrics['edgeCases'].append('idle-http-closed')
                    print('EDGE idle HTTP closed', flush=True)
                    # 超过 4 KiB 的单个应用记录不得导致 OOM 或影响 BLE。
                    reader, writer = await asyncio.wait_for(asyncio.open_connection(
                        args.host, 8443, ssl=context, server_hostname='probe'), 15)
                    try:
                        try:
                            writer.write(b'GET / HTTP/1.1\r\nHost: probe\r\nX-Probe: ' + b'x' * 8192 + b'\r\n\r\n')
                            await writer.drain()
                            data = await asyncio.wait_for(reader.read(2048), 7)
                        except (ssl.SSLError, ConnectionError):
                            data = b''
                        if data and not data.startswith(b'HTTP/1.1 4'):
                            raise ValueError('oversize record not rejected')
                    finally:
                        writer.close()
                        try: await writer.wait_closed()
                        except Exception: pass
                    metrics['edgeCases'].append('oversize-record-rejected')
                    print('EDGE oversize record rejected', flush=True)
                except Exception as error:
                    metrics['errors'].append('EDGE: ' + repr(error))
                    print('EDGE ERROR ' + repr(error), flush=True)
            while time.monotonic() - started < args.seconds:
                writer = None
                begin = time.monotonic()
                try:
                    reader, writer = await asyncio.wait_for(asyncio.open_connection(
                        args.host, 8443, ssl=context, server_hostname='MimiVent-P3-resource-probe'), 15)
                    peer = writer.get_extra_info('ssl_object')
                    digest = hashlib.sha256(peer.getpeercert(binary_form=True)).hexdigest()
                    if pin is None: pin = digest
                    if pin != digest: raise ValueError('probe certificate changed during run')
                    writer.write(b'GET / HTTP/1.1\r\nHost: probe\r\nConnection: close\r\n\r\n')
                    await writer.drain()
                    data = await asyncio.wait_for(reader.read(2048), 6)
                    if b'401 Unauthorized' not in data: raise ValueError('probe unexpectedly accepted request')
                    metrics['tls'].append(round((time.monotonic()-begin)*1000, 2))
                    print('TLS handshake + HTTP 401 OK', flush=True)
                except Exception as error:
                    metrics['errors'].append('TLS: ' + repr(error))
                    print('TLS ERROR ' + repr(error), flush=True)
                finally:
                    if writer:
                        writer.close()
                        try: await writer.wait_closed()
                        except Exception: pass
                await asyncio.sleep(2)
        if args.ble_only:
            await ble_loop()
        else:
            await asyncio.gather(ble_loop(), tls_loop())
    metrics['certificateSha256'] = pin
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(metrics, indent=2))
    print(f"BLE successes={len(metrics['ble'])}, TLS successes={len(metrics['tls'])}, errors={len(metrics['errors'])}", flush=True)
    if metrics['errors'] or (not args.ble_only and not metrics['tls']): raise SystemExit(1)

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--device', required=True)
    parser.add_argument('--host', required=True)
    parser.add_argument('--edge-cases', action='store_true')
    parser.add_argument('--ble-only', action='store_true')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--output', required=True)
    asyncio.run(run(parser.parse_args()))
