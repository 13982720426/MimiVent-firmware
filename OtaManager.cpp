#include "OtaManager.h"

#include <esp_app_format.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <cstring>
#include <strings.h>

#include "AppConfig.h"
#include "FanController.h"
#include "FirmwareVersion.h"
#include "OtaBleAdapter.h"
#include "OtaCompression.h"
#include "StatisticsStore.h"

namespace {
constexpr uint32_t kTransferTimeoutMs = 30000;
constexpr uint32_t kMaxFirmwareBytes = 1769472;
constexpr uint32_t kCheckpointBytes = 16 * 1024;
constexpr uint32_t kEraseSectorBytes = 4096;
constexpr uint32_t kOtaPackageBlockBytes = 16 * 1024;
constexpr uint32_t kOtaSignatureBytes = 384;
constexpr uint32_t kOtaPackageFixedHeaderBytes = 128 + kOtaSignatureBytes;
constexpr uint32_t kOtaPackageDescriptorBytes = 16;
constexpr uint32_t kOtaDeltaHeaderBytes = 192 + kOtaSignatureBytes;
constexpr uint32_t kOtaDeltaRecordHeaderBytes = 24;
constexpr uint32_t kOtaDeltaBlockBytes = 4096;
constexpr uint32_t kOtaSignatureOffsetFull = 128;
constexpr uint32_t kOtaSignatureOffsetDelta = 192;
constexpr size_t kShaUpdateChunkBytes = 1024;
constexpr uint32_t kMaxDeltaBytes = kMaxFirmwareBytes +
    ((kMaxFirmwareBytes + kOtaDeltaBlockBytes - 1) / kOtaDeltaBlockBytes) *
        kOtaDeltaRecordHeaderBytes + kOtaDeltaHeaderBytes;
constexpr uint32_t kOtaPackageMaxBlocks =
    (kMaxFirmwareBytes + kOtaPackageBlockBytes - 1) / kOtaPackageBlockBytes;
constexpr uint32_t kOtaPackageMaxHeaderBytes =
    kOtaPackageFixedHeaderBytes + kOtaPackageMaxBlocks * kOtaPackageDescriptorBytes;
using OtaBleAdapter::Message;
using OtaBleAdapter::MessageType;
constexpr size_t kMaxChunkBytes = OtaBleAdapter::kMaxChunkBytes;
volatile bool active = false;
volatile bool disconnectPending = false;
bool authorized = false;
uint32_t lastActivityAt = 0;
volatile uint16_t ownerConnection = BLE_HS_CONN_HANDLE_NONE;
uint32_t expectedSize = 0;
uint32_t receivedSize = 0;
uint32_t expectedPackageSize = 0;
uint32_t packageReceivedSize = 0;
uint32_t packageCheckpointOffset = 0;
bool compressedPackage = false;
bool deltaPackage = false;
String expectedBaseDigest;
String expectedBaseVersion;
uint32_t expectedBaseSize = 0;
const esp_partition_t* basePartition = nullptr;
String expectedPackageDigest;
uint8_t otaPackageHeader[kOtaPackageFixedHeaderBytes] = {};
uint32_t otaPackageHeaderBytes = 0;
bool otaPackageHeaderValid = false;
uint32_t otaPackageBlockCount = 0;
uint32_t otaPackageBlockIndex = 0;
uint8_t otaPackageDescriptor[kOtaPackageDescriptorBytes] = {};
uint32_t otaPackageDescriptorBytes = 0;
uint32_t otaPackageBlockBytes = 0;
uint8_t otaPackageBlock[kOtaPackageBlockBytes] = {};
uint8_t otaImageBlock[kOtaPackageBlockBytes] = {};
uint32_t otaPackageBlockRawLength = 0;
uint32_t otaPackageBlockStoredLength = 0;
uint32_t otaPackageBlockCrc = 0;
uint8_t otaPackageBlockCodec = 0;
uint8_t otaDeltaHeader[kOtaDeltaHeaderBytes] = {};
uint32_t otaDeltaHeaderReceived = 0;
bool otaDeltaHeaderValid = false;
uint8_t otaDeltaRecord[kOtaDeltaRecordHeaderBytes] = {};
uint32_t otaDeltaRecordHeaderReceived = 0;
uint32_t otaDeltaRecordCount = 0;
uint32_t otaDeltaRecordIndex = 0;
uint32_t otaDeltaRecordDataReceived = 0;
uint32_t otaDeltaTargetOffset = 0;
uint32_t otaDeltaOutputLength = 0;
uint32_t otaDeltaSourceOffset = 0;
uint32_t otaDeltaPayloadLength = 0;
uint32_t otaDeltaOutputCrc = 0;
uint8_t otaDeltaRecordType = 0;
uint8_t otaDeltaData[kOtaDeltaBlockBytes] = {};
uint8_t otaDeltaOutput[kOtaDeltaBlockBytes] = {};
uint8_t imageHeader[sizeof(esp_image_header_t)] = {};
size_t imageHeaderBytes = 0;
String expectedVersion;
String expectedDigest;
String lastStatus = "LOCKED";
const esp_partition_t* targetPartition = nullptr;
esp_ota_handle_t otaHandle = 0;
bool verifiedForInstall = false;
bool installedTask = false;
uint32_t checkpointOffset = 0;
uint32_t nextCheckpointOffset = kCheckpointBytes;
uint32_t lastLoggedAckOffset = 0;
String pendingAckStatus;
uint16_t pendingAckConnection = BLE_HS_CONN_HANDLE_NONE;
volatile uint16_t authorizedConnection = BLE_HS_CONN_HANDLE_NONE;
volatile bool connectStatusPending = false;
portMUX_TYPE connectionStateMux = portMUX_INITIALIZER_UNLOCKED;
mbedtls_sha256_context shaContext;
uint32_t crc32(const uint8_t* data, size_t length);
void setStatus(const String& status);

// RSA-3072 公钥只用于验签；对应私钥保存在发布机仓库之外。
static const unsigned char kOtaSigningPublicKey[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEArxXTZJLVzTsDxnKiPes7\n"
    "uG/1iQc4tU/bkQPFDdSefTBTEUkh9GFXErAjpKY2c3X7HJeZvlCw93+VGquoa/WK\n"
    "Gfc+ZSovMI4D5umSsVhTp/P36QdmyPat78t53SWrzmhQVc9VzoNL1uuGRSwWLz0g\n"
    "MJl89tigincaDw4QicnQmHeDr7JN9q2tUkSHYqpT1Nq+z9Hi/t5KpDBrjIu3u5Xl\n"
    "R1cVXNfAWKm3S15rjmX93n4SNgoDaxybYehl2YbkO0TzXXypDRfBL3FmIRPbgEFq\n"
    "om1YnhNXH4g8ezEnZoDpblkUejozojWORhcuSmX5JbNsDcTlsCFcU/gB14H0qnUO\n"
    "+QNhK6HMtBC1YwQUbrQ8qWPhP8QtuUxfP1FUry3fcCgISZPOZBAjyCV4sGzEDLaD\n"
    "lnxDuoeYEEfLCx2B1oSK89LFmGIUhhUK2d/PtqsYjyimr5ejU8eV10JvtmRWighU\n"
    "krPEUz9GnY47oEkKr8s3ifWLROGXeXzJQDrMeVqMkzBxAgMBAAE=\n"
    "-----END PUBLIC KEY-----\n";

bool verifyImageSignature(const uint8_t digest[32], const uint8_t* signature) {
  mbedtls_pk_context key;
  mbedtls_pk_init(&key);
  const int parseResult = mbedtls_pk_parse_public_key(
      &key, kOtaSigningPublicKey, sizeof(kOtaSigningPublicKey));
  if (parseResult != 0) {
    mbedtls_pk_free(&key);
    return false;
  }
  mbedtls_pk_rsassa_pss_options options{};
  options.mgf1_hash_id = MBEDTLS_MD_SHA256;
  options.expected_salt_len = 32;
  const int result = mbedtls_pk_verify_ext(
      MBEDTLS_PK_RSASSA_PSS, &options, &key, MBEDTLS_MD_SHA256,
      digest, 32, signature, kOtaSignatureBytes);
  mbedtls_pk_free(&key);
  return result == 0;
}

String commandTaskId(const String& command) {
  const int separator = command.indexOf('|');
  if (separator < 0) return String();
  const int requestSeparator = command.indexOf('|', separator + 1);
  return command.substring(separator + 1,
                           requestSeparator < 0 ? command.length() : requestSeparator);
}

OtaCheckpoint currentCheckpoint{};
OtaCheckpointStore checkpointStore;
bool checkpointStoreReady = false;

String getStableDeviceId() {
  char identifier[13] = {};
  const uint64_t mac = ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL;
  snprintf(identifier, sizeof(identifier), "%012llX",
           static_cast<unsigned long long>(mac));
  return String(identifier);
}

bool loadCheckpoint(OtaCheckpoint* record) {
  return checkpointStore.load(record, getStableDeviceId());
}

bool isAuthorizedConnection(uint16_t connection) {
  portENTER_CRITICAL(&connectionStateMux);
  const bool result = authorized && authorizedConnection == connection;
  portEXIT_CRITICAL(&connectionStateMux);
  return result;
}

bool saveCheckpoint(uint32_t offset) {
  const uint32_t durablePackageOffset = (compressedPackage || deltaPackage)
      ? packageCheckpointOffset
      : offset;
  if (!checkpointStore.save(&currentCheckpoint, offset,
                            durablePackageOffset)) return false;
  checkpointOffset = offset;
  nextCheckpointOffset = offset + kCheckpointBytes;
  return true;
}

bool clearCheckpoint() {
  if (!checkpointStore.clear()) return false;
  memset(&currentCheckpoint, 0, sizeof(currentCheckpoint));
  checkpointOffset = 0;
  nextCheckpointOffset = kCheckpointBytes;
  return true;
}

void suspendUpdate(const char* reason) {
  if (otaHandle) {
    esp_ota_abort(otaHandle);
    otaHandle = 0;
  }
  if (active) mbedtls_sha256_free(&shaContext);
  active = false;
  receivedSize = checkpointOffset;
  packageReceivedSize = (compressedPackage || deltaPackage)
      ? packageCheckpointOffset : checkpointOffset;
  fanControllerSetAuto(millis(), FanControlSource::OTA);
  setStatus(String("PAUSED|") + ((compressedPackage || deltaPackage) ? packageCheckpointOffset : checkpointOffset) + "|" + expectedPackageSize + "|" +
            expectedVersion + "|" + expectedDigest + "|" +
            currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId + "|" + currentCheckpoint.packageDigest + "|" + reason);
  ownerConnection = BLE_HS_CONN_HANDLE_NONE;
}

void setStatus(const String& status) {
  lastStatus = status;
  if (status.startsWith("ACK|")) {
    pendingAckStatus = status;
    pendingAckConnection = ownerConnection;
    const int separator = status.indexOf('|');
    const uint32_t acknowledged = strtoul(status.substring(separator + 1).c_str(), nullptr, 10);
    const uint32_t transferSize = (compressedPackage || deltaPackage)
        ? expectedPackageSize : expectedSize;
    if (acknowledged == transferSize ||
        (acknowledged >= lastLoggedAckOffset &&
         acknowledged - lastLoggedAckOffset >= 64 * 1024)) {
      Serial.printf("OTA progress: received=%lu/%lu checkpoint=%lu\n",
                    static_cast<unsigned long>(acknowledged),
                    static_cast<unsigned long>(transferSize),
                    static_cast<unsigned long>((compressedPackage || deltaPackage)
                                                  ? packageCheckpointOffset
                                                  : checkpointOffset));
      lastLoggedAckOffset = acknowledged;
    }
    return;
  }
  const bool terminal = status.startsWith("ERROR|") ||
      status.startsWith("PAUSED|") || status.startsWith("VERIFIED|") ||
      status.startsWith("INSTALLED|") || status.startsWith("INSTALLING|") ||
      status.startsWith("DONE|");
  if (terminal) {
    pendingAckStatus = "";
    pendingAckConnection = BLE_HS_CONN_HANDLE_NONE;
    OtaBleAdapter::clearPendingCallbackStatuses();
  }
  OtaBleAdapter::publishStatus(status, ownerConnection);
  Serial.printf("OTA: %s\n", status.c_str());
}

bool parseHexDigest(const String& digest) {
  if (digest.length() != 64) return false;
  for (size_t i = 0; i < digest.length(); ++i) {
    const char c = digest[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

String digestToHex(const uint8_t* bytes) {
  static const char hex[] = "0123456789abcdef";
  char output[65] = {};
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = hex[bytes[i] >> 4];
    output[i * 2 + 1] = hex[bytes[i] & 0x0F];
  }
  return String(output);
}

bool updateSha256InChunks(mbedtls_sha256_context* context,
                          const uint8_t* data, size_t length) {
  if (!context || (!data && length > 0)) return false;
  size_t offset = 0;
  while (offset < length) {
    const size_t count = min(kShaUpdateChunkBytes, length - offset);
    if (mbedtls_sha256_update(context, data + offset, count) != 0) return false;
    offset += count;
  }
  return true;
}

String calculatePartitionDigest(const esp_partition_t* partition,
                               uint32_t imageSize) {
  if (!partition || imageSize == 0 || imageSize > partition->size) return String();
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  if (mbedtls_sha256_starts(&context, 0) != 0) {
    mbedtls_sha256_free(&context);
    return String();
  }
  uint8_t buffer[1024];
  uint32_t offset = 0;
  while (offset < imageSize) {
    const size_t count = min(sizeof(buffer), static_cast<size_t>(imageSize - offset));
    if (esp_partition_read(partition, offset, buffer, count) != ESP_OK ||
        !updateSha256InChunks(&context, buffer, count)) {
      mbedtls_sha256_free(&context);
      return String();
    }
    offset += count;
  }
  uint8_t digest[32] = {};
  const int finishResult = mbedtls_sha256_finish(&context, digest);
  mbedtls_sha256_free(&context);
  return finishResult == 0 ? digestToHex(digest) : String();
}

bool verifyPartitionDigest(const esp_partition_t* partition, uint32_t imageSize,
                          const String& expectedDigest) {
  const String actual = calculatePartitionDigest(partition, imageSize);
  return !actual.isEmpty() && actual.equalsIgnoreCase(expectedDigest);
}

bool verifyCandidatePartition() {
  if (!verifyPartitionDigest(targetPartition, expectedSize, expectedDigest)) return false;
  esp_app_desc_t description{};
  return esp_ota_get_partition_description(targetPartition, &description) == ESP_OK &&
         expectedVersion == String(description.version);
}

bool installCandidateWasRejected() {
  if (!targetPartition) return false;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(targetPartition, &state) != ESP_OK) return false;
  return state == ESP_OTA_IMG_ABORTED || state == ESP_OTA_IMG_INVALID;
}

bool persistBootRollback() {
  if (!checkpointStore.recordRejectedImage(currentCheckpoint.taskId,
                                            currentCheckpoint.version,
                                            currentCheckpoint.digest)) {
    return false;
  }
  currentCheckpoint.installIntent = 0;
  if (!checkpointStore.save(&currentCheckpoint, currentCheckpoint.offset,
                            currentCheckpoint.packageOffset)) {
    currentCheckpoint.installIntent = 1;
    return false;
  }
  verifiedForInstall = false;
  installedTask = false;
  lastStatus = String("ERROR|BOOT_ROLLBACK|") + currentCheckpoint.taskId +
               "|" + currentCheckpoint.version + "|" + currentCheckpoint.digest;
  return true;
}

const esp_partition_t* findAppPartition(uint32_t address) {
  esp_partition_iterator_t it = esp_partition_find(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (it) {
    const esp_partition_t* candidate = esp_partition_get(it);
    if (candidate && candidate->address == address) {
      esp_partition_iterator_release(it);
      return candidate;
    }
    it = esp_partition_next(it);
  }
  return nullptr;
}

bool isSemanticVersion(const String& candidate) {
  if (candidate.length() > 15) return false;
  unsigned int major = 0, minor = 0, patch = 0;
  char trailing = 0;
  return sscanf(candidate.c_str(), "%u.%u.%u%c", &major, &minor, &patch,
                &trailing) == 3;
}

int compareSemanticVersions(const String& left, const String& right) {
  unsigned int leftParts[3] = {}, rightParts[3] = {};
  sscanf(left.c_str(), "%u.%u.%u", &leftParts[0], &leftParts[1], &leftParts[2]);
  sscanf(right.c_str(), "%u.%u.%u", &rightParts[0], &rightParts[1], &rightParts[2]);
  for (size_t index = 0; index < 3; ++index) {
    if (leftParts[index] < rightParts[index]) return -1;
    if (leftParts[index] > rightParts[index]) return 1;
  }
  return 0;
}

void abortUpdate(const char* reason, bool restoreAuto) {
  if (otaHandle) {
    esp_ota_abort(otaHandle);
    otaHandle = 0;
  }
  if (active) mbedtls_sha256_free(&shaContext);
  active = false;
  expectedSize = receivedSize = 0;
  imageHeaderBytes = 0;
  verifiedForInstall = false;
  targetPartition = nullptr;
  OtaCheckpoint saved{};
  if (loadCheckpoint(&saved) && saved.imageSize > 0) {
    setStatus(String("PAUSED|") + ((saved.compressedPackage || saved.deltaPackage) ? saved.packageOffset : saved.offset) + "|" + ((saved.compressedPackage || saved.deltaPackage) ? saved.packageSize : saved.imageSize) + "|" +
              saved.version + "|" + saved.digest + "|" + saved.taskId + "|" +
              saved.stableDeviceId + "|" + saved.packageDigest + "|ERROR:" + reason);
  } else {
    setStatus(String("ERROR|") + reason);
  }
  ownerConnection = BLE_HS_CONN_HANDLE_NONE;
  if (restoreAuto) fanControllerSetAuto(millis(), FanControlSource::OTA);
}

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8U) |
         (static_cast<uint32_t>(data[2]) << 16U) |
         (static_cast<uint32_t>(data[3]) << 24U);
}

uint16_t readU16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8U);
}

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
  }
  return ~crc;
}

