#include "app/PrintSubmission.h"

#include <utility>

#include "app/JsonMini.h"

namespace rtb {

int printEnqueueStatus(Result result) {
  switch (result) {
    case Result::Ok: return 200;
    // Duplicate job_id / duplicate content: the request is well formed, it just
    // collides with a job that already exists. Same code on both endpoints.
    case Result::Busy: return 409;
    default: return 400;
  }
}

PrintSubmitOutcome submitJsonPrint(const char* body, size_t body_len,
                                   const PrintLimits& limits,
                                   size_t default_row_bytes, uint16_t default_feed_dots,
                                   PrintQueue& queue, uint32_t now_ms) {
  PrintSubmitOutcome out;

  // One pass: bounded sizing, in-place base64, single bitmap allocation, and
  // both the logical ceiling (413) and the heap budget (503) before it.
  ParsedPrintRequest parsed;
  const PrintParseError parse_error =
      parsePrintRequest(body, body_len, limits, default_row_bytes, default_feed_dots,
                        parsed);
  if (parse_error != PrintParseError::None) {
    out.http_status = printParseErrorStatus(parse_error);
    out.body.reserve(96);
    out.body += "{\"ok\":false,\"error\":\"";
    out.body += printParseErrorName(parse_error);
    out.body += "\"}";
    return out;
  }

  PrintJob job;
  job.job_id = parsed.job_id;         // short
  job.rows = std::move(parsed.rows);  // ownership moves: no second bitmap
  job.row_bytes = parsed.row_bytes;
  job.row_count = parsed.row_count;
  job.feed_dots = parsed.feed_dots;

  const char* reason = "";
  const Result result = queue.enqueueMove(std::move(job), now_ms, reason);

  out.http_status = printEnqueueStatus(result);
  out.body.reserve(256);  // fixed-size response: no growth under memory pressure
  out.body += "{\"ok\":";
  out.body += (result == Result::Ok ? "true" : "false");
  out.body += ",\"result\":\"";
  out.body += resultName(result);
  out.body += "\",\"reason\":\"";
  out.body += jsonEscape(reason);
  out.body += "\",\"job_id\":\"";
  out.body += jsonEscape(job.job_id.c_str());
  out.body += "\",\"dedup_key\":\"";
  out.body += jsonEscape(job.dedup_key.c_str());
  out.body += "\",\"rows\":";
  out.body += std::to_string(job.row_count);
  out.body += "}";
  return out;
}

}  // namespace rtb
