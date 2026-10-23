#!/usr/bin/env python3
"""将 HTTPS/Wi-Fi 共用的单页控制台压缩并嵌入 Arduino 固件。"""

from __future__ import annotations

import gzip
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HTML_PATH = ROOT / "web" / "index.html"
HEADER_PATH = ROOT / "WebDashboardAsset.h"
FAVICON_SOURCE = ROOT / "FaviconAsset.h"
FAVICON_PATH = ROOT / "web" / "favicon.png"


def main() -> None:
    html = HTML_PATH.read_bytes()
    compressed = gzip.compress(html, compresslevel=9, mtime=0)
    rows = [", ".join(f"0x{byte:02X}" for byte in compressed[i : i + 16]) for i in range(0, len(compressed), 16)]
    body = ",\n    ".join(rows)
    HEADER_PATH.write_text(
        "#pragma once\n\n"
        "#include <Arduino.h>\n\n"
        "// 由 scripts/build_web_asset.py 从 web/index.html 生成；不要手工编辑。\n"
        "const uint8_t kDashboardGzip[] PROGMEM = {\n    "
        + body
        + "\n};\n"
        "constexpr size_t kDashboardGzipSize = sizeof(kDashboardGzip);\n",
        encoding="utf-8",
    )
    source = FAVICON_SOURCE.read_text(encoding="utf-8")
    start = source.index("{", source.index("kFaviconPng"))
    end = source.index("};", start)
    favicon = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", source[start:end]))
    FAVICON_PATH.write_bytes(favicon)
    print(f"Generated {HEADER_PATH.name}: {len(html)} -> {len(compressed)} bytes")
    print(f"Generated {FAVICON_PATH.relative_to(ROOT)}: {len(favicon)} bytes")


if __name__ == "__main__":
    main()