bool validateOtaPackageHeader() {
  static const uint8_t expectedMagic[8] = {'M', 'V', 'O', 'T', 'A', '2', 0, 0};
  if (memcmp(otaPackageHeader, expectedMagic, sizeof(expectedMagic)) != 0 ||
      readU16(otaPackageHeader + 8) != 2 ||
      readU32(otaPackageHeader + 12) != kOtaPackageFixedHeaderBytes ||
      readU32(otaPackageHeader + 16) != expectedSize ||
      readU32(otaPackageHeader + 20) != kOtaPackageBlockBytes ||
      readU32(otaPackageHeader + 24) !=
          (expectedSize + kOtaPackageBlockBytes - 1) / kOtaPackageBlockBytes ||
      readU32(otaPackageHeader + 124) != crc32(otaPackageHeader, 124)) {
    return false;
  }
  const uint8_t versionLength = otaPackageHeader[60];
  const uint8_t profileLength = otaPackageHeader[76];
  if (versionLength == 0 || versionLength >= 16 ||
      profileLength == 0 || profileLength > 32 ||
      memchr(otaPackageHeader + 61, 0, versionLength) != nullptr ||
      memchr(otaPackageHeader + 77, 0, profileLength) != nullptr) return false;
  char packageVersion[16] = {};
  char packageProfile[33] = {};
  memcpy(packageVersion, otaPackageHeader + 61, versionLength);
  memcpy(packageProfile, otaPackageHeader + 77, profileLength);
  if (expectedVersion != String(packageVersion) ||
      String(packageProfile) != String(FIRMWARE_PARTITION_PROFILE)) return false;
  if (digestToHex(otaPackageHeader + 28).equalsIgnoreCase(expectedDigest) == false) {
    return false;
  }
  otaPackageBlockCount = readU32(otaPackageHeader + 24);
  memcpy(currentCheckpoint.signature,
         otaPackageHeader + kOtaSignatureOffsetFull, kOtaSignatureBytes);
  otaPackageHeaderValid = true;
  return otaPackageBlockCount > 0 && otaPackageBlockCount <= kOtaPackageMaxBlocks;
}

