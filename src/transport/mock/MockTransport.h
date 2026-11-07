#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "transport/base/PrinterTransport.h"

namespace rtb {

// In-memory PrinterTransport. Used by host tests and by the firmware's
// Mock Printer debug mode. It records every byte a protocol emits, can
// simulate an offline printer, dropped writes, and inbound notifications.
// It never touches Bluetooth hardware.
class MockTransport : public PrinterTransport {
 public:
  explicit MockTransport(TransportKind emulated_kind = TransportKind::Mock);

  TransportKind kind() const override;
  const char* name() const override { return "mock"; }

  Result scan(const ScanRequest& request, ScanResult& out) override;
  Result connect(const Endpoint& endpoint) override;
  void disconnect() override;
  Result write(const uint8_t* data, size_t len, size_t& written) override;
  Result read(uint8_t* buffer, size_t capacity, size_t& received,
              uint32_t timeout_ms = 0) override;
  Result subscribe(NotificationCallback callback) override;
  bool isConnected() const override;
  TransportCapabilities getCapabilities() const override;

  // ---- Mock controls ------------------------------------------------------
  void setOffline(bool offline) { offline_ = offline; }
  bool isOffline() const { return offline_; }
  void setDropWrites(bool drop) { drop_writes_ = drop; }
  void setMaxWriteChunk(size_t chunk) { max_chunk_ = chunk ? chunk : 1; }
  void setScanEndpoints(const std::vector<Endpoint>& endpoints) { scan_endpoints_ = endpoints; }
  void setAvailable(bool available) { available_ = available; }
  void pushNotification(const uint8_t* data, size_t len);

  // ---- Inspection ---------------------------------------------------------
  const std::vector<uint8_t>& writtenBytes() const { return written_; }
  const std::vector<std::vector<uint8_t> >& writtenChunks() const { return chunks_; }
  size_t writeCount() const { return write_count_; }
  size_t connectCount() const { return connect_count_; }
  size_t disconnectCount() const { return disconnect_count_; }
  bool notifySubscribed() const { return notify_subscribed_; }
  const Endpoint& connectedEndpoint() const { return connected_endpoint_; }
  void clearLog();

 private:
  TransportKind emulated_kind_;
  bool connected_ = false;
  bool offline_ = false;
  bool drop_writes_ = false;
  bool available_ = true;
  bool notify_subscribed_ = false;
  size_t max_chunk_ = 20;
  size_t write_count_ = 0;
  size_t connect_count_ = 0;
  size_t disconnect_count_ = 0;
  Endpoint connected_endpoint_;
  std::vector<Endpoint> scan_endpoints_;
  std::vector<uint8_t> written_;
  std::vector<std::vector<uint8_t> > chunks_;
  std::vector<uint8_t> rx_;
  NotificationCallback callback_;
};

}  // namespace rtb
