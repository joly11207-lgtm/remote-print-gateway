#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "app/PrintQueue.h"
#include "app/PrintRequestParser.h"

namespace rtb {

// HTTP status for a queue outcome. Shared by BOTH print entry points so one
// business result can never be reported with two different codes:
//   Ok    -> 200
//   Busy  -> 409 (duplicate job_id, or duplicate content when job_id is empty)
//   other -> 400
int printEnqueueStatus(Result result);

struct PrintSubmitOutcome {
  int http_status = 200;
  std::string body;  // JSON response body
};

// Everything POST /api/print does after the body has been handed over, with no
// Arduino / WebServer dependency (so the host tests run the real code):
//   bounded in-place parse -> job_id / limit / heap-budget checks -> enqueueMove
//   -> response body.
// `limits` carries the logical ceilings plus the live heap budget; `now_ms` is
// injected exactly like PrintQueue does.
PrintSubmitOutcome submitJsonPrint(const char* body, size_t body_len,
                                   const PrintLimits& limits,
                                   size_t default_row_bytes, uint16_t default_feed_dots,
                                   PrintQueue& queue, uint32_t now_ms);

}  // namespace rtb
