#include "WifiProvisioningManager.h"
#include "WifiBindingStore.h"
#include "WifiHttpsServer.h"
#include "WifiDeviceIdentity.h"
#include "OtaManager.h"
#include "WifiLegacyStorage.h"

#if MIMIVENT_ENABLE_WIFI

#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_crc.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr char kNvsNamespace[] = "mv-wifi-v1";
constexpr char kCredentialsKey[] = "credentials";
constexpr size_t kMaxNetworks = 32;
constexpr uint32_t kScanTtlMs = 60000;
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint16_t kCredentialFormat = 1;

enum class WifiState : uint8_t {
  kUnconfigured,
  kScanning,
  kConnecting,
  kConnected,
  kFailed,
  kInterrupted,
};

struct Credentials {
  uint16_t format;
  uint8_t ssidLength;
  char ssid[33];
  char password[65];
  char clientId[37];
  char requestId[37];
  uint8_t requestDigest[32];
  uint32_t checksum;
};

struct ScanEntry {
  String ssid;
  int32_t rssi;
  bool secure;
  bool supported;
};

Preferences preferences;
Credentials savedCredentials{};
bool credentialsLoaded = false;
WifiState state = WifiState::kUnconfigured;
String stateError;
String activeRequestId;
String activeClientId;
String pendingSsid;
String pendingPassword;
String pendingRequestId;
String pendingClientId;
uint8_t pendingDigest[32]{};
uint32_t connectStartedMs = 0;
bool reconnectingOldNetwork = false;
bool restoringSavedCredentials = false;
#if MIMIVENT_WIFI_READ_CONTROL
bool otaRadioSuspended=false;
#endif
String scanId;
uint32_t scanStartedMs = 0;
bool scanReady = false;
ScanEntry scanEntries[kMaxNetworks]{};
size_t scanCount = 0;
uint32_t scanSequence = 0;

const char* stateName() {
  switch (state) {
    case WifiState::kUnconfigured: return "unconfigured";
    case WifiState::kScanning: return "scanning";
    case WifiState::kConnecting: return "connecting";
    case WifiState::kConnected: return "connected";
    case WifiState::kFailed: return "failed";
    case WifiState::kInterrupted: return "interrupted";
  }
  return "failed";
}

String jsonString(cJSON* object) {
  char* encoded = cJSON_PrintUnformatted(object);
  if (!encoded) return "{\"error\":\"OUT_OF_MEMORY\"}";
  String result(encoded);
  cJSON_free(encoded);
  return result;
}

String errorResponse(const char* code) {
  cJSON* root = cJSON_CreateObject();
  if (!root) return "{\"error\":\"OUT_OF_MEMORY\"}";
  cJSON_AddStringToObject(root, "error", code);
  String result = jsonString(root);
  cJSON_Delete(root);
  return result;
}

void addWifiStatus(cJSON* root) {
#if MIMIVENT_WIFI_READ_CONTROL
  cJSON_AddBoolToObject(root,"wifiReadAvailable",wifiHttpsServerReady());
  cJSON_AddStringToObject(root,"deviceId",wifiDeviceId());
  cJSON_AddStringToObject(root,"identityState",wifiDeviceIdentityState());
  cJSON_AddStringToObject(root,"bindingsState",wifiBindingState());
#endif
  cJSON_AddStringToObject(root, "state", stateName());
  cJSON_AddBoolToObject(root, "connected", WiFi.status() == WL_CONNECTED);
  if (WiFi.status() == WL_CONNECTED) {
    cJSON_AddStringToObject(root, "ip", WiFi.localIP().toString().c_str());
    cJSON_AddStringToObject(root, "ssid", WiFi.SSID().c_str());
  }
  if (!stateError.isEmpty()) cJSON_AddStringToObject(root, "error", stateError.c_str());
  if (!activeRequestId.isEmpty()) cJSON_AddStringToObject(root, "requestId", activeRequestId.c_str());
}

uint32_t credentialsChecksum(const Credentials& value) {
  return esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&value),
                      offsetof(Credentials, checksum));
}

