#pragma once

#if defined(ARDUINO)
#include <WebServer.h>

#include <stddef.h>

#include <string>

#include "app/RawPrintAssembler.h"

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

  // ---- POST /api/print/raw ------------------------------------------------
  // The body IS the bitmap: no JSON, no base64, no body copy. Metadata rides in
  // X-RTB-* headers because the raw branch of Parsing.cpp parses request
  // headers before the body and does NOT parse query arguments.
  RawPrintMeta rawPrintMetaFromHeaders();
  void onRawPrintChunk();  // 4th on() callback: START / WRITE / END / ABORTED
  void onRawPrintDone();   // 3rd on() callback: reached only after RAW_END
  void handleRawPrintChunk();  // allocation-free wrapper target
  // Builds the response without allocating: true = send (status, body).
  bool finishRawPrint(int& status, char* body, size_t body_size);
  // Last-resort containment: drop the bitmap and make the response say 503.
  void rawPrintFailOom();
  void rawPrintReset();

  WebServer& server_;
  BridgeApp& app_;
  RawPrintAssembler raw_print_;
  RawBeginResult raw_begin_result_ = RawBeginResult::Ok;
  uint32_t raw_started_ms_ = 0;
};

}  // namespace rtb
#endif
