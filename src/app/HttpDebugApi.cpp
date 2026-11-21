#include "app/HttpDebugApi.h"

#if defined(ARDUINO)

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <new>

#include "app/BridgeApp.h"
#include "app/HeapBudget.h"
#include "app/PrintSubmission.h"
#include "app/RawPrintAssembler.h"

namespace rtb {
namespace {

std::string headerToString(const String& value) {
  return std::string(value.c_str(), value.length());
}

}  // namespace

HttpDebugApi::HttpDebugApi(WebServer& server, BridgeApp& app)
    : server_(server), app_(app) {}

void HttpDebugApi::sendJson(int code, const std::string& body) {
  server_.send(code, "application/json", body.c_str());
}

void HttpDebugApi::begin() {
  // /api/print/raw carries its metadata in headers: the raw branch of
  // Parsing.cpp parses request headers before the body and never calls
  // _parseArguments(), so query arguments are NOT available inside the raw
  // callback. collectHeaders() must run once, before server_.begin().
  static const char* kCollectedHeaders[] = {"X-RTB-Job-Id", "X-RTB-Row-Bytes",
                                            "X-RTB-Feed-Dots", "X-RTB-Width"};
  server_.collectHeaders(kCollectedHeaders, 4);

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

  // ---- V0.2 gateway surface (used by the website's RealGatewayAdapter) ------
  // POST /api/scan: discovery + GATT confirmation of the best candidate. It is a
  // POST because it connects (read-only) to verify the printer's characteristics.
  server_.on("/api/scan", HTTP_POST, [this]() {
    try {
      sendJson(200, app_.scanDetailJson(std::string(server_.arg("plain").c_str())));
    } catch (const std::bad_alloc&) {
      static const char kOom[] =
          "{\"ok\":false,\"error\":\"insufficient_memory\",\"devices\":[]}";
      server_.send(503, "application/json", kOom);
    }
  });

  // POST /api/safe-test: executes one registry entry (SafeTestRegistry.h). The
  // body names an id; it can never carry a command. An executed test answers 200
  // with status passed/failed; a non-registry id answers 422.
  server_.on("/api/safe-test", HTTP_POST, [this]() {
    try {
      int status = 200;
      const std::string response =
          app_.safeTestJson(std::string(server_.arg("plain").c_str()), status);
      sendJson(status, response);
    } catch (const std::bad_alloc&) {
      static const char kOom[] = "{\"ok\":false,\"error\":\"insufficient_memory\"}";
      server_.send(503, "application/json", kOom);
    }
  });

  server_.on("/api/print", HTTP_POST, [this]() {
    // Everything that can allocate lives inside this try block. The primary
    // fix for the aborts is the bounded, copy-free parse in PrintRequestParser;
    // this catch is only the last-resort guard so a residual allocation failure
    // can never again turn into std::terminate -> abort() -> SW_CPU_RESET.
    // Only bad_alloc is swallowed: no other exception is hidden here.
    try {
      // Borrow the WebServer's own body buffer. No std::string copy: with ~45 KB
      // free and a fragmented heap that copy was the allocation that threw.
      const String& body = server_.arg("plain");
      int status = 200;
      const std::string response =
          app_.submitPrintJson(body.c_str(), body.length(), status);
      const bool ok = response.find("\"ok\":true") != std::string::npos;
      sendJson(ok ? 200 : status, response);
    } catch (const std::bad_alloc&) {
      // Static literal on purpose: building any std::string here could throw
      // again while the heap is already exhausted. Nothing was enqueued.
      static const char kOomResponse[] =
          "{\"ok\":false,\"error\":\"oom_print_request\"}";
      Serial.println("oom_print_request");
      server_.send(503, "application/json", kOomResponse);
    }
  });

  // ---- POST /api/print/raw ------------------------------------------------
  // Body: raw 1bpp bitmap bytes, exactly Content-Length of them.
  // Headers: X-RTB-Job-Id, X-RTB-Row-Bytes, X-RTB-Feed-Dots, X-RTB-Width.
  // The 4th callback puts the request on WebServer's raw branch; the 3rd one
  // runs only after a complete body (RAW_END), never on abort.
  server_.on("/api/print/raw", HTTP_POST, [this]() { onRawPrintDone(); },
             [this]() { onRawPrintChunk(); });

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

RawPrintMeta HttpDebugApi::rawPrintMetaFromHeaders() {
  RawPrintMeta meta;
  if (server_.hasHeader("X-RTB-Job-Id")) {
    meta.job_id = headerToString(server_.header("X-RTB-Job-Id"));
  }
  if (server_.hasHeader("X-RTB-Row-Bytes")) {
    const long value = server_.header("X-RTB-Row-Bytes").toInt();
    meta.row_bytes = value > 0 ? static_cast<size_t>(value) : 0;
  }
  if (server_.hasHeader("X-RTB-Feed-Dots")) {
    long value = server_.header("X-RTB-Feed-Dots").toInt();
    if (value < 0) value = 0;
    if (value > 0xFFFF) value = 0xFFFF;
    meta.feed_dots = static_cast<uint16_t>(value);
  }
  if (server_.hasHeader("X-RTB-Width")) {
    const long value = server_.header("X-RTB-Width").toInt();
    meta.width_dots = value > 0 ? static_cast<size_t>(value) : 0;
  }
  return meta;
}

void HttpDebugApi::rawPrintFailOom() {
  // Containment for any allocation failure in the raw request: release the
  // bitmap immediately (abort() only frees) and make the eventual response say
  // 503 instead of blaming the client. Nothing was enqueued.
  raw_print_.abort();
  raw_begin_result_ = RawBeginResult::InsufficientMemory;
  raw_started_ms_ = 0;
  Serial.println("[rawprint] oom contained");
}

void HttpDebugApi::onRawPrintChunk() {
  // Every allocation of the raw lifecycle (header -> std::string, the assembler
  // metadata copy, ...) happens behind this guard. An OOM must end the request
  // here: never leave an active assembler holding its 64 KB block, never abort
  // the chip, never enqueue a half-assembled job.
  try {
    handleRawPrintChunk();
  } catch (const std::bad_alloc&) {
    rawPrintFailOom();
  }
}

void HttpDebugApi::handleRawPrintChunk() {
  HTTPRaw& raw = server_.raw();

  switch (raw.status) {
    case RAW_START: {
      raw_started_ms_ = millis();
      const int content_length = server_.clientContentLength();
      RawPrintMeta meta = rawPrintMetaFromHeaders();

      const BridgeApp::PrintDefaults defaults = app_.printDefaults();
      if (meta.row_bytes == 0) meta.row_bytes = defaults.row_bytes;
      if (!server_.hasHeader("X-RTB-Feed-Dots")) meta.feed_dots = defaults.feed_dots;

      PrintLimits limits;
      // Leave a quarter of the largest contiguous block for the rest of the
      // request: response buffer, queue bookkeeping, Wi-Fi stack. Checking the
      // largest block (not total free heap) is what makes a fragmented heap
      // answer 503 instead of throwing inside resize(). Same helper the JSON
      // endpoint uses, so both paths judge memory by the same rule.
      const size_t largest = largestHeapBlockBytes();
      const size_t budget = heapBudgetBytes(largest);

      raw_begin_result_ =
          raw_print_.begin(content_length > 0 ? static_cast<size_t>(content_length) : 0,
                           meta, limits, budget);

      if (raw_begin_result_ == RawBeginResult::Ok) {
        Serial.printf(
            "[rawprint] start cl=%u row_bytes=%u rows=%u free=%u largest=%u\n",
            static_cast<unsigned>(raw_print_.expected()),
            static_cast<unsigned>(meta.row_bytes),
            static_cast<unsigned>(raw_print_.expected() / meta.row_bytes),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(largest));
      } else {
        Serial.printf("[rawprint] rejected cl=%d: %s (free=%u largest=%u)\n",
                      content_length, raw_print_.errorName(),
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(largest));
      }
      break;
    }

    case RAW_WRITE:
      if (!raw_print_.active()) break;  // rejected request: drop the bytes
      if (!raw_print_.write(raw.buf, raw.currentSize)) {
        Serial.printf("[rawprint] write error at %u/%u: %s\n",
                      static_cast<unsigned>(raw_print_.received()),
                      static_cast<unsigned>(raw_print_.expected()),
                      raw_print_.errorName());
      }
      break;

    case RAW_END:
      // Nothing to do here: Parsing.cpp returns true right after RAW_END and
      // the 3rd callback (onRawPrintDone) runs the commit.
      break;

    case RAW_ABORTED:
      // Parsing.cpp returns false right after this: onRawPrintDone never runs
      // and no HTTP response is sent. Drop everything here; never enqueue.
      Serial.printf("[rawprint] aborted at %u/%u bytes\n",
                    static_cast<unsigned>(raw_print_.received()),
                    static_cast<unsigned>(raw_print_.expected()));
      rawPrintReset();
      break;
  }
}

void HttpDebugApi::onRawPrintDone() {
  // Reached only after a complete body (RAW_END -> _parseRequest true).
  int status = 400;
  char body[256];
  bool respond = false;

  // The finalize path allocates too: finish() copies the job_id and enqueueMove
  // builds the stored job. All of it stays inside this guard, and the response
  // is built afterwards with snprintf only, so the error path cannot allocate.
  try {
    respond = finishRawPrint(status, body, sizeof(body));
  } catch (const std::bad_alloc&) {
    rawPrintFailOom();
    status = 503;
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"insufficient_memory\"}");
    respond = true;
  }

  if (respond) {
    try {
      server_.send(status, "application/json", body);
    } catch (const std::bad_alloc&) {
      // The request already failed cleanly and the assembler is reset; if even
      // the response buffer cannot be allocated, dropping the connection is the
      // safe outcome (no reboot, no half job).
      Serial.println("[rawprint] oom response dropped");
    }
  }

  rawPrintReset();  // idempotent: releases any remaining bitmap
}

bool HttpDebugApi::finishRawPrint(int& status, char* body, size_t body_size) {
  if (raw_begin_result_ != RawBeginResult::Ok) {
    const RawBeginResult result = raw_begin_result_;
    status = rawBeginResultStatus(result);
    snprintf(body, body_size, "{\"ok\":false,\"error\":\"%s\"}",
             rawBeginResultName(result));
    return true;
  }
  if (!raw_print_.active()) {
    status = 400;
    snprintf(body, body_size, "{\"ok\":false,\"error\":\"invalid_request\"}");
    return true;
  }

  PrintJob job;
  if (!raw_print_.finish(job)) {
    // errorName() points at a literal, but reset() clears the pointer, so use it
    // before the state is dropped. A finalize-time allocation failure is a
    // server condition (503); anything else is the client's fault (400).
    const bool oom = raw_print_.lastResult() == RawBeginResult::InsufficientMemory;
    status = oom ? 503 : 400;
    const char* error = raw_print_.errorName();
    snprintf(body, body_size, "{\"ok\":false,\"error\":\"%s\"}",
             error && error[0] ? error : "invalid_bitmap");
    return true;
  }

  const size_t bytes = job.rows.size();  // captured before the queue takes it
  const char* reason = "";
  const Result result = app_.enqueueJob(job, reason);
  const uint32_t elapsed = raw_started_ms_ ? millis() - raw_started_ms_ : 0;

  Serial.printf(
      "[rawprint] finish bytes=%u rows=%u result=%s reason=%s ms=%u free=%u "
      "largest=%u\n",
      static_cast<unsigned>(bytes), static_cast<unsigned>(job.row_count),
      resultName(result), reason, static_cast<unsigned>(elapsed),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));

  // 200 / 409 (duplicate) / 400 through the same mapper the JSON endpoint uses.
  status = printEnqueueStatus(result);
  if (result == Result::Ok) {
    snprintf(body, body_size,
             "{\"ok\":true,\"job_id\":\"%s\",\"dedup_key\":\"%s\",\"rows\":%u,"
             "\"row_bytes\":%u,\"bytes\":%u,\"ms\":%u}",
             job.job_id.c_str(), job.dedup_key.c_str(),
             static_cast<unsigned>(job.row_count),
             static_cast<unsigned>(job.row_bytes), static_cast<unsigned>(bytes),
             static_cast<unsigned>(elapsed));
  } else {
    // Nothing was enqueued: the queue rejects before taking ownership.
    snprintf(body, body_size, "{\"ok\":false,\"error\":\"%s\"}", reason);
  }
  return true;
}

void HttpDebugApi::rawPrintReset() {
  raw_print_.reset();
  raw_begin_result_ = RawBeginResult::Ok;
  raw_started_ms_ = 0;
}

}  // namespace rtb

#endif  // ARDUINO
