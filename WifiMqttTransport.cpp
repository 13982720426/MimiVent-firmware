#include "WifiMqttTransport.h"

#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include "ControlApi.h"
#include "OtaManager.h"
#include "StatisticsStore.h"
#include "WifiBindingStore.h"
#include "WifiDeviceIdentity.h"
#include "WifiControlService.h"
#include "WifiStateRevision.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_crc.h>
#include <mqtt_client.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <utility>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <mbedtls/md.h>

namespace {
constexpr char kNamespace[] = "mv-mqtt";
constexpr char kConfigKey[] = "broker";
constexpr uint32_t kConfigMagic = 0x4D564D31;
// state 负载约 563 字节，command/ack 也远小于 1 KB；收到更大消息按越界丢弃。
// 单条消息上限。state 随字段增加已到 1015 B，加上 topic 与封装开销约 1076 B，
// 原来的 1024 会把状态发布整条丢弃（串口 "state message exceeds 1024 bytes"），
// NAS 收不到状态后就判设备离线、控制命令也无人接收。
constexpr size_t kMaxMessage = 1536;
// data 嵌套在 JSON 字符串中，引号/反斜杠会再次转义；不能只算原始字节。
// 为 queryId、分片编号等信封字段保留 192 B，切片时累计转义后的长度。
//
// 改动此值时务必同步同级 MimiVent-gateway/server.mjs 的 query-reply 两处上限
// （payload.length 与 reply.data 的字节检查）。gateway 侧超限会静默丢弃
// 整片且不回 ack，设备将无限重发直到 30 秒超时，表现为统计查询永久转圈。
constexpr size_t kQueryChunkBytes = 1280;
constexpr size_t kQueryEncodedDataBudget = kMaxMessage - 192;
constexpr uint32_t kQueryChunkPacingMs = 20;
constexpr size_t kMaxQueryResponseBytes = 8192;
constexpr size_t kMaxQueryChunks = 72;
constexpr uint32_t kQueryReplyTimeoutMs = 30000;
constexpr uint32_t kQueryAckTimeoutMs = 1200;
constexpr uint32_t kTimeReplyTimeoutMs = 2000;
uint8_t freshnessKey[32]{};
bool freshnessKeyInitialized = false;
int commandSubscription = -1;
int querySubscription = -1;
int queryAckSubscription = -1;
int timeReplySubscription = -1;
std::atomic<bool> commandReady{false};
bool queryReady = false;
bool commandSubscribed = false;
bool querySubscribed = false;
bool queryAckSubscribed = false;
std::atomic<bool> timeReplySubscribed{false};
constexpr uint32_t kPublishIntervalMs = 5000;
// 状态变化检测周期。每轮都要跑一次 /api/status 序列化 + cJSON 解析 + 指纹，
// 250 ms 相当于每秒 4 轮分配 churn，是持续性的碎片来源。
// 心跳本身是 5 秒、App 侧轮询是 3 秒，1 秒的检测延迟对用户不可感知，
// 而 churn 直接降到原来的 1/4。
constexpr uint32_t kStateChangeCheckMs = 1000;
// outbox 需容纳一条大状态（约 1100 B）加 ACK/查询回执的余量。
constexpr size_t kOutboxLimitBytes = 3072;
// 状态负载上限是 1 KiB；队列预算还需容纳 topic 和 MQTT 包头。
// 留出 128 B 封装空间，同时给控制回执保留至少 768 B 的总队列余量。
constexpr size_t kTelemetryBudgetBytes = kMaxMessage + 128;
std::atomic<uint32_t> queueRejected{0};
std::atomic<uint32_t> enqueueFailures{0};

struct BrokerConfig {
  uint32_t magic;
  uint16_t port;
  char host[64];
  char username[33];
  char password[65];
  uint32_t checksum;
};

BrokerConfig config{};
bool configured = false;
std::atomic<bool> connected{false};
bool restartRequested = false;
uint32_t lastPublishAt = 0;
uint32_t lastStateCheckAt = 0;
std::atomic<bool> stateSnapshotRequested{true};
WifiStateRevision stateRevision;
Preferences preferences;
esp_mqtt_client_handle_t client = nullptr;
char brokerUri[96]{};
char clientId[48]{};
char commandTopic[96]{};
char queryTopic[96]{};
char stateTopic[96]{};
char availabilityTopic[96]{};
char ackTopic[96]{};
char queryReplyTopic[96]{};
char queryAckTopic[96]{};
char timeRequestTopic[96]{};
char timeReplyTopic[96]{};
char incomingTopic[96]{};
char incomingPayload[kMaxMessage + 1]{};
size_t incomingLength = 0;
size_t incomingExpected = 0;
bool incomingRetained = false;
SemaphoreHandle_t queryReplyMutex = nullptr;
String pendingQueryBody;
char pendingQueryId[37]{};
uint16_t pendingQueryStatus = 200;
uint16_t pendingQueryChunkIndex = 0;
uint16_t pendingQueryChunkCount = 0;
uint16_t pendingQueryPublishFailures = 0;
uint8_t pendingQueryAckRetries = 0;
size_t pendingQueryOffsets[kMaxQueryChunks + 1]{};
uint32_t pendingQueryStartedAt = 0;
uint32_t pendingQueryLastActionAt = 0;
bool pendingQueryReply = false;
bool pendingQueryAwaitingAck = false;
std::atomic<int32_t> acknowledgedQueryChunk{-1};
char pendingTimeNonce[33]{};
uint32_t pendingTimeRequestAt = 0;
uint32_t nextTimeRequestAt = 0;
SemaphoreHandle_t timeSyncMutex = nullptr;
bool timeRequestInFlight = false;
bool timeSyncCompleteThisSession = false;

uint32_t configChecksum(const BrokerConfig& value) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&value),
                      offsetof(BrokerConfig, checksum));
}

bool readConfig() {
  if (!preferences.begin(kNamespace, true)) return false;
  const size_t length = preferences.getBytesLength(kConfigKey);
  BrokerConfig candidate{};
  const size_t read = length == sizeof(candidate)
      ? preferences.getBytes(kConfigKey, &candidate, sizeof(candidate)) : 0;
  preferences.end();
  if (read != sizeof(candidate) || candidate.magic != kConfigMagic ||
      candidate.checksum != configChecksum(candidate) || candidate.port == 0 ||
      candidate.host[0] == '\0' || candidate.username[0] == '\0' ||
      candidate.password[0] == '\0') return false;
  config = candidate;
  return true;
}

bool validPrivateHost(const char* host) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  char tail = '\0';
  if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
      a > 255 || b > 255 || c > 255 || d > 255) return false;
  return a == 10 || (a == 172 && b >= 16 && b <= 31) ||
      (a == 192 && b == 168);
}

