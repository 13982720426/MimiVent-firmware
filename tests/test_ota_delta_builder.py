import hashlib
import importlib.util
import struct
import unittest
import zlib
import subprocess
import tempfile
from pathlib import Path

BUILDER_PATH = Path(__file__).parents[1] / "scripts" / "build_ota_delta.py"
SPEC = importlib.util.spec_from_file_location("build_ota_delta", BUILDER_PATH)
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


def image(version: str, body: bytes) -> bytes:
    markers = (
        f"MIMIVENT_FIRMWARE_VERSION={version}\0"
        "MIMIVENT_PARTITION_PROFILE=statistics-v2-ota-v1\0"
        "MIMIVENT_STATISTICS_SCHEMA=3\0"
        "MIMIVENT_MIN_READABLE_STORAGE_SCHEMA=2\0"
    ).encode()
    return markers + b"\0" * (4096 - len(markers)) + body


class OtaDeltaBuilderTest(unittest.TestCase):
    def test_delta_round_trip_and_copies_unchanged_block(self):
        shared = b"unchanged-code-block" * 256
        base = image("0.4.0", b"A" * 4096 + shared)
        target = image("0.4.1", b"B" * 4096 + shared)
        with tempfile.TemporaryDirectory() as temp_dir:
            key = Path(temp_dir) / "test-key.pem"
            subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                            "rsa_keygen_bits:3072", "-out", str(key)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            public_key = Path(temp_dir) / "public.pem"
            subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-out",
                            str(public_key)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            delta = BUILDER.build(base, target, key)
            signature_file = Path(temp_dir) / "signature.bin"
            signature_file.write_bytes(delta[192:576])
            verified = subprocess.run([
                "openssl", "dgst", "-sha256", "-verify",
                str(public_key), "-signature", str(signature_file),
                "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:32",
            ], input=target, capture_output=True, check=True)
            self.assertIn(b"Verified OK", verified.stdout)

        self.assertEqual(delta[:8], b"MVDLT1\0\0")
        self.assertEqual(struct.unpack_from("<H", delta, 162)[0], 2)
        self.assertEqual(delta[24:56], hashlib.sha256(base).digest())
        self.assertEqual(delta[56:88], hashlib.sha256(target).digest())
        self.assertEqual(struct.unpack_from("<I", delta, 188)[0], zlib.crc32(delta[:188]))

        target_length = struct.unpack_from("<I", delta, 16)[0]
        count = struct.unpack_from("<I", delta, 20)[0]
        cursor = BUILDER.HEADER_SIZE
        output = bytearray()
        record_types = []
        for _ in range(count):
            kind, _, _, output_offset, output_length, source_offset, payload_length, checksum = struct.unpack_from(
                "<BBHIIIII", delta, cursor
            )
            cursor += BUILDER.RECORD_SIZE
            record_types.append(kind)
            self.assertEqual(output_offset, len(output))
            if kind == 0:
                chunk = base[source_offset : source_offset + output_length]
            else:
                payload = delta[cursor : cursor + payload_length]
                cursor += payload_length
                chunk = zlib.decompress(payload, wbits=-15) if kind == 1 else payload
            self.assertEqual(zlib.crc32(chunk) & 0xFFFFFFFF, checksum)
            output.extend(chunk)

        self.assertEqual(cursor, len(delta))
        self.assertEqual(len(output), target_length)
        self.assertEqual(bytes(output), target)
        self.assertIn(0, record_types)


if __name__ == "__main__":
    unittest.main()
