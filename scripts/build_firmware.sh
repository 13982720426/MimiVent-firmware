#!/usr/bin/env bash
set -euo pipefail

wifi_mode="${1:-off}"
case "$wifi_mode" in
  off|on|probe|probe4k|identity4k|pairprobe4k|read4k) ;;
  *)
    echo "用法：$0 [off|on|probe|probe4k|identity4k|pairprobe4k|read4k]" >&2
    exit 2
    ;;
esac

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
temp_root="$(mktemp -d "${TMPDIR:-/tmp}/mimivent-build.XXXXXX")"
sketch_dir="$temp_root/MimiVent"
build_dir="$repo_dir/build/firmware-wifi-$wifi_mode"
mkdir -p "$sketch_dir" "$repo_dir/build"
trap 'rm -rf "$temp_root"' EXIT

rsync -a \
  --exclude='.git' \
  --exclude='.codegraph' \
  --exclude='.DS_Store' \
  --exclude='build' \
  "$repo_dir/" "$sketch_dir/"

fqbn="esp32:esp32:esp32c3:PartitionScheme=huge_app"
if [[ "$wifi_mode" == "probe" || "$wifi_mode" == "probe4k" || "$wifi_mode" == "identity4k" || "$wifi_mode" == "pairprobe4k" || "$wifi_mode" == "read4k" ]]; then
  fqbn="esp32:esp32:esp32c3:PartitionScheme=huge_app,CDCOnBoot=cdc"
fi
compile_args=(
  --fqbn "$fqbn"
  --build-property upload.maximum_size=1769472
  --build-path "$build_dir"
)
if [[ "$wifi_mode" == "on" ]]; then
  compile_args+=(--build-property compiler.cpp.extra_flags=-DMIMIVENT_ENABLE_WIFI=1)
elif [[ "$wifi_mode" == "read4k" ]]; then
  compile_args+=(--build-property "compiler.cpp.extra_flags=-DMIMIVENT_ENABLE_WIFI=1 -DMIMIVENT_WIFI_READ_CONTROL=1 -DMIMIVENT_OTA_DYNAMIC_WORKSPACE=1")
  compile_args+=(--build-property "compiler.c.elf.extra_flags=-Wl,--wrap=esp_tls_server_session_create")
elif [[ "$wifi_mode" == "probe" || "$wifi_mode" == "probe4k" || "$wifi_mode" == "identity4k" || "$wifi_mode" == "pairprobe4k" || "$wifi_mode" == "read4k" ]]; then
  identity_flag=0
  pairing_flag=0
  if [[ "$wifi_mode" == "identity4k" ]]; then identity_flag=1; fi
  if [[ "$wifi_mode" == "pairprobe4k" || "$wifi_mode" == "read4k" ]]; then pairing_flag=1; fi
  compile_args+=(--build-property "compiler.cpp.extra_flags=-DMIMIVENT_ENABLE_WIFI=1 -DMIMIVENT_WIFI_TLS_PROBE=1 -DMIMIVENT_OTA_DYNAMIC_WORKSPACE=1 -DMIMIVENT_WIFI_IDENTITY_PROBE=$identity_flag -DMIMIVENT_BLE_PAIRING_PROBE=$pairing_flag")
  compile_args+=(--build-property "compiler.c.elf.extra_flags=-Wl,--wrap=esp_tls_server_session_create")
fi

if [[ "$wifi_mode" == "probe4k" || "$wifi_mode" == "identity4k" || "$wifi_mode" == "pairprobe4k" || "$wifi_mode" == "read4k" ]]; then
  sdk_overlay="$repo_dir/build/sdk-tls-4096"
  python3 - "$sdk_overlay" <<'PYVERIFY'
import hashlib, json, pathlib, sys
sdk = pathlib.Path(sys.argv[1])
m = json.loads((sdk / 'tls-overlay-manifest.json').read_text())
assert m['contentBytes'] == 4096 and len(m['rebuiltMembers']) == 17
assert hashlib.sha256((sdk / 'lib/libmbedtls_2.a').read_bytes()).hexdigest() == m['overlayLibrarySha256']
for path, expected in m['overlayConfigHashes'].items():
    assert hashlib.sha256((sdk / path).read_bytes()).hexdigest() == expected
for path in ('dio_qspi/include/sdkconfig.h', 'qio_qspi/include/sdkconfig.h'):
    assert '#define CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN 4096' in (sdk / path).read_text()
    assert '#define CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN 4096' in (sdk / path).read_text()
    assert '#define CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN 4096' in (sdk / path).read_text()
PYVERIFY
  compile_args+=(--build-property "compiler.sdk.path=$sdk_overlay")
fi

arduino-cli compile "${compile_args[@]}" "$sketch_dir"
image="$build_dir/MimiVent.ino.bin"
if [[ ! -f "$image" ]]; then
  echo "没有找到固件镜像：$image" >&2
  exit 1
fi
echo "WiFi=$wifi_mode image_bytes=$(wc -c < "$image" | tr -d ' ')"
shasum -a 256 "$image"
