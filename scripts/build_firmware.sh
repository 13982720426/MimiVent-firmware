#!/usr/bin/env bash
set -euo pipefail

wifi_mode="${1:-off}"
case "$wifi_mode" in
  off|on|read4k) ;;
  *)
    echo "用法：$0 [off|on|read4k]" >&2
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
if [[ "$wifi_mode" == "read4k" ]]; then
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
  # 设备端 HTTPS 已移除；WiFi 控制与读取只通过本地 MQTT/NAS 网关。
  compile_args+=(--build-property "compiler.cpp.extra_flags=-DMIMIVENT_ENABLE_WIFI=1 -DMIMIVENT_WIFI_READ_CONTROL=1 -DMIMIVENT_OTA_DYNAMIC_WORKSPACE=1 -DMIMIVENT_BLE_PAIRING_PROBE=1")
fi

arduino-cli compile "${compile_args[@]}" "$sketch_dir"
image="$build_dir/MimiVent.ino.bin"
if [[ ! -f "$image" ]]; then
  echo "没有找到固件镜像：$image" >&2
  exit 1
fi
echo "WiFi=$wifi_mode image_bytes=$(wc -c < "$image" | tr -d ' ')"
shasum -a 256 "$image"
