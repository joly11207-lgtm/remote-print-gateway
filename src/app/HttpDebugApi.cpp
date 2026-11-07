#include "app/HttpDebugApi.h"

#if defined(ARDUINO)

#include "app/BridgeApp.h"

namespace rtb {

HttpDebugApi::HttpDebugApi(WebServer& server, BridgeApp& app)
    : server_(server), app_(app) {}

void HttpDebugApi::sendJson(int code, const std::string& body) {
  server_.send(code, "application/json", body.c_str());
}

void HttpDebugApi::begin() {
  server_.on("/", HTTP_GET, [this]() {
    server_.send(200, "text/plain",
                 "Remote Thermal Printer Bridge V0.1 - JSON debug API only; "
                 "no real printer verified yet.\n");
  });

  server_.on("/api/status", HTTP_GET,
             [this]() { sendJson(200, app_.statusJson()); });
  server_.on("/api/capabilities", HTTP_GET,
             [this]() { sendJson(200, app_.compatibilityJson()); });
  server_.on("/api/compatibility", HTTP_GET,
             [this]() { sendJson(200, app_.compatibilityJson()); });
  server_.on("/api/queue", HTTP_GET,
             [this]() { sendJson(200, app_.queueJson()); });
  server_.on("/api/scan", HTTP_GET,
             [this]() { sendJson(200, app_.scanJson()); });

  server_.on("/api/print", HTTP_POST, [this]() {
    const std::string body = server_.arg("plain").c_str();
    const std::string response = app_.submitPrintJson(body);
    const bool ok = response.find("\"ok\":true") != std::string::npos;
    sendJson(ok ? 200 : 400, response);
  });

  server_.on("/api/mock", HTTP_POST, [this]() {
    sendJson(200, app_.setMockJson(server_.arg("plain").c_str()));
  });

  server_.on("/api/connect", HTTP_POST, [this]() {
    const std::string response = app_.connectJson(server_.arg("plain").c_str());
    const bool ok = response.find("\"ok\":true") != std::string::npos;
    sendJson(ok ? 200 : 400, response);
  });

  server_.on("/api/disconnect", HTTP_POST, [this]() {
    app_.disconnectPrinter();
    sendJson(200, "{\"ok\":true}");
  });

  server_.on("/api/ota", HTTP_GET, [this]() { sendJson(501, app_.otaJson()); });
  server_.on("/api/ota", HTTP_POST, [this]() { sendJson(501, app_.otaJson()); });

  server_.onNotFound([this]() { sendJson(404, "{\"error\":\"not_found\"}"); });
}

}  // namespace rtb

#endif  // ARDUINO
