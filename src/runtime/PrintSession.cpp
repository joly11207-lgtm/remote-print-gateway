#include "runtime/PrintSession.h"

#include <string>

#include "runtime/Platform.h"

namespace rtb {

void PrintSession::State::onNotification(PrinterProtocol& protocol,
                                        const uint8_t* data, size_t len) {
  PrinterStatus parsed;
  if (protocol.parseStatus(data, len, parsed) != Result::Ok) return;
  status = parsed;
  // 0xAE flow control: 0x10 = stop transmitting, 0x00 = resume.
  if (std::string(parsed.source_opcode) == "AE") paused = parsed.flow_pause;
}

PrintSession::PrintSession(PrinterTransport& transport, PrinterProtocol& protocol,
                           const ProtocolContext& ctx, const SessionConfig& config)
    : transport_(transport),
      protocol_(protocol),
      ctx_(ctx),
      config_(config),
      state_(new State) {}

PrintSession::~PrintSession() { end(); }

void PrintSession::end() {
  if (subscribed_) {
    transport_.unsubscribe();
    subscribed_ = false;
  }
  state_->paused = false;
}

Result PrintSession::begin(uint32_t now_ms) {
  last_status_ms_ = now_ms;
  // Never capture `this`: a stale callback must not be able to touch a
  // destroyed session. weak_ptr.lock() fails as soon as this session dies.
  const std::weak_ptr<State> weak_state = state_;
  PrinterProtocol* protocol = &protocol_;
  Result r = transport_.subscribe(
      [weak_state, protocol](const uint8_t* data, size_t len) {
        std::shared_ptr<State> alive = weak_state.lock();
        if (alive) alive->onNotification(*protocol, data, len);
      });
  if (r != Result::Ok) return r;
  subscribed_ = true;
  r = protocol_.statusRequest(transport_);
  sleepMs(config_.command_delay_ms);
  return r;
}

Result PrintSession::execute(const PrintJob& job) {
  if (!transport_.isConnected()) return Result::NotConnected;
  if (job.row_bytes != ctx_.width_bytes) return Result::InvalidArgument;
  if (job.rows.size() < job.row_bytes * job.row_count) return Result::InvalidArgument;

  Result r = protocol_.init(ctx_, transport_);
  if (r != Result::Ok) return r;
  sleepMs(config_.command_delay_ms);

  for (size_t row = 0; row < job.row_count; ++row) {
    if (state_->paused && !waitForResume()) return Result::Busy;
    ++rows_attempted_;  // a failed write may still have emitted bytes
    r = protocol_.printRow(ctx_, job.rows.data() + row * job.row_bytes,
                           job.row_bytes, transport_);
    if (r != Result::Ok) return r;
    ++rows_dispatched_;
    // >= 8 ms between rows; below that the BLE stack can lock up (FloksiPet).
    sleepMs(config_.row_delay_ms < 8 ? 8 : config_.row_delay_ms);
  }

  sleepMs(config_.command_delay_ms);
  return protocol_.finish(ctx_, transport_);
}

Result PrintSession::keepAlive(uint32_t now_ms) {
  if (now_ms - last_status_ms_ < config_.status_poll_ms) return Result::Ok;
  last_status_ms_ = now_ms;
  return protocol_.statusRequest(transport_);
}

bool PrintSession::waitForResume() {
  for (uint8_t i = 0; i < config_.max_resume_polls && state_->paused; ++i) {
    sleepMs(config_.resume_poll_ms);
  }
  return !state_->paused;
}

}  // namespace rtb
