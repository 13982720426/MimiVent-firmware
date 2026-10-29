#!/usr/bin/env python3
"""Build a MimiVent .mvdelta patch from a base and target ESP image."""

import argparse
import hashlib
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

MAGIC = b"MVDLT1\0\0"
HEADER_SIZE = 576
LEGACY_HEADER_SIZE = 192
RECORD_SIZE = 24
BLOCK_SIZE = 4096


def marker(data: bytes, name: str) -> str:
    match = re.search(name.encode() + rb"=([^\x00\r\n]+)", data)
    if not match:
        raise ValueError(f"image metadata missing: {name}")
    return match.group(1).decode("ascii")


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def sign_image(target: bytes, private_key: Path) -> bytes:
    with tempfile.NamedTemporaryFile() as key_file:
        key_file.write(private_key.read_bytes())
        key_file.flush()
        result = subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", key_file.name,
             "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32"],
            input=target, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        )
    if len(result.stdout) != 384:
        raise ValueError("signing key must be RSA-3072")
    return result.stdout


def build(base: bytes, target: bytes, private_key: Path) -> bytes:
    base_version = marker(base, "MIMIVENT_FIRMWARE_VERSION")
    target_version = marker(target, "MIMIVENT_FIRMWARE_VERSION")
    base_profile = marker(base, "MIMIVENT_PARTITION_PROFILE")
    target_profile = marker(target, "MIMIVENT_PARTITION_PROFILE")
    if base_profile != target_profile:
        raise ValueError("base/target partition profiles differ")
    if len(base_version) > 15 or len(target_version) > 15 or len(target_profile) > 32:
        raise ValueError("firmware metadata exceeds the delta header limits")
    statistics_schema = int(marker(target, "MIMIVENT_STATISTICS_SCHEMA"))
    if not 0 <= statistics_schema <= 0xFFFF:
        raise ValueError("statistics schema is outside uint16 range")

    # Index 1 KiB-spaced base windows to retain matches after small insertions.
    source_index: dict[bytes, int] = {}
    for source_offset in range(0, max(1, len(base) - BLOCK_SIZE + 1), 1024):
        chunk = base[source_offset : source_offset + BLOCK_SIZE]
        if len(chunk) == BLOCK_SIZE:
            source_index.setdefault(hashlib.sha256(chunk).digest(), source_offset)

    records = bytearray()
    record_count = (len(target) + BLOCK_SIZE - 1) // BLOCK_SIZE
    for target_offset in range(0, len(target), BLOCK_SIZE):
        chunk = target[target_offset : target_offset + BLOCK_SIZE]
        source_offset = source_index.get(hashlib.sha256(chunk).digest()) if len(chunk) == BLOCK_SIZE else None
        if source_offset is not None and base[source_offset : source_offset + len(chunk)] == chunk:
            record = struct.pack("<BBHIIII I", 0, 0, 0, target_offset, len(chunk), source_offset, 0, crc32(chunk))
            records.extend(record)
            continue
        compressed = zlib.compressobj(level=9, wbits=-15)
        encoded = compressed.compress(chunk) + compressed.flush()
        codec = 1 if len(encoded) < len(chunk) else 2
        payload = encoded if codec == 1 else chunk
        record = struct.pack("<BBHIIII I", codec, 0, 0, target_offset, len(chunk), 0, len(payload), crc32(chunk))
        records.extend(record)
        records.extend(payload)

    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    struct.pack_into("<HHIII", header, 8, 2, HEADER_SIZE, len(base), len(target), record_count)
    header[24:56] = hashlib.sha256(base).digest()
    header[56:88] = hashlib.sha256(target).digest()
    header[88] = len(base_version)
    header[89 : 89 + len(base_version)] = base_version.encode("ascii")
    header[104] = len(target_version)
    header[105 : 105 + len(target_version)] = target_version.encode("ascii")
    header[120] = len(target_profile)
    header[121 : 121 + len(target_profile)] = target_profile.encode("ascii")
    struct.pack_into("<H", header, 153, statistics_schema)
    struct.pack_into("<I", header, 156, BLOCK_SIZE)
    struct.pack_into("<H", header, 160, RECORD_SIZE)
    struct.pack_into("<I", header, 188, crc32(header[:188]))
    header[LEGACY_HEADER_SIZE:] = sign_image(target, private_key)
    return bytes(header + records)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path, help="currently installed .bin image")
    parser.add_argument("target", type=Path, help="target .bin image")
    parser.add_argument("-o", "--output", type=Path, help="output .mvdelta path")
    parser.add_argument("--key", type=Path, required=True, help="offline RSA-3072 private key PEM")
    args = parser.parse_args()
    output = args.output or args.target.with_suffix(".mvdelta")
    delta = build(args.base.read_bytes(), args.target.read_bytes(), args.key)
    output.write_bytes(delta)
    print(f"base={args.base.stat().st_size} target={args.target.stat().st_size} delta={len(delta)} output={output}")


if __name__ == "__main__":
    main()
