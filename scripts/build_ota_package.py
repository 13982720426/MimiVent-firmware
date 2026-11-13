#!/usr/bin/env python3
"""Build a signed MimiVent .mvota v2 package from an ESP32-C3 image."""

import argparse
import hashlib
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path

MAGIC = b"MVOTA2\0\0"
HEADER_SIZE = 512
# 必须与 OtaManager.cpp 的 kOtaPackageBlockBytes 保持一致。
BLOCK_SIZE = 4 * 1024


def marker(image: bytes, name: str) -> str:
    match = re.search(name.encode() + rb"=([^\x00\r\n]+)", image)
    if not match:
        raise ValueError(f"firmware metadata missing: {name}")
    return match.group(1).decode("ascii")


def sign_image(image: bytes, private_key: Path) -> bytes:
    with tempfile.NamedTemporaryFile() as key_file:
        key_file.write(private_key.read_bytes())
        key_file.flush()
        result = subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", key_file.name,
             "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32"],
            input=image, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        )
    if len(result.stdout) != 384:
        raise ValueError("signing key must be RSA-3072")
    return result.stdout


def build(image: bytes, private_key: Path) -> bytes:
    version = marker(image, "MIMIVENT_FIRMWARE_VERSION").encode("ascii")
    profile = marker(image, "MIMIVENT_PARTITION_PROFILE").encode("ascii")
    if not image or len(version) > 15 or len(profile) > 32:
        raise ValueError("image or firmware metadata exceeds format limits")
    count = (len(image) + BLOCK_SIZE - 1) // BLOCK_SIZE
    header = bytearray(HEADER_SIZE)
    header[:8] = MAGIC
    struct.pack_into("<H", header, 8, 2)
    struct.pack_into("<IIII", header, 12, HEADER_SIZE, len(image), BLOCK_SIZE, count)
    header[28:60] = hashlib.sha256(image).digest()
    header[60] = len(version)
    header[61:61 + len(version)] = version
    header[76] = len(profile)
    header[77:77 + len(profile)] = profile
    struct.pack_into("<I", header, 124, zlib.crc32(header[:124]) & 0xFFFFFFFF)
    header[128:] = sign_image(image, private_key)

    records = bytearray()
    for offset in range(0, len(image), BLOCK_SIZE):
        raw = image[offset:offset + BLOCK_SIZE]
        compressed = zlib.compressobj(level=6, wbits=-15)
        encoded = compressed.compress(raw) + compressed.flush()
        codec = 1 if len(encoded) < len(raw) else 0
        payload = encoded if codec else raw
        records.extend(struct.pack("<IIB3xI", len(raw), len(payload), codec,
                                   zlib.crc32(raw) & 0xFFFFFFFF))
        records.extend(payload)
    return bytes(header + records)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="compiled ESP32-C3 firmware .bin")
    parser.add_argument("--key", type=Path, required=True, help="offline RSA-3072 private key PEM")
    parser.add_argument("-o", "--output", type=Path, help="output signed .mvota path")
    args = parser.parse_args()
    image = args.image.read_bytes()
    package = build(image, args.key)
    output = args.output or args.image.with_suffix(".mvota")
    output.write_bytes(package)
    print(f"image={len(image)} package={len(package)} version={marker(image, 'MIMIVENT_FIRMWARE_VERSION')} output={output}")


if __name__ == "__main__":
    main()
