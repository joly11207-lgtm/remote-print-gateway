#include "app/BridgeApp.h"

#if defined(ARDUINO)

#include <Arduino.h>

#include <vector>

#include "app/Base64.h"
#include "app/Discovery.h"
#include "app/JsonMini.h"
#include "app/OtaService.h"
#include "bitmap/BitmapPipeline.h"
#include "boards/BoardCapabilities.h"
#include "config/RtbConfig.h"
#include "printers/catalog/DeviceCatalog.h"
#include "protocols/base/ProtocolFingerprint.h"
#include "runtime/PrintSession.h"

namespace rtb {

const char* printerStateName(PrinterState state) {
  switch (state) {
    case PrinterState::Offline: return "offline";
    case PrinterState::Scanning: return "scanning";
    case PrinterState::Connecting: return "connecting";
    case PrinterState::Connected: return "connected";
    case PrinterState::Printing: return "printing";
    case PrinterState::Recovering: return "recovering";
    default: return "unknown";
  }
}

BridgeApp::BridgeApp() : server_(80), api_(server_, *this) {}

void BridgeApp::begin() {
  refreshBoardCapabilities();
  wifi_.begin();
  active_ = &ble_;
  state_ = PrinterState::Offline;
  next_retry_ms_ = 0;
  backoff_ms_ = 2000;

  // Mock Printer: a GB01-like endpoint that exists only in RAM.
  std::vector<Endpoint> endpoints;
  Endpoint mock_ep;
  mock_ep.kind = TransportKind::Mock;
  mock_ep.name = "GB01";
  mock_ep.address = "AA:BB:CC:DD:EE:FF";
  mock_ep.rssi = -60;
  mock_ep.service_uuid = "0000AE30-0000-1000-8000-00805F9B34FB";
  mock_ep.write_uuid = "AE01";
  mock_ep.notify_uuid = "AE02";
  endpoints.push_back(mock_ep);
  mock_.setScanEndpoints(endpoints);

  api_.begin();
  server_.begin();
  Serial.println("Remote Thermal Printer Bridge V0.1 - compile/mock verified only");
}

void BridgeApp::setMockMode(bool enabled) {
  if (mock_mode_ == enabled && active_) return;
  ble_.disconnect();
  mock_.disconnect();
  active_ = enabled ? static_cast<PrinterTransport*>(&mock_)
                    : static_cast<PrinterTransport*>(&ble_);
  mock_mode_ = enabled;
  state_ = PrinterState::Offline;
  profile_ = nullptr;
  next_retry_ms_ = 0;
  backoff_ms_ = 2000;
  last_error_.clear();
}

void BridgeApp::printerOffline(const char* reason, uint32_t now_ms) {
  state_ = PrinterState::Recovering;
  last_error_ = reason ? reason : "unknown";
  active_->disconnect();
  next_retry_ms_ = now_ms + backoff_ms_;
  backoff_ms_ = backoff_ms_ * 2 > 30000 ? 30000 : backoff_ms_ * 2;
}

void BridgeApp::attemptConnect(uint32_t now_ms) {
  if (now_ms < next_retry_ms_) return;
  if (state_ == PrinterState::Connected || state_ == PrinterState::Printing) return;
  state_ = PrinterState::Scanning;
  wifi_.setBleBusy(true);

  // Phase 1: AE30-filtered scan. Phase 2 (if needed): unfiltered scan so a
  // device that supports AE30 but does not advertise it can still be matched
  // by name / advertisement / fingerprint. See app/Discovery.cpp.
  const DiscoveryResult discovery = discoverPrinter(*active_);
  if (discovery.result != Result::Ok || !discovery.match.profile) {
    wifi_.setBleBusy(false);
    printerOffline("no_supported_profile", now_ms);
    return;
  }

  state_ = PrinterState::Connecting;
  // UUIDs come from the profile, so connect() still performs full GATT
  // service/characteristic verification on the real device.
  const Endpoint prepared = prepareConnectEndpoint(discovery.endpoint, *discovery.match.profile);
  const Result r = active_->connect(prepared);
  wifi_.setBleBusy(false);
  if (r != Result::Ok) {
    printerOffline(resultName(r), now_ms);
    return;
  }
  profile_ = discovery.match.profile;
  endpoint_ = prepared;
  if (active_ == &ble_) ble_.setMaxWriteChunk(profile_->write_chunk);
  state_ = PrinterState::Connected;
  backoff_ms_ = 2000;   // auto recovery succeeded
  last_error_.clear();
}

void BridgeApp::runPendingJob(uint32_t now_ms) {
  PrintJob* job = queue_.nextPending(now_ms);
  if (!job) return;
  if (!profile_) {
    queue_.markWaitingPrinter(job->dedup_key, "profile_unresolved", now_ms, backoff_ms_);
    printerOffline("profile_unresolved", now_ms);
    return;
  }

  ProtocolContext ctx = makeContext(*profile_);
  if (job->feed_dots) ctx.settings.feed_dots = job->feed_dots;
  SessionConfig cfg;
  cfg.row_delay_ms = profile_->row_delay_ms;
  cfg.command_delay_ms = 10;
  cfg.status_poll_ms = 30000;

  queue_.markPrinting(job->dedup_key);
  state_ = PrinterState::Printing;
  PrintSession session(*active_, tiny_, ctx, cfg);
  wifi_.setBleBusy(true);
  Result r = session.begin(now_ms);
  if (r == Result::Ok) r = session.execute(*job);
  const size_t rows_attempted = session.rowsAttempted();
  const size_t rows_dispatched = session.rowsDispatched();
  session.end();  // drop the transport callback before this scope ends
  wifi_.setBleBusy(false);

  const JobOutcome outcome = classifyJobOutcome(r, rows_attempted);
  if (outcome == JobOutcome::Complete) {
    queue_.markDone(job->dedup_key);
    // Disconnect after each job so Wi-Fi gets the shared 2.4 GHz radio back.
    active_->disconnect();
    state_ = PrinterState::Offline;
    next_retry_ms_ = now_ms;
    last_error_.clear();
    return;
  }

  const std::string reason(resultName(r));
  if (outcome == JobOutcome::SafeRetry) {
    // No bitmap row was handed to the transport: no pixels can be on paper,
    // so an automatic whole-job retry is physically safe.
    queue_.markWaitingPrinter(job->dedup_key, reason.c_str(), now_ms, backoff_ms_);
    printerOffline(reason.c_str(), now_ms);
    return;
  }

  // Print data was already sent. Physical "exactly once" cannot be guaranteed:
  // freeze the job, never auto-reprint it, let a human inspect the paper.
  const std::string partial_reason = "uncertain_partial:" + reason;
  queue_.markUncertainPartial(job->dedup_key, partial_reason.c_str(),
                              rows_attempted, rows_dispatched);
  active_->disconnect();
  state_ = PrinterState::Offline;
  next_retry_ms_ = now_ms;  // other queued jobs may still reconnect
  last_error_ = partial_reason;
}

void BridgeApp::tick() {
  const uint32_t now = millis();
  wifi_.tick(now);
  server_.handleClient();

  if (!queue_.hasPending()) return;
  if (!active_->isConnected()) {
    PrintJob* waiting = queue_.nextPending(now);
    if (waiting) {
      queue_.markWaitingPrinter(waiting->dedup_key, "printer_offline", now, backoff_ms_);
    }
    attemptConnect(now);
    return;
  }
  runPendingJob(now);
  if (state_ == PrinterState::Connected && now - last_keepalive_ms_ > 30000) {
    last_keepalive_ms_ = now;
    tiny_.statusRequest(*active_);
  }
}

CompatibilityInput BridgeApp::makeCompatibilityInput() const {
  CompatibilityInput in;
  in.board = &boardCapabilities();
  in.ble = ble_.getCapabilities();
  in.bluetooth_classic = classic_stub_.getCapabilities();
  in.classic_mock = classic_mock_.getCapabilities();
  in.mock = mock_.getCapabilities();
  in.tiny = tiny_.capabilities();
  in.escpos = escpos_.capabilities();
  in.profile_count = DeviceCatalog::count();
  in.wifi_connected = wifi_.isConnected();
  in.printer_state = printerStateName(state_);
  in.active_transport = active_ ? active_->name() : "none";
  in.pending_jobs = queue_.stats().pending();
  return in;
}

std::string BridgeApp::statusJson() const {
  const BoardCapabilities& caps = boardCapabilities();
  const QueueStats s = queue_.stats();
  std::string out = "{\"version\":\"" + std::string(RTB_VERSION) + "\"";
  out += ",\"target\":\"" + std::string(RTB_TARGET_ID) + "\"";
  out += ",\"chip\":\"" + std::string(caps.chip_name) + "\"";
  out += ",\"wifi\":\"" + std::string(wifi_.modeName()) + "\"";
  out += ",\"ip\":\"" + wifi_.localIp() + "\"";
  out += ",\"printer_state\":\"" + std::string(printerStateName(state_)) + "\"";
  out += ",\"last_error\":\"" + jsonEscape(last_error_.c_str()) + "\"";
  out += ",\"active_transport\":\"" +
         std::string(active_ ? active_->name() : "none") + "\"";
  out += ",\"mock_mode\":" + std::string(mock_mode_ ? "true" : "false");
  out += ",\"profile\":\"" +
         std::string(profile_ ? profile_->model_id : "none") + "\"";
  out += ",\"pending_jobs\":" + std::to_string(s.pending());
  out += ",\"done_jobs\":" + std::to_string(s.done);
  out += ",\"uncertain_jobs\":" + std::to_string(s.uncertain);
  out += ",\"flash_size\":" + std::to_string(caps.flash_size);
  out += ",\"psram_size\":" + std::to_string(caps.psram_size);
  out += ",\"real_printer_tested\":false}";
  return out;
}

std::string BridgeApp::compatibilityJson() const {
  return buildCompatibilityReport(makeCompatibilityInput());
}

std::string BridgeApp::queueJson() const {
  std::string out;
  queue_.toJson(out);
  return out;
}

std::string BridgeApp::scanJson() {
  ScanRequest request;
  request.duration_ms = 1500;
  ScanResult found;
  const Result r = active_->scan(request, found);
  std::string out = "{\"result\":\"" + std::string(resultName(r)) + "\"";
  out += ",\"transport\":\"" + std::string(active_->name()) + "\",\"endpoints\":[";
  for (size_t i = 0; i < found.endpoints.size(); ++i) {
    const Endpoint& ep = found.endpoints[i];
    CatalogMatch m = DeviceCatalog::match(ep);
    if (i) out += ",";
    out += "{\"name\":\"" + jsonEscape(ep.name.c_str()) + "\"";
    out += ",\"address\":\"" + jsonEscape(ep.address.c_str()) + "\"";
    out += ",\"rssi\":" + std::to_string(ep.rssi);
    out += ",\"service_uuid\":\"" + jsonEscape(ep.service_uuid.c_str()) + "\"";
    out += ",\"fingerprint\":\"" + std::string(protocolFamilyName(m.fingerprint.family)) + "\"";
    out += ",\"confidence\":" + std::to_string(m.fingerprint.confidence);
    out += ",\"evidence\":\"" + jsonEscape(m.fingerprint.evidence) + "\"";
    out += ",\"model_id\":\"" +
           std::string(m.profile ? m.profile->model_id : "unmatched") + "\"}";
  }
  out += "]}";
  return out;
}

std::string BridgeApp::submitPrintJson(const std::string& body) {
  const bool test_pattern = jsonBoolField(body, "test", false);
  const std::string job_id = jsonStringField(body, "job_id");
  const std::string rows_b64 = jsonStringField(body, "rows_b64");
  int row_bytes = jsonIntField(body, "row_bytes", 0);
  if (row_bytes <= 0) row_bytes = profile_ ? (profile_->width_dots + 7) / 8 : 48;
  int feed = jsonIntField(body, "feed", profile_ ? profile_->feed_dots : 64);
  if (feed < 0) feed = 0;

  std::vector<uint8_t> rows;
  if (test_pattern) {
    makeTestPattern(static_cast<size_t>(row_bytes), 24, rows);
  } else if (!base64Decode(rows_b64, rows)) {
    return "{\"ok\":false,\"error\":\"invalid_base64\"}";
  }
  if (rows.empty() || rows.size() % static_cast<size_t>(row_bytes) != 0) {
    return "{\"ok\":false,\"error\":\"invalid_bitmap\"}";
  }

  PrintJob job;
  job.job_id = job_id;
  job.rows = rows;
  job.row_bytes = static_cast<size_t>(row_bytes);
  job.row_count = rows.size() / static_cast<size_t>(row_bytes);
  job.feed_dots = static_cast<uint16_t>(feed);
  const char* reason = "";
  const Result r = queue_.enqueue(job, millis(), reason);
  std::string out = "{\"ok\":" + std::string(r == Result::Ok ? "true" : "false");
  out += ",\"result\":\"" + std::string(resultName(r)) + "\"";
  out += ",\"reason\":\"" + jsonEscape(reason) + "\"";
  out += ",\"job_id\":\"" + jsonEscape(job.job_id.c_str()) + "\"";
  out += ",\"dedup_key\":\"" + jsonEscape(job.dedup_key.c_str()) + "\"";
  out += ",\"rows\":" + std::to_string(job.row_count) + "}";
  return out;
}

std::string BridgeApp::setMockJson(const std::string& body) {
  const bool enabled = jsonBoolField(body, "enabled", true);
  const bool offline = jsonBoolField(body, "offline", false);
  const bool drop = jsonBoolField(body, "drop_writes", false);
  setMockMode(enabled);
  mock_.setOffline(offline);
  mock_.setDropWrites(drop);

  const int status = jsonIntField(body, "status", -1);
  if (status >= 0) {
    uint8_t frame[16];
    size_t frame_len = 0;
    const uint8_t payload[1] = {static_cast<uint8_t>(status)};
    if (TinyProtocol::buildFrame(TinyProtocol::kGetStatus, payload, 1, frame,
                                 sizeof(frame), frame_len) == Result::Ok) {
      mock_.pushNotification(frame, frame_len);
    }
  }

  std::string out = "{\"ok\":true,\"mock_mode\":" + std::string(enabled ? "true" : "false");
  out += ",\"offline\":" + std::string(offline ? "true" : "false");
  out += ",\"drop_writes\":" + std::string(drop ? "true" : "false");
  out += ",\"status_byte\":" + std::to_string(status);
  out += ",\"note\":\"software mock only, no real printer\"}";
  return out;
}

std::string BridgeApp::connectJson(const std::string& body) {
  const std::string address = jsonStringField(body, "address");
  const std::string model = jsonStringField(body, "model_id");
  if (address.empty()) return "{\"ok\":false,\"error\":\"address_required\"}";

  const DeviceProfile* p = DeviceCatalog::findByModelId(model.c_str());
  if (!p) p = DeviceCatalog::findByModelId("gb01");
  Endpoint ep;
  ep.kind = active_->kind();
  ep.address = address;
  ep.name = p ? p->display_name : "Unknown";
  ep.service_uuid = p ? p->service_uuid : "AE30";
  ep.write_uuid = p ? p->write_uuid : "AE01";
  ep.notify_uuid = p ? p->notify_uuid : "AE02";

  const Result r = active_->connect(ep);
  if (r == Result::Ok) {
    profile_ = p;
    endpoint_ = ep;
    state_ = PrinterState::Connected;
    last_error_.clear();
  } else {
    printerOffline(resultName(r), millis());
  }
  return std::string("{\"ok\":") + (r == Result::Ok ? "true" : "false") +
         ",\"result\":\"" + resultName(r) + "\"}";
}

std::string BridgeApp::otaJson() const {
  OtaService ota;
  std::string out = "{\"interface_reserved\":" + std::string(ota.interfaceReserved() ? "true" : "false");
  out += ",\"available\":" + std::string(ota.available() ? "true" : "false");
  out += ",\"state\":\"" + std::string(ota.state()) + "\"";
  out += ",\"note\":\"" + jsonEscape(ota.entryPointNote()) + "\"";
  out += ",\"error\":\"" + std::string(RTB_ERR_NOT_IMPLEMENTED) + "\"}";
  return out;
}

void BridgeApp::disconnectPrinter() {
  if (active_) active_->disconnect();
  state_ = PrinterState::Offline;
  profile_ = nullptr;
  last_error_.clear();
}

}  // namespace rtb

#endif  // ARDUINO
