#include "app/RawPrintAssembler.h"

#include <new>
#include <string.h>

#include "app/JobId.h"

namespace rtb {
namespace {

// 512 bytes = 4096 dots per row, comfortably above any head this firmware
// targets (X18 is 384 dots / 48 bytes).
const size_t kMaxRowBytes = 512;

}  // namespace

const char* rawBeginResultName(RawBeginResult result) {
  switch (result) {
    case RawBeginResult::Ok: return "ok";
    case RawBeginResult::InvalidArgument: return "invalid_request";
    case RawBeginResult::PayloadTooLarge: return "payload_too_large";
    case RawBeginResult::InsufficientMemory: return "insufficient_memory";
    default: return "invalid_request";
  }
}

int rawBeginResultStatus(RawBeginResult result) {
  switch (result) {
    case RawBeginResult::Ok: return 200;
    case RawBeginResult::PayloadTooLarge: return 413;
    case RawBeginResult::InsufficientMemory: return 503;
    case RawBeginResult::InvalidArgument: return 400;
    default: return 400;
  }
}

bool RawPrintAssembler::validWidth(size_t width_dots, size_t row_bytes) const {
  if (width_dots == 0) return true;  // optional field
  if (width_dots > kMaxRowBytes * 8) return false;
  return (width_dots + 7) / 8 == row_bytes;
}

RawBeginResult RawPrintAssembler::begin(size_t content_length, const RawPrintMeta& meta,
                                        const PrintLimits& limits,
                                        size_t heap_budget_bytes) {
  reset();

  if (content_length == 0) {
    error_ = "empty_body";
    last_result_ = RawBeginResult::InvalidArgument;
    return last_result_;
  }
  if (meta.row_bytes == 0 || meta.row_bytes > kMaxRowBytes) {
    error_ = "invalid_row_bytes";
    last_result_ = RawBeginResult::InvalidArgument;
    return last_result_;
  }
  // Oversized is checked before anything is allocated, and before the more
  // specific geometry errors, so a hostile length always reads as 413.
  if (content_length > limits.max_bitmap_bytes) {
    error_ = "payload_too_large";
    last_result_ = RawBeginResult::PayloadTooLarge;
    return last_result_;
  }
  if (content_length % meta.row_bytes != 0) {
    error_ = "bitmap_not_row_aligned";
    last_result_ = RawBeginResult::InvalidArgument;
    return last_result_;
  }
  if (!isValidJobId(meta.job_id)) {
    error_ = "invalid_job_id";
    last_result_ = RawBeginResult::InvalidArgument;
    return last_result_;
  }
  if (!validWidth(meta.width_dots, meta.row_bytes)) {
    error_ = "invalid_width";
    last_result_ = RawBeginResult::InvalidArgument;
    return last_result_;
  }
  // Budget check first: on a fragmented heap resize() would throw, and an
  // exception on the ESP32 is a reboot risk. 0 disables the check (host tests
  // that intentionally exercise the allocator).
  if (heap_budget_bytes != 0 && content_length > heap_budget_bytes) {
    error_ = "insufficient_memory";
    last_result_ = RawBeginResult::InsufficientMemory;
    return last_result_;
  }

  // Both the bitmap and the job_id copy can fail. If either does, no buffer may
  // survive in the assembler: a half-built request must never look active.
  try {
    rows_.resize(content_length);
    meta_ = meta;  // copies meta.job_id (small, but still an allocation)
  } catch (const std::bad_alloc&) {
    std::vector<uint8_t>().swap(rows_);
    meta_ = RawPrintMeta();  // assignment to an empty string never allocates
    error_ = "insufficient_memory";
    last_result_ = RawBeginResult::InsufficientMemory;
    return last_result_;
  }

  expected_ = content_length;
  received_ = 0;
  active_ = true;
  error_ = "";
  last_result_ = RawBeginResult::Ok;
  return RawBeginResult::Ok;
}

bool RawPrintAssembler::write(const uint8_t* data, size_t length) {
  if (!active_) return false;
  if (error_[0] != '\0') return false;
  if (length == 0) return true;
  if (!data) {
    error_ = "null_chunk";
    return false;
  }
  // Never write past Content-Length: a client that sends extra bytes gets a
  // clean 400 later, not a heap overflow now.
  if (length > expected_ - received_) {
    error_ = "body_overflow";
    return false;
  }
  memcpy(rows_.data() + received_, data, length);
  received_ += length;
  return true;
}

bool RawPrintAssembler::finish(PrintJob& job) {
  if (!active_) return false;
  if (error_[0] != '\0') {
    abort();
    return false;
  }
  if (received_ != expected_) {
    // Short body: the client promised more than it sent. Never enqueued.
    error_ = "body_truncated";
    abort();
    return false;
  }
  if (expected_ == 0 || meta_.row_bytes == 0 || expected_ % meta_.row_bytes != 0) {
    error_ = "invalid_bitmap";
    abort();
    return false;
  }

  // Handing the bitmap over still copies the job_id string, so this section is
  // not allocation-free. On failure the buffer is released and the assembler is
  // left reusable; the caller sees "insufficient_memory" (503) and no job.
  try {
    job.job_id = meta_.job_id;
    job.row_bytes = meta_.row_bytes;
    job.row_count = expected_ / meta_.row_bytes;
    job.feed_dots = meta_.feed_dots;
    job.rows = std::move(rows_);  // ownership moves; no second bitmap is created
  } catch (const std::bad_alloc&) {
    abort();
    error_ = "insufficient_memory";
    last_result_ = RawBeginResult::InsufficientMemory;
    return false;
  }
  rows_.clear();

  active_ = false;
  expected_ = 0;
  received_ = 0;
  return true;
}

void RawPrintAssembler::abort() {
  // clear() would keep the capacity, i.e. keep the 64 KB block reserved after a
  // failed request. Swapping with an empty vector really returns it to the heap.
  std::vector<uint8_t>().swap(rows_);
  active_ = false;
  expected_ = 0;
  received_ = 0;
  meta_ = RawPrintMeta();
}

void RawPrintAssembler::reset() {
  abort();
  last_result_ = RawBeginResult::Ok;
  error_ = "";
}

}  // namespace rtb