bool writeDecodedOtaBlock(const uint8_t* data, size_t length) {
  if (!data || length == 0 || length > kOtaPackageBlockBytes ||
      receivedSize + length > expectedSize) return false;
  auto writeSequential = [](const uint8_t* bytes, size_t byteCount) {
    size_t written = 0;
    while (written < byteCount) {
      const size_t count = min(static_cast<size_t>(kEraseSectorBytes),
                                byteCount - written);
      if (esp_ota_write(otaHandle, bytes + written, count) != ESP_OK) return false;
      written += count;
      // 让出 loop 任务，使 NimBLE 主机及时处理连接事件和 OTA 流控。
      delay(1);
    }
    return true;
  };
  size_t offset = 0;
  if (imageHeaderBytes < sizeof(imageHeader)) {
    const size_t count = min(length, sizeof(imageHeader) - imageHeaderBytes);
    memcpy(imageHeader + imageHeaderBytes, data, count);
    imageHeaderBytes += count;
    offset += count;
    if (imageHeaderBytes == sizeof(imageHeader)) {
      esp_image_header_t header{};
      memcpy(&header, imageHeader, sizeof(header));
      if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32C3 ||
          !writeSequential(imageHeader, sizeof(imageHeader))) {
        return false;
      }
    }
  }
  if (offset < length && !writeSequential(data + offset, length - offset)) return false;
  if (!updateSha256InChunks(&shaContext, data, length)) return false;
  receivedSize += length;
  return true;
}

bool finishOtaPackageBlock() {
  size_t decodedLength = 0;
  if (otaPackageBlockCodec == 0) {
    if (otaPackageBlockStoredLength != otaPackageBlockRawLength) return false;
    memcpy(otaImageBlock, otaPackageBlock, otaPackageBlockRawLength);
    decodedLength = otaPackageBlockRawLength;
  } else if (otaPackageBlockCodec == 1) {
    if (!otaInflateBlock(otaPackageBlock, otaPackageBlockStoredLength,
                         otaImageBlock, sizeof(otaImageBlock), &decodedLength) ||
        decodedLength != otaPackageBlockRawLength) return false;
  } else {
    return false;
  }
  if (crc32(otaImageBlock, decodedLength) != otaPackageBlockCrc ||
      !writeDecodedOtaBlock(otaImageBlock, decodedLength)) return false;
  ++otaPackageBlockIndex;
  otaPackageDescriptorBytes = 0;
  otaPackageBlockBytes = 0;
  otaPackageBlockRawLength = 0;
  otaPackageBlockStoredLength = 0;
  const uint32_t expectedDurable = min(receivedSize, expectedSize);
  if (expectedDurable < expectedSize &&
      (expectedDurable % kCheckpointBytes) == 0) {
    packageCheckpointOffset = packageReceivedSize;
  }
  if (expectedDurable < expectedSize && expectedDurable > checkpointOffset &&
      !saveCheckpoint(expectedDurable)) return false;
  return true;
}

bool consumeOtaPackageBytes(const uint8_t* data, size_t length) {
  size_t cursor = 0;
  while (cursor < length) {
    if (!otaPackageHeaderValid) {
      const size_t count = min(length - cursor,
          static_cast<size_t>(kOtaPackageFixedHeaderBytes - otaPackageHeaderBytes));
      memcpy(otaPackageHeader + otaPackageHeaderBytes, data + cursor, count);
      otaPackageHeaderBytes += count;
      packageReceivedSize += count;
      cursor += count;
      if (otaPackageHeaderBytes == kOtaPackageFixedHeaderBytes &&
          !validateOtaPackageHeader()) return false;
      continue;
    }
    if (otaPackageBlockIndex >= otaPackageBlockCount) return false;
    if (otaPackageDescriptorBytes < kOtaPackageDescriptorBytes) {
      const size_t count = min(length - cursor,
          static_cast<size_t>(kOtaPackageDescriptorBytes - otaPackageDescriptorBytes));
      memcpy(otaPackageDescriptor + otaPackageDescriptorBytes, data + cursor, count);
      otaPackageDescriptorBytes += count;
      packageReceivedSize += count;
      cursor += count;
      if (otaPackageDescriptorBytes == kOtaPackageDescriptorBytes) {
        otaPackageBlockRawLength = readU32(otaPackageDescriptor);
        otaPackageBlockStoredLength = readU32(otaPackageDescriptor + 4);
        otaPackageBlockCodec = otaPackageDescriptor[8];
        otaPackageBlockCrc = readU32(otaPackageDescriptor + 12);
        const uint32_t expectedRawLength = min(kOtaPackageBlockBytes,
            expectedSize - receivedSize);
        if (otaPackageBlockRawLength != expectedRawLength ||
            otaPackageBlockStoredLength == 0 ||
            otaPackageBlockStoredLength > kOtaPackageBlockBytes ||
            otaPackageBlockCodec > 1) return false;
      }
      continue;
    }
    const size_t count = min(length - cursor,
        static_cast<size_t>(otaPackageBlockStoredLength - otaPackageBlockBytes));
    memcpy(otaPackageBlock + otaPackageBlockBytes, data + cursor, count);
    otaPackageBlockBytes += count;
    packageReceivedSize += count;
    cursor += count;
    if (otaPackageBlockBytes == otaPackageBlockStoredLength &&
        !finishOtaPackageBlock()) return false;
  }
  return true;
}

bool validateOtaDeltaHeader() {
  static const uint8_t expectedMagic[8] = {'M', 'V', 'D', 'L', 'T', '1', 0, 0};
  if (memcmp(otaDeltaHeader, expectedMagic, sizeof(expectedMagic)) != 0 ||
      readU16(otaDeltaHeader + 8) != 2 ||
      readU16(otaDeltaHeader + 10) != kOtaDeltaHeaderBytes ||
      readU32(otaDeltaHeader + 12) != expectedBaseSize ||
      readU32(otaDeltaHeader + 16) != expectedSize ||
      readU32(otaDeltaHeader + 156) != kOtaDeltaBlockBytes ||
      readU16(otaDeltaHeader + 160) != kOtaDeltaRecordHeaderBytes ||
      readU32(otaDeltaHeader + 188) != crc32(otaDeltaHeader, 188) ||
      digestToHex(otaDeltaHeader + 24).equalsIgnoreCase(expectedBaseDigest) == false ||
      digestToHex(otaDeltaHeader + 56).equalsIgnoreCase(expectedDigest) == false) {
    return false;
  }
  const uint8_t baseVersionLength = otaDeltaHeader[88];
  const uint8_t targetVersionLength = otaDeltaHeader[104];
  const uint8_t profileLength = otaDeltaHeader[120];
  if (baseVersionLength == 0 || baseVersionLength > 15 ||
      targetVersionLength == 0 || targetVersionLength > 15 ||
      profileLength == 0 || profileLength > 32 ||
      memchr(otaDeltaHeader + 89, 0, baseVersionLength) != nullptr ||
      memchr(otaDeltaHeader + 105, 0, targetVersionLength) != nullptr ||
      memchr(otaDeltaHeader + 121, 0, profileLength) != nullptr) return false;
  char baseVersion[16] = {};
  char targetVersion[16] = {};
  char partitionProfile[33] = {};
  memcpy(baseVersion, otaDeltaHeader + 89, baseVersionLength);
  memcpy(targetVersion, otaDeltaHeader + 105, targetVersionLength);
  memcpy(partitionProfile, otaDeltaHeader + 121, profileLength);
  if (String(baseVersion) != expectedBaseVersion ||
      String(targetVersion) != expectedVersion ||
      String(partitionProfile) != String(FIRMWARE_PARTITION_PROFILE)) return false;
  otaDeltaRecordCount = readU32(otaDeltaHeader + 20);
  memcpy(currentCheckpoint.signature,
         otaDeltaHeader + kOtaSignatureOffsetDelta, kOtaSignatureBytes);
  otaDeltaHeaderValid = otaDeltaRecordCount > 0 &&
      otaDeltaRecordCount ==
          (expectedSize + kOtaDeltaBlockBytes - 1) / kOtaDeltaBlockBytes;
  return otaDeltaHeaderValid;
}

