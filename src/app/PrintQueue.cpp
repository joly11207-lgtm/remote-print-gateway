#include "app/PrintQueue.h"

#include <utility>

#include "app/Hash.h"

namespace rtb {

const char* jobStateName(JobState state) {
  switch (state) {
    case JobState::Queued: return "queued";
    case JobState::WaitingPrinter: return "waiting_printer";
    case JobState::Printing: return "printing";
    case JobState::Done: return "done";
    case JobState::Failed: return "failed";
    case JobState::Dropped: return "dropped";
    case JobState::UncertainPartial: return "uncertain_partial";
    default: return "unknown";
  }
}

namespace {
std::string jsonEscape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// Free a finished job's bitmap instead of keeping it until the slot is evicted.
// A done job is only reported through row_count/rows_attempted, so the payload
// is dead weight: 3 KB per X18 sheet, up to 16 slots = 48 KB on a 320 KB chip
// where the whole print path has to coexist with Wi-Fi + BLE. Without this the
// free heap stepped down ~5 KB per print and eventually starved the next
// request. swap() (not clear()) is what actually returns the block to the heap.
void releasePayload(PrintJob& job) {
  std::vector<uint8_t>().swap(job.rows);
}
}  // namespace

PrintQueue::PrintQueue(size_t max_jobs, size_t max_job_bytes)
    : max_jobs_(max_jobs ? max_jobs : 1), max_job_bytes_(max_job_bytes) {}

bool PrintQueue::seen(const std::string& key, bool id_based) const {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].state == JobState::Dropped) continue;
    if (jobs_[i].dedup_by_id == id_based && jobs_[i].dedup_key == key) return true;
  }
  for (size_t i = 0; i < recent_.size(); ++i) {
    if (recent_[i].id_based == id_based && recent_[i].key == key) return true;
  }
  return false;
}

void PrintQueue::remember(const std::string& key, uint32_t now_ms, bool id_based) {
  for (size_t i = 0; i < recent_.size(); ++i) {
    if (recent_[i].id_based == id_based && recent_[i].key == key) {
      recent_[i].finished_ms = now_ms;
      return;
    }
  }
  RecentEntry e;
  e.key = key;
  e.finished_ms = now_ms;
  e.id_based = id_based;
  recent_.push_back(e);
  while (recent_.size() > 32) recent_.erase(recent_.begin());
}

void PrintQueue::evictTerminalIfFull() {
  while (jobs_.size() >= max_jobs_) {
    size_t victim = jobs_.size();
    // Prefer ordinary terminal jobs; keep uncertain_partial records visible
    // as long as possible (they need a human decision). Only when nothing else
    // is evictable do we drop the oldest uncertain record to avoid a dead queue.
    for (size_t i = 0; i < jobs_.size(); ++i) {
      if (jobs_[i].state == JobState::Done || jobs_[i].state == JobState::Failed ||
          jobs_[i].state == JobState::Dropped) {
        victim = i;
        break;
      }
    }
    if (victim == jobs_.size()) {
      for (size_t i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].state == JobState::UncertainPartial) {
          victim = i;
          break;
        }
      }
    }
    if (victim == jobs_.size()) break;  // all jobs are active; cannot evict
    jobs_.erase(jobs_.begin() + static_cast<std::ptrdiff_t>(victim));
  }
}

Result PrintQueue::enqueue(PrintJob& job, uint32_t now_ms, const char*& reason) {
  // Legacy copy path: the caller keeps its bitmap. The body runs on a copy so
  // enqueueMove() can still hand ownership over without a second allocation on
  // the zero-copy path.
  PrintJob copy = job;
  const Result result = enqueueMove(std::move(copy), now_ms, reason);
  // Echo what enqueueMove() derived, so callers can build a response from the
  // job they still hold.
  job.dedup_key = copy.dedup_key;
  job.content_key = copy.content_key;
  job.content_hash = copy.content_hash;
  job.dedup_by_id = copy.dedup_by_id;
  job.state = copy.state;
  job.created_ms = copy.created_ms;
  job.retry_after_ms = copy.retry_after_ms;
  job.attempts = copy.attempts;
  return result;
}

