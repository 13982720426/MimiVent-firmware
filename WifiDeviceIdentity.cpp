#include "WifiDeviceIdentity.h"
#if MIMIVENT_ENABLE_WIFI && (MIMIVENT_WIFI_IDENTITY_PROBE || MIMIVENT_WIFI_READ_CONTROL)
#include "WifiIdentityRecord.h"
#include <bootloader_random.h>
#include <esp_random.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/sha256.h>
#include <nvs.h>
#include <stdlib.h>

namespace {
using namespace WifiIdentityRecord;
constexpr char kNamespace[] = "mv-identity";
uint8_t* record = nullptr;
size_t recordLength = 0;
const char* state = "UNINITIALIZED";
bool initialized = false;
char publicId[33]{},publicPin[65]{};
int randomBytes(void*, unsigned char* bytes, size_t length) {
  esp_fill_random(bytes, length);
  return 0;
}
struct BootEntropy {
  BootEntropy() { bootloader_random_enable(); }
  ~BootEntropy() { bootloader_random_disable(); }
};
struct Key {
  mbedtls_pk_context value;
  Key() { mbedtls_pk_init(&value); }
  ~Key() { mbedtls_pk_free(&value); }
};
bool validateCrypto(const uint8_t* data, size_t length) {
  if (inspect(data, length) != State::kValid) return false;
  const size_t certLength = get16(data + 32), keyLength = get16(data + 34);
  Key key;
  mbedtls_x509_crt cert; mbedtls_x509_crt_init(&cert);
  const bool valid = mbedtls_x509_crt_parse_der(&cert, data + kHeader, certLength) == 0 &&
      mbedtls_pk_parse_key(&key.value, data + kHeader + certLength, keyLength,
                          nullptr, 0, randomBytes, nullptr) == 0 &&
      mbedtls_pk_get_type(&key.value) == MBEDTLS_PK_ECKEY &&
      mbedtls_pk_get_bitlen(&key.value) == 256 &&
      mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(key.value)) == MBEDTLS_ECP_DP_SECP256R1 &&
      mbedtls_pk_check_pair(&cert.pk, &key.value, randomBytes, nullptr) == 0;
  mbedtls_x509_crt_free(&cert);
  return valid;
}
bool readSlot(nvs_handle_t storage, const char* key, uint8_t* data, size_t* length,
              bool* exists) {
  *length = 0;
  const esp_err_t result = nvs_get_blob(storage, key, nullptr, length);
  *exists = result != ESP_ERR_NVS_NOT_FOUND;
  if (result == ESP_ERR_NVS_NOT_FOUND) return true;
  if (result != ESP_OK || *length == 0 || *length > kMaximum) return false;
  return nvs_get_blob(storage, key, data, length) == ESP_OK;
}
bool saveSlot(nvs_handle_t storage, const char* key, uint8_t* data, size_t length,
              uint32_t sequence) {
  if (!seal(data, length, sequence, get32(data + 12), get16(data + 32), get16(data + 34))) return false;
  if (nvs_set_blob(storage, key, data, length) != ESP_OK || nvs_commit(storage) != ESP_OK) return false;
  uint8_t* verify = static_cast<uint8_t*>(malloc(kMaximum));
  if (!verify) return false;
  size_t readLength = 0; bool exists = false;
  const bool ok = readSlot(storage, key, verify, &readLength, &exists) && exists &&
      readLength == length && memcmp(verify, data, length) == 0;
  // 临时缓冲包含私钥，释放前清除。
  volatile uint8_t* clear = verify;
  for (size_t i = 0; i < kMaximum; ++i) clear[i] = 0;
  free(verify);
  return ok;
}
bool generate(uint8_t* data, size_t* length) {
  Key key;
  mbedtls_x509write_cert cert; mbedtls_x509write_crt_init(&cert);
  esp_fill_random(data + 16, 16);
  uint8_t serial[16]; esp_fill_random(serial, sizeof(serial));
  serial[0] &= 0x7f; serial[15] |= 1;
  char subject[64] = "CN=MimiVent-";
  for (size_t i = 0; i < 16; ++i) snprintf(subject + 12 + i * 2, 3, "%02x", data[16 + i]);
  bool ok = mbedtls_pk_setup(&key.value, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.value), randomBytes, nullptr) == 0;
  int certLength = -1, keyLength = -1;
  if (ok) {
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key.value);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key.value);
    ok = mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial)) == 0 &&
        mbedtls_x509write_crt_set_subject_name(&cert, subject) == 0 &&
        mbedtls_x509write_crt_set_issuer_name(&cert, subject) == 0 &&
        mbedtls_x509write_crt_set_validity(&cert, "20240101000000", "21240101000000") == 0 &&
        mbedtls_x509write_crt_set_basic_constraints(&cert, 0, -1) == 0 &&
        mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0;
    if (ok) certLength = mbedtls_x509write_crt_der(&cert, data + kHeader, kMaxCertificate, randomBytes, nullptr);
    if (certLength > 0) {
      memmove(data + kHeader, data + kHeader + kMaxCertificate - certLength, certLength);
      uint8_t* output = data + kHeader + certLength;
      keyLength = mbedtls_pk_write_key_der(&key.value, output, kMaxKey);
      if (keyLength > 0) memmove(output, output + kMaxKey - keyLength, keyLength);
    }
  }
  mbedtls_x509write_crt_free(&cert);
  if (!ok || certLength <= 0 || keyLength <= 0) return false;
  *length = kHeader + certLength + keyLength;
  return seal(data, *length, 1, 1, certLength, keyLength) && validateCrypto(data, *length);
}
void clearBuffer(uint8_t* data) {
  if (!data) return;
  volatile uint8_t* clear = data;
  for (size_t i = 0; i < kMaximum; ++i) clear[i] = 0;
  free(data);
}
}

