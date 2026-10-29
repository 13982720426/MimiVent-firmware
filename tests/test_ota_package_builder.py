import hashlib
import importlib.util
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

BUILDER_PATH = Path(__file__).parents[1] / "scripts" / "build_ota_package.py"
SPEC = importlib.util.spec_from_file_location("build_ota_package", BUILDER_PATH)
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class OtaPackageBuilderTest(unittest.TestCase):
    def test_signed_package_v2_round_trip_and_signature(self):
        metadata = (
            "MIMIVENT_FIRMWARE_VERSION=0.4.1\0"
            "MIMIVENT_PARTITION_PROFILE=statistics-v2-ota-v1\0"
        ).encode()
        image = metadata + b"A" * 90000
        with tempfile.TemporaryDirectory() as temp_dir:
            key = Path(temp_dir) / "test-key.pem"
            public_key = Path(temp_dir) / "public.pem"
            signature_file = Path(temp_dir) / "signature.bin"
            subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                            "rsa_keygen_bits:3072", "-out", str(key)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-out",
                            str(public_key)], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
            package = BUILDER.build(image, key)
            self.assertEqual(package[:8], b"MVOTA2\0\0")
            self.assertEqual(struct.unpack_from("<H", package, 8)[0], 2)
            self.assertEqual(struct.unpack_from("<I", package, 12)[0], 512)
            self.assertEqual(package[28:60], hashlib.sha256(image).digest())
            signature_file.write_bytes(package[128:512])
            verified = subprocess.run([
                "openssl", "dgst", "-sha256", "-verify", str(public_key),
                "-signature", str(signature_file), "-sigopt", "rsa_padding_mode:pss",
                "-sigopt", "rsa_pss_saltlen:32",
            ], input=image, capture_output=True, check=True)
            self.assertIn(b"Verified OK", verified.stdout)
            signature_file.write_bytes(bytes([package[128] ^ 0x01]) + package[129:512])
            rejected = subprocess.run([
                "openssl", "dgst", "-sha256", "-verify", str(public_key),
                "-signature", str(signature_file), "-sigopt", "rsa_padding_mode:pss",
                "-sigopt", "rsa_pss_saltlen:32",
            ], input=image, capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)

            cursor = 512
            output = bytearray()
            block_count = struct.unpack_from("<I", package, 24)[0]
            for _ in range(block_count):
                raw_length, stored_length, codec = struct.unpack_from("<IIB", package, cursor)
                checksum = struct.unpack_from("<I", package, cursor + 12)[0]
                cursor += 16
                stored = package[cursor:cursor + stored_length]
                raw = zlib.decompress(stored, wbits=-15) if codec else stored
                self.assertEqual(len(raw), raw_length)
                self.assertEqual(zlib.crc32(raw) & 0xFFFFFFFF, checksum)
                output.extend(raw)
                cursor += stored_length
            self.assertEqual(cursor, len(package))
            self.assertEqual(bytes(output), image)


if __name__ == "__main__":
    unittest.main()