Result PrintQueue::enqueueMove(PrintJob&& job, uint32_t now_ms, const char*& reason) {
  clock_ms_ = now_ms;
  reason = "";
  if (job.rows.empty() || job.row_bytes == 0 || job.row_count == 0 ||
      job.rows.size() < job.row_bytes * job.row_count) {
    reason = "invalid_bitmap";
    return Result::InvalidArgument;
  }
  if (job.rows.size() > max_job_bytes_) {
    reason = "job_too_large";
    return Result::InvalidArgument;
  }
  if (job.content_hash == 0) {
    job.content_hash = fnv1a32(job.rows.data(), job.rows.size());
  }
  job.dedup_by_id = !job.job_id.empty();
  job.content_key = hashKey(job.content_hash);
  job.dedup_key = job.dedup_by_id ? job.job_id : job.content_key;
  // Only ONE lookup, in ONE namespace. Never also check content_key here:
  // identical content under a different valid job_id must be printable.
  if (seen(job.dedup_key, job.dedup_by_id)) {
    reason = "duplicate_job";
    return Result::Busy;
  }
  if (jobs_.size() >= max_jobs_) evictTerminalIfFull();
  if (jobs_.size() >= max_jobs_) {
    reason = "queue_full";
    return Result::Busy;
  }
  job.state = JobState::Queued;
  job.created_ms = now_ms;
  job.retry_after_ms = now_ms;
  job.attempts = 0;
  job.rows_attempted = 0;
  job.rows_dispatched = 0;
  job.last_error.clear();
  // Hand the bitmap over instead of copying it. A 64 KB job must exist exactly
  // once in RAM: /api/print/raw keeps the only other reference in its
  // assembler, and that one is moved in here. Only `rows` is moved - job_id,
  // dedup_key and the counters stay in `job` because callers and the host
  // tests read them back after enqueue().
  PrintJob stored;
  stored.job_id = job.job_id;
  stored.dedup_key = job.dedup_key;
  stored.content_key = job.content_key;
  stored.dedup_by_id = job.dedup_by_id;
  stored.row_bytes = job.row_bytes;
  stored.row_count = job.row_count;
  stored.feed_dots = job.feed_dots;
  stored.content_hash = job.content_hash;
  stored.created_ms = job.created_ms;
  stored.retry_after_ms = job.retry_after_ms;
  stored.attempts = job.attempts;
  stored.rows_attempted = job.rows_attempted;
  stored.rows_dispatched = job.rows_dispatched;
  stored.state = job.state;
  stored.last_error = job.last_error;
  stored.rows = std::move(job.rows);
  jobs_.push_back(std::move(stored));
  job.rows.clear();  // the caller no longer owns a bitmap
  return Result::Ok;
}

bool PrintQueue::hasPending() const {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    const JobState s = jobs_[i].state;
    if (s == JobState::Queued || s == JobState::WaitingPrinter ||
        s == JobState::Printing) return true;
  }
  return false;
}

PrintJob* PrintQueue::nextPending(uint32_t now_ms) {
  clock_ms_ = now_ms;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    PrintJob& j = jobs_[i];
    if ((j.state == JobState::Queued || j.state == JobState::WaitingPrinter) &&
        j.retry_after_ms <= now_ms) return &j;
  }
  return nullptr;
}

PrintJob* PrintQueue::find(const std::string& job_id) {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].job_id == job_id) return &jobs_[i];
  }
  return nullptr;
}

void PrintQueue::markPrinting(const std::string& dedup_key) {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].dedup_key == dedup_key) jobs_[i].state = JobState::Printing;
  }
}

void PrintQueue::markDone(const std::string& dedup_key) {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].dedup_key == dedup_key) {
      jobs_[i].state = JobState::Done;
      jobs_[i].last_error.clear();
      releasePayload(jobs_[i]);
      remember(jobs_[i].dedup_key, clock_ms_, jobs_[i].dedup_by_id);
    }
  }
}

