#pragma once

#include <stddef.h>

#include <string>

namespace rtb {

// job_id contract, shared by BOTH print entry points:
//   POST /api/print       (small / legacy JSON)
//   POST /api/print/raw   (the official bitmap/image path)
//
// A job_id is an idempotency key, not free text: bounded length and a small
// alphabet keep it printable in a URL, a log line and a JSON response.
//
// Empty is legal on both endpoints and means "no id": PrintQueue then dedups by
// content hash instead of by job_id. That is a different namespace on purpose
// (see PrintQueue.h) and must not be changed by validation.
//
// One validator instead of one rule set per endpoint, so /api/print and
// /api/print/raw cannot drift apart again.
const size_t kMaxJobIdLength = 64;

inline bool isJobIdChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
}

inline bool isValidJobId(const std::string& job_id) {
  if (job_id.size() > kMaxJobIdLength) return false;
  for (size_t i = 0; i < job_id.size(); ++i) {
    if (!isJobIdChar(job_id[i])) return false;
  }
  return true;
}

}  // namespace rtb