bool finishOtaDeltaRecord() {
  if (otaDeltaRecordType == 0) {
    if (otaDeltaPayloadLength != 0 ||
        esp_partition_read(basePartition, otaDeltaSourceOffset, otaDeltaOutput,
                           otaDeltaOutputLength) != ESP_OK) return false;
  } else if (otaDeltaRecordType == 1) {
    size_t outputLength = 0;
    if (!otaInflateBlock(otaDeltaData, otaDeltaPayloadLength, otaDeltaOutput,
                         sizeof(otaDeltaOutput), &outputLength) ||
        outputLength != otaDeltaOutputLength) return false;
  } else if (otaDeltaRecordType == 2) {
    if (otaDeltaPayloadLength != otaDeltaOutputLength) return false;
    memcpy(otaDeltaOutput, otaDeltaData, otaDeltaOutputLength);
  } else {
    return false;
  }
  if (otaDeltaTargetOffset != receivedSize ||
      crc32(otaDeltaOutput, otaDeltaOutputLength) != otaDeltaOutputCrc ||
      !writeDecodedOtaBlock(otaDeltaOutput, otaDeltaOutputLength)) return false;
  ++otaDeltaRecordIndex;
  otaDeltaRecordHeaderReceived = 0;
  otaDeltaRecordDataReceived = 0;
  if (receivedSize < expectedSize &&
      (receivedSize % kCheckpointBytes) == 0) {
    packageCheckpointOffset = packageReceivedSize;
    if (!saveCheckpoint(receivedSize)) return false;
  }
  return true;
}

bool consumeOtaDeltaBytes(const uint8_t* data, size_t length) {
  size_t cursor = 0;
  while (cursor < length) {
    if (!otaDeltaHeaderValid) {
      const size_t count = min(length - cursor,
          static_cast<size_t>(kOtaDeltaHeaderBytes - otaDeltaHeaderReceived));
      memcpy(otaDeltaHeader + otaDeltaHeaderReceived, data + cursor, count);
      otaDeltaHeaderReceived += count;
      packageReceivedSize += count;
      cursor += count;
      if (otaDeltaHeaderReceived == kOtaDeltaHeaderBytes &&
          !validateOtaDeltaHeader()) return false;
      continue;
    }
    if (otaDeltaRecordIndex >= otaDeltaRecordCount) return false;
    if (otaDeltaRecordHeaderReceived < kOtaDeltaRecordHeaderBytes) {
      const size_t count = min(length - cursor,
          static_cast<size_t>(kOtaDeltaRecordHeaderBytes - otaDeltaRecordHeaderReceived));
      memcpy(otaDeltaRecord + otaDeltaRecordHeaderReceived, data + cursor, count);
      otaDeltaRecordHeaderReceived += count;
      packageReceivedSize += count;
      cursor += count;
      if (otaDeltaRecordHeaderReceived == kOtaDeltaRecordHeaderBytes) {
        otaDeltaRecordType = otaDeltaRecord[0];
        otaDeltaTargetOffset = readU32(otaDeltaRecord + 4);
        otaDeltaOutputLength = readU32(otaDeltaRecord + 8);
        otaDeltaSourceOffset = readU32(otaDeltaRecord + 12);
        otaDeltaPayloadLength = readU32(otaDeltaRecord + 16);
        otaDeltaOutputCrc = readU32(otaDeltaRecord + 20);
        const uint32_t expectedLength = min(kOtaDeltaBlockBytes,
            expectedSize - receivedSize);
        if (otaDeltaTargetOffset != receivedSize ||
            otaDeltaOutputLength != expectedLength ||
            otaDeltaOutputLength == 0 || otaDeltaOutputLength > kOtaDeltaBlockBytes ||
            otaDeltaRecordType > 2 ||
            (otaDeltaRecordType == 0 &&
             (otaDeltaPayloadLength != 0 ||
              otaDeltaSourceOffset + otaDeltaOutputLength > expectedBaseSize)) ||
            (otaDeltaRecordType != 0 &&
             (otaDeltaPayloadLength == 0 || otaDeltaPayloadLength > kOtaDeltaBlockBytes))) {
          return false;
        }
        if (otaDeltaRecordType == 0) {
          if (!finishOtaDeltaRecord()) return false;
        }
      }
      continue;
    }
    const size_t count = min(length - cursor,
        static_cast<size_t>(otaDeltaPayloadLength - otaDeltaRecordDataReceived));
    memcpy(otaDeltaData + otaDeltaRecordDataReceived, data + cursor, count);
    otaDeltaRecordDataReceived += count;
    packageReceivedSize += count;
    cursor += count;
    if (otaDeltaRecordDataReceived == otaDeltaPayloadLength &&
        !finishOtaDeltaRecord()) return false;
  }
  return true;
}

