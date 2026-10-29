#pragma once

#include <Arduino.h>

// OTA 任务持久记录，与传输协议、Flash 写入调度分离。
struct OtaCheckpoint {
  uint32_t magic;
  uint16_t formatVersion;
  uint16_t reservedVersion;
  uint32_t sequence;
  uint32_t imageSize;
  uint32_t packageSize;
  uint32_t offset;
  uint32_t packageOffset;
  uint32_t partitionAddress;
  uint8_t compressedPackage;
  uint8_t deltaPackage;
  uint8_t installIntent;
  uint8_t allowDowngrade;
  char taskId[33];
  char stableDeviceId[13];
  char version[16];
  char digest[65];
  char packageDigest[65];
  char baseDigest[65];
  char baseVersion[16];
  uint32_t baseImageSize;
  uint8_t signature[384];
  uint32_t checksum;
};

struct OtaRejectedImage {
  uint32_t magic;
  uint16_t formatVersion;
  char taskId[33];
  char version[16];
  char digest[65];
  uint32_t checksum;
};

class OtaCheckpointStore {
 public:
  bool begin();
  bool load(OtaCheckpoint* record, const String& expectedDeviceId);
  bool save(OtaCheckpoint* record, uint32_t imageOffset,
            uint32_t packageOffset = 0);
  bool recordRejectedImage(const char* taskId, const char* version,
                           const char* digest);
  bool isRejectedImage(const String& digest);
  bool isRejectedTask(const char* taskId);
  bool clearRejectedImage(const char* taskId);
  bool clear();
};