bool validText(const char* value, size_t minLength, size_t maxLength) {
  const size_t length = strnlen(value, maxLength + 1);
  if (length < minLength || length > maxLength) return false;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t ch = static_cast<uint8_t>(value[i]);
    if (ch < 0x21 || ch > 0x7e) return false;
  }
  return true;
}

bool saveConfig(const BrokerConfig& source) {
  BrokerConfig value = source;
  value.magic = kConfigMagic;
  value.checksum = configChecksum(value);
  if (!preferences.begin(kNamespace, false)) return false;
  const size_t written = preferences.putBytes(kConfigKey, &value, sizeof(value));
  preferences.end();
  if (written != sizeof(value)) return false;
  config = value;
  configured = true;
  restartRequested = true;
  return true;
}

String errorJson(const char* error) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return "{\"error\":\"OUT_OF_MEMORY\"}";
  cJSON_AddStringToObject(root, "error", error);
  char* raw = cJSON_PrintUnformatted(root);
  String result = raw ? raw : "{\"error\":\"OUT_OF_MEMORY\"}";
  cJSON_free(raw);
  cJSON_Delete(root);
  return result;
}

bool publishText(const char* topic, const char* payload, int qos, bool retain) {
  if (!client || !connected) return false;
  // 由 MQTT 任务发送，避免 socket 超时阻塞 PIR/风机主循环。
  // 状态只用一半预算，给 ready/执行结果 ACK 保留空间；SDK 再执行并发硬上限。
  const size_t bytes=strlen(topic)+strlen(payload)+16;
  const int queued=esp_mqtt_client_get_outbox_size(client);
  const size_t budget=retain?kTelemetryBudgetBytes:kOutboxLimitBytes;
  if(queued<0||bytes>budget||static_cast<size_t>(queued)>budget-bytes){
    ++queueRejected;return false;
  }
  const int result=esp_mqtt_client_enqueue(client, topic, payload, 0, qos, retain, true);
  if(result<0){++enqueueFailures;return false;}
  return true;
}

bool publishAck(const char* requestId, uint16_t status, const String& body, const char* phase=nullptr) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {++enqueueFailures;return false;}
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddStringToObject(root, "requestId", requestId ? requestId : "");
  cJSON_AddStringToObject(root, "status", phase ? phase : (status < 400 ? "applied" : "rejected"));
  cJSON_AddNumberToObject(root, "httpStatus", status);
  cJSON* parsed = cJSON_ParseWithLength(body.c_str(), body.length());
  if (parsed) cJSON_AddItemToObject(root, "body", parsed);
  else cJSON_AddStringToObject(root, "error", "INVALID_RESPONSE");
  char* raw = cJSON_PrintUnformatted(root);
  bool queued=false;
  if (raw) {
    queued=publishText(ackTopic, raw, 1, false);
    cJSON_free(raw);
  }else ++enqueueFailures;
  cJSON_Delete(root);
  return queued;
}

bool requiredString(cJSON* root, const char* name, const char** value) {
  cJSON* item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsString(item) || !item->valuestring) return false;
  *value = item->valuestring;
  return true;
}