void beginUpdate(const Message& message) {
  if (!isAuthorizedConnection(message.connection)) {
    setStatus("ERROR|LOCKED"); return;
  }
  if (!checkpointStoreReady) {
    setStatus("ERROR|CHECKPOINT_STORE"); return;
  }
  if (active) { setStatus("ERROR|BUSY"); return; }
  const String packet(reinterpret_cast<const char*>(message.bytes), message.length);
  const bool packageMode = packet.startsWith("BEGIN2|");
  const bool deltaMode = packet.startsWith("BEGIN_DELTA|");
  if (!packageMode && !deltaMode) {
    setStatus("ERROR|SIGNED_PACKAGE_REQUIRED"); return;
  }
  String taskId, version, packageDigest, digest, baseVersion, baseDigest;
  bool allowDowngrade = false;
  uint32_t packageSize = 0, size = 0, baseSize = 0;
  if (deltaMode) {
    String fields[11];
    int start = 0;
    for (int index = 0; index < 10; ++index) {
      const int separator = packet.indexOf('|', start);
      if (separator < 0) { setStatus("ERROR|FORMAT"); return; }
      fields[index] = packet.substring(start, separator);
      start = separator + 1;
    }
    fields[10] = packet.substring(start);
    if (fields[0] != "BEGIN_DELTA") { setStatus("ERROR|FORMAT"); return; }
    if (fields[10] != "0" && fields[10] != "1") {
      setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
    }
    taskId = fields[1];
    version = fields[2];
    packageSize = strtoul(fields[3].c_str(), nullptr, 10);
    packageDigest = fields[4];
    size = strtoul(fields[5].c_str(), nullptr, 10);
    digest = fields[6];
    baseVersion = fields[7];
    baseSize = strtoul(fields[8].c_str(), nullptr, 10);
    baseDigest = fields[9];
    allowDowngrade = fields[10] == "1";
  } else {
    const int first = packet.indexOf('|');
    const int second = first < 0 ? -1 : packet.indexOf('|', first + 1);
    const int third = second < 0 ? -1 : packet.indexOf('|', second + 1);
    const int fourth = third < 0 ? -1 : packet.indexOf('|', third + 1);
    const int fifth = fourth < 0 ? -1 : packet.indexOf('|', fourth + 1);
    const int sixth = fifth < 0 ? -1 : packet.indexOf('|', fifth + 1);
    const int seventh = sixth < 0 ? -1 : packet.indexOf('|', sixth + 1);
    if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
        fifth < 0 || sixth < 0 || seventh < 0) {
      setStatus("ERROR|FORMAT"); return;
    }
    const String downgradeConfirmation = packet.substring(seventh + 1);
    if (downgradeConfirmation != "0" && downgradeConfirmation != "1") {
      setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
    }
    allowDowngrade = downgradeConfirmation == "1";
    taskId = packet.substring(first + 1, second);
    version = packet.substring(second + 1, third);
    packageSize = strtoul(packet.substring(third + 1, fourth).c_str(), nullptr, 10);
    packageDigest = packet.substring(fourth + 1, fifth);
    size = strtoul(packet.substring(fifth + 1, sixth).c_str(), nullptr, 10);
    digest = packet.substring(sixth + 1, seventh);
  }
  if (!isSemanticVersion(version)) { setStatus("ERROR|VERSION_FORMAT"); return; }
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  esp_app_desc_t runningDescription{};
  if (!runningPartition ||
      esp_ota_get_partition_description(runningPartition, &runningDescription) != ESP_OK ||
      !isSemanticVersion(String(runningDescription.version))) {
    setStatus("ERROR|RUNNING_VERSION"); return;
  }
  const bool isDowngrade = compareSemanticVersions(
      version, String(runningDescription.version)) < 0;
  if (isDowngrade != allowDowngrade) {
    setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
  }
  if (size < 1024 || size > kMaxFirmwareBytes || packageSize < 128 ||
      packageSize > (deltaMode ? kMaxDeltaBytes
                               : kMaxFirmwareBytes + kOtaPackageMaxHeaderBytes) ||
      !parseHexDigest(digest) || !parseHexDigest(packageDigest)) {
    setStatus("ERROR|SIZE_OR_HASH"); return;
  }
  if (checkpointStore.isRejectedImage(digest)) {
    setStatus("ERROR|CANDIDATE_REJECTED");
    return;
  }
  if (deltaMode) {
    if (!isSemanticVersion(baseVersion) || baseSize < 1024 ||
        !parseHexDigest(baseDigest) || packageSize < kOtaDeltaHeaderBytes) {
      setStatus("ERROR|DELTA_METADATA"); return;
    }
    basePartition = esp_ota_get_running_partition();
    esp_image_metadata_t baseMetadata{};
    esp_app_desc_t baseDescription{};
    const esp_partition_pos_t basePosition = {
        .offset = basePartition ? basePartition->address : 0,
        .size = basePartition ? basePartition->size : 0,
    };
    if (!basePartition ||
        esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &basePosition, &baseMetadata) != ESP_OK ||
        esp_ota_get_partition_description(basePartition, &baseDescription) != ESP_OK ||
        baseMetadata.image_len != baseSize || String(baseDescription.version) != baseVersion ||
        !calculatePartitionDigest(basePartition, baseMetadata.image_len)
             .equalsIgnoreCase(baseDigest)) {
      setStatus("ERROR|BASE_MISMATCH"); return;
    }
  } else {
    basePartition = nullptr;
  }
  if (taskId.isEmpty() || taskId.length() > 32) { setStatus("ERROR|TASK_ID"); return; }
  OtaCheckpoint saved{};
  if (loadCheckpoint(&saved) && saved.offset == size && saved.imageSize == size &&
      saved.packageSize == packageSize && saved.compressedPackage == packageMode &&
      saved.deltaPackage == deltaMode &&
      saved.allowDowngrade == (allowDowngrade ? 1 : 0) &&
      strcmp(saved.taskId, taskId.c_str()) == 0 &&
      strcmp(saved.version, version.c_str()) == 0 &&
      strcasecmp(saved.digest, digest.c_str()) == 0 &&
      strcasecmp(saved.packageDigest, packageDigest.c_str()) == 0 &&
      strcasecmp(saved.baseDigest, baseDigest.c_str()) == 0) {
    targetPartition = findAppPartition(saved.partitionAddress);
    if (!targetPartition) { setStatus("ERROR|FLASH_PARTITION"); return; }
    currentCheckpoint = saved;
    expectedVersion = version;
    expectedDigest = digest;
    expectedSize = size;
    expectedPackageSize = packageSize;
    expectedPackageDigest = packageDigest;
    compressedPackage = packageMode;
    deltaPackage = deltaMode;
    if (saved.installIntent && installCandidateWasRejected()) {
      ownerConnection = message.connection;
      setStatus(persistBootRollback() ? lastStatus :
                String("ERROR|BOOT_ROLLBACK_RECORD"));
      return;
    }
    if (!verifyCandidatePartition()) {
      verifiedForInstall = false;
      setStatus("ERROR|CANDIDATE_CHANGED");
      return;
    }
    verifiedForInstall = !installedTask;
    ownerConnection = message.connection;
    if (installedTask) setStatus(String("INSTALLED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
    else if (currentCheckpoint.installIntent) setStatus(String("INSTALLING|") + expectedVersion + "|" + currentCheckpoint.taskId);
    else setStatus(String("VERIFIED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
    return;
  }
  if (loadCheckpoint(&saved) &&
      (strcmp(saved.taskId, taskId.c_str()) != 0 || saved.imageSize != size ||
       saved.packageSize != packageSize || saved.compressedPackage != packageMode ||
       saved.deltaPackage != deltaMode ||
       saved.allowDowngrade != (allowDowngrade ? 1 : 0) ||
       strcmp(saved.version, version.c_str()) != 0 ||
       strcasecmp(saved.digest, digest.c_str()) != 0 ||
       strcasecmp(saved.packageDigest, packageDigest.c_str()) != 0 ||
       strcasecmp(saved.baseDigest, baseDigest.c_str()) != 0)) {
    const bool replacingRejectedImage =
        checkpointStore.isRejectedImage(String(saved.digest)) &&
        strcasecmp(saved.digest, digest.c_str()) != 0;
    if (!replacingRejectedImage) {
      setStatus("ERROR|TASK_MISMATCH");
      return;
    }
    // 仅自动替换已确认回滚的旧任务；保留拒绝镜像记录，阻止同一镜像重试。
    if (!clearCheckpoint()) {
      setStatus("ERROR|CHECKPOINT_CLEAR");
      return;
    }
    targetPartition = nullptr;
    verifiedForInstall = false;
    installedTask = false;
  }
  if (verifiedForInstall || currentCheckpoint.installIntent) {
    setStatus(currentCheckpoint.installIntent
                  ? "ERROR|INSTALL_COMMITTED"
                  : "ERROR|TASK_PENDING");
    return;
  }
  installedTask = false;
  expectedVersion = version;
  expectedDigest = digest;
  expectedSize = size;
  expectedPackageSize = packageSize;
  expectedPackageDigest = packageDigest;
  compressedPackage = packageMode;
  deltaPackage = deltaMode;
  expectedBaseDigest = baseDigest;
  expectedBaseVersion = baseVersion;
  expectedBaseSize = baseSize;
  lastLoggedAckOffset = 0;
  const bool resumable = loadCheckpoint(&saved) && saved.offset > 0 &&
      strcmp(saved.taskId, taskId.c_str()) == 0 &&
      saved.imageSize == size && strcmp(saved.version, version.c_str()) == 0 &&
      saved.packageSize == packageSize && saved.compressedPackage == packageMode &&
      saved.deltaPackage == deltaMode &&
      saved.allowDowngrade == (allowDowngrade ? 1 : 0) &&
      strcasecmp(saved.baseDigest, baseDigest.c_str()) == 0 &&
      strcasecmp(saved.packageDigest, packageDigest.c_str()) == 0 &&
      strcasecmp(saved.digest, digest.c_str()) == 0 &&
      saved.offset < size && (saved.offset % kEraseSectorBytes) == 0 &&
      saved.packageOffset > 0 && saved.packageOffset < packageSize;
  targetPartition = nullptr;
  if (resumable) {
    targetPartition = findAppPartition(saved.partitionAddress);
  } else {
    if (!clearCheckpoint()) {
      setStatus("ERROR|CHECKPOINT_CLEAR");
      return;
    }
    targetPartition = esp_ota_get_next_update_partition(nullptr);
  }
  if (!targetPartition || size > targetPartition->size) {
    targetPartition = nullptr;
    setStatus("ERROR|FLASH_PARTITION"); return;
  }
  esp_err_t beginResult;
  if (resumable) {
    beginResult = esp_ota_resume(targetPartition, OTA_WITH_SEQUENTIAL_WRITES,
                                 saved.offset, &otaHandle);
    currentCheckpoint = saved;
    checkpointOffset = saved.offset;
    nextCheckpointOffset = saved.offset + kCheckpointBytes;
    receivedSize = saved.offset;
    packageCheckpointOffset = saved.packageOffset;
    packageReceivedSize = saved.packageOffset;
  } else {
    ownerConnection = message.connection;
    setStatus(String("ERASING|") + size);
    // 顺序写入会在数据到达时逐段擦除，避免一次性擦完整个 OTA 分区时阻塞 BLE。
    beginResult = esp_ota_begin(targetPartition, OTA_WITH_SEQUENTIAL_WRITES,
                               &otaHandle);
    receivedSize = 0;
    packageReceivedSize = 0;
    checkpointOffset = 0;
    packageCheckpointOffset = 0;
    nextCheckpointOffset = kCheckpointBytes;
    memset(imageHeader, 0, sizeof(imageHeader));
    imageHeaderBytes = 0;
  }
  if (beginResult != ESP_OK) {
    targetPartition = nullptr;
    otaHandle = 0;
    setStatus("ERROR|FLASH_BEGIN"); return;
  }
  if (!resumable) {
    currentCheckpoint = {};
    currentCheckpoint.imageSize = size;
    currentCheckpoint.packageSize = packageSize;
    currentCheckpoint.partitionAddress = targetPartition->address;
    currentCheckpoint.compressedPackage = packageMode ? 1 : 0;
    currentCheckpoint.deltaPackage = deltaMode ? 1 : 0;
    currentCheckpoint.allowDowngrade = allowDowngrade ? 1 : 0;
    strlcpy(currentCheckpoint.taskId, taskId.c_str(), sizeof(currentCheckpoint.taskId));
    const String deviceId = getStableDeviceId();
    strlcpy(currentCheckpoint.stableDeviceId, deviceId.c_str(), sizeof(currentCheckpoint.stableDeviceId));
    strlcpy(currentCheckpoint.version, version.c_str(), sizeof(currentCheckpoint.version));
    strlcpy(currentCheckpoint.digest, digest.c_str(), sizeof(currentCheckpoint.digest));
    strlcpy(currentCheckpoint.packageDigest, packageDigest.c_str(), sizeof(currentCheckpoint.packageDigest));
    strlcpy(currentCheckpoint.baseDigest, baseDigest.c_str(), sizeof(currentCheckpoint.baseDigest));
    strlcpy(currentCheckpoint.baseVersion, baseVersion.c_str(), sizeof(currentCheckpoint.baseVersion));
    currentCheckpoint.baseImageSize = baseSize;
    if (!saveCheckpoint(0)) {
      abortUpdate("CHECKPOINT_INIT", true); return;
    }
  }
  ownerConnection = message.connection;
  mbedtls_sha256_init(&shaContext);
  mbedtls_sha256_starts(&shaContext, 0);
  if (resumable) {
    uint8_t buffer[1024];
    uint32_t offset = 0;
    while (offset < saved.offset) {
      const size_t count = min(sizeof(buffer), static_cast<size_t>(saved.offset - offset));
      if (esp_partition_read(targetPartition, offset, buffer, count) != ESP_OK) {
        abortUpdate("RESUME_READ", true); return;
      }
      if (!updateSha256InChunks(&shaContext, buffer, count)) {
        abortUpdate("RESUME_HASH", true); return;
      }
      offset += count;
    }
    imageHeaderBytes = sizeof(imageHeader);
    otaPackageHeaderBytes = packageMode ? kOtaPackageFixedHeaderBytes : 0;
    otaPackageHeaderValid = packageMode;
    if (packageMode) {
      memcpy(otaPackageHeader + kOtaSignatureOffsetFull,
             saved.signature, kOtaSignatureBytes);
    }
    otaPackageBlockCount = packageMode
        ? (expectedSize + kOtaPackageBlockBytes - 1) / kOtaPackageBlockBytes
        : 0;
    otaPackageBlockIndex = packageMode ? saved.offset / kOtaPackageBlockBytes : 0;
    otaPackageDescriptorBytes = otaPackageBlockBytes = 0;
    otaDeltaHeaderReceived = deltaMode ? kOtaDeltaHeaderBytes : 0;
    otaDeltaHeaderValid = deltaMode;
    if (deltaMode) {
      memcpy(otaDeltaHeader + kOtaSignatureOffsetDelta,
             saved.signature, kOtaSignatureBytes);
    }
    otaDeltaRecordCount = deltaMode
        ? (expectedSize + kOtaDeltaBlockBytes - 1) / kOtaDeltaBlockBytes
        : 0;
    otaDeltaRecordIndex = deltaMode ? saved.offset / kOtaDeltaBlockBytes : 0;
    otaDeltaRecordHeaderReceived = otaDeltaRecordDataReceived = 0;
  }
  if (!resumable) {
    otaPackageHeaderBytes = 0;
    otaPackageHeaderValid = false;
    otaPackageBlockCount = otaPackageBlockIndex = otaPackageBlockBytes = 0;
    otaPackageDescriptorBytes = 0;
    memset(otaPackageHeader, 0, sizeof(otaPackageHeader));
    otaDeltaHeaderReceived = 0;
    otaDeltaHeaderValid = false;
    otaDeltaRecordCount = otaDeltaRecordIndex = 0;
    otaDeltaRecordHeaderReceived = otaDeltaRecordDataReceived = 0;
    memset(otaDeltaHeader, 0, sizeof(otaDeltaHeader));
  }
  active = true;
  lastActivityAt = millis();
  // 更新期间禁止风机输出，避免重启或供电波动时继电器状态不确定。
  fanControllerSetLockedOff(millis(), FanControlSource::OTA);
  setStatus(String(resumable ? "RESUMED|" : "READY|") + expectedVersion + "|" + expectedPackageSize + "|" + packageReceivedSize + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
}

void processMessage(const Message& message) {
  if (message.type == MessageType::CONTROL) {
    const String command(reinterpret_cast<const char*>(message.bytes), message.length);
    if (command == "STATUS") {
      if (!active || ownerConnection == message.connection) ownerConnection = message.connection;
      setStatus(lastStatus);
      return;
    }
    if (command == "CAPS") {
      if (!active || ownerConnection == message.connection) ownerConnection = message.connection;
      OtaBleAdapter::publishStatus(String("CAPS|2|resume=1|pause=1|install=1|maxChunk=512|window=4|checkpoint=16384|sha256=1|signature=1|signatureFormat=1|downgrade=1|package=1|packageFormat=2|delta=1|deltaFormat=2|partitionProfile=") + FIRMWARE_PARTITION_PROFILE + "|deviceId=" + getStableDeviceId(), message.connection);
      return;
    }
    if (command == "INFO") {
      if (active && ownerConnection != message.connection) {
        OtaBleAdapter::publishStatus("ERROR|NOT_OWNER", message.connection);
        return;
      }
      const esp_partition_t* running = esp_ota_get_running_partition();
      esp_image_metadata_t metadata{};
      const esp_partition_pos_t position = {
          .offset = running ? running->address : 0,
          .size = running ? running->size : 0,
      };
      esp_app_desc_t description{};
      if (!running || esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) != ESP_OK ||
          esp_ota_get_partition_description(running, &description) != ESP_OK) {
        OtaBleAdapter::publishStatus("ERROR|RUNNING_IMAGE_INFO", message.connection);
        return;
      }
      const String digest = calculatePartitionDigest(running, metadata.image_len);
      if (digest.isEmpty()) {
        OtaBleAdapter::publishStatus("ERROR|RUNNING_IMAGE_HASH", message.connection);
        return;
      }
      if (!active) ownerConnection = message.connection;
      OtaBleAdapter::publishStatus(String("INFO|") + description.version + "|" + metadata.image_len + "|" +
                digest + "|" + FIRMWARE_PARTITION_PROFILE + "|" +
                statisticsStoreSchemaVersion() + "|" + getStableDeviceId(), message.connection);
      return;
    }
    if (command == "QUERY") {
      if (!active || ownerConnection == message.connection) ownerConnection = message.connection;
      OtaCheckpoint saved{};
      const bool hasSavedCheckpoint = loadCheckpoint(&saved);
      if (active) setStatus(String("UPLOADING|") + ((compressedPackage || deltaPackage) ? packageReceivedSize : receivedSize) + "|" + ((compressedPackage || deltaPackage) ? packageCheckpointOffset : checkpointOffset) + "|" + ((compressedPackage || deltaPackage) ? expectedPackageSize : expectedSize) + "|" + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId + "|" + OtaBleAdapter::availableDataSlots());
      else if (installedTask) setStatus(String("INSTALLED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
      else if (lastStatus.startsWith("ERROR|BOOT_ROLLBACK")) setStatus(lastStatus);
      else if (verifiedForInstall) {
        ownerConnection = message.connection;
        setStatus(String("VERIFIED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
      }
      else if (lastStatus.startsWith("PAUSED|") && lastStatus.indexOf("|ERROR:") >= 0) setStatus(lastStatus);
      else if (hasSavedCheckpoint &&
               checkpointStore.isRejectedImage(String(saved.digest))) {
        setStatus(String("ERROR|BOOT_ROLLBACK|") + saved.taskId + "|" +
                  saved.version + "|" + saved.digest);
      }
      else if (hasSavedCheckpoint) setStatus(String("PAUSED|") + ((saved.compressedPackage || saved.deltaPackage) ? saved.packageOffset : saved.offset) + "|" + ((saved.compressedPackage || saved.deltaPackage) ? saved.packageSize : saved.imageSize) + "|" + saved.version + "|" + saved.digest + "|" + saved.taskId + "|" + saved.stableDeviceId + "|" + saved.packageDigest);
      else setStatus("IDLE");
      return;
    }
    if (command.startsWith("PAUSE|")) {
      const String taskId = commandTaskId(command);
      if (active && message.connection == ownerConnection &&
          taskId == String(currentCheckpoint.taskId)) {
        suspendUpdate("USER");
      } else {
        OtaCheckpoint saved{};
        if (!active && loadCheckpoint(&saved) && taskId == String(saved.taskId)) {
          ownerConnection = message.connection;
          setStatus(String("PAUSED|") + ((saved.compressedPackage || saved.deltaPackage) ? saved.packageOffset : saved.offset) + "|" + ((saved.compressedPackage || saved.deltaPackage) ? saved.packageSize : saved.imageSize) + "|" +
                    saved.version + "|" + saved.digest + "|" + saved.taskId + "|" + saved.stableDeviceId + "|" + saved.packageDigest + "|ALREADY_PAUSED");
        } else {
          setStatus("ERROR|TASK_MISMATCH");
        }
      }
      return;
    }
    if (command.startsWith("ABANDON|")) {
      if (active) {
        OtaBleAdapter::publishStatus("ERROR|BUSY", message.connection);
        return;
      }
      // 暂停会清除 ownerConnection；放弃命令来自当前连接，需将确认发回该连接。
      ownerConnection = message.connection;
      const String taskId = commandTaskId(command);
      OtaCheckpoint saved{};
      if (!loadCheckpoint(&saved)) {
        if (checkpointStore.isRejectedTask(taskId.c_str()) &&
            checkpointStore.clearRejectedImage(taskId.c_str())) setStatus("IDLE");
        else setStatus("ERROR|TASK_MISMATCH");
        return;
      }
      if (taskId != String(saved.taskId)) { setStatus("ERROR|TASK_MISMATCH"); return; }
      if (saved.installIntent && !installedTask && !installCandidateWasRejected()) {
        // 安装意图先于启动分区切换持久化。只有 boot 分区已切到候选槽，
        // 才算提交安装；若在两步之间断电，仍允许用户放弃或替换任务。
        const esp_partition_t* bootPartition = esp_ota_get_boot_partition();
        if (!bootPartition || bootPartition->address == saved.partitionAddress) {
          setStatus("ERROR|INSTALL_COMMITTED"); return;
        }
      }
      if (!clearCheckpoint()) {
        setStatus("ERROR|CHECKPOINT_CLEAR");
        return;
      }
      if (!checkpointStore.clearRejectedImage(taskId.c_str())) {
        setStatus("ERROR|ROLLBACK_RECORD_CLEAR");
        return;
      }
      targetPartition = nullptr;
      verifiedForInstall = false;
      installedTask = false;
      setStatus("IDLE");
      return;
    }
    if (command.startsWith("INSTALL|")) {
      const String taskId = commandTaskId(command);
      const int firstSeparator = command.indexOf('|');
      const int secondSeparator = firstSeparator < 0
          ? -1 : command.indexOf('|', firstSeparator + 1);
      const int thirdSeparator = secondSeparator < 0
          ? -1 : command.indexOf('|', secondSeparator + 1);
      if (secondSeparator < 0 || thirdSeparator < 0) {
        setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
      }
      const String downgradeConfirmation =
          command.substring(secondSeparator + 1, thirdSeparator);
      if (downgradeConfirmation != "0" && downgradeConfirmation != "1") {
        setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
      }
      const bool allowDowngrade = downgradeConfirmation == "1";
      if (allowDowngrade != (currentCheckpoint.allowDowngrade == 1)) {
        setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
      }
      if (installedTask && taskId == String(currentCheckpoint.taskId)) {
        setStatus(String("INSTALLED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
        return;
      }
      if (!verifiedForInstall || !targetPartition ||
          message.connection != ownerConnection ||
          taskId != String(currentCheckpoint.taskId)) {
        setStatus("ERROR|NOT_VERIFIED"); return;
      }
      if (!verifyCandidatePartition()) {
        verifiedForInstall = false;
        fanControllerSetAuto(millis(), FanControlSource::OTA);
        setStatus("ERROR|CANDIDATE_CHANGED");
        return;
      }
      const esp_partition_t* runningPartition = esp_ota_get_running_partition();
      esp_app_desc_t runningDescription{};
      if (!runningPartition ||
          esp_ota_get_partition_description(runningPartition, &runningDescription) != ESP_OK ||
          (compareSemanticVersions(expectedVersion, String(runningDescription.version)) < 0) !=
              allowDowngrade) {
        setStatus("ERROR|DOWNGRADE_CONFIRMATION"); return;
      }
      currentCheckpoint.installIntent = 1;
      if (!saveCheckpoint(expectedSize)) {
        currentCheckpoint.installIntent = 0;
        setStatus("ERROR|INSTALL_INTENT");
        return;
      }
      fanControllerSetLockedOff(millis(), FanControlSource::OTA);
      if (esp_ota_set_boot_partition(targetPartition) != ESP_OK) {
        currentCheckpoint.installIntent = 0;
        saveCheckpoint(expectedSize);
        setStatus("ERROR|SET_BOOT_PARTITION");
        fanControllerSetAuto(millis(), FanControlSource::OTA);
        return;
      }
      verifiedForInstall = false;
      installedTask = false;
      setStatus(String("INSTALLING|") + expectedVersion);
      Serial.printf("OTA install committed: version=%s\n", expectedVersion.c_str());
      delay(300);
      ESP.restart();
      return;
    }
    if (command == "ABORT") {
      if (active && message.connection == ownerConnection) suspendUpdate("ABORT");
      else setStatus("ERROR|NOT_OWNER");
      return;
    }
    if (command.startsWith("BEGIN|") || command.startsWith("BEGIN2|") ||
        command.startsWith("BEGIN_DELTA|")) {
      beginUpdate(message); return;
    }
    setStatus("ERROR|COMMAND");
    return;
  }
  if (!active) return;
  if (message.connection != ownerConnection) {
    setStatus("ERROR|NOT_OWNER"); return;
  }
  const uint8_t* frame = message.bytes;
  const size_t payloadLength = message.length - 8;
  const uint32_t offset = readU32(frame);
  const uint32_t expectedCrc = readU32(frame + 4);
  const uint8_t* payload = frame + 8;
  const uint32_t transferSize = (compressedPackage || deltaPackage)
      ? expectedPackageSize : expectedSize;
  const uint32_t currentOffset = (compressedPackage || deltaPackage)
      ? packageReceivedSize : receivedSize;
  if (payloadLength == 0 || offset + payloadLength > transferSize ||
      crc32(payload, payloadLength) != expectedCrc) {
    abortUpdate("FRAME", true); return;
  }
  if (compressedPackage) {
    if (offset != currentOffset) {
      setStatus(String("OFFSET|") + currentOffset);
      return;
    }
    if (!consumeOtaPackageBytes(payload, payloadLength)) {
      abortUpdate("PACKAGE_BLOCK", true); return;
    }
    lastActivityAt = millis();
    setStatus(String("ACK|") + packageReceivedSize + "|" + expectedPackageSize + "|" +
              OtaBleAdapter::availableDataSlots());
    return;
  }
  if (deltaPackage) {
    if (offset != currentOffset) {
      setStatus(String("OFFSET|") + currentOffset);
      return;
    }
    if (!consumeOtaDeltaBytes(payload, payloadLength)) {
      abortUpdate("DELTA_RECORD", true); return;
    }
    lastActivityAt = millis();
    setStatus(String("ACK|") + packageReceivedSize + "|" + expectedPackageSize + "|" +
              OtaBleAdapter::availableDataSlots());
    return;
  }
  size_t duplicateBytes = 0;
  if (offset < receivedSize) {
    duplicateBytes = min(payloadLength, static_cast<size_t>(receivedSize - offset));
    uint8_t duplicateBuffer[kMaxChunkBytes] = {};
    if (esp_partition_read(targetPartition, offset, duplicateBuffer, duplicateBytes) != ESP_OK ||
        memcmp(duplicateBuffer, payload, duplicateBytes) != 0) {
      abortUpdate("FRAME_CONFLICT", true); return;
    }
    if (duplicateBytes == payloadLength) {
      lastActivityAt = millis();
      const UBaseType_t available = OtaBleAdapter::availableDataSlots();
      setStatus(String("ACK|") + receivedSize + "|" + expectedSize + "|" + available);
      return;
    }
  } else if (offset > receivedSize) {
    setStatus(String("OFFSET|") + receivedSize);
    return;
  }
  const uint8_t* newPayload = payload + duplicateBytes;
  const size_t newPayloadLength = payloadLength - duplicateBytes;
  size_t payloadOffset = 0;
  if (imageHeaderBytes < sizeof(imageHeader)) {
    const size_t copyLength = min(newPayloadLength, sizeof(imageHeader) - imageHeaderBytes);
    memcpy(imageHeader + imageHeaderBytes, newPayload, copyLength);
    imageHeaderBytes += copyLength;
    payloadOffset += copyLength;
    if (imageHeaderBytes == sizeof(imageHeader)) {
      esp_image_header_t header{};
      memcpy(&header, imageHeader, sizeof(header));
      if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32C3) {
        abortUpdate("IMAGE_TARGET", true); return;
      }
      if (esp_ota_write(otaHandle, imageHeader, sizeof(imageHeader)) != ESP_OK) {
        abortUpdate("FLASH_WRITE", true); return;
      }
    }
  }
  if (payloadOffset < newPayloadLength &&
      esp_ota_write(otaHandle, newPayload + payloadOffset,
                    newPayloadLength - payloadOffset) != ESP_OK) {
    abortUpdate("FLASH_WRITE", true); return;
  }
  if (!updateSha256InChunks(&shaContext, newPayload, newPayloadLength)) {
    abortUpdate("HASH_UPDATE", true); return;
  }
  receivedSize += newPayloadLength;
  const uint32_t durableOffset = (receivedSize / kCheckpointBytes) * kCheckpointBytes;
  if (durableOffset < expectedSize &&
      durableOffset >= nextCheckpointOffset && durableOffset > checkpointOffset &&
      !saveCheckpoint(durableOffset)) {
    abortUpdate("CHECKPOINT_WRITE", true); return;
  }
  lastActivityAt = millis();
  const UBaseType_t available = OtaBleAdapter::availableDataSlots();
  setStatus(String("ACK|") + receivedSize + "|" + expectedSize + "|" + available);
}

void finishUpdate(uint16_t connection, const String& taskId) {
  if (taskId != String(currentCheckpoint.taskId)) {
    setStatus("ERROR|TASK_MISMATCH"); return;
  }
  if (verifiedForInstall) {
    setStatus(String("VERIFIED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
    return;
  }
  if (!active || connection != ownerConnection) { setStatus("ERROR|NOT_OWNER"); return; }
  if (receivedSize != expectedSize ||
      ((compressedPackage || deltaPackage) &&
       (packageReceivedSize != expectedPackageSize ||
        (compressedPackage &&
         (!otaPackageHeaderValid || otaPackageBlockIndex != otaPackageBlockCount)) ||
        (deltaPackage &&
         (!otaDeltaHeaderValid || otaDeltaRecordIndex != otaDeltaRecordCount)))) ) {
    abortUpdate("INCOMPLETE", true); return;
  }
  uint8_t digest[32] = {};
  mbedtls_sha256_finish(&shaContext, digest);
  mbedtls_sha256_free(&shaContext);
  active = false;
  if (!digestToHex(digest).equalsIgnoreCase(expectedDigest)) {
    abortUpdate("HASH", true); return;
  }
  const uint8_t* signature = deltaPackage
      ? otaDeltaHeader + kOtaSignatureOffsetDelta
      : otaPackageHeader + kOtaSignatureOffsetFull;
  if (!verifyImageSignature(digest, signature)) {
    abortUpdate("SIGNATURE", true); return;
  }
  if (esp_ota_end(otaHandle) != ESP_OK) {
    otaHandle = 0;
    abortUpdate("FLASH_FINALIZE", true); return;
  }
  otaHandle = 0;
  active = false;
  if (!verifyPartitionDigest(targetPartition, expectedSize, expectedDigest)) {
    abortUpdate("FLASH_HASH", true); return;
  }
  esp_app_desc_t imageDescription{};
  if (esp_ota_get_partition_description(targetPartition, &imageDescription) != ESP_OK ||
      expectedVersion != String(imageDescription.version)) {
    abortUpdate("VERSION_MISMATCH", true); return;
  }
  verifiedForInstall = true;
  if (compressedPackage || deltaPackage) packageCheckpointOffset = expectedPackageSize;
  if (!saveCheckpoint(expectedSize)) {
    setStatus("ERROR|CHECKPOINT_FINAL");
    fanControllerSetAuto(millis(), FanControlSource::OTA);
    return;
  }
  if (taskId == "legacy") {
    // 旧协议在 FINISH 后立即安装；持久化意图后再切换启动分区，兼容首刷 OTA V2。
    currentCheckpoint.installIntent = 1;
    if (!saveCheckpoint(expectedSize) ||
        esp_ota_set_boot_partition(targetPartition) != ESP_OK) {
      currentCheckpoint.installIntent = 0;
      saveCheckpoint(expectedSize);
      verifiedForInstall = true;
      fanControllerSetAuto(millis(), FanControlSource::OTA);
      setStatus("ERROR|LEGACY_INSTALL");
      return;
    }
    verifiedForInstall = false;
    setStatus(String("DONE|") + expectedVersion);
    Serial.printf("Legacy OTA install committed: version=%s\n", expectedVersion.c_str());
    delay(300);
    ESP.restart();
    return;
  }
  fanControllerSetAuto(millis(), FanControlSource::OTA);
  setStatus(String("VERIFIED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId);
  Serial.printf("OTA verified: version=%s size=%lu\n", expectedVersion.c_str(),
                static_cast<unsigned long>(expectedSize));
}
}  // namespace

void otaManagerBegin() {
  checkpointStoreReady = checkpointStore.begin();
  if (!checkpointStoreReady) {
    lastStatus = "ERROR|CHECKPOINT_STORE";
    Serial.println("OTA disabled: checkpoint storage unavailable");
  }
  OtaCheckpoint saved{};
  if (loadCheckpoint(&saved) && saved.offset == saved.imageSize && saved.imageSize > 0) {
    currentCheckpoint = saved;
    expectedSize = saved.imageSize;
    expectedVersion = saved.version;
    expectedDigest = saved.digest;
    targetPartition = findAppPartition(saved.partitionAddress);
    const esp_partition_t* runningPartition = esp_ota_get_running_partition();
    installedTask = targetPartition && runningPartition &&
                    targetPartition->address == runningPartition->address;
    verifiedForInstall = targetPartition != nullptr && !installedTask;
    const bool previouslyRejected = checkpointStore.isRejectedImage(
        String(currentCheckpoint.digest));
    if (verifiedForInstall && previouslyRejected) {
      verifiedForInstall = false;
      lastStatus = String("ERROR|BOOT_ROLLBACK|") + currentCheckpoint.taskId +
                   "|" + currentCheckpoint.version + "|" + currentCheckpoint.digest;
    } else if (verifiedForInstall && currentCheckpoint.installIntent) {
      if (installCandidateWasRejected()) {
        verifiedForInstall = false;
        if (persistBootRollback()) {
          Serial.println("OTA candidate was rejected by the bootloader; rollback identity persisted");
        } else {
          lastStatus = "ERROR|BOOT_ROLLBACK_RECORD";
          Serial.println("OTA rollback detected but could not persist rejection identity");
        }
      } else {
        // 安装意图可能在切换启动分区前断电。启动恢复只报告待安装状态，
        // 由 App 查询并再次取得用户确认，不能在启动阶段自动重试重启。
        lastStatus = String("VERIFIED|") + expectedVersion + "|" + expectedDigest + "|" +
                     currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId;
      }
    }
    if (installedTask) lastStatus = String("INSTALLED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId;
    else if (lastStatus.startsWith("ERROR|BOOT_ROLLBACK")) {
      // 保留回滚终态，不能被后续 VERIFIED 分支覆盖。
    } else if (verifiedForInstall) {
      lastStatus = String("VERIFIED|") + expectedVersion + "|" + expectedDigest + "|" + currentCheckpoint.taskId + "|" + currentCheckpoint.stableDeviceId;
    }
  }
  mbedtls_sha256_init(&shaContext);
  Serial.println("BLE OTA ready; authorization follows the active BLE session");
}

void otaManagerConfirmBoot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) return;
  esp_app_desc_t description{};
  if (esp_ota_get_partition_description(running, &description) != ESP_OK ||
      strcmp(description.version, FIRMWARE_VERSION) != 0) {
    Serial.println("OTA boot self-check failed; image remains unconfirmed for rollback");
    return;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    Serial.printf("OTA boot confirmed: version=%s\n", description.version);
  } else {
    Serial.println("OTA boot confirmation failed; bootloader rollback remains enabled");
  }
}

void otaManagerUpdate() {
  const uint32_t now = millis();
  bool refreshReadableStatus = false;
  portENTER_CRITICAL(&connectionStateMux);
  if (connectStatusPending) {
    connectStatusPending = false;
    refreshReadableStatus = true;
  }
  portEXIT_CRITICAL(&connectionStateMux);
  if (refreshReadableStatus) {
    if (!verifiedForInstall && !installedTask &&
        (lastStatus.isEmpty() || lastStatus == "READY")) {
      lastStatus = "READY";
    }
    OtaBleAdapter::setReadableStatus(lastStatus);
  }
  OtaBleAdapter::drainCallbackStatuses();
  if (disconnectPending) {
    disconnectPending = false;
    if (active) suspendUpdate("DISCONNECTED");
  }
  if (active && now - lastActivityAt > kTransferTimeoutMs) {
    suspendUpdate("TIMEOUT");
  }
  Message message{};
  bool finishRequested = false;
  uint16_t finishConnection = BLE_HS_CONN_HANDLE_NONE;
  String finishTaskId;
  while (OtaBleAdapter::receiveControl(&message)) {
    const String command(reinterpret_cast<const char*>(message.bytes), message.length);
    if (message.type == MessageType::CONTROL &&
        (command.startsWith("FINISH|") || command == "FINISH")) {
      finishRequested = true;
      finishConnection = message.connection;
      finishTaskId = command == "FINISH" ? String("legacy") : commandTaskId(command);
    }
    else processMessage(message);
  }
  while (OtaBleAdapter::receiveData(&message)) processMessage(message);
  if (finishRequested) finishUpdate(finishConnection, finishTaskId);
  if (!pendingAckStatus.isEmpty() &&
      pendingAckConnection == ownerConnection) {
    const String status = pendingAckStatus;
    const uint16_t connection = pendingAckConnection;
    pendingAckStatus = "";
    pendingAckConnection = BLE_HS_CONN_HANDLE_NONE;
    OtaBleAdapter::publishStatus(status, connection);
  }
}

void otaManagerOnConnect() {
  portENTER_CRITICAL(&connectionStateMux);
  if (!active) {
    authorized = false;
    authorizedConnection = BLE_HS_CONN_HANDLE_NONE;
  }
  connectStatusPending = true;
  portEXIT_CRITICAL(&connectionStateMux);
  Serial.println("BLE connected; OTA awaits encrypted bonded session");
}

void otaManagerOnSecureSession(uint16_t connection) {
  if (connection == BLE_HS_CONN_HANDLE_NONE) return;
  portENTER_CRITICAL(&connectionStateMux);
  if (!active || ownerConnection == connection) {
    authorizedConnection = connection;
    authorized = true;
  }
  portEXIT_CRITICAL(&connectionStateMux);
  Serial.printf("OTA authorized for bonded connection %u\n", connection);
}

void otaManagerOnDisconnect(uint16_t connection) {
  // BLE 回调在 NimBLE 任务中执行；只投递标志，Flash 清理留给主循环。
  portENTER_CRITICAL(&connectionStateMux);
  if (authorizedConnection == connection) {
    authorized = false;
    authorizedConnection = BLE_HS_CONN_HANDLE_NONE;
  }
  if (ownerConnection == connection) disconnectPending = true;
  portEXIT_CRITICAL(&connectionStateMux);
}

bool otaManagerIsActive() { return active; }

bool otaManagerIsOwnerConnection(uint16_t connection) {
  return ownerConnection == connection;
}

String otaManagerStatusJson() {
  bool sessionAuthorized = false;
  portENTER_CRITICAL(&connectionStateMux);
  sessionAuthorized = authorized;
  portEXIT_CRITICAL(&connectionStateMux);
  return String("{\"authorized\":") + (sessionAuthorized ? "true" : "false") +
         ",\"active\":" + (active ? "true" : "false") +
         ",\"status\":\"" + lastStatus + "\",\"firmwareVersion\":\"" +
         FIRMWARE_VERSION + "\"}";
}
