#!/usr/bin/env python3
"""按时间追加串口实验日志。打开前清除 DTR/RTS；USB CDC 驱动仍可能触发复位。依赖 pyserial。"""
import argparse
from datetime import datetime
from pathlib import Path
import time
import serial

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='/dev/cu.usbmodem1101')
parser.add_argument('--seconds', type=int, default=240)
parser.add_argument('--output', required=True)
args = parser.parse_args()
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
port = serial.Serial(port=None, baudrate=115200, timeout=0.5)
port.dtr = False
port.rts = False
port.port = args.port
port.open()
try:
    with output.open('a', encoding='utf-8') as log:
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            line = port.readline().decode('utf-8', errors='replace').rstrip()
            if not line:
                continue
            entry = f'{datetime.now().astimezone().isoformat()} {line}'
            log.write(entry + '\n')
            log.flush()
            print(entry, flush=True)
except KeyboardInterrupt:
    pass
finally:
    port.close()