bool validCredentials(const Credentials& value) {
  if (value.format != kCredentialFormat || value.ssidLength == 0 ||
      value.ssidLength > 32 || value.ssid[value.ssidLength] != '\0' ||
      value.checksum != credentialsChecksum(value) ||
      strnlen(value.clientId, sizeof(value.clientId)) >= sizeof(value.clientId) ||
      strnlen(value.requestId, sizeof(value.requestId)) >= sizeof(value.requestId)) {
    return false;
  }
  return strnlen(value.password, sizeof(value.password)) < sizeof(value.password);
}

bool loadCredentials() {
  preferences.begin(kNvsNamespace, true);
  const size_t storedLength = preferences.getBytesLength(kCredentialsKey);
  Credentials candidate{};
  const size_t readLength = storedLength == sizeof(candidate)
      ? preferences.getBytes(kCredentialsKey, &candidate, sizeof(candidate))
      : 0;
  preferences.end();
  if (readLength != sizeof(candidate) || !validCredentials(candidate)) return false;
  savedCredentials = candidate;
  return true;
}

bool saveCredentials(const Credentials& source) {
  Credentials value = source;
  value.format = kCredentialFormat;
  value.checksum = credentialsChecksum(value);
  preferences.begin(kNvsNamespace, false);
  const size_t written = preferences.putBytes(kCredentialsKey, &value, sizeof(value));
  preferences.end();
  if (written != sizeof(value)) return false;
  savedCredentials = value;
  credentialsLoaded = true;
  return true;
}

bool validSsid(const String& ssid) {
  const size_t length = ssid.length();
  return length > 0 && length <= 32 &&
      std::memchr(ssid.c_str(), '\0', length) == nullptr;
}

bool validPassword(const String& password) {
  const size_t length = password.length();
  if (length == 64) {
    for (size_t i = 0; i < length; ++i) {
      const char c = password[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
  }
  if (length < 8 || length > 63) return false;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(password[i]);
    if (c < 0x20 || c > 0x7E) return false;
  }
  return true;
}

bool digestConfig(const String& ssid, const String& password, uint8_t digest[32]) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
      mbedtls_sha256_update(&context,
          reinterpret_cast<const uint8_t*>(ssid.c_str()), ssid.length()) == 0 &&
      mbedtls_sha256_update(&context,
          reinterpret_cast<const uint8_t*>(password.c_str()), password.length()) == 0 &&
      mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}

bool sameDigest(const uint8_t left[32], const uint8_t right[32]) {
  uint8_t difference = 0;
  for (size_t i = 0; i < 32; ++i) difference |= left[i] ^ right[i];
  return difference == 0;
}

String makeScanId() {
  ++scanSequence;
  return String(static_cast<uint32_t>(esp_random()), HEX) + "-" +
      String(scanSequence, HEX);
}

bool scanSnapshotValid() {
  return scanReady && millis() - scanStartedMs <= kScanTtlMs;
}

bool beginScan() {
  if (state == WifiState::kConnecting || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    Serial.printf("[wifi-scan] rejected state=%u status=%d freeHeap=%u\n",
                  static_cast<unsigned>(state), static_cast<int>(WiFi.status()),
                  ESP.getFreeHeap());
    return false;
  }
  WiFi.scanDelete();
  scanCount = 0;
  scanReady = false;
  scanId = makeScanId();
  scanStartedMs = millis();
  state = WifiState::kScanning;
  stateError = "";
  const int started = WiFi.scanNetworks(true, false, false, 300);
  Serial.printf("[wifi-scan] start result=%d mode=%d status=%d freeHeap=%u\n",
                started, static_cast<int>(WiFi.getMode()),
                static_cast<int>(WiFi.status()), ESP.getFreeHeap());
  return true;
}

