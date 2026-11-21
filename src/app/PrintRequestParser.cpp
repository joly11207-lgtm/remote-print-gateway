#include "app/PrintRequestParser.h"

#include <string.h>

#include "app/Base64.h"
#include "app/JobId.h"
#include "app/JsonMini.h"
#include "bitmap/BitmapPipeline.h"

namespace rtb {
namespace {

// Rows generated for the built-in checkerboard when "test":true is used.
const size_t kTestPatternRows = 24;

bool contains(const char* haystack, size_t haystack_len, const char* needle) {
  const size_t needle_len = strlen(needle);
  if (!haystack || needle_len == 0 || haystack_len < needle_len) return false;
  for (size_t i = 0; i + needle_len <= haystack_len; ++i) {
    if (memcmp(haystack + i, needle, needle_len) == 0) return true;
  }
  return false;
}

PrintParseError finishBitmap(ParsedPrintRequest& out, size_t row_bytes) {
  if (row_bytes == 0) return PrintParseError::InvalidBitmap;
  if (out.rows.empty()) return PrintParseError::InvalidBitmap;
  if (out.rows.size() % row_bytes != 0) return PrintParseError::InvalidBitmap;
  out.row_bytes = row_bytes;
  out.row_count = out.rows.size() / row_bytes;
  return PrintParseError::None;
}

}  // namespace

const char* printParseErrorName(PrintParseError error) {
  switch (error) {
    case PrintParseError::None: return "ok";
    case PrintParseError::InvalidJson: return "invalid_json";
    case PrintParseError::MissingRows: return "missing_rows";
    case PrintParseError::InvalidBase64: return "invalid_base64";
    case PrintParseError::InvalidBitmap: return "invalid_bitmap";
    case PrintParseError::InvalidJobId: return "invalid_job_id";
    case PrintParseError::PayloadTooLarge: return "payload_too_large";
    case PrintParseError::InsufficientMemory: return "insufficient_memory";
    default: return "invalid_request";
  }
}

int printParseErrorStatus(PrintParseError error) {
  // Oversized input is a distinct client error so the caller can tell "too big"
  // apart from "malformed" without parsing the body. A legal payload that does
  // not fit in the current heap is a server-side condition, not a client error.
  switch (error) {
    case PrintParseError::PayloadTooLarge: return 413;
    case PrintParseError::InsufficientMemory: return 503;
    default: return 400;
  }
}

PrintParseError parsePrintRequest(const char* body, size_t body_len,
                                  const PrintLimits& limits,
                                  size_t default_row_bytes, uint16_t default_feed_dots,
                                  ParsedPrintRequest& out) {
  out.test_pattern = false;
  out.job_id.clear();
  out.rows.clear();
  out.row_bytes = 0;
  out.row_count = 0;
  out.feed_dots = 0;

  // [1] Whole-body gate before anything is allocated.
  if (!body) return PrintParseError::InvalidJson;
  if (body_len > limits.max_body_bytes) return PrintParseError::PayloadTooLarge;
  if (!contains(body, body_len, "{")) return PrintParseError::InvalidJson;

  out.test_pattern = jsonBoolFieldIn(body, body_len, "test", false);
  // job_id is short by contract (it is the idempotency key); copying it is fine.
  out.job_id = jsonStringFieldIn(body, body_len, "job_id");
  // Same contract as /api/print/raw (JobId.h): empty is allowed, anything else
  // must be a bounded, printable id. Rejecting it here keeps both endpoints
  // consistent instead of validating only on the raw path.
  if (!isValidJobId(out.job_id)) return PrintParseError::InvalidJobId;

  int row_bytes = jsonIntFieldIn(body, body_len, "row_bytes", 0);
  if (row_bytes <= 0) row_bytes = static_cast<int>(default_row_bytes);
  if (row_bytes <= 0) return PrintParseError::InvalidBitmap;
  if (static_cast<size_t>(row_bytes) > limits.max_bitmap_bytes) {
    return PrintParseError::PayloadTooLarge;
  }
  int feed = jsonIntFieldIn(body, body_len, "feed", static_cast<int>(default_feed_dots));
  if (feed < 0) feed = 0;
  if (feed > 0xFFFF) feed = 0xFFFF;
  out.feed_dots = static_cast<uint16_t>(feed);

  if (out.test_pattern) {
    // The generated bitmap is row_bytes * kTestPatternRows, so the same two
    // gates apply as for a decoded payload. Without this, a legal row_bytes
    // could still ask makeTestPattern() for megabytes of push_back growth.
    const size_t total = static_cast<size_t>(row_bytes) * kTestPatternRows;
    if (total > limits.max_bitmap_bytes) return PrintParseError::PayloadTooLarge;
    if (limits.heap_budget_bytes != 0 && total > limits.heap_budget_bytes) {
      return PrintParseError::InsufficientMemory;
    }
    makeTestPattern(static_cast<size_t>(row_bytes), kTestPatternRows, out.rows);
    return finishBitmap(out, static_cast<size_t>(row_bytes));
  }

  // [2] Reference rows_b64 in place: no std::string, no push_back growth.
  const JsonFieldSlice slice = jsonStringSliceIn(body, body_len, "rows_b64");
  if (!slice.found) {
    // Distinguish "no field at all" from "field present but broken".
    return contains(body, body_len, "\"rows_b64\"") ? PrintParseError::InvalidJson
                                                    : PrintParseError::MissingRows;
  }
  if (slice.escaped) return PrintParseError::InvalidBase64;
  if (slice.size == 0) return PrintParseError::MissingRows;

  // [3] Predict the decoded size and enforce BOTH limits BEFORE allocating.
  // The estimate over-approximates by at most 2 bytes (base64 padding), so the
  // early rejection allows that slack and the exact size is verified after the
  // decode. Allocation therefore never exceeds max_bitmap_bytes + 2.
  const size_t max_decoded = base64MaxDecodedSize(slice.size);
  if (max_decoded > limits.max_bitmap_bytes + 2) {
    return PrintParseError::PayloadTooLarge;
  }
  // Logical limit passed; now ask whether the heap can actually hold it. The two
  // failures are deliberately different: "too big" is the client's fault (413),
  // "no contiguous block right now" is a runtime condition (503).
  if (limits.heap_budget_bytes != 0 && max_decoded > limits.heap_budget_bytes) {
    return PrintParseError::InsufficientMemory;
  }

  // [4] Exactly ONE allocation, then decode in place into it.
  out.rows.resize(max_decoded);
  size_t decoded = 0;
  if (!base64DecodeInto(slice.data, slice.size, out.rows.data(), max_decoded, decoded)) {
    out.rows.clear();  // no further allocation on the error path
    return PrintParseError::InvalidBase64;
  }
  if (decoded > limits.max_bitmap_bytes) {
    out.rows.clear();
    return PrintParseError::PayloadTooLarge;
  }
  // Shrinking never reallocates, so the single buffer stays the same one.
  out.rows.resize(decoded);

  return finishBitmap(out, static_cast<size_t>(row_bytes));
}

}  // namespace rtb
