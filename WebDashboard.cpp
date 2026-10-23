// 本模块提供手机网页、统一时间滚轮和 HTTP API；页面只负责显示状态与提交操作。
#include "WebDashboard.h"

#include "WebDashboardAsset.h"

#include <WebServer.h>

#include "AppConfig.h"
#include "ControlApi.h"
#include "FaviconAsset.h"
#include "SettingsStore.h"

namespace {

WebServer server(80);

void sendApiResponse(const ControlApiRequest& request) {
  const ControlApiResponse result = controlApiDispatch(request);
  server.sendHeader("Cache-Control", "no-store");
  server.send(result.statusCode, result.contentType.c_str(), result.body);
}

bool buildApiRequest(ControlApiRequest* request, const char** errorMessage) {
  *errorMessage = "profile must be production or dev";
  request->method = server.method() == HTTP_GET ? "GET" : "POST";
  request->path = server.uri();
  const String profileValue = server.arg("profile");
  if (profileValue.length()) {
    if (!settingsStoreParseProfile(profileValue, &request->profile)) return false;
  } else {
    request->profile = TimingProfile::PRODUCTION;
  }
  request->mode = server.arg("mode");
  request->stage = server.arg("stage");
  request->replace = server.arg("replace") == "1";
  request->switchProfile = server.arg("switch") == "1";
  if (request->path == "/api/settings" && request->method == "POST") {
    const char* fields[] = {"startupCooldownMs", "leaveConfirmMs", "autoExhaustMs", "manualExhaustMs", "pauseMs"};
    for (const char* field : fields) {
      if (!server.hasArg(field)) return true;  // dispatch 层会返回缺少字段错误。
    }
    if (!controlApiParseTimeValue(server.arg("startupCooldownMs"), false,
                                  &request->settings.startupCooldownMs) ||
        !controlApiParseTimeValue(server.arg("leaveConfirmMs"), true,
                                  &request->settings.leaveConfirmMs) ||
        !controlApiParseTimeValue(server.arg("autoExhaustMs"), false,
                                  &request->settings.autoExhaustMs) ||
        !controlApiParseTimeValue(server.arg("manualExhaustMs"), false,
                                  &request->settings.manualExhaustMs) ||
        !controlApiParseTimeValue(server.arg("pauseMs"), false,
                                  &request->settings.pauseMs)) {
      *errorMessage = "invalid timing value";
      return false;
    }
    request->hasSettings = true;
  }
  return true;
}

void handleApiRequest() {
  ControlApiRequest request;
  const char* errorMessage = nullptr;
  if (!buildApiRequest(&request, &errorMessage)) {
    String body = "{\"error\":\"";
    body += errorMessage;
    body += "\"}";
    server.send(400, "application/json; charset=utf-8", body);
    return;
  }
  sendApiResponse(request);
}

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<PGM_P>(kDashboardGzip), kDashboardGzipSize);
}

void handleFavicon() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "image/png", reinterpret_cast<PGM_P>(kFaviconPng), sizeof(kFaviconPng));
}

void handleWifiConfig() {
  String payload;
  payload.reserve(sizeof(AppConfig::kApSsid) + sizeof(AppConfig::kApPassword) + 1);
  payload += AppConfig::kApSsid;
  payload += '\n';
  payload += AppConfig::kApPassword;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", payload);
}

void handleNotFound() { server.send(404, "text/plain; charset=utf-8", "Not found"); }

}  // namespace

void webDashboardBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/dev", HTTP_GET, handleRoot);
  server.on("/dev/", HTTP_GET, handleRoot);
  server.on("/favicon.png", HTTP_GET, handleFavicon);
  server.on("/api/status", HTTP_GET, handleApiRequest);
  server.on("/api/settings", HTTP_GET, handleApiRequest);
  server.on("/api/settings", HTTP_POST, handleApiRequest);
  server.on("/api/settings/reset", HTTP_POST, handleApiRequest);
  server.on("/api/config", HTTP_GET, handleWifiConfig);
  server.on("/api/timed", HTTP_POST, handleApiRequest);
  server.on("/api/test-fan", HTTP_POST, handleApiRequest);
  server.on("/api/auto", HTTP_POST, handleApiRequest);
  server.on("/api/lockoff", HTTP_POST, handleApiRequest);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webDashboardUpdate() { server.handleClient(); }
