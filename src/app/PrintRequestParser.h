#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "config/RtbConfig.h"

namespace rtb {

// Parsing of POST /api/print bodies, deliberately free of any Arduino
// dependency so the host tests can exercise the exact code the firmware runs.
//
// POST /api/print is the SMALL / LEGACY JSON endpoint. Large print payloads
// MUST use POST /api/print/raw: Arduino's WebServer mallocs the whole body and
// then copies it into a String BEFORE the handler runs, so no check inside this
// module can undo that allocation. RTB_HTTP_MAX_BODY_BYTES is therefore a small
// conservative ceiling for legacy clients, not a large-transfer mechanism.
//
// Design constraints (all of them exist because of one real crash):
//   * the body is taken as a (pointer, length) buffer: the caller's buffer is
//     NOT copied, so peak memory is "HTTP body + final bitmap" only;
//   * rows_b64 is NEVER materialised as a std::string; it is referenced in
//     place through JsonFieldSlice and decoded straight into the final bitmap;
//   * the destination vector is sized ONCE from the base64 length; no
//     push_back growth, no intermediate decoded buffer;
//   * every size is checked against a limit BEFORE allocating.
enum class PrintParseError : uint8_t {
  None = 0,
  InvalidJson,        // field present but the document is malformed/truncated
  MissingRows,        // neither "test" nor a non-empty "rows_b64"
  InvalidBase64,      // bad alphabet, escapes, or a truncated final group
  InvalidBitmap,      // row_bytes/row count inconsistent
  InvalidJobId,       // job_id violates the shared JobId.h contract
  PayloadTooLarge,    // body, encoded field or decoded bitmap over the limit
  InsufficientMemory, // logically allowed, but no contiguous heap for it
};

const char* printParseErrorName(PrintParseError error);

// HTTP status the debug API should answer with:
//   payload_too_large  -> 413
//   insufficient_memory-> 503
//   everything else     -> 400
int printParseErrorStatus(PrintParseError error);

struct PrintLimits {
  // Configured (logical) ceilings. Exceeding these is a client error: 413.
  size_t max_body_bytes = RTB_HTTP_MAX_BODY_BYTES;
  size_t max_bitmap_bytes = RTB_MAX_JOB_BITMAP_BYTES;
  // Runtime ceiling sampled from the live heap (HeapBudget.h). Exceeding this is
  // NOT a client error: the payload is legal, there is just no memory: 503.
  // 0 = unknown / no check, which keeps host tests deterministic.
  size_t heap_budget_bytes = 0;
};

struct ParsedPrintRequest {
  bool test_pattern = false;
  std::string job_id;         // short field: a copy here is harmless
  std::vector<uint8_t> rows;  // FINAL bitmap, allocated exactly once
  size_t row_bytes = 0;
  size_t row_count = 0;
  uint16_t feed_dots = 0;
};

// `default_row_bytes` / `default_feed_dots` come from the resolved DeviceProfile
// (or the 48-byte / 64-dot fallbacks when no profile is known yet) so that this
// module stays independent of the catalog.
PrintParseError parsePrintRequest(const char* body, size_t body_len,
                                  const PrintLimits& limits,
                                  size_t default_row_bytes, uint16_t default_feed_dots,
                                  ParsedPrintRequest& out);

// Convenience overload for tests / callers that already own a std::string.
inline PrintParseError parsePrintRequest(const std::string& body,
                                         const PrintLimits& limits,
                                         size_t default_row_bytes,
                                         uint16_t default_feed_dots,
                                         ParsedPrintRequest& out) {
  return parsePrintRequest(body.data(), body.size(), limits, default_row_bytes,
                           default_feed_dots, out);
}

}  // namespace rtb
