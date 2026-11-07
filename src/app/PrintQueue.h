#pragma once

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#include "transport/base/TransportTypes.h"

namespace rtb {

enum class JobState : uint8_t {
  Queued = 0,
  WaitingPrinter = 1,
  Printing = 2,
  Done = 3,
  Failed = 4,
  Dropped = 5,
  // Print data was already sent; the physical outcome is unknown. Terminal for
  // auto-retry: never resend the whole job automatically.
  UncertainPartial = 6,
};

const char* jobStateName(JobState state);

// Retry policy for a finished/failed session. Kept pure so host Mock tests can
// verify it without an Arduino build. Physical "exactly once" is NOT claimed:
// as soon as a row has been attempted, the physical outcome is unknown.
enum class JobOutcome : uint8_t {
  Complete = 0,
  SafeRetry = 1,        // no print data sent: whole-job auto retry is safe
  UncertainPartial = 2, // print data sent: freeze, human decision required
};

inline JobOutcome classifyJobOutcome(Result result, size_t rows_attempted) {
  if (result == Result::Ok) return JobOutcome::Complete;
  return rows_attempted == 0 ? JobOutcome::SafeRetry : JobOutcome::UncertainPartial;
}

struct PrintJob {
  std::string job_id;       // client supplied; sole idempotency key when present
  std::string dedup_key;    // job_id when present, otherwise content hash
  std::string content_key;  // informational content hash; NEVER blocks an id'd job
  bool dedup_by_id = false; // true = key namespace is job_id, false = content hash
  std::vector<uint8_t> rows;  // 1bpp, MSB-first, row_bytes * row_count
  size_t row_bytes = 0;
  size_t row_count = 0;
  uint16_t feed_dots = 0;
  uint32_t content_hash = 0;
  uint32_t created_ms = 0;
  uint32_t retry_after_ms = 0;
  uint32_t attempts = 0;
  // Progress at the point of failure. Non-zero rows_attempted means print data
  // may already be on paper, so an automatic whole-job retry is unsafe.
  size_t rows_attempted = 0;
  size_t rows_dispatched = 0;
  JobState state = JobState::Queued;
  std::string last_error;
};

struct QueueStats {
  size_t queued = 0;
  size_t waiting = 0;
  size_t printing = 0;
  size_t done = 0;
  size_t failed = 0;
  size_t dropped = 0;
  size_t uncertain = 0;  // terminal: needs a human decision, no auto retry
  size_t pending() const { return queued + waiting + printing; }
};

// Local print job queue: dedup, offline waiting, bounded exponential retry.
// All time values are injected (now_ms), so host Mock tests are deterministic.
//
// Dedup semantics (two independent key namespaces):
//   job_id non-empty -> dedup_key = job_id, idempotency by job_id ONLY.
//                       Identical content under a different job_id is allowed
//                       (needed for manual reprint after uncertain_partial).
//   job_id empty     -> dedup_key = content_hash, protects no-ID requests
//                       against accidental resubmission.
// A content hash must never block a request that carries a valid job_id.
class PrintQueue {
 public:
  explicit PrintQueue(size_t max_jobs = 16, size_t max_job_bytes = 64 * 1024);

  // Returns Result::Busy for a duplicate job (reason = "duplicate_job") or a
  // full queue (reason = "queue_full"). The job's dedup_key / hash are filled in.
  Result enqueue(PrintJob& job, uint32_t now_ms, const char*& reason);

  bool hasPending() const;
  PrintJob* nextPending(uint32_t now_ms);
  PrintJob* find(const std::string& job_id);

  void markPrinting(const std::string& dedup_key);
  void markDone(const std::string& dedup_key);
  void markFailed(const std::string& dedup_key, const char* reason);
  void markWaitingPrinter(const std::string& dedup_key, const char* reason,
                          uint32_t now_ms, uint32_t backoff_ms);
  // Terminal: records progress + last_error, sets no retry deadline.
  void markUncertainPartial(const std::string& dedup_key, const char* reason,
                            size_t rows_attempted, size_t rows_dispatched);

  QueueStats stats() const;
  void toJson(std::string& out) const;
  void clear();
  size_t size() const { return jobs_.size(); }

 private:
  struct RecentEntry {
    std::string key;
    uint32_t finished_ms = 0;
    bool id_based = false;
  };

  bool seen(const std::string& key, bool id_based) const;
  void remember(const std::string& key, uint32_t now_ms, bool id_based);
  void evictTerminalIfFull();

  size_t max_jobs_;
  size_t max_job_bytes_;
  uint32_t clock_ms_ = 0;
  std::deque<PrintJob> jobs_;
  std::vector<RecentEntry> recent_;
};

}  // namespace rtb
