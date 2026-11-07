#include "transport/ble/BleTransport.h"

#include "boards/BoardCapabilities.h"
#include "config/RtbConfig.h"

#if RTB_ENABLE_BLE
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLERemoteCharacteristic.h>
#include <BLERemoteService.h>
#include <BLEScan.h>
#include <BLEUtils.h>

#include <algorithm>
#include <string>
#include <vector>
#endif

namespace rtb {

#if RTB_ENABLE_BLE

namespace {

std::string expandUuid(const std::string& short_or_full, const char* fallback) {
  std::string s = short_or_full.empty() ? std::string(fallback) : short_or_full;
  if (s.size() == 4) return "0000" + s + "-0000-1000-8000-00805F9B34FB";
  if (s.size() == 8) return s + "-0000-1000-8000-00805F9B34FB";
  return s;
}

const char* kDefaultService = "0000AE30-0000-1000-8000-00805F9B34FB";
const char* kDefaultWrite = "0000AE01-0000-1000-8000-00805F9B34FB";
const char* kDefaultNotify = "0000AE02-0000-1000-8000-00805F9B34FB";

}  // namespace

struct BleTransport::Impl {
  BLEClient* client = nullptr;
  BLERemoteCharacteristic* write_char = nullptr;
  BLERemoteCharacteristic* notify_char = nullptr;
  BLEScan* scan = nullptr;
  bool initialized = false;
  bool connected = false;
  bool notify_subscribed = false;
  size_t max_chunk = 20;
  std::vector<Endpoint> scan_results;
  std::vector<uint8_t> rx;
  NotificationCallback callback;
  Endpoint connected_endpoint;

  class ScanCallbacks : public BLEAdvertisedDeviceCallbacks {
   public:
    explicit ScanCallbacks(Impl* owner) : owner_(owner) {}
    void onResult(BLEAdvertisedDevice device) override {
      Endpoint ep;
      ep.kind = TransportKind::Ble;
      ep.address = device.getAddress().toString();
      ep.name = device.getName();
      ep.rssi = device.getRSSI();
      if (device.haveServiceUUID()) {
        ep.service_uuid = device.getServiceUUID().toString();
        ep.advertised_services.push_back(ep.service_uuid);
      }
      owner_->scan_results.push_back(ep);
    }

   private:
    Impl* owner_;
  };

  class ClientCallbacks : public BLEClientCallbacks {
   public:
    explicit ClientCallbacks(Impl* owner) : owner_(owner) {}
    void onConnect(BLEClient*) override { owner_->connected = true; }
    void onDisconnect(BLEClient*) override {
      owner_->connected = false;
      owner_->write_char = nullptr;
      owner_->notify_char = nullptr;
      owner_->notify_subscribed = false;
      // A spontaneous link drop can still be followed by a late notify.
      owner_->callback = NotificationCallback();
    }

   private:
    Impl* owner_;
  };

