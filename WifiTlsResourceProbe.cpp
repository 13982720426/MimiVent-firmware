#include "WifiTlsResourceProbe.h"
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_TLS_PROBE
#include "OtaManager.h"
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <esp_heap_caps.h>
#include <esp_https_server.h>
#include <esp_random.h>
#include <nvs.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <lwip/sockets.h>
#include "WifiTlsIdleTracker.h"
#include "WifiDeviceIdentity.h"

namespace {
// 仅用于 P3-A 实验的保守起始预算，未通过真机测量不能用于生产放行。
#if CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN <= 4096
constexpr size_t kMinHeap = 64 * 1024;
constexpr size_t kMinBlock = 24 * 1024;
#else
constexpr size_t kMinHeap = 96 * 1024;
constexpr size_t kMinBlock = 48 * 1024;
#endif
struct ProbeIdentity { char cert[1200]; char key[320]; };
ProbeIdentity* identity = nullptr;
httpd_handle_t server = nullptr;
constexpr uint32_t kIdleMs = 15000;
WifiTlsIdleTracker idleTracker;
WifiTlsIdleTracker::Work idleWork{};
portMUX_TYPE idleMux = portMUX_INITIALIZER_UNLOCKED;
bool attempted = false;
uint32_t lastReport = 0, connectedAt = 0;
void report(const char* phase, bool httpsTask = false) {
  const auto caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  Serial.printf("[p3-resource] phase=%s wifi=%d ble=%u free=%u largest=%u minimum=%u stack=%u httpsTask=%d\n",
      phase, WiFi.status(),
      NimBLEDevice::getServer() ? NimBLEDevice::getServer()->getConnectedCount() : 0,
      heap_caps_get_free_size(caps),
      heap_caps_get_largest_free_block(caps), heap_caps_get_minimum_free_size(caps),
      uxTaskGetStackHighWaterMark(nullptr), httpsTask);
}
int randomBytes(void*, unsigned char* bytes, size_t length) {
  // 只在 WiFi 已启动后生成实验密钥，使用无线电熵源。
  esp_fill_random(bytes, length);
  return 0;
}
bool makeCertificate() {
  identity = static_cast<ProbeIdentity*>(calloc(1, sizeof(ProbeIdentity)));
  if (!identity) return false;
  mbedtls_pk_context key; mbedtls_pk_init(&key);
  mbedtls_x509write_cert cert; mbedtls_x509write_crt_init(&cert);
  uint8_t serial[16]; esp_fill_random(serial, sizeof(serial));
  serial[0] &= 0x7f; serial[15] |= 1;
  bool ok = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
      mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), randomBytes, nullptr) == 0 &&
      mbedtls_pk_write_key_pem(&key, reinterpret_cast<unsigned char*>(identity->key), sizeof(identity->key)) == 0;
  if (ok) {
    mbedtls_x509write_crt_set_version(&cert, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert, &key);
    mbedtls_x509write_crt_set_issuer_key(&cert, &key);
    ok = mbedtls_x509write_crt_set_serial_raw(&cert, serial, sizeof(serial)) == 0 &&
        mbedtls_x509write_crt_set_subject_name(&cert, "CN=MimiVent-P3-resource-probe") == 0 &&
        mbedtls_x509write_crt_set_issuer_name(&cert, "CN=MimiVent-P3-resource-probe") == 0 &&
        mbedtls_x509write_crt_set_validity(&cert, "20240101000000", "21240101000000") == 0 &&
        mbedtls_x509write_crt_set_basic_constraints(&cert, 0, -1) == 0 &&
        mbedtls_x509write_crt_set_key_usage(&cert, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) == 0 &&
        mbedtls_x509write_crt_pem(&cert, reinterpret_cast<unsigned char*>(identity->cert), sizeof(identity->cert), randomBytes, nullptr) == 0;
  }
  mbedtls_x509write_crt_free(&cert); mbedtls_pk_free(&key);
  return ok;
}
void closeIdleInServerTask(void*) {
  // 在 HTTPD owner task 检查 generation，防止旧 fd 被新连接复用后误关。
  portENTER_CRITICAL(&idleMux);
  const bool valid = idleTracker.valid(idleWork, millis(), kIdleMs);
  const int socket = idleWork.socket;
  portEXIT_CRITICAL(&idleMux);
  if (valid) {
    shutdown(socket, SHUT_RDWR); // SDK 仍拥有 fd，由其统一 close/free。
    report("idle-session-shutdown", true);
  }
  portENTER_CRITICAL(&idleMux);
  idleTracker.completed();
  portEXIT_CRITICAL(&idleMux);
}
esp_err_t denyBusiness(httpd_req_t* request) {
  portENTER_CRITICAL(&idleMux);
  idleTracker.touch(httpd_req_to_sockfd(request), millis());
  portEXIT_CRITICAL(&idleMux);
  report("request", true);
  httpd_resp_set_status(request, "401 Unauthorized");
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Connection", "close");
  return httpd_resp_sendstr(request, "{\"error\":\"AUTH_REQUIRED\"}");
}
void tlsEvent(esp_https_server_user_cb_arg_t* event) {
  int socket = -1;
  if (esp_tls_get_conn_sockfd(event->tls, &socket) == ESP_OK) {
    portENTER_CRITICAL(&idleMux);
    if (event->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE) idleTracker.opened(socket, millis());
    else idleTracker.closed(socket);
    portEXIT_CRITICAL(&idleMux);
  }
  if (event->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE) {
    const auto* ssl = static_cast<const mbedtls_ssl_context*>(esp_tls_get_ssl_context(event->tls));
    if (ssl) Serial.printf("[p3-resource] tls-runtime-max-in=%d max-out=%d\n",
        mbedtls_ssl_get_max_in_record_payload(ssl), mbedtls_ssl_get_max_out_record_payload(ssl));
  }
  report(event->user_cb_state == HTTPD_SSL_USER_CB_SESS_CREATE
      ? "tls-connected" : "tls-closed", true);
}
void releaseIdentity() {
  if (identity) {
    volatile char* key = identity->key;
    for (size_t i = 0; i < sizeof(identity->key); ++i) key[i] = 0;
    free(identity); identity = nullptr;
  }
}
void startProbe() {
  attempted = true;
  report("before-tls-budget");
  const auto caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  if (heap_caps_get_free_size(caps) < kMinHeap ||
      heap_caps_get_largest_free_block(caps) < kMinBlock) {
    Serial.printf("[p3-resource] gate=BLOCKED requiredFree=%u requiredLargest=%u; HTTPS not started\n",
        kMinHeap, kMinBlock);
    return;
  }
#if MIMIVENT_WIFI_IDENTITY_PROBE
  if (!wifiDeviceIdentityReady()) {
    Serial.printf("[p3-resource] identity-unavailable=%s; no ephemeral fallback\n", wifiDeviceIdentityState());
    return;
  }
#else
  if (!makeCertificate()) {
    report("certificate-failed"); releaseIdentity(); return;
  }
#endif
  report("certificate-ready");
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.stack_size = 10240;
  config.httpd.max_open_sockets = 1;
  config.httpd.backlog_conn = 1;
  config.httpd.recv_wait_timeout = 5;
  config.httpd.send_wait_timeout = 5;
  config.port_secure = 8443;
  config.tls_handshake_timeout_ms = 10000;
#if MIMIVENT_WIFI_IDENTITY_PROBE
  config.servercert = wifiDeviceCertificate(&config.servercert_len);
  config.prvtkey_pem = wifiDevicePrivateKey(&config.prvtkey_len);
#else
  config.servercert = reinterpret_cast<const uint8_t*>(identity->cert);
  config.servercert_len = strlen(identity->cert) + 1;
  config.prvtkey_pem = reinterpret_cast<const uint8_t*>(identity->key);
  config.prvtkey_len = strlen(identity->key) + 1;
#endif
  config.user_cb = tlsEvent;
  const esp_err_t result = httpd_ssl_start(&server, &config);
  if (result != ESP_OK) {
    server = nullptr;
    Serial.printf("[p3-resource] https-start-error=%s\n", esp_err_to_name(result));
    releaseIdentity(); return;
  }
  const httpd_uri_t uri = { .uri = "/", .method = HTTP_GET, .handler = denyBusiness, .user_ctx = nullptr };
  if (httpd_register_uri_handler(server, &uri) != ESP_OK) {
    httpd_ssl_stop(server); server = nullptr; releaseIdentity(); return;
  }
  report("https-listening");
  Serial.printf("[p3-resource] probe-host=%s port=8443 business=DENIED persisted-cert=%d\n",
      WiFi.localIP().toString().c_str(), MIMIVENT_WIFI_IDENTITY_PROBE);
}
}
void wifiTlsResourceProbeBegin() {
  Serial.println("[p3-resource] EXPERIMENTAL; binding/control NOT implemented");
  report("before-radios");
  Serial.printf("[p3-resource] compiled-tls-content=%u\n", CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN);
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
    Serial.printf("[p3-resource] nvs-used=%u nvs-free=%u nvs-total=%u\n",
        stats.used_entries, stats.free_entries, stats.total_entries);
  }
}
void wifiTlsResourceProbeSuspendForOta() {
  if (server) {
    httpd_ssl_stop(server); server = nullptr; releaseIdentity();
    report("https-stopped-before-ota");
  }
}
void wifiTlsResourceProbeUpdate() {
  const uint32_t now = millis();
  if (now - lastReport >= 5000) {
    lastReport = now; report(server ? "steady-with-tls" : "steady-no-tls");
  }
  if (server && (otaManagerIsActive() || WiFi.status() != WL_CONNECTED)) {
    httpd_ssl_stop(server); server = nullptr; releaseIdentity(); report("https-stopped");
  }
  if (server) {
    portENTER_CRITICAL(&idleMux);
    const bool closeIdle = idleTracker.schedule(now, kIdleMs, &idleWork);
    portEXIT_CRITICAL(&idleMux);
    if (closeIdle && httpd_queue_work(server, closeIdleInServerTask, nullptr) != ESP_OK) {
      portENTER_CRITICAL(&idleMux);
      idleTracker.completed();
      portEXIT_CRITICAL(&idleMux);
    }
  }
  if (WiFi.status() != WL_CONNECTED) { connectedAt = 0; return; }
  if (!NimBLEDevice::getServer() || !NimBLEDevice::getServer()->getConnectedCount()) {
    connectedAt = 0; return;
  }
  if (!connectedAt) connectedAt = now;
  if (!attempted && !otaManagerIsActive() && now - connectedAt >= 10000) startProbe();
}
#endif
