#include "OtaCheckpointStore.h"

#include <Preferences.h>
#include <cstddef>
#include <cstring>
#include <strings.h>

namespace {
constexpr uint32_t kCheckpointMagic = 0x4D564F32UL;
constexpr uint32_t kRejectedImageMagic = 0x4D565242UL;
constexpr uint16_t kCheckpointFormatVersion = 6;
constexpr char kPreferencesNamespace[] = "ota_task";

Preferences preferences;
bool storeReady = false;

uint32_t checksumBytes(const uint8_t* data, size_t length) {
  uint32_t checksum = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    checksum ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      checksum = (checksum >> 1) ^ (0xEDB88320UL & (0UL - (checksum & 1UL)));
    }
  }
  return ~checksum;
}

uint32_t recordChecksum(const OtaCheckpoint& record) {
  return checksumBytes(reinterpret_cast<const uint8_t*>(&record),
                       offsetof(OtaCheckpoint, checksum));
}

bool readRecord(const char* key, OtaCheckpoint* record,
                const String& expectedDeviceId) {
  if (preferences.getBytesLength(key) != sizeof(*record) ||
      preferences.getBytes(key, record, sizeof(*record)) != sizeof(*record)) {
    return false;
  }
  return record->magic == kCheckpointMagic &&
         record->formatVersion == kCheckpointFormatVersion &&
         expectedDeviceId == String(record->stableDeviceId) &&
         record->checksum == recordChecksum(*record);
}

bool readRejectedImage(OtaRejectedImage* record) {
  if (preferences.getBytesLength("rejected") != sizeof(*record) ||
      preferences.getBytes("rejected", record, sizeof(*record)) != sizeof(*record)) {
    return false;
  }
  return record->magic == kRejectedImageMagic &&
         record->formatVersion == 1 &&
         record->checksum == checksumBytes(
             reinterpret_cast<const uint8_t*>(record),
             offsetof(OtaRejectedImage, checksum));
}

}  // namespace

bool OtaCheckpointStore::begin() {
  storeReady = preferences.begin(kPreferencesNamespace, false);
  return storeReady;
}

bool OtaCheckpointStore::load(OtaCheckpoint* record,
                               const String& expectedDeviceId) {
  if (!storeReady || !record) return false;
  OtaCheckpoint first{}, second{};
  const bool hasFirst = readRecord("cp0", &first, expectedDeviceId);
  const bool hasSecond = readRecord("cp1", &second, expectedDeviceId);
  if (!hasFirst && !hasSecond) return false;
  *record = !hasSecond || (hasFirst && first.sequence >= second.sequence)
                ? first
                : second;
  return true;
}

bool OtaCheckpointStore::save(OtaCheckpoint* record, uint32_t imageOffset,
                              uint32_t packageOffset) {
  if (!storeReady || !record) return false;
  OtaCheckpoint next = *record;
  next.magic = kCheckpointMagic;
  next.formatVersion = kCheckpointFormatVersion;
  next.reservedVersion = 0;
  ++next.sequence;
  next.offset = imageOffset;
  next.packageOffset = packageOffset;
  next.checksum = recordChecksum(next);
  const char* key = (next.sequence & 1U) ? "cp1" : "cp0";
  if (preferences.putBytes(key, &next, sizeof(next)) != sizeof(next)) return false;
  *record = next;
  return true;
}

bool OtaCheckpointStore::recordRejectedImage(const char* taskId,
                                              const char* version,
                                              const char* digest) {
  if (!storeReady || !taskId || !version || !digest ||
      strlen(taskId) > 32 || strlen(version) > 15 || strlen(digest) != 64) {
    return false;
  }
  OtaRejectedImage record{};
  record.magic = kRejectedImageMagic;
  record.formatVersion = 1;
  strlcpy(record.taskId, taskId, sizeof(record.taskId));
  strlcpy(record.version, version, sizeof(record.version));
  strlcpy(record.digest, digest, sizeof(record.digest));
  record.checksum = checksumBytes(
      reinterpret_cast<const uint8_t*>(&record),
      offsetof(OtaRejectedImage, checksum));
  if (preferences.putBytes("rejected", &record, sizeof(record)) != sizeof(record)) {
    return false;
  }
  OtaRejectedImage verify{};
  return readRejectedImage(&verify) &&
         strcasecmp(verify.digest, digest) == 0 &&
         strcmp(verify.taskId, taskId) == 0;
}

bool OtaCheckpointStore::isRejectedImage(const String& digest) {
  if (!storeReady || digest.length() != 64) return false;
  OtaRejectedImage record{};
  return readRejectedImage(&record) &&
         digest.equalsIgnoreCase(record.digest);
}

bool OtaCheckpointStore::isRejectedTask(const char* taskId) {
  if (!storeReady || !taskId) return false;
  OtaRejectedImage record{};
  return readRejectedImage(&record) && strcmp(record.taskId, taskId) == 0;
}

bool OtaCheckpointStore::clearRejectedImage(const char* taskId) {
  if (!storeReady || !taskId) return false;
  OtaRejectedImage record{};
  if (!readRejectedImage(&record)) return true;
  if (strcmp(record.taskId, taskId) != 0) return false;
  return preferences.remove("rejected");
}

bool OtaCheckpointStore::clear() {
  if (!storeReady) return false;
  preferences.remove("cp0");
  preferences.remove("cp1");
  return preferences.getBytesLength("cp0") == 0 &&
         preferences.getBytesLength("cp1") == 0;
}
