#pragma once

#if defined(ARDUINO)
#include <WebServer.h>

#include <string>

namespace rtb {

class BridgeApp;

// JSON-only debug HTTP API. Deliberately NOT a website, UI, template or cloud
// service - those are all LATER scope.
class HttpDebugApi {
 public:
  HttpDebugApi(WebServer& server, BridgeApp& app);
  void begin();

 private:
  void sendJson(int code, const std::string& body);

  WebServer& server_;
  BridgeApp& app_;
};

}  // namespace rtb
#endif
