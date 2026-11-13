#include "WifiControlLease.h"

#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {
constexpr uint32_t kLeaseIdleMs = 20000;
char leaseTokenId[33]{};
uint32_t leaseTouchedAt = 0;
portMUX_TYPE leaseMux = portMUX_INITIALIZER_UNLOCKED;
}

bool wifiControlLeaseAcquire(const char* tokenId, uint32_t nowMs) {
  if (tokenId == nullptr || tokenId[0] == '\0' || strnlen(tokenId, sizeof(leaseTokenId)) >= sizeof(leaseTokenId)) return false;
  bool acquired = false;
  portENTER_CRITICAL(&leaseMux);
  if (leaseTokenId[0] == '\0' || uint32_t(nowMs - leaseTouchedAt) > kLeaseIdleMs || strcmp(leaseTokenId, tokenId) == 0) {
    strncpy(leaseTokenId, tokenId, sizeof(leaseTokenId) - 1);
    leaseTokenId[sizeof(leaseTokenId) - 1] = '\0';
    leaseTouchedAt = nowMs;
    acquired = true;
  }
  portEXIT_CRITICAL(&leaseMux);
  return acquired;
}

void wifiControlLeaseRelease(const char* tokenId) {
  if (tokenId == nullptr) return;
  portENTER_CRITICAL(&leaseMux);
  if (strcmp(leaseTokenId, tokenId) == 0) {
    leaseTokenId[0] = '\0';
    leaseTouchedAt = 0;
  }
  portEXIT_CRITICAL(&leaseMux);
}

void wifiControlLeaseReset() {
  portENTER_CRITICAL(&leaseMux);
  leaseTokenId[0] = '\0';
  leaseTouchedAt = 0;
  portEXIT_CRITICAL(&leaseMux);
}
#endif
