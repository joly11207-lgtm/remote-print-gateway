#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "app/PrintQueue.h"
#include "protocols/base/PrinterProtocol.h"
#include "protocols/base/ProtocolTypes.h"
#include "transport/base/PrinterTransport.h"

namespace rtb {

struct SessionConfig {
  uint16_t row_delay_ms = 10;
  uint16_t command_delay_ms = 5;
  uint32_t status_poll_ms = 30000;
  uint32_t resume_poll_ms = 50;
  uint8_t max_resume_polls = 40;
};

// Stateful print-session behaviour lives here (between protocol and transport):
// notify subscription, keep-alive status requests, flow-control pause/resume,
// per-row pacing. Neither PrinterProtocol nor PrinterTransport owns this state.
//
// Callback lifetime: the registered notification lambda captures a
// std::weak_ptr to the session State, NEVER `this`. end() / ~PrintSession()
// unsubscribe from the transport. Therefore a callback that survives the
// session (or a late notify on a BLE stack) becomes a safe no-op.
class PrintSession {
 public:
  PrintSession(PrinterTransport& transport, PrinterProtocol& protocol,
               const ProtocolContext& ctx, const SessionConfig& config);
  ~PrintSession();

  PrintSession(const PrintSession&) = delete;
  PrintSession& operator=(const PrintSession&) = delete;

  Result begin(uint32_t now_ms);
  Result execute(const PrintJob& job);
  Result keepAlive(uint32_t now_ms);

  // Explicitly ends the session: drops the transport's callback.
  // Idempotent; the destructor calls it too.
  void end();

  bool paused() const { return state_->paused; }
  const PrinterStatus& status() const { return state_->status; }

  // Mid-job progress. rowsAttempted() is incremented BEFORE a row is handed
  // to the transport (the write may have partially reached the printer), and
  // rowsDispatched() only after the transport accepted it. Any attempt > 0
  // means physical "exactly once" can no longer be guaranteed, so the job
  // must NOT be auto-retried as a whole.
  size_t rowsAttempted() const { return rows_attempted_; }
  size_t rowsDispatched() const { return rows_dispatched_; }

 private:
  struct State {
    PrinterStatus status;
    bool paused = false;
    void onNotification(PrinterProtocol& protocol, const uint8_t* data, size_t len);
  };

  bool waitForResume();

  PrinterTransport& transport_;
  PrinterProtocol& protocol_;
  ProtocolContext ctx_;
  SessionConfig config_;
  std::shared_ptr<State> state_;
  bool subscribed_ = false;
  size_t rows_attempted_ = 0;
  size_t rows_dispatched_ = 0;
  uint32_t last_status_ms_ = 0;
};

}  // namespace rtb
