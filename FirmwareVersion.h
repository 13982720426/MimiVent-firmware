#pragma once

// 遵循语义化版本：MAJOR.MINOR.PATCH。功能兼容的小改动增加 PATCH，新增功能增加 MINOR。
#define FIRMWARE_VERSION_MAJOR 0
#define FIRMWARE_VERSION_MINOR 7
#define FIRMWARE_VERSION_PATCH 0
#define FIRMWARE_VERSION "0.7.0"
#define FIRMWARE_IMAGE_MARKER "MIMIVENT_FIRMWARE_VERSION=" FIRMWARE_VERSION
// OTA App 使用此镜像元数据判断降级后的统计存储兼容性。
// schema 6：StoreHeaderV5 增加了 migratedThroughDayKey（52→56 字节）。
// 布局变更必须升版本号，否则旧布局的快照会被按新布局解析而校验失败。
// schema 7：事件容量 512 → 256，事件数组变小属于落盘布局变更。
// schema 8：DailyRecord 的两个逐小时数组由 uint32_t 收窄为 uint16_t
// （单小时最多 3600 秒，uint16 足够），单日记录 312 B → 216 B。
#define FIRMWARE_STATISTICS_SCHEMA 8
#define FIRMWARE_STATISTICS_SCHEMA_MARKER "MIMIVENT_STATISTICS_SCHEMA=8"
// OTA 元数据声明的连续可读范围下限；schema 4 有单独迁移实现，schema 5 不支持。
#define FIRMWARE_MIN_READABLE_STATISTICS_SCHEMA 6
#define FIRMWARE_MIN_READABLE_STATISTICS_SCHEMA_MARKER \
  "MIMIVENT_MIN_READABLE_STORAGE_SCHEMA=6"
// 旧 App 兼容字段：/api/status 与 /api/stats 的 statisticsSchema 必须恒为 4。
#define FIRMWARE_STATISTICS_SCHEMA_COMPAT 4
// 与 Minimal SPIFFS v0.3.1 的数据分区布局不兼容；首次采用需 USB 写入分区表。
#define FIRMWARE_PARTITION_PROFILE "statistics-v2-ota-v1"
#define FIRMWARE_PARTITION_PROFILE_MARKER \
  "MIMIVENT_PARTITION_PROFILE=" FIRMWARE_PARTITION_PROFILE

// BLE 控制协议 v2 从 App v0.3.0 起可用；更旧版本不能控制此固件。
// 0.6.0 是第一个认识短键统计 JSON 的 App；更早的版本会把所有桶读成 0。
// 本发布版本已移除设备 HTTPS；NAS 控制由支持 MQTT 的 App 0.7.0 及更新版本提供。
#define FIRMWARE_MINIMUM_APP_VERSION "0.7.0"
