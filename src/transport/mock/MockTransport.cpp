#include "transport/mock/MockTransport.h"

#include <algorithm>
#include <cstddef>
#include <string.h>

#include "protocols/base/ProtocolFingerprint.h"

namespace rtb {

MockTransport::MockTransport(TransportKind emulated_kind) : emulated_kind_(emulated_kind) {
  connected_endpoint_.kind = emulated_kind_;
}

TransportKind MockTransport::kind() const { return emulated_kind_; }

Result MockTransport::scan(const ScanRequest& request, ScanResult& out) {
  out.endpoints.clear();
  if (offline_) {
    out.result = Result::NotFound;
    return out.result;
  }
  for (size_t i = 0; i < scan_endpoints_.size(); ++i) {
    const Endpoint& e = scan_endpoints_[i];
    if (!request.name_filter.empty() &&
        !containsIgnoreCase(e.name, request.name_filter)) continue;
    if (!request.service_uuid_filter.empty() &&
        !containsIgnoreCase(e.service_uuid, request.service_uuid_filter)) continue;
    out.endpoints.push_back(e);
    if (out.endpoints.size() >= request.max_results) break;
  }
  out.result = out.endpoints.empty() ? Result::NotFound : Result::Ok;
  return out.result;
}

Result MockTransport::connect(const Endpoint& endpoint) {
  if (!available_) return Result::NotSupportedOnThisTarget;
  if (offline_) return Result::NotConnected;
  connected_ = true;
  connected_endpoint_ = endpoint;
  ++connect_count_;
  return Result::Ok;
}

void MockTransport::disconnect() {
  if (connected_) ++disconnect_count_;
  connected_ = false;
  // Late notifications must never reach a callback whose owner has ended.
  callback_ = NotificationCallback();
  notify_subscribed_ = false;
}

Result MockTransport::write(const uint8_t* data, size_t len, size_t& written) {
  written = 0;
  if (!connected_) return Result::NotConnected;
  if (offline_) return Result::IoError;
  if (!data || len == 0) return Result::InvalidArgument;
  if (drop_writes_) return Result::IoError;

  ++write_count_;
  size_t offset = 0;
  while (offset < len) {
    const size_t n = std::min(max_chunk_, len - offset);
    const uint8_t* begin = data + offset;
    chunks_.push_back(std::vector<uint8_t>(begin, begin + n));
    written_.insert(written_.end(), begin, begin + n);
    offset += n;
  }
  written = len;
  return Result::Ok;
}

Result MockTransport::read(uint8_t* buffer, size_t capacity, size_t& received,
                           uint32_t timeout_ms) {
  (void)timeout_ms;
  received = 0;
  if (!connected_) return Result::NotConnected;
  if (rx_.empty() || !buffer || capacity == 0) return Result::Ok;
  const size_t n = std::min(capacity, rx_.size());
  memcpy(buffer, rx_.data(), n);
  rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(n));
  received = n;
  return Result::Ok;
}

Result MockTransport::subscribe(NotificationCallback callback) {
  // An empty callback is the unsubscribe signal (see PrinterTransport.h).
  callback_ = callback;
  notify_subscribed_ = static_cast<bool>(callback_);
  return Result::Ok;
}

bool MockTransport::isConnected() const { return connected_ && !offline_; }

TransportCapabilities MockTransport::getCapabilities() const {
  TransportCapabilities c;
  c.available = available_;
  c.supports_scan = true;
  c.supports_notifications = true;
  c.requires_notify_subscription = true;
  c.max_write_chunk = max_chunk_;
  c.recommended_row_delay_ms = 0;
  c.unavailable_reason = available_ ? nullptr : RTB_ERR_NOT_SUPPORTED_ON_THIS_TARGET;
  c.note = "software mock transport, no radio involved";
  return c;
}

void MockTransport::pushNotification(const uint8_t* data, size_t len) {
  if (!data || len == 0) return;
  rx_.insert(rx_.end(), data, data + len);
  if (callback_) callback_(data, len);
}

void MockTransport::clearLog() {
  written_.clear();
  chunks_.clear();
  rx_.clear();
  write_count_ = 0;
  connect_count_ = 0;
  disconnect_count_ = 0;
}

}  // namespace rtb