String scanResponse(const String& requestedScanId, size_t offset, size_t limit) {
  if (state == WifiState::kScanning) {
    const int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) {
      cJSON* root = cJSON_CreateObject();
      cJSON_AddStringToObject(root, "state", "scanning");
      cJSON_AddStringToObject(root, "scanId", scanId.c_str());
      String encoded = jsonString(root);
      cJSON_Delete(root);
      return encoded;
    }
    if (result < 0) {
      Serial.printf("[wifi-scan] failed result=%d mode=%d status=%d freeHeap=%u\n",
                    result, static_cast<int>(WiFi.getMode()),
                    static_cast<int>(WiFi.status()), ESP.getFreeHeap());
      state = WifiState::kFailed;
      stateError = "SCAN_FAILED";
      scanReady = false;
      WiFi.scanDelete();
      return errorResponse("SCAN_FAILED");
    }
    const int count = result;
    Serial.printf("[wifi-scan] complete raw=%d mode=%d status=%d freeHeap=%u\n",
                  count, static_cast<int>(WiFi.getMode()),
                  static_cast<int>(WiFi.status()), ESP.getFreeHeap());
    for (int i = 0; i < count && scanCount < kMaxNetworks; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty() || ssid.length() > 32) continue;
      const wifi_auth_mode_t auth = WiFi.encryptionType(i);
      const bool secure = auth != WIFI_AUTH_OPEN;
      const bool supported = auth == WIFI_AUTH_WPA2_PSK ||
          auth == WIFI_AUTH_WPA2_WPA3_PSK;
      size_t duplicate = scanCount;
      for (size_t j = 0; j < scanCount; ++j) {
        if (scanEntries[j].ssid == ssid) { duplicate = j; break; }
      }
      if (duplicate < scanCount) {
        if (WiFi.RSSI(i) > scanEntries[duplicate].rssi) {
          scanEntries[duplicate] = {ssid, WiFi.RSSI(i), secure, supported};
        }
      } else {
        scanEntries[scanCount++] = {ssid, WiFi.RSSI(i), secure, supported};
      }
    }
    std::sort(scanEntries, scanEntries + scanCount,
              [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
    Serial.printf("[wifi-scan] accepted=%u freeHeap=%u\n",
                  static_cast<unsigned>(scanCount), ESP.getFreeHeap());
    scanReady = true;
    state = WiFi.status() == WL_CONNECTED
        ? WifiState::kConnected
        : credentialsLoaded ? WifiState::kFailed : WifiState::kUnconfigured;
  }

  if (!scanSnapshotValid() || requestedScanId != scanId) return errorResponse("SCAN_EXPIRED");
  if (offset > scanCount || limit < 1 || limit > 12) return errorResponse("INVALID_REQUEST");
  cJSON* root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "state", "completed");
  cJSON_AddStringToObject(root, "scanId", scanId.c_str());
  cJSON_AddNumberToObject(root, "offset", offset);
  cJSON_AddNumberToObject(root, "total", scanCount);
  const size_t end = std::min(scanCount, offset + limit);
  cJSON_AddNumberToObject(root, "nextOffset", end);
  cJSON_AddBoolToObject(root, "hasMore", end < scanCount);
  cJSON* networks = cJSON_AddArrayToObject(root, "networks");
  for (size_t i = offset; i < end; ++i) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ssid", scanEntries[i].ssid.c_str());
    cJSON_AddNumberToObject(item, "rssi", scanEntries[i].rssi);
    cJSON_AddBoolToObject(item, "secure", scanEntries[i].secure);
    cJSON_AddBoolToObject(item, "supported", scanEntries[i].supported);
    cJSON_AddItemToArray(networks, item);
  }
  String encoded = jsonString(root);
  cJSON_Delete(root);
  return encoded;
}

void startConnect(const String& clientId, const String& requestId,
                  const String& ssid, const String& password,
                  const uint8_t digest[32]) {
  if (state == WifiState::kScanning || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
    stateError = "RADIO_BUSY";
    return;
  }
  if (state == WifiState::kConnecting) {
    if (requestId == pendingRequestId && clientId == pendingClientId &&
        sameDigest(digest, pendingDigest)) return;
    stateError = "RADIO_BUSY";
    return;
  }
  if (!activeRequestId.isEmpty() && requestId == activeRequestId &&
      clientId == activeClientId) {
    if (sameDigest(digest, savedCredentials.requestDigest)) return;
    stateError = "REQUEST_ID_CONFLICT";
    return;
  }
  pendingSsid = ssid;
  pendingPassword = password;
  pendingRequestId = requestId;
  pendingClientId = clientId;
  std::memcpy(pendingDigest, digest, sizeof(pendingDigest));
  stateError = "";
  reconnectingOldNetwork = false;
  restoringSavedCredentials = false;
  connectStartedMs = millis();
  state = WifiState::kConnecting;
  activeRequestId = requestId;
  activeClientId = clientId;
  WiFi.disconnect(false, false);
  WiFi.begin(pendingSsid.c_str(), pendingPassword.c_str());
}

