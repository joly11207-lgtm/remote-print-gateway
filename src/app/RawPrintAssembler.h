#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "app/PrintQueue.h"
#include "app/PrintRequestParser.h"

namespace rtb {

// Result of the RAW_START validation. The HTTP layer maps this 1:1 to a status
// code so "too big" / "malformed" / "out of memory" stay distinguishable.
enum class RawBeginResult : uint8_t {
  Ok = 0,
  InvalidArgument = 1,   // 400
  PayloadTooLarge = 2,   // 413
  InsufficientMemory = 3 // 503
};

const char* rawBeginResultName(RawBeginResult result);
int rawBeginResultStatus(RawBeginResult result);

// Small metadata that travels in HTTP headers, never in the body: the body is
// the bitmap and nothing else.
struct RawPrintMeta {
  std::string job_id;    // empty allowed -> the queue dedups by content hash
  size_t row_bytes = 0;
  uint16_t feed_dots = 0;
  size_t width_dots = 0; // optional, 0 = not supplied
};

// Streaming assembler for POST /api/print/raw.
//
// Owns exactly ONE buffer for the whole request: the final bitmap, sized from
// Content-Length at RAW_START. RAW_WRITE memcpys straight into it at the
// current offset - no body copy, no base64, no second bitmap. `finish()` moves
// that buffer into the PrintJob, so at no point does a second full-size copy
// exist.
//
// Deliberately free of Arduino dependencies: the host tests exercise the exact
// code the firmware runs.
class RawPrintAssembler {
 public:
  bool active() const { return active_; }
  size_t expected() const { return expected_; }
  size_t received() const { return received_; }
  RawBeginResult lastResult() const { return last_result_; }
  // Non-empty after a rejected / failed request; points to a string literal.
  const char* errorName() const { return error_; }

  // Validates Content-Length and metadata, then allocates the final bitmap
  // once. `heap_budget_bytes` is the caller's memory budget (0 = no budget
  // check); it lets an impossible allocation fail as 503 instead of a
  // std::bad_alloc during resize.
  RawBeginResult begin(size_t content_length, const RawPrintMeta& meta,
                       const PrintLimits& limits, size_t heap_budget_bytes);

  // Appends one raw chunk. Returns false (and latches errorName()) when the
  // chunk would overrun Content-Length or when the request already failed.
  bool write(const uint8_t* data, size_t length);

  // Verifies "received == Content-Length" and the geometry, then hands the
  // bitmap to `job`. On failure the buffer is released and job stays untouched.
  bool finish(PrintJob& job);

  // Drops the buffer immediately and resets the state machine.
  void abort();

  void reset();

 private:
  bool validWidth(size_t width_dots, size_t row_bytes) const;

  std::vector<uint8_t> rows_;
  RawPrintMeta meta_;
  size_t expected_ = 0;
  size_t received_ = 0;
  bool active_ = false;
  RawBeginResult last_result_ = RawBeginResult::Ok;
  const char* error_ = "";
};

}  // namespace rtb