void wifiDeviceIdentityBegin() {
  if (initialized) return;
  initialized = true;
  // bootloader 熵源与无线硬件不可同时使用；setup 保证此时 BLE/WiFi 尚未启动。
  BootEntropy entropy;
  nvs_handle_t storage;
  if (nvs_open(kNamespace, NVS_READWRITE, &storage) != ESP_OK) {
    state = "STORAGE_UNAVAILABLE"; return;
  }
  uint8_t* a = static_cast<uint8_t*>(calloc(1, kMaximum));
  uint8_t* b = static_cast<uint8_t*>(calloc(1, kMaximum));
  bool success = false;
  state = "MEMORY_UNAVAILABLE";
  if (a && b) {
    size_t al = 0, bl = 0; bool ae = false, be = false;
    const bool readA = readSlot(storage, "slot-a", a, &al, &ae);
    const bool readB = readSlot(storage, "slot-b", b, &bl, &be);
    // 超长/无法读取的记录也必须保留，不能按“空槽”重新生成身份。
    if (!readA || !readB) state = "RECORD_UNREADABLE";
    else if (!ae && !be) {
      state = "GENERATION_FAILED";
      if (generate(a, &al)) {
        state = "STORE_FAILED";
        // 两槽保存同一身份；任一写失败均停止 HTTPS，后续启动可恢复完整槽。
        if (saveSlot(storage, "slot-a", a, al, 1) && saveSlot(storage, "slot-b", a, al, 2)) success = true;
      }
    } else {
      state = (inspect(a, al) == State::kUnsupported || inspect(b, bl) == State::kUnsupported)
          ? "MIGRATION_REQUIRED" : "IDENTITY_CORRUPT";
      const int selected = select(a, al, b, bl);
      if (selected >= 0) {
        if (selected == 1) { memcpy(a, b, bl); al = bl; }
        success = validateCrypto(a, al);
      }
    }
    if (success) {
      // DER 编码使用尾部缓冲；清掉实际记录以外的残留，再缩小常驻内存。
      volatile uint8_t* tail = a;
      for (size_t i = al; i < kMaximum; ++i) tail[i] = 0;
      uint8_t* compact = static_cast<uint8_t*>(realloc(a, al));
      record = compact ? compact : a;
      recordLength = al; a = nullptr; state = "READY";
    }
  }
  clearBuffer(a); clearBuffer(b);
  nvs_close(storage);
  Serial.printf("[wifi-identity] state=%s recordBytes=%u\n", state, unsigned(recordLength));
  if (success) {
    uint8_t pin[32]; char pinHex[65], idHex[33];
    mbedtls_sha256(record + kHeader, get16(record + 32), pin, 0);
    for (size_t i = 0; i < 32; ++i) snprintf(pinHex + i * 2, 3, "%02x", pin[i]);
    for (size_t i = 0; i < 16; ++i) snprintf(idHex + i * 2, 3, "%02x", record[16 + i]);
    strcpy(publicId,idHex);strcpy(publicPin,pinHex);
    Serial.printf("[wifi-identity] deviceId=%s certPinSha256=%s sequence=%lu generation=%lu\n",
        idHex, pinHex, (unsigned long)get32(record + 8), (unsigned long)get32(record + 12));
  }
}
bool wifiDeviceIdentityReady() { return record != nullptr; }
const char* wifiDeviceIdentityState() { return state; }
const char* wifiDeviceId() { return publicId; }
const char* wifiDevicePin() { return publicPin; }
const uint8_t* wifiDeviceCertificate(size_t* length) {
  *length = record ? get16(record + 32) : 0;
  return record ? record + kHeader : nullptr;
}
const uint8_t* wifiDevicePrivateKey(size_t* length) {
  *length = record ? get16(record + 34) : 0;
  return record ? record + kHeader + get16(record + 32) : nullptr;
}
#endif