void PrintQueue::markFailed(const std::string& dedup_key, const char* reason) {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].dedup_key == dedup_key) {
      jobs_[i].state = JobState::Failed;
      jobs_[i].last_error = reason ? reason : "";
      releasePayload(jobs_[i]);
      remember(jobs_[i].dedup_key, clock_ms_, jobs_[i].dedup_by_id);
    }
  }
}

void PrintQueue::markWaitingPrinter(const std::string& dedup_key, const char* reason,
                                   uint32_t now_ms, uint32_t backoff_ms) {
  clock_ms_ = now_ms;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].dedup_key == dedup_key) {
      jobs_[i].state = JobState::WaitingPrinter;
      jobs_[i].retry_after_ms = now_ms + backoff_ms;
      jobs_[i].attempts += 1;
      jobs_[i].last_error = reason ? reason : "";
    }
  }
}

void PrintQueue::markUncertainPartial(const std::string& dedup_key, const char* reason,
                                      size_t rows_attempted, size_t rows_dispatched) {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (jobs_[i].dedup_key == dedup_key) {
      jobs_[i].state = JobState::UncertainPartial;
      jobs_[i].last_error = reason ? reason : "";
      jobs_[i].rows_attempted = rows_attempted;
      jobs_[i].rows_dispatched = rows_dispatched;
      jobs_[i].retry_after_ms = 0;  // never auto-retried
      releasePayload(jobs_[i]);
      // Intentionally NOT added to recent_. The frozen job stays in jobs_ but
      // only occupies the job_id namespace, so a manual reprint with a NEW
      // job_id and identical content is accepted (see dedup semantics).
    }
  }
}

QueueStats PrintQueue::stats() const {
  QueueStats s;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    switch (jobs_[i].state) {
      case JobState::Queued: ++s.queued; break;
      case JobState::WaitingPrinter: ++s.waiting; break;
      case JobState::Printing: ++s.printing; break;
      case JobState::Done: ++s.done; break;
      case JobState::Failed: ++s.failed; break;
      case JobState::Dropped: ++s.dropped; break;
      case JobState::UncertainPartial: ++s.uncertain; break;
    }
  }
  return s;
}

void PrintQueue::toJson(std::string& out) const {
  const QueueStats s = stats();
  out = "{\"stats\":{\"queued\":" + std::to_string(s.queued) +
        ",\"waiting\":" + std::to_string(s.waiting) +
        ",\"printing\":" + std::to_string(s.printing) +
        ",\"done\":" + std::to_string(s.done) +
        ",\"failed\":" + std::to_string(s.failed) +
        ",\"uncertain\":" + std::to_string(s.uncertain) +
        ",\"pending\":" + std::to_string(s.pending()) + "},\"jobs\":[";
  for (size_t i = 0; i < jobs_.size(); ++i) {
    const PrintJob& j = jobs_[i];
    if (i) out += ",";
    out += "{\"job_id\":\"" + jsonEscape(j.job_id) + "\"";
    out += ",\"dedup_key\":\"" + jsonEscape(j.dedup_key) + "\"";
    out += ",\"state\":\"" + std::string(jobStateName(j.state)) + "\"";
    out += ",\"rows\":" + std::to_string(j.row_count);
    out += ",\"row_bytes\":" + std::to_string(j.row_bytes);
    out += ",\"attempts\":" + std::to_string(j.attempts);
    out += ",\"rows_attempted\":" + std::to_string(j.rows_attempted);
    out += ",\"rows_dispatched\":" + std::to_string(j.rows_dispatched);
    out += ",\"content_hash\":" + std::to_string(j.content_hash);
    out += ",\"last_error\":\"" + jsonEscape(j.last_error) + "\"}";
  }
  out += "]}";
}

void PrintQueue::clear() {
  jobs_.clear();
  recent_.clear();
  clock_ms_ = 0;
}

}  // namespace rtb
