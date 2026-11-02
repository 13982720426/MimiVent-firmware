#include "WifiTlsResourceProbe.h"
#if MIMIVENT_ENABLE_WIFI && (MIMIVENT_WIFI_TLS_PROBE || MIMIVENT_WIFI_READ_CONTROL)
#include <esp_tls.h>
#include <lwip/sockets.h>

// 本版本 SDK 的握手总时限在 WANT_READ/WANT_WRITE 返回后检查。
// 先限制阻塞 IO，防止不发 ClientHello 的客户端永久占用唯一 TLS 槽。
extern "C" int __real_esp_tls_server_session_create(
    esp_tls_cfg_server_t* config, int socket, esp_tls_t* tls);
extern "C" int __wrap_esp_tls_server_session_create(
    esp_tls_cfg_server_t* config, int socket, esp_tls_t* tls) {
  const timeval handshakeIo = {1, 0};
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &handshakeIo, sizeof(handshakeIo)) != 0 ||
      setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &handshakeIo, sizeof(handshakeIo)) != 0) {
    return -1;
  }
  const int result = __real_esp_tls_server_session_create(config, socket, tls);
  if (result == 0) {
    // 与探针 httpd 的 recv/send_wait_timeout 保持一致。
    const timeval applicationIo = {5, 0};
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &applicationIo, sizeof(applicationIo)) != 0 ||
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &applicationIo, sizeof(applicationIo)) != 0) {
      // 会话已经创建，交由 SDK 调用方关闭；不能返回失败令其丢失已创建对象。
      shutdown(socket, SHUT_RDWR);
    }
  }
  return result;
}
#endif