String handleRequest(const String& request, const char* owner) {
  cJSON* root = cJSON_ParseWithLength(request.c_str(), request.length());
  if (!root) return errorResponse("INVALID_REQUEST");
  cJSON* opItem = cJSON_GetObjectItemCaseSensitive(root, "op");
  const String op = cJSON_IsString(opItem) ? String(opItem->valuestring) : "";
  String result;
  if (op == "wifi.status") {
    cJSON* response = cJSON_CreateObject();
    addWifiStatus(response);
    result = jsonString(response);
    cJSON_Delete(response);
  } else if (op == "wifi.scan") {
    // 设备扫描与 BLE 共存时内存不足，所有客户端统一使用手机侧扫描。
    result = errorResponse("SCAN_UNSUPPORTED");
  } else if (op == "wifi.configure") {
    if (!wifiBindingOwnerAllowed(owner)) {
      result = errorResponse("PERMISSION_DENIED");
      cJSON_Delete(root);
      return result;
    }
    cJSON* clientItem = cJSON_GetObjectItemCaseSensitive(root, "clientId");
    cJSON* requestItem = cJSON_GetObjectItemCaseSensitive(root, "requestId");
    cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    cJSON* ssidItem = cJSON_GetObjectItemCaseSensitive(payload, "ssid");
    cJSON* passwordItem = cJSON_GetObjectItemCaseSensitive(payload, "password");
    if (!cJSON_IsString(clientItem) || !cJSON_IsString(requestItem) ||
        !cJSON_IsString(ssidItem) || !cJSON_IsString(passwordItem)) {
      result = errorResponse("INVALID_REQUEST");
    } else {
      const String clientId(clientItem->valuestring);
      const String requestId(requestItem->valuestring);
      const String ssid(ssidItem->valuestring);
      const String password(passwordItem->valuestring);
      uint8_t digest[32]{};
      if (clientId.isEmpty() || requestId.isEmpty() || !validSsid(ssid) ||
          !validPassword(password) || !digestConfig(ssid, password, digest)) {
        result = errorResponse("INVALID_CREDENTIALS");
      } else {
        startConnect(clientId, requestId, ssid, password, digest);
        if (stateError == "REQUEST_ID_CONFLICT") result = errorResponse("REQUEST_ID_CONFLICT");
        else if (stateError == "RADIO_BUSY") result = errorResponse("RADIO_BUSY");
        else {
          cJSON* response = cJSON_CreateObject();
          addWifiStatus(response);
          result = jsonString(response);
          cJSON_Delete(response);
        }
      }
    }
  } else {
    result = errorResponse("UNSUPPORTED_OPERATION");
  }
  cJSON_Delete(root);
  return result;
}

void finishConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    if (restoringSavedCredentials) {
      restoringSavedCredentials = false;
      reconnectingOldNetwork = false;
      state = WifiState::kConnected;
      stateError = "";
      return;
    }
    if (reconnectingOldNetwork) {
      state = WifiState::kInterrupted;
      if (stateError.isEmpty()) stateError = "CONNECT_FAILED";
      pendingPassword = "";
      return;
    }
    Credentials next{};
    next.format = kCredentialFormat;
    next.ssidLength = static_cast<uint8_t>(pendingSsid.length());
    std::memcpy(next.ssid, pendingSsid.c_str(), pendingSsid.length() + 1);
    std::memcpy(next.password, pendingPassword.c_str(), pendingPassword.length() + 1);
    std::strncpy(next.clientId, pendingClientId.c_str(), sizeof(next.clientId) - 1);
    std::strncpy(next.requestId, pendingRequestId.c_str(), sizeof(next.requestId) - 1);
    std::memcpy(next.requestDigest, pendingDigest, sizeof(pendingDigest));
    if (saveCredentials(next)) {
      state = WifiState::kConnected;
      stateError = "";
      credentialsLoaded = true;
    } else {
      stateError = "CREDENTIALS_SAVE_FAILED";
      WiFi.disconnect(false, false);
      if (credentialsLoaded) {
        reconnectingOldNetwork = true;
        connectStartedMs = millis();
        WiFi.begin(savedCredentials.ssid, savedCredentials.password);
      } else {
        state = WifiState::kFailed;
      }
    }
    pendingPassword = "";
    return;
  }
  if (millis() - connectStartedMs < kConnectTimeoutMs) return;
  if (!reconnectingOldNetwork && credentialsLoaded) {
    WiFi.disconnect(false, false);
    reconnectingOldNetwork = true;
    restoringSavedCredentials = true;
    connectStartedMs = millis();
    WiFi.begin(savedCredentials.ssid, savedCredentials.password);
    return;
  }
  state = WifiState::kFailed;
  stateError = "CONNECT_FAILED";
  pendingPassword = "";
  if (reconnectingOldNetwork) state = WifiState::kInterrupted;
}

}  // namespace

void wifiProvisioningBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  credentialsLoaded = loadCredentials();
  if (credentialsLoaded) {
    activeRequestId = savedCredentials.requestId;
    activeClientId = savedCredentials.clientId;
    WiFi.begin(savedCredentials.ssid, savedCredentials.password);
    state = WifiState::kConnecting;
    restoringSavedCredentials = true;
    connectStartedMs = millis();
  }
}

void wifiProvisioningUpdate() {
#if MIMIVENT_WIFI_READ_CONTROL
  if(otaRadioSuspended){
    if(otaManagerIsActive())return;
    otaRadioSuspended=false;WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.setAutoReconnect(true);
    if(credentialsLoaded){
      WiFi.begin(savedCredentials.ssid,savedCredentials.password);
      restoringSavedCredentials=true;state=WifiState::kConnecting;connectStartedMs=millis();
    }else state=WifiState::kUnconfigured;
    Serial.println("[wifi-read] radio resumed after OTA");
  }
#endif
  if (state == WifiState::kConnecting) finishConnection();
  if (WiFi.status() == WL_CONNECTED && state != WifiState::kConnecting) {
    state = WifiState::kConnected;
    stateError = "";
  } else if (WiFi.status() != WL_CONNECTED && state == WifiState::kConnected) {
    state = credentialsLoaded ? WifiState::kInterrupted : WifiState::kUnconfigured;
  }
  if (state == WifiState::kScanning && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    // 由 wifi.scan 查询路径收集结果，保留扫描态直到客户端拉取快照。
  }
}

String wifiProvisioningHandleRequest(const String& request,const String& owner) {
#if MIMIVENT_WIFI_READ_CONTROL
  cJSON* root=cJSON_ParseWithLength(request.c_str(),request.length());
  if(root){auto* v=cJSON_GetObjectItemCaseSensitive(root,"op");
    if(otaManagerIsActive()&&(!cJSON_IsString(v)||strcmp(v->valuestring,"wifi.status"))){cJSON_Delete(root);return errorResponse("OTA_BUSY");}
    if(cJSON_IsString(v)&&(!strcmp(v->valuestring,"wifi.storage.legacyStatus")||!strcmp(v->valuestring,"wifi.storage.cleanupLegacy"))){
      String result=wifiLegacyStorageRequest(root,owner.c_str());cJSON_Delete(root);return result;
    }
    if(cJSON_IsString(v)&&(strncmp(v->valuestring,"wifi.bind.",10)==0||!strcmp(v->valuestring,"wifi.clients")||!strcmp(v->valuestring,"wifi.revoke"))){
      String result=wifiBindingRequest(root,owner.c_str());cJSON_Delete(root);return result;
    }
    cJSON_Delete(root);
  }
#endif
  return handleRequest(request,owner.c_str());
}

bool wifiProvisioningIsReady() {
  return state == WifiState::kConnected && WiFi.status() == WL_CONNECTED;
}

#if MIMIVENT_WIFI_READ_CONTROL
void wifiProvisioningSuspendForOta(){
  if(otaRadioSuspended)return;
  otaRadioSuspended=true;WiFi.setAutoReconnect(false);
  WiFi.disconnect(true,false);state=WifiState::kInterrupted;
  Serial.println("[wifi-read] radio suspended to reclaim OTA memory");
}
#endif

#endif  // MIMIVENT_ENABLE_WIFI