  ScanCallbacks* scan_cb = nullptr;
  ClientCallbacks* client_cb = nullptr;
};

namespace {
BleTransport::Impl* g_active_impl = nullptr;  // single active BLE link in V0.1

void notifyTrampoline(BLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  BleTransport::Impl* impl = g_active_impl;
  if (!impl || !impl->connected || !data || !length) return;
  impl->rx.insert(impl->rx.end(), data, data + length);
  if (impl->callback) impl->callback(data, length);
}
}  // namespace

BleTransport::BleTransport() : impl_(new Impl) {
  impl_->scan_cb = new Impl::ScanCallbacks(impl_);
  impl_->client_cb = new Impl::ClientCallbacks(impl_);
}

BleTransport::~BleTransport() {
  disconnect();
  delete impl_->scan_cb;
  delete impl_->client_cb;
  delete impl_;
}

TransportKind BleTransport::kind() const { return TransportKind::Ble; }

void BleTransport::setMaxWriteChunk(size_t chunk) {
  impl_->max_chunk = chunk ? chunk : 1;
}

bool BleTransport::notifySubscribed() const { return impl_->notify_subscribed; }

Result BleTransport::scan(const ScanRequest& request, ScanResult& out) {
  out.endpoints.clear();
  if (!impl_->initialized) {
    BLEDevice::init("RTB-V0.1");
    impl_->scan = BLEDevice::getScan();
    impl_->scan->setAdvertisedDeviceCallbacks(impl_->scan_cb);
    impl_->scan->setActiveScan(true);
    impl_->scan->setInterval(100);
    impl_->scan->setWindow(99);
    impl_->initialized = true;
  }
  impl_->scan_results.clear();
  impl_->scan->clearResults();

  uint32_t seconds = request.duration_ms / 1000;
  if (seconds < 1) seconds = 1;
  if (seconds > 10) seconds = 10;
  impl_->scan->start(seconds, false);
  impl_->scan->stop();
  impl_->scan->clearResults();

  for (size_t i = 0; i < impl_->scan_results.size(); ++i) {
    const Endpoint& ep = impl_->scan_results[i];
    if (!request.name_filter.empty() &&
        ep.name.find(request.name_filter) == std::string::npos) continue;
    if (!request.service_uuid_filter.empty() &&
        ep.service_uuid.find(request.service_uuid_filter) == std::string::npos) continue;
    out.endpoints.push_back(ep);
    if (out.endpoints.size() >= request.max_results) break;
  }
  out.result = out.endpoints.empty() ? Result::NotFound : Result::Ok;
  return out.result;
}

Result BleTransport::connect(const Endpoint& endpoint) {
  if (endpoint.address.empty()) return Result::InvalidArgument;
  if (!impl_->initialized) {
    BLEDevice::init("RTB-V0.1");
    impl_->scan = BLEDevice::getScan();
    impl_->initialized = true;
  }
  if (!impl_->client) {
    impl_->client = BLEDevice::createClient();
    impl_->client->setClientCallbacks(impl_->client_cb);
  }
  if (impl_->connected) disconnect();

  BLEAddress address(endpoint.address);
  if (!impl_->client->connect(address)) return Result::NotConnected;

  const std::string service_uuid = expandUuid(endpoint.service_uuid, kDefaultService);
  const std::string write_uuid = expandUuid(endpoint.write_uuid, kDefaultWrite);
  const std::string notify_uuid = expandUuid(endpoint.notify_uuid, kDefaultNotify);

  BLERemoteService* service = impl_->client->getService(service_uuid.c_str());
  if (!service) {
    disconnect();
    return Result::NotFound;
  }
  impl_->write_char = service->getCharacteristic(write_uuid.c_str());
  if (!impl_->write_char) {
    disconnect();
    return Result::NotFound;
  }
  impl_->notify_char = service->getCharacteristic(notify_uuid.c_str());
  if (impl_->notify_char) {
    // Mandatory notify subscription for iPrint-family printers.
    impl_->notify_char->registerForNotify(notifyTrampoline);
    impl_->notify_subscribed = true;
  }
  g_active_impl = impl_;
  impl_->connected_endpoint = endpoint;
  impl_->connected = true;
  return Result::Ok;
}

void BleTransport::disconnect() {
  if (impl_->client && impl_->connected) impl_->client->disconnect();
  impl_->connected = false;
  impl_->write_char = nullptr;
  impl_->notify_char = nullptr;
  impl_->notify_subscribed = false;
  // Clear before g_active_impl: no late notify may reach a dead PrintSession.
  impl_->callback = NotificationCallback();
  if (g_active_impl == impl_) g_active_impl = nullptr;
}

Result BleTransport::write(const uint8_t* data, size_t len, size_t& written) {
  written = 0;
  if (!impl_->connected || !impl_->write_char) return Result::NotConnected;
  if (!data || len == 0) return Result::InvalidArgument;
  size_t offset = 0;
  while (offset < len) {
    const size_t n = std::min(impl_->max_chunk, len - offset);
    impl_->write_char->writeValue(const_cast<uint8_t*>(data + offset), n, false);
    offset += n;
  }
  written = len;
  return Result::Ok;
}

Result BleTransport::read(uint8_t* buffer, size_t capacity, size_t& received,
                          uint32_t timeout_ms) {
  (void)timeout_ms;
  received = 0;
  if (!impl_->connected) return Result::NotConnected;
  if (impl_->rx.empty() || !buffer || capacity == 0) return Result::Ok;
  const size_t n = std::min(capacity, impl_->rx.size());
  std::copy(impl_->rx.begin(), impl_->rx.begin() + static_cast<std::ptrdiff_t>(n), buffer);
  impl_->rx.erase(impl_->rx.begin(), impl_->rx.begin() + static_cast<std::ptrdiff_t>(n));
  received = n;
  return Result::Ok;
}

Result BleTransport::subscribe(NotificationCallback callback) {
  // Replaces any previous subscription; an empty callback unsubscribes.
  impl_->callback = callback;
  return Result::Ok;
}

bool BleTransport::isConnected() const {
  return impl_->connected && impl_->client && impl_->client->isConnected();
}

TransportCapabilities BleTransport::getCapabilities() const {
  TransportCapabilities c;
  c.available = RTB_ENABLE_BLE != 0 && boardCapabilities().ble;
  c.supports_scan = true;
  c.supports_notifications = true;
  c.requires_notify_subscription = true;
  c.max_write_chunk = impl_->max_chunk;
  c.recommended_row_delay_ms = 8;  // FloksiPet: < 8 ms can lock up the BLE stack
  c.note = "BLE GATT AE30/AE01/AE02; Wi-Fi/BLE coexistence handled in BridgeApp";
  return c;
}

#else  // RTB_ENABLE_BLE == 0 : host build, no Bluetooth headers at all

struct BleTransport::Impl { int unused; };

BleTransport::BleTransport() : impl_(new Impl) {}
BleTransport::~BleTransport() { delete impl_; }
TransportKind BleTransport::kind() const { return TransportKind::Ble; }
void BleTransport::setMaxWriteChunk(size_t) {}
bool BleTransport::notifySubscribed() const { return false; }
Result BleTransport::scan(const ScanRequest&, ScanResult& out) {
  out.endpoints.clear();
  out.result = Result::NotSupportedOnThisTarget;
  return out.result;
}
Result BleTransport::connect(const Endpoint&) { return Result::NotSupportedOnThisTarget; }
void BleTransport::disconnect() {}
Result BleTransport::write(const uint8_t*, size_t, size_t& written) {
  written = 0;
  return Result::NotSupportedOnThisTarget;
}
Result BleTransport::read(uint8_t*, size_t, size_t& received, uint32_t) {
  received = 0;
  return Result::NotSupportedOnThisTarget;
}
Result BleTransport::subscribe(NotificationCallback) {
  return Result::NotSupportedOnThisTarget;
}
bool BleTransport::isConnected() const { return false; }
TransportCapabilities BleTransport::getCapabilities() const {
  TransportCapabilities c;
  c.unavailable_reason = RTB_ERR_NOT_SUPPORTED_ON_THIS_TARGET;
  c.note = "compiled without BLE (host test build)";
  return c;
}

#endif

}  // namespace rtb