bool validQueryId(const char* value) {
  if (!value || strlen(value) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? value[i] != '-' : !isxdigit(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

String queryValue(const String& path, const char* key) {
  const int queryAt = path.indexOf('?');
  if (queryAt < 0) return String();
  const String prefix = String(key) + "=";
  const String query = path.substring(queryAt + 1);
  int start = 0;
  while (start <= query.length()) {
    const int end = query.indexOf('&', start);
    const String item = end < 0 ? query.substring(start) : query.substring(start, end);
    if (item.startsWith(prefix)) return item.substring(prefix.length());
    if (end < 0) break;
    start = end + 1;
  }
  return String();
}

bool publishQueryChunk(const char* queryId, uint16_t status, uint16_t index,
                       uint16_t count, const String& data) {
  cJSON* root = cJSON_CreateObject();
  if (!root) { ++enqueueFailures; return false; }
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON_AddStringToObject(root, "queryId", queryId);
  cJSON_AddNumberToObject(root, "statusCode", status);
  cJSON_AddNumberToObject(root, "chunkIndex", index);
  cJSON_AddNumberToObject(root, "chunkCount", count);
  cJSON_AddStringToObject(root, "data", data.c_str());
  char* raw = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!raw) { ++enqueueFailures; return false; }
  const size_t bytes = strlen(raw);
  const bool queued = bytes <= kMaxMessage &&
      publishText(queryReplyTopic, raw, 1, false);
  if (bytes > kMaxMessage) ++queueRejected;
  cJSON_free(raw);
  return queued;
}

bool publishStandaloneQueryReply(const char* queryId, uint16_t status,
                                 const char* error) {
  String body = "{\"error\":\"";
  body += error;
  body += "\"}";
  return publishQueryChunk(queryId, status, 0, 1, body);
}

void resetPendingQueryReply() {
  pendingQueryBody = String();
  pendingQueryId[0] = '\0';
  pendingQueryChunkIndex = pendingQueryChunkCount = 0;
  pendingQueryPublishFailures = 0;
  pendingQueryAckRetries = 0;
  pendingQueryReply = pendingQueryAwaitingAck = false;
  acknowledgedQueryChunk.store(-1, std::memory_order_release);
}

bool publishQueryReply(const char* queryId, uint16_t status, String&& body) {
  if (!queryId || !validQueryId(queryId)) return false;
  size_t first = 0;
  size_t last = body.length();
  while (first < last && isspace(static_cast<unsigned char>(body[first]))) ++first;
  while (last > first && isspace(static_cast<unsigned char>(body[last - 1]))) --last;
  // ControlApi 已负责序列化；这里只做轻量外形检查，避免为大统计响应
  // 再构造一棵完整 cJSON 树抬高 ESP32 的瞬时内存峰值。
  const bool validBody = last - first >= 2 && body[first] == '{' && body[last - 1] == '}';
  if (!validBody) {
    status = 502;
    body = "{\"error\":\"INVALID_QUERY_RESPONSE\"}";
  } else if (body.length() > kMaxQueryResponseBytes) {
    status = 413;
    body = "{\"error\":\"QUERY_RESPONSE_TOO_LARGE\"}";
  }

  if (!queryReplyMutex || xSemaphoreTake(queryReplyMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    ++queueRejected;
    return publishStandaloneQueryReply(queryId, 429, "QUERY_BUSY");
  }
  if (pendingQueryReply) {
    xSemaphoreGive(queryReplyMutex);
    return publishStandaloneQueryReply(queryId, 429, "QUERY_BUSY");
  }

  pendingQueryBody = std::move(body);
  strlcpy(pendingQueryId, queryId, sizeof(pendingQueryId));
  pendingQueryStatus = status;
  pendingQueryChunkIndex = 0;
  pendingQueryChunkCount = 0;
  pendingQueryPublishFailures = 0;
  pendingQueryAckRetries = 0;
  size_t offset = 0;
  pendingQueryOffsets[0] = 0;
  while (offset < pendingQueryBody.length() &&
         pendingQueryChunkCount < kMaxQueryChunks) {
    size_t end = offset;
    size_t encodedBytes = 0;
    while (end < pendingQueryBody.length() && end - offset < kQueryChunkBytes) {
      const auto ch = static_cast<uint8_t>(pendingQueryBody[end]);
      const size_t escapedBytes = ch < 0x20 ? 6 : ((ch == '"' || ch == '\\') ? 2 : 1);
      if (encodedBytes + escapedBytes > kQueryEncodedDataBudget) break;
      encodedBytes += escapedBytes;
      ++end;
    }
    // 不在 UTF-8 多字节字符中间截断，保证每个 data 字段都是合法 UTF-8。
    while (end < pendingQueryBody.length() && end > offset &&
           (static_cast<uint8_t>(pendingQueryBody[end]) & 0xC0) == 0x80) --end;
    if (end == offset) break;
    offset = end;
    ++pendingQueryChunkCount;
    pendingQueryOffsets[pendingQueryChunkCount] = offset;
  }
  if (offset != pendingQueryBody.length() || pendingQueryChunkCount == 0) {
    resetPendingQueryReply();
    xSemaphoreGive(queryReplyMutex);
    return publishStandaloneQueryReply(queryId, 413, "QUERY_RESPONSE_TOO_LARGE");
  }
  pendingQueryStartedAt = pendingQueryLastActionAt = millis();
  pendingQueryReply = true;
  pendingQueryAwaitingAck = false;
  acknowledgedQueryChunk.store(-1, std::memory_order_release);
  xSemaphoreGive(queryReplyMutex);
  return true;
}

uint32_t hashStateValue(const cJSON* item, uint32_t hash) {
  const uint8_t type = static_cast<uint8_t>(item->type & 0xFF);
  hash = esp_crc32_le(hash, &type, sizeof(type));
  if (item->string) {
    hash = esp_crc32_le(hash, reinterpret_cast<const uint8_t*>(item->string), strlen(item->string));
  }
  if (cJSON_IsString(item) && item->valuestring) {
    hash = esp_crc32_le(hash, reinterpret_cast<const uint8_t*>(item->valuestring), strlen(item->valuestring));
  } else if (cJSON_IsNumber(item)) {
    hash = esp_crc32_le(hash, reinterpret_cast<const uint8_t*>(&item->valuedouble), sizeof(item->valuedouble));
  } else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
    for (const cJSON* child = item->child; child; child = child->next) {
      hash = hashStateValue(child, hash);
    }
  }
  return hash;
}

bool calculateStateFingerprint(const cJSON* root, uint32_t* fingerprint) {
  if (!root || !cJSON_IsObject(root) || !fingerprint) return false;
  uint32_t hash = 0;
  for (const cJSON* field = root->child; field; field = field->next) {
    // 运行计时会随采样推进，不属于状态版本；心跳仍带最新计时值。
    if (field->string && (!strcmp(field->string, "fanRunMs") ||
        !strcmp(field->string, "remainingMs") ||
        !strcmp(field->string, "durationMs") ||
        !strcmp(field->string, "phaseDurationMs"))) continue;
    hash = hashStateValue(field, hash);
  }
  *fingerprint = hash;
  return true;
}

void handleQueryAck(const char* payload, size_t length, bool retained) {
  if (retained) return;
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (!root) return;
  const char* queryId = nullptr;
  cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
  cJSON* chunkIndex = cJSON_GetObjectItemCaseSensitive(root, "chunkIndex");
  if (cJSON_IsNumber(schema) && schema->valueint == 1 &&
      requiredString(root, "queryId", &queryId) && validQueryId(queryId) &&
      cJSON_IsNumber(chunkIndex) && chunkIndex->valuedouble >= 0 &&
      chunkIndex->valuedouble <= UINT16_MAX &&
      chunkIndex->valuedouble == static_cast<uint16_t>(chunkIndex->valuedouble) &&
      queryReplyMutex && xSemaphoreTake(queryReplyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (pendingQueryReply && pendingQueryAwaitingAck &&
        strcmp(queryId, pendingQueryId) == 0 &&
        static_cast<uint16_t>(chunkIndex->valueint) == pendingQueryChunkIndex) {
      // 最后一片确认到达时立即释放查询槽；网关随后可能在同一批数据里发来下一页。
      if (pendingQueryChunkIndex + 1 >= pendingQueryChunkCount) {
        resetPendingQueryReply();
      } else {
        acknowledgedQueryChunk.store(chunkIndex->valueint, std::memory_order_release);
      }
    }
    xSemaphoreGive(queryReplyMutex);
  }
  cJSON_Delete(root);
}

void updateTimeSync() {
  if (!commandReady || !timeReplySubscribed || !timeSyncMutex ||
      xSemaphoreTake(timeSyncMutex, 0) != pdTRUE) return;
  const uint32_t now = millis();
  if (timeRequestInFlight && uint32_t(now - pendingTimeRequestAt) > kTimeReplyTimeoutMs) {
    Serial.println("[mqtt-time] reply timeout");
    timeRequestInFlight = false;
    nextTimeRequestAt = now + 5000UL;
  }
  // 蓝牙手机可能在当前 MQTT 会话已完成 NAS 校时后再次校准设备时钟。
  // 来源变成 phone 表示 NAS 的会话内成功标记已过期，应重新向可信 NAS 取时。
  controlApiLock();
  const bool nasClockStillCurrent =
      strcmp(statisticsStoreClockSyncSource(), "nas") == 0;
  controlApiUnlock();
  if (timeSyncCompleteThisSession && !nasClockStillCurrent) {
    timeSyncCompleteThisSession = false;
    nextTimeRequestAt = now + 1000UL;
    Serial.println("[mqtt-time] phone sync superseded NAS; refreshing from NAS");
  }
  if (timeRequestInFlight || timeSyncCompleteThisSession ||
      static_cast<int32_t>(now - nextTimeRequestAt) < 0) {
    xSemaphoreGive(timeSyncMutex);
    return;
  }

  uint8_t nonce[16]{};
  esp_fill_random(nonce, sizeof(nonce));
  for (size_t i = 0; i < sizeof(nonce); ++i) snprintf(pendingTimeNonce + i * 2, 3, "%02x", nonce[i]);
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    nextTimeRequestAt = now + 5000UL;
    ++enqueueFailures;
    xSemaphoreGive(timeSyncMutex);
    return;
  }
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON_AddStringToObject(root, "nonce", pendingTimeNonce);
  cJSON_AddNumberToObject(root, "sentUptimeMs", now);
  char* raw = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!raw) {
    nextTimeRequestAt = now + 5000UL;
    ++enqueueFailures;
    xSemaphoreGive(timeSyncMutex);
    return;
  }
  pendingTimeRequestAt = now;
  timeRequestInFlight = true;
  const bool queued = publishText(timeRequestTopic, raw, 1, false);
  cJSON_free(raw);
  if (!queued) {
    timeRequestInFlight = false;
    nextTimeRequestAt = now + 5000UL;
  }
  xSemaphoreGive(timeSyncMutex);
}

void handleTimeReply(const char* payload, size_t length, bool retained) {
  if (retained || !timeSyncMutex) return;
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (!root) return;
  const char* nonce = nullptr;
  cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
  cJSON* status = cJSON_GetObjectItemCaseSensitive(root, "status");
  cJSON* utcMs = cJSON_GetObjectItemCaseSensitive(root, "utcMs");
  cJSON* accuracy = cJSON_GetObjectItemCaseSensitive(root, "accuracyMs");
  const bool headerValid = cJSON_IsNumber(schema) && schema->valueint == 1 &&
      requiredString(root, "nonce", &nonce);
  if (!headerValid) { cJSON_Delete(root); return; }
  if (xSemaphoreTake(timeSyncMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    cJSON_Delete(root);
    return;
  }
  if (!timeRequestInFlight || strcmp(nonce, pendingTimeNonce) != 0) {
    xSemaphoreGive(timeSyncMutex);
    cJSON_Delete(root);
    return;
  }
  const uint32_t now = millis();
  const uint32_t roundTripMs = uint32_t(now - pendingTimeRequestAt);
  timeRequestInFlight = false;
  if (roundTripMs > kTimeReplyTimeoutMs) {
    Serial.printf("[mqtt-time] late reply rtt=%lu ms\n", static_cast<unsigned long>(roundTripMs));
    nextTimeRequestAt = now + 5000UL;
    xSemaphoreGive(timeSyncMutex);
    cJSON_Delete(root);
    return;
  }
  if (!cJSON_IsString(status) || strcmp(status->valuestring, "available") != 0 ||
      !cJSON_IsNumber(utcMs) || utcMs->valuedouble < 1700000000000.0 ||
      utcMs->valuedouble > static_cast<double>(UINT32_MAX) * 1000.0 ||
      utcMs->valuedouble != static_cast<uint64_t>(utcMs->valuedouble) ||
      !cJSON_IsNumber(accuracy) || accuracy->valuedouble < 0 || accuracy->valuedouble > 1000) {
    const cJSON* reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    Serial.printf("[mqtt-time] source unavailable reason=%s\n",
                  cJSON_IsString(reason) ? reason->valuestring : "quality_unknown");
    nextTimeRequestAt = now + 60000UL;
    xSemaphoreGive(timeSyncMutex);
    cJSON_Delete(root);
    return;
  }
  // Clock/StatisticsStore is shared with the main loop and BLE request dispatch.
  // Serialize all related reads and the clock mutation with their common API lock.
  controlApiLock();
  if (!statisticsStoreTimezoneInitialized()) {
    controlApiUnlock();
    Serial.println("[mqtt-time] valid sample deferred until BLE initializes the statistics timezone");
    nextTimeRequestAt = now + 5000UL;
    xSemaphoreGive(timeSyncMutex);
    cJSON_Delete(root);
    return;
  }
  if (!statisticsStoreClockSyncSafe()) {
    controlApiUnlock();
    Serial.println("[mqtt-time] valid sample deferred until active statistics segment ends");
    nextTimeRequestAt = now + 30000UL;
    xSemaphoreGive(timeSyncMutex);
    cJSON_Delete(root);
    return;
  }
  const uint64_t estimatedUtcMs = static_cast<uint64_t>(utcMs->valuedouble) + roundTripMs / 2;
  const uint32_t epochSeconds = static_cast<uint32_t>(estimatedUtcMs / 1000ULL);
  const int16_t timezoneOffset = static_cast<int16_t>(
      statisticsStoreTimezoneOffsetSeconds() / 60);
  const bool clockSet = statisticsStoreSetClockFromSource(
      epochSeconds, timezoneOffset, StatisticsClockSource::kNas);
  controlApiUnlock();
  if (!clockSet) {
    Serial.println("[mqtt-time] sample rejected by clock conflict/safety policy");
    nextTimeRequestAt = now + 30000UL;
    xSemaphoreGive(timeSyncMutex);
    cJSON_Delete(root);
    return;
  }
  timeSyncCompleteThisSession = true;
  Serial.printf("[mqtt-time] synchronized source=nas accuracy=%dms rtt=%lu ms\n",
                accuracy->valueint, static_cast<unsigned long>(roundTripMs));
  xSemaphoreGive(timeSyncMutex);
  cJSON_Delete(root);
}

void updateQueryReply() {
  if (!queryReplyMutex || xSemaphoreTake(queryReplyMutex, 0) != pdTRUE) return;
  if (!pendingQueryReply) {
    xSemaphoreGive(queryReplyMutex);
    return;
  }
  const uint32_t now = millis();
  if (uint32_t(now - pendingQueryStartedAt) >= kQueryReplyTimeoutMs) {
    Serial.printf("[mqtt] query reply timed out id=%s chunk=%u/%u\n", pendingQueryId,
                  static_cast<unsigned>(pendingQueryChunkIndex + 1),
                  static_cast<unsigned>(pendingQueryChunkCount));
    resetPendingQueryReply();
    xSemaphoreGive(queryReplyMutex);
    return;
  }
  const int32_t acked = acknowledgedQueryChunk.exchange(-1, std::memory_order_acq_rel);
  if (pendingQueryAwaitingAck && acked == pendingQueryChunkIndex) {
    pendingQueryAwaitingAck = false;
    pendingQueryAckRetries = 0;
    pendingQueryPublishFailures = 0;
    ++pendingQueryChunkIndex;
    if (pendingQueryChunkIndex >= pendingQueryChunkCount) {
      resetPendingQueryReply();
      xSemaphoreGive(queryReplyMutex);
      return;
    }
  }
  if (pendingQueryAwaitingAck &&
      uint32_t(now - pendingQueryLastActionAt) >= kQueryAckTimeoutMs) {
    if (pendingQueryAckRetries >= 2) {
      Serial.printf("[mqtt] query ack missing id=%s chunk=%u\n", pendingQueryId,
                    static_cast<unsigned>(pendingQueryChunkIndex));
      resetPendingQueryReply();
      xSemaphoreGive(queryReplyMutex);
      return;
    }
    ++pendingQueryAckRetries;
    pendingQueryAwaitingAck = false;
  }
  if (!pendingQueryAwaitingAck &&
      uint32_t(now - pendingQueryLastActionAt) >= kQueryChunkPacingMs) {
    const size_t begin = pendingQueryOffsets[pendingQueryChunkIndex];
    const size_t end = pendingQueryOffsets[pendingQueryChunkIndex + 1];
    const String part = pendingQueryBody.substring(begin, end);
    char queryId[37];
    strlcpy(queryId, pendingQueryId, sizeof(queryId));
    const uint16_t status = pendingQueryStatus;
    const uint16_t index = pendingQueryChunkIndex;
    const uint16_t count = pendingQueryChunkCount;
    // 先登记等待状态，再放锁发送。SDK 可能同步分发 ACK；不能一边持有
    // 查询锁一边等待 MQTT 客户端锁，否则回调拿不到查询锁会丢掉 ACK。
    pendingQueryAwaitingAck = true;
    pendingQueryLastActionAt = now;
    xSemaphoreGive(queryReplyMutex);
    const bool queued = publishQueryChunk(queryId, status, index, count, part);
    if (xSemaphoreTake(queryReplyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    // 发送期间最后一片 ACK 可能已清槽并接收了下一条查询。
    if (pendingQueryReply && strcmp(queryId, pendingQueryId) == 0 &&
        index == pendingQueryChunkIndex) {
      if (queued) {
        pendingQueryPublishFailures = 0;
      } else {
        pendingQueryAwaitingAck = false;
        if (++pendingQueryPublishFailures >= 30) {
          Serial.printf("[mqtt] query chunk enqueue failed id=%s chunk=%u\n", pendingQueryId,
                        static_cast<unsigned>(pendingQueryChunkIndex));
          resetPendingQueryReply();
        }
      }
    }
  }
  xSemaphoreGive(queryReplyMutex);
}

void handleQuery(const char* payload, size_t length, bool retained) {
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (!root) return;
  const char *queryId = nullptr, *tokenId = nullptr, *token = nullptr, *path = nullptr;
  cJSON* schemaVersion = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
  if (retained || !cJSON_IsNumber(schemaVersion) || schemaVersion->valueint != 1 ||
      !requiredString(root, "queryId", &queryId) || !validQueryId(queryId) ||
      !requiredString(root, "tokenId", &tokenId) || strlen(tokenId) != 32 ||
      !requiredString(root, "token", &token) ||
      !requiredString(root, "path", &path) || strlen(path) > 180) {
    cJSON_Delete(root);
    return;
  }
  const auto auth = wifiBindingAuthorize(token, millis());
  if (!auth.valid || auth.pending || strcmp(auth.tokenId, tokenId)) {
    publishQueryReply(queryId, 401, "{\"error\":\"BINDING_INVALID\"}");
    cJSON_Delete(root);
    return;
  }

  ControlApiRequest request;
  request.method = "GET";
  const String fullPath(path);
  const int queryAt = fullPath.indexOf('?');
  request.path = queryAt < 0 ? fullPath : fullPath.substring(0, queryAt);
  if (request.path != "/api/status" && request.path != "/api/settings" &&
      request.path != "/api/stats" && request.path != "/api/stats/range" &&
      request.path != "/api/events") {
    publishQueryReply(queryId, 404, "{\"error\":\"QUERY_PATH_NOT_ALLOWED\"}");
    cJSON_Delete(root);
    return;
  }
  request.mode = queryValue(fullPath, "mode");
  request.eventStart = queryValue(fullPath, "start");
  request.eventEnd = queryValue(fullPath, "end");
  request.eventKind = queryValue(fullPath, "kind");
  request.eventOffset = queryValue(fullPath, "offset");
  request.eventBeforeId = queryValue(fullPath, "beforeId");
  request.eventDataEpoch = queryValue(fullPath, "dataEpoch");
  request.eventLimit = queryValue(fullPath, "limit");
  request.rangeFrom = queryValue(fullPath, "from");
  request.rangeTo = queryValue(fullPath, "to");
  request.rangeTz = queryValue(fullPath, "tzOffsetSeconds");
  ControlApiResponse response = controlApiDispatch(request);
  publishQueryReply(queryId, response.statusCode, std::move(response.body));
  cJSON_Delete(root);
}

String freshnessProof(const char* id,const char* token,const char* path,const char* body,uint32_t issued) {
  const String data=String(id)+"\n"+token+"\n"+path+"\n"+body+"\n"+String(issued);
  uint8_t hash[32];
  const auto* info=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if(!info){
    Serial.println("[mqtt] hmac: sha256 info unavailable");
    return String();
  }
  const int rc=mbedtls_md_hmac(info,freshnessKey,sizeof(freshnessKey),
      reinterpret_cast<const uint8_t*>(data.c_str()),data.length(),hash);
  if(rc!=0){
    // mbedtls_md_hmac 内部会 malloc md context，内存紧张时失败。
    Serial.printf("[mqtt] hmac failed rc=%d heap=%u largest=%u\n",rc,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return String();
  }
  char hex[65];for(size_t i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);
  return String(hex);
}

void handleCommand(const char* payload, size_t length, bool retained) {
  cJSON* root = cJSON_ParseWithLength(payload, length);
  if (!root) {
    publishAck("", 400, "{\"error\":\"INVALID_JSON\"}");
    return;
  }
  const char *requestId = nullptr, *path = nullptr, *body = nullptr, *token = nullptr;
  cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "version");
  if (retained || !cJSON_IsNumber(version) || version->valueint != 2 ||
      !requiredString(root, "requestId", &requestId) ||
      !requiredString(root, "path", &path) ||
      !requiredString(root, "body", &body) ||
      !requiredString(root, "token", &token)) {
    publishAck(requestId ? requestId : "", 400, "{\"error\":\"INVALID_COMMAND\"}");
    cJSON_Delete(root);
    return;
  }
  uint16_t status = 500;
  String response;
  const char* phase=nullptr;requiredString(root,"phase",&phase);
  if(phase&&!strcmp(phase,"prepare")) {
    const auto auth=wifiBindingAuthorize(token,millis());
    if(!auth.valid||auth.pending){publishAck(requestId,401,"{\"error\":\"BINDING_INVALID\"}");cJSON_Delete(root);return;}
    const uint32_t issued=millis();
    const String proof=freshnessProof(requestId,token,path,body,issued);
    if(proof.length()!=64){publishAck(requestId,503,"{\"error\":\"RESOURCE_BUSY\"}");cJSON_Delete(root);return;}
    response="{\"issuedAtMs\":"+String(issued)+",\"challenge\":\""+proof+"\"}";
    publishAck(requestId,200,response,"ready");cJSON_Delete(root);return;
  }
  const char* proof=nullptr;
  auto* issuedValue=cJSON_GetObjectItemCaseSensitive(root,"issuedAtMs");
  if(!phase||strcmp(phase,"execute")||!requiredString(root,"challenge",&proof)||
      !cJSON_IsNumber(issuedValue)||issuedValue->valuedouble<0||issuedValue->valuedouble>UINT32_MAX||
      issuedValue->valuedouble!=static_cast<uint32_t>(issuedValue->valuedouble)) {
    publishAck(requestId,400,"{\"error\":\"FRESHNESS_REQUIRED\"}");cJSON_Delete(root);return;
  }
  const uint32_t issued=static_cast<uint32_t>(issuedValue->valuedouble);
  const String expected=freshnessProof(requestId,token,path,body,issued);
  if(expected.length()!=64||expected!=proof) {
    publishAck(requestId,409,"{\"error\":\"COMMAND_EXPIRED\"}");cJSON_Delete(root);return;
  }
  if (!wifiControlServiceCommand(token, requestId, path, body, issued, &status, &response)) {
    status = 503;
    response = "{\"error\":\"RESOURCE_BUSY\"}";
  }
  publishAck(requestId, status, response);
  cJSON_Delete(root);
}

void eventHandler(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
  if (!event) return;
  if (eventId == MQTT_EVENT_CONNECTED) {
    Serial.println("[mqtt] CONNECTED");
    connected = true;
    commandReady=false;
    queryReady=false;
    commandSubscribed=false;
    querySubscribed=false;
    queryAckSubscribed=false;
    timeReplySubscribed=false;
    if (timeSyncMutex && xSemaphoreTake(timeSyncMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      timeRequestInFlight=false;
      timeSyncCompleteThisSession=false;
      nextTimeRequestAt=millis()+1000UL;
      xSemaphoreGive(timeSyncMutex);
    }
    commandSubscription=esp_mqtt_client_subscribe(client, commandTopic, 1);
    querySubscription=esp_mqtt_client_subscribe(client, queryTopic, 1);
    queryAckSubscription=esp_mqtt_client_subscribe(client, queryAckTopic, 1);
    timeReplySubscription=esp_mqtt_client_subscribe(client, timeReplyTopic, 1);
    if(commandSubscription<0)++enqueueFailures;
    if(querySubscription<0)++enqueueFailures;
    if(queryAckSubscription<0)++enqueueFailures;
    if(timeReplySubscription<0)++enqueueFailures;
  } else if(eventId==MQTT_EVENT_PUBLISHED&&connected&&
            (commandSubscription<0||querySubscription<0||queryAckSubscription<0||timeReplySubscription<0)) {
    // 重连时队列可能仍满；旧消息获确认释放空间后，在 MQTT 任务内重试订阅。
    if(commandSubscription<0){
      commandSubscription=esp_mqtt_client_subscribe(client,commandTopic,1);
      if(commandSubscription<0)++enqueueFailures;
    }
    if(querySubscription<0) {
      querySubscription=esp_mqtt_client_subscribe(client,queryTopic,1);
      if(querySubscription<0)++enqueueFailures;
    }
    if(queryAckSubscription<0) {
      queryAckSubscription=esp_mqtt_client_subscribe(client,queryAckTopic,1);
      if(queryAckSubscription<0)++enqueueFailures;
    }
    if(timeReplySubscription<0) {
      timeReplySubscription=esp_mqtt_client_subscribe(client,timeReplyTopic,1);
      if(timeReplySubscription<0)++enqueueFailures;
    }
  } else if(eventId==MQTT_EVENT_SUBSCRIBED&&event->msg_id==commandSubscription) {
    if(event->data_len!=1||!event->data||static_cast<uint8_t>(event->data[0])!=1)return;
    commandSubscribed=true;
    if(!commandReady){commandReady=true;publishText(availabilityTopic,"online",1,true);}
    stateSnapshotRequested.store(true);
  } else if(eventId==MQTT_EVENT_SUBSCRIBED&&event->msg_id==querySubscription) {
    if(event->data_len!=1||!event->data||static_cast<uint8_t>(event->data[0])!=1)return;
    querySubscribed=true;
    queryReady=queryAckSubscribed;
  } else if(eventId==MQTT_EVENT_SUBSCRIBED&&event->msg_id==queryAckSubscription) {
    if(event->data_len!=1||!event->data||static_cast<uint8_t>(event->data[0])!=1)return;
    queryAckSubscribed=true;
    queryReady=querySubscribed;
  } else if(eventId==MQTT_EVENT_SUBSCRIBED&&event->msg_id==timeReplySubscription) {
    if(event->data_len!=1||!event->data||static_cast<uint8_t>(event->data[0])!=1)return;
    timeReplySubscribed=true;
  } else if (eventId == MQTT_EVENT_DISCONNECTED) {
    if (connected) Serial.println("[mqtt] DISCONNECTED");
    connected = false;
    commandReady=false;
    queryReady=false;
    timeReplySubscribed=false;
    if (timeSyncMutex && xSemaphoreTake(timeSyncMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      timeRequestInFlight=false;
      xSemaphoreGive(timeSyncMutex);
    }
    if (queryReplyMutex && xSemaphoreTake(queryReplyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      resetPendingQueryReply();
      xSemaphoreGive(queryReplyMutex);
    }
  } else if (eventId == MQTT_EVENT_ERROR) {
    if (event->error_handle) {
      Serial.printf("[mqtt] ERROR type=%d sockErrno=%d connectReturn=%d\n",
                    static_cast<int>(event->error_handle->error_type),
                    event->error_handle->esp_transport_sock_errno,
                    event->error_handle->connect_return_code);
    } else {
      Serial.println("[mqtt] ERROR (no handle)");
    }
  } else if (eventId == MQTT_EVENT_DATA) {
    if (event->current_data_offset == 0) {
      incomingLength = 0;
      incomingExpected = static_cast<size_t>(event->total_data_len);
      incomingRetained = event->retain;
      if (event->topic && event->topic_len > 0 &&
          static_cast<size_t>(event->topic_len) < sizeof(incomingTopic)) {
        memcpy(incomingTopic, event->topic, event->topic_len);
        incomingTopic[event->topic_len] = '\0';
      } else incomingTopic[0] = '\0';
    }
    if (incomingExpected > kMaxMessage || event->data_len < 0 ||
        incomingLength + static_cast<size_t>(event->data_len) > kMaxMessage) {
      incomingLength = incomingExpected = 0;
      return;
    }
    memcpy(incomingPayload + incomingLength, event->data, event->data_len);
    incomingLength += static_cast<size_t>(event->data_len);
    if (incomingLength == incomingExpected &&
        (strcmp(incomingTopic, commandTopic) == 0 ||
         strcmp(incomingTopic, queryTopic) == 0 ||
         strcmp(incomingTopic, queryAckTopic) == 0 ||
         strcmp(incomingTopic, timeReplyTopic) == 0)) {
      incomingPayload[incomingLength] = '\0';
      if (strcmp(incomingTopic, commandTopic) == 0 && commandReady)
        handleCommand(incomingPayload, incomingLength, incomingRetained);
      else if (strcmp(incomingTopic, queryTopic) == 0 && queryReady)
        handleQuery(incomingPayload, incomingLength, incomingRetained);
      else if (strcmp(incomingTopic, queryAckTopic) == 0 && queryReady)
        handleQueryAck(incomingPayload, incomingLength, incomingRetained);
      else if (strcmp(incomingTopic, timeReplyTopic) == 0 && timeReplySubscribed)
        handleTimeReply(incomingPayload, incomingLength, incomingRetained);
      incomingLength = incomingExpected = 0;
    }
  }
}

void destroyClient() {
  if (!client) return;
  if (queryReplyMutex && xSemaphoreTake(queryReplyMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    resetPendingQueryReply();
    xSemaphoreGive(queryReplyMutex);
  }
  if (connected) publishText(availabilityTopic, "offline", 1, true);
  connected = false;
  commandReady=false;
  queryReady=false;
  timeReplySubscribed=false;
  if (timeSyncMutex && xSemaphoreTake(timeSyncMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    timeRequestInFlight=false;
    xSemaphoreGive(timeSyncMutex);
  }
  esp_mqtt_client_stop(client);
  esp_mqtt_client_destroy(client);
  client = nullptr;
}

bool startClient() {
  if (!configured || WiFi.status() != WL_CONNECTED || !wifiDeviceIdentityReady() ||
      !wifiBindingReady()) return false;
  const char* device = wifiDeviceId();
  snprintf(brokerUri, sizeof(brokerUri), "mqtt://%s:%u", config.host,
           static_cast<unsigned>(config.port));
  snprintf(clientId, sizeof(clientId), "mimivent-%s", device);
  snprintf(commandTopic, sizeof(commandTopic), "mimivent/v1/%s/command", device);
  snprintf(queryTopic, sizeof(queryTopic), "mimivent/v1/%s/query", device);
  snprintf(stateTopic, sizeof(stateTopic), "mimivent/v1/%s/state", device);
  snprintf(availabilityTopic, sizeof(availabilityTopic), "mimivent/v1/%s/availability", device);
  snprintf(ackTopic, sizeof(ackTopic), "mimivent/v1/%s/ack", device);
  snprintf(queryReplyTopic, sizeof(queryReplyTopic), "mimivent/v1/%s/query-reply", device);
  snprintf(queryAckTopic, sizeof(queryAckTopic), "mimivent/v1/%s/query-ack", device);
  snprintf(timeRequestTopic, sizeof(timeRequestTopic), "mimivent/v1/%s/time-request", device);
  snprintf(timeReplyTopic, sizeof(timeReplyTopic), "mimivent/v1/%s/time-reply", device);
  esp_mqtt_client_config_t mqtt{};
  mqtt.broker.address.uri = brokerUri;
  mqtt.credentials.username = config.username;
  mqtt.credentials.authentication.password = config.password;
  mqtt.credentials.client_id = clientId;
  mqtt.session.keepalive = 30;
  mqtt.session.last_will.topic = availabilityTopic;
  mqtt.session.last_will.msg = "offline";
  mqtt.session.last_will.msg_len = 7;
  mqtt.session.last_will.qos = 1;
  mqtt.session.last_will.retain = 1;
  mqtt.network.reconnect_timeout_ms = 5000;
  mqtt.outbox.limit = kOutboxLimitBytes;
  // 显式收窄收发缓冲：设备堆只剩约 12 KB 连续块，而 TLS 握手要 16–18 KB，
  // 默认各 1024 字节的缓冲会把 WiFi 直连推到握手失败。
  // 收发缓冲需覆盖单条上限，否则大状态要靠 SDK 分片，超时时更容易整条丢。
  mqtt.buffer.size = 1536;
  mqtt.buffer.out_size = 1536;
  client = esp_mqtt_client_init(&mqtt);
  if (!client) return false;
  if (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, eventHandler, nullptr) != ESP_OK ||
      esp_mqtt_client_start(client) != ESP_OK) {
    esp_mqtt_client_destroy(client);
    client = nullptr;
    return false;
  }
  return true;
}
}

void wifiMqttBegin() {
  // 每次启动生成一次即可；重连期间必须保留，否则 prepare 与 execute
  // 落在不同 MQTT 会话时会被误判为 challenge 过期。
  if (!freshnessKeyInitialized) {
    esp_fill_random(freshnessKey, sizeof(freshnessKey));
    freshnessKeyInitialized = true;
  }
  if (!queryReplyMutex) queryReplyMutex = xSemaphoreCreateMutex();
  if (!timeSyncMutex) timeSyncMutex = xSemaphoreCreateMutex();
  configured = readConfig();
}

void wifiMqttSuspendForOta() {
  // OTA 控制消息在 bluetoothManagerUpdate 主循环处理；与 wifiMqttUpdate
  // 同一执行上下文调用，避免并发销毁 ESP-MQTT 客户端。
  if (client) destroyClient();
}

void wifiMqttUpdate() {
  if (otaManagerIsActive() || WiFi.status() != WL_CONNECTED || !wifiBindingReady()) {
    // 这几个条件原本完全静默，串口看不出 MQTT 到底卡在哪一环。
    static uint32_t lastIdleLogMs = 0;
    const uint32_t nowMs = millis();
    if (uint32_t(nowMs - lastIdleLogMs) >= 5000) {
      lastIdleLogMs = nowMs;
      Serial.printf("[mqtt] idle ota=%d wifi=%d binding=%d configured=%d\n",
                    otaManagerIsActive() ? 1 : 0,
                    WiFi.status() == WL_CONNECTED ? 1 : 0,
                    wifiBindingReady() ? 1 : 0, configured ? 1 : 0);
    }
    if (client && (otaManagerIsActive() || WiFi.status() != WL_CONNECTED)) destroyClient();
    return;
  }
  if (restartRequested) {
    restartRequested = false;
    destroyClient();
  }
  if (!client && configured) {
    const bool started = startClient();
    Serial.printf("[mqtt] start attempt -> %d (%s:%u)\n", started ? 1 : 0,
                  config.host, static_cast<unsigned>(config.port));
  } else if (!client && !configured) {
    static uint32_t lastUnconfiguredLogMs = 0;
    const uint32_t nowMs = millis();
    if (uint32_t(nowMs - lastUnconfiguredLogMs) >= 5000) {
      lastUnconfiguredLogMs = nowMs;
      Serial.println("[mqtt] broker not configured (wifi.mqtt.configure never applied)");
    }
  }
  if (!connected || !client) return;
  updateTimeSync();
  updateQueryReply();
  if (!commandReady) return;
  const uint32_t now = millis();
  const bool snapshotRequested = stateSnapshotRequested.exchange(false);
  const bool heartbeatDue = uint32_t(now - lastPublishAt) >= kPublishIntervalMs;
  if (!snapshotRequested && !heartbeatDue &&
      uint32_t(now - lastStateCheckAt) < kStateChangeCheckMs) return;
  lastStateCheckAt = now;
  ControlApiRequest request;request.method = "GET";request.path = "/api/status";
  const auto status = controlApiDispatch(request);
  if(status.statusCode!=200) {
    if (snapshotRequested) stateSnapshotRequested.store(true);
    return;
  }
  cJSON* root=cJSON_ParseWithLength(status.body.c_str(),status.body.length());
  uint32_t fingerprint = 0;
  if(!root||!cJSON_IsObject(root)||!calculateStateFingerprint(root, &fingerprint)){
    if(root)cJSON_Delete(root);
    ++enqueueFailures;
    Serial.println("[mqtt] state fingerprint failed");
    return;
  }
  const bool changed = stateRevision.observe(fingerprint);
  if (!snapshotRequested && !heartbeatDue && !changed) {
    cJSON_Delete(root);
    return;
  }

  // Gateway 用绑定 token 的哈希签发/续期只读缓存授权；绝不把明文 token 放入状态。
  if (!wifiBindingAddAuthTokens(root)) {
    cJSON_Delete(root);
    ++enqueueFailures;
    Serial.println("[mqtt] state auth snapshot failed");
    lastPublishAt = now;
    return;
  }
  cJSON* envelope = cJSON_CreateObject();
  const char* deviceId = wifiDeviceId();
  const char* bootId = statisticsStoreBootId();
  const uint64_t sampledUptimeMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
  const bool metadataReady = envelope && deviceId && deviceId[0] && bootId && bootId[0] &&
      cJSON_AddNumberToObject(envelope, "schemaVersion", 1) &&
      cJSON_AddStringToObject(envelope, "deviceId", deviceId) &&
      cJSON_AddStringToObject(envelope, "bootId", bootId) &&
      cJSON_AddNumberToObject(envelope, "seq", stateRevision.sequence()) &&
      cJSON_AddNumberToObject(envelope, "sampledUptimeMs", static_cast<double>(sampledUptimeMs));
  if (!metadataReady) {
    if (envelope) cJSON_Delete(envelope);
    cJSON_Delete(root);
    ++enqueueFailures;
    Serial.println("[mqtt] state envelope allocation failed");
    lastPublishAt = now;
    return;
  }
  if (!cJSON_AddItemToObject(envelope, "state", root)) {
    cJSON_Delete(envelope);
    cJSON_Delete(root);
    ++enqueueFailures;
    Serial.println("[mqtt] state envelope allocation failed");
    lastPublishAt = now;
    return;
  }
  char* payload=cJSON_PrintUnformatted(envelope);
  // root 已移入 envelope；后续统一由 envelope 释放整棵 JSON 树。
  cJSON_Delete(envelope);
  lastPublishAt = now;
  if(payload){
    const size_t payloadLength = strlen(payload);
    if (payloadLength <= kMaxMessage) {
      publishText(stateTopic,payload,1,true);
    } else {
      ++enqueueFailures;
      Serial.printf("[mqtt] state message exceeds %u bytes\n", static_cast<unsigned>(kMaxMessage));
    }
    cJSON_free(payload);
  }else ++enqueueFailures;
}

bool wifiMqttConfigured() { return configured; }
bool wifiMqttConnected() { return commandReady; }

String wifiMqttHandleRequest(cJSON* root, const char* owner) {
  const char* op = nullptr;
  if (!requiredString(root, "op", &op)) return errorJson("INVALID_REQUEST");
  if (!strcmp(op, "wifi.mqtt.status")) {
    cJSON* result = cJSON_CreateObject();
    if (!result) return errorJson("OUT_OF_MEMORY");
    cJSON_AddBoolToObject(result, "configured", configured);
    cJSON_AddBoolToObject(result, "connected", commandReady);
    cJSON_AddNumberToObject(result,"outboxBytes",client?esp_mqtt_client_get_outbox_size(client):0);
    cJSON_AddNumberToObject(result,"queueRejected",queueRejected.load());
    cJSON_AddNumberToObject(result,"enqueueFailures",enqueueFailures.load());
    if (configured) {
      cJSON_AddStringToObject(result, "host", config.host);
      cJSON_AddNumberToObject(result, "port", config.port);
    }
    char* raw = cJSON_PrintUnformatted(result);
    String output = raw ? raw : "{\"error\":\"OUT_OF_MEMORY\"}";
    cJSON_free(raw);cJSON_Delete(result);return output;
  }
  if (!strcmp(op, "wifi.mqtt.clear")) {
    if (!wifiBindingOwnerAllowed(owner)) return errorJson("PERMISSION_DENIED");
    if (!preferences.begin(kNamespace, false)) return errorJson("STORAGE_UNAVAILABLE");
    const bool ok = preferences.remove(kConfigKey);preferences.end();
    if (!ok) return errorJson("STORAGE_UNAVAILABLE");
    configured = false;restartRequested = true;
    return "{\"configured\":false}";
  }
  if (strcmp(op, "wifi.mqtt.configure") != 0) return errorJson("UNSUPPORTED_OPERATION");
  if (!wifiBindingOwnerAllowed(owner)) return errorJson("PERMISSION_DENIED");
  cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
  const char *host = nullptr, *username = nullptr, *password = nullptr;
  cJSON* port = cJSON_GetObjectItemCaseSensitive(payload, "port");
  if (!requiredString(payload, "host", &host) ||
      !requiredString(payload, "username", &username) ||
      !requiredString(payload, "password", &password) || !cJSON_IsNumber(port) ||
      port->valueint < 1 || port->valueint > 65535 || !validPrivateHost(host) ||
      !validText(username, 1, 32) || !validText(password, 8, 64)) {
    return errorJson("INVALID_BROKER_CONFIG");
  }
  BrokerConfig candidate{};candidate.port = static_cast<uint16_t>(port->valueint);
  strncpy(candidate.host, host, sizeof(candidate.host) - 1);
  strncpy(candidate.username, username, sizeof(candidate.username) - 1);
  strncpy(candidate.password, password, sizeof(candidate.password) - 1);
  if (!saveConfig(candidate)) return errorJson("STORAGE_UNAVAILABLE");
  return "{\"configured\":true,\"connected\":false}";
}
#endif
