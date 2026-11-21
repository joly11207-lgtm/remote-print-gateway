#include "app/BridgeApp.h"

#if defined(ARDUINO)

#include <Arduino.h>

#include <new>
#include <string.h>

#include <utility>
#include <vector>

#include "app/Base64.h"
#include "app/Discovery.h"
#include "app/HeapBudget.h"
#include "app/JsonMini.h"
#include "app/OtaService.h"
#include "app/PrintRequestParser.h"
#include "app/PrintSubmission.h"
#include "app/SafeTestRegistry.h"
#include "bitmap/BitmapPipeline.h"
#include "boards/BoardCapabilities.h"
#include "config/RtbConfig.h"
#include "printers/catalog/DeviceCatalog.h"
#include "protocols/base/ProtocolFingerprint.h"
#include "runtime/PrintSession.h"

#if RTB_DEBUG_HEAP
#include <esp_heap_caps.h>
#endif

namespace rtb {

namespace {

// Serial scan bounds (V1.2). The BLE scan itself is blocking, so it is short;
// the endpoint ceiling keeps the JSON reply around 2 KB, which at 115200 baud
// is roughly 0.2 s of writing and stays inside the browser's line buffer.
constexpr size_t kSerialScanMaxEndpoints = 12;
constexpr size_t kSerialScanReported = 10;

// Hardware-validation telemetry only (RTB_DEBUG_HEAP=1). Never part of the
// normal build: no allocation, no side effects besides the serial line.
void logHeap(const char* tag) {
#if RTB_DEBUG_HEAP
  Serial.printf("[HEAP] %-24s free=%u largest8=%u min_free=%u\n", tag,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(ESP.getMinFreeHeap()));
#else
  (void)tag;
#endif
}

// Identity reported by GET /api/status. The gateway row keeps its own id; this
// one lets an operator match the radio to the board it is talking to.
const char* hardwareFamilyName() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  return "ESP32-S3-WROOM-1";
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  return "ESP32-C3";
#else
  return "ESP32-WROOM-32E";
#endif
}

std::string gatewayIdValue() {
  char buffer[32];
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(buffer, sizeof(buffer), "gw-esp32-%06llx",
           static_cast<unsigned long long>(mac & 0xFFFFFFULL));
  return std::string(buffer);
}

std::string jsonStringArray(const std::vector<std::string>& values) {
  std::string out = "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) out += ",";
    out += "\"" + jsonEscape(values[i].c_str()) + "\"";
  }
  out += "]";
  return out;
}

std::string jsonStringArray(const char* const* values, size_t count) {
  std::string out = "[";
  for (size_t i = 0; i < count; ++i) {
    if (i) out += ",";
    out += "\"" + jsonEscape(values[i]) + "\"";
  }
  out += "]";
  return out;
}

}  // namespace

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

  // Identity for the read-only USB handshake. It is the same id and hardware
  // family GET /api/status reports, so the browser and the API cannot disagree.
  serial_control_.setIdentity(gatewayIdValue(), hardwareFamilyName(), RTB_VERSION);
  Serial.println("Remote Thermal Printer Bridge V0.1 - compile/mock verified only");
  Serial.println("[ctrl] usb control ready, commands: gateway_info, scan");
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
  // Heap telemetry for the raw entry point: the bitmap is still owned by the
  // queue here, so this sample shows "owned + about to be streamed to BLE".
  logHeap("job_printing");
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
    logHeap("job_done");
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
  pollSerialControl();
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

// V1.1/V1.2 USB control channel. Strictly bounded: a few bytes per tick, one
// fixed line buffer, and only two commands in total (identity + discovery). A
// client that floods the port cannot starve the print queue or the HTTP server,
// and it can never reach a printer, safe test, BLE connect or firmware path
// from here.
void BridgeApp::pollSerialControl() {
  size_t budget = 64;
  while (budget-- > 0) {
    if (Serial.available() <= 0) break;
    const int value = Serial.read();
    if (value < 0) break;
    const char c = static_cast<char>(value);

    if (c == '\n') {
      if (serial_discard_) {
        serial_discard_ = false;
        serial_line_len_ = 0;
        printSerialLine(serial_control_.errorLine("line_too_long"));
      } else if (serial_line_len_ > 0) {
        serial_line_[serial_line_len_] = '\0';
        const SerialControl::Dispatch dispatch =
            serial_control_.dispatch(serial_line_, serial_line_len_);
        serial_line_len_ = 0;

        if (dispatch.command == SerialControl::Command::kScan) {
          // Blocking for the whole scan: leave the byte loop first, so the RX
          // buffer is not held while the radio runs.
          runSerialScan(SerialControl::clampScanDuration(dispatch.duration_ms));
          return;
        }
        if (!dispatch.reply.empty()) printSerialLine(dispatch.reply);
      }
      continue;
    }

    if (serial_discard_) continue;
    if (serial_line_len_ >= SerialControl::kMaxLineBytes) {
      // Over the ceiling: drop the rest of the line, answer once on newline.
      serial_discard_ = true;
      serial_line_len_ = 0;
      continue;
    }
    serial_line_[serial_line_len_++] = c;
  }
}

void BridgeApp::printSerialLine(const std::string& line) {
  Serial.print(line.c_str());
  Serial.print('\n');
}

// The only radio operation reachable from the serial port. It scans, then maps
// each endpoint through the same fingerprinter the HTTP API uses. It does NOT
// connect to anything: no GATT probe, no safe test, no print.
void BridgeApp::runSerialScan(uint32_t duration_ms) {
  if (state_ == PrinterState::Printing || !active_) {
    // Never interleave a scan with a job that owns the 2.4 GHz radio.
    printSerialLine(serial_control_.errorLine("busy"));
    return;
  }

  ScanRequest request;
  request.duration_ms = duration_ms;
  request.max_results = kSerialScanMaxEndpoints;

  ScanResult found;
  Result outcome = Result::IoError;
  wifi_.setBleBusy(true);
  const uint32_t started_ms = millis();
  try {
    outcome = active_->scan(request, found);
  } catch (const std::bad_alloc&) {
    // Same guard the HTTP scan path uses: a hostile advertiser count must not
    // abort the firmware.
    outcome = Result::IoError;
    found.endpoints.clear();
  }
  const uint32_t elapsed_ms = millis() - started_ms;
  wifi_.setBleBusy(false);

  // Recognised devices go to the front until the buffer is half full, unknown
  // advertisers fill it from the back. A truncation can therefore never hide
  // the printer behind a wall of unrelated BLE gadgets.
  SerialScanDevice devices[kSerialScanReported];
  size_t front = 0;
  size_t back = kSerialScanReported;
  for (const Endpoint& endpoint : found.endpoints) {
    const CatalogMatch match = DeviceCatalog::match(endpoint);
    SerialScanDevice* slot = nullptr;
    if (match.profile != nullptr) {
      if (front < back) slot = &devices[front++];
    } else if (back > front) {
      slot = &devices[--back];
    }
    if (slot == nullptr) continue;
    slot->name = endpoint.name.c_str();
    slot->address = endpoint.address.c_str();
    slot->rssi = endpoint.rssi;
    slot->service_uuid = endpoint.service_uuid.c_str();
    slot->fingerprint = protocolFamilyName(match.fingerprint.family);
    slot->confidence = match.fingerprint.confidence;
    slot->evidence = match.fingerprint.evidence;
    slot->model_id = match.profile ? match.profile->model_id : "unmatched";
    slot->match_reason = match.match_reason;
  }
  const size_t reported = front + (kSerialScanReported - back);

  printSerialLine(serial_control_.scanLine(devices, reported, resultName(outcome),
                                           static_cast<unsigned long>(elapsed_ms),
                                           found.endpoints.size()));
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
  // Gateway identity contract for the website adapter (V0.2). `online` is
  // trivially true: this response is being served right now.
  out += ",\"gateway_id\":\"" + gatewayIdValue() + "\"";
  out += ",\"hardware_family\":\"" + std::string(hardwareFamilyName()) + "\"";
  out += ",\"online\":true";
  out += ",\"status\":\"online\"";
  out += ",\"supported_transports\":[\"ble\"]";
  out += ",\"supported_protocols\":[\"tiny_iprint\"]";
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
  // Same window as discovery: a 1.5 s scan was short enough to miss a printer
  // that advertises on a slow interval, which made /api/scan look flaky.
  request.duration_ms = 3000;
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

std::string BridgeApp::scanDetailJson(const std::string& body) {
  int duration = jsonIntField(body, "duration_ms", 4000);
  if (duration < 500) duration = 500;
  if (duration > 10000) duration = 10000;
  const bool inspect = jsonBoolField(body, "inspect", true);

  ScanRequest request;
  request.duration_ms = static_cast<uint32_t>(duration);
  ScanResult found;
  const uint32_t started = millis();
  wifi_.setBleBusy(true);
  const Result scan_result = active_->scan(request, found);
  wifi_.setBleBusy(false);

  // Only a catalog-matched candidate is worth a connection; among those, prefer
  // the one with the strongest advertisement evidence.
  int best = -1;
  int best_confidence = -1;
  for (size_t i = 0; i < found.endpoints.size(); ++i) {
    const CatalogMatch candidate = DeviceCatalog::match(found.endpoints[i]);
    if (!candidate.profile) continue;
    const bool better = candidate.fingerprint.confidence > best_confidence ||
                        (candidate.fingerprint.confidence == best_confidence &&
                         (best < 0 || found.endpoints[i].rssi > found.endpoints[best].rssi));
    if (better) {
      best = static_cast<int>(i);
      best_confidence = candidate.fingerprint.confidence;
    }
  }

  // GATT evidence: connect() looks up the profile's service + write + notify
  // characteristics, so a successful connect proves AE30/AE01/AE02 exist. The
  // link is dropped immediately - this is a read-only identification step.
  bool verified = false;
  // Never pull the radio away from a job that is actually printing.
  if (inspect && state_ != PrinterState::Printing && best >= 0 &&
      !found.endpoints[best].address.empty()) {
    const CatalogMatch chosen = DeviceCatalog::match(found.endpoints[best]);
    const Endpoint prepared = prepareConnectEndpoint(found.endpoints[best], *chosen.profile);
    wifi_.setBleBusy(true);
    verified = active_->connect(prepared) == Result::Ok;
    active_->disconnect();
    wifi_.setBleBusy(false);
  }

  const uint32_t elapsed = millis() - started;
  std::string out = "{\"result\":\"" + std::string(resultName(scan_result)) + "\"";
  out += ",\"transport\":\"" + std::string(active_->name()) + "\"";
  out += ",\"elapsed_ms\":" + std::to_string(elapsed);
  out += ",\"scanned\":" + std::to_string(found.endpoints.size());
  out += ",\"devices\":[";
  for (size_t i = 0; i < found.endpoints.size(); ++i) {
    const Endpoint& ep = found.endpoints[i];
    const CatalogMatch m = DeviceCatalog::match(ep);
    const bool is_best = static_cast<int>(i) == best;
    if (i) out += ",";
    out += "{\"name\":\"" + jsonEscape(ep.name.c_str()) + "\"";
    out += ",\"address\":\"" + jsonEscape(ep.address.c_str()) + "\"";
    out += ",\"rssi\":" + std::to_string(ep.rssi);
    out += ",\"transport\":\"" + std::string(active_->name()) + "\"";
    out += ",\"advertised_services\":" + jsonStringArray(ep.advertised_services);
    out += ",\"gatt_verified\":" + std::string(is_best && verified ? "true" : "false");
    if (is_best && verified && m.profile) {
      const char* services[] = {m.profile->service_uuid};
      const char* writes[] = {m.profile->write_uuid};
      const char* notifies[] = {m.profile->notify_uuid};
      out += ",\"gatt_services\":" + jsonStringArray(services, 1);
      out += ",\"write_characteristics\":" + jsonStringArray(writes, 1);
      out += ",\"notify_characteristics\":" + jsonStringArray(notifies, 1);
    }
    out += ",\"model_id\":\"" +
           std::string(m.profile ? m.profile->model_id : "unmatched") + "\"";
    out += ",\"fingerprint\":\"" +
           std::string(protocolFamilyName(m.fingerprint.family)) + "\"";
    out += ",\"confidence\":" + std::to_string(m.fingerprint.confidence);
    out += "}";
  }
  out += "]}";
  return out;
}

std::string BridgeApp::safeTestJson(const std::string& body, int& http_status) {
  http_status = 200;

  // The caller names an entry; it never supplies bytes. Unknown ids are refused
  // here, so the HTTP surface cannot be used as a raw command channel.
  const std::string safe_test_id = jsonStringField(body, "safe_test_id");
  const SafeTestSpec* spec = findSafeTest(safe_test_id);
  if (spec == nullptr || strcmp(spec->protocol, "tiny_iprint") != 0 || spec->moves_paper) {
    http_status = 422;
    return std::string("{\"ok\":false,\"error\":\"safe_test_not_allowed\"}");
  }

  const std::string address = jsonStringField(body, "address");
  if (address.empty()) {
    http_status = 400;
    return std::string("{\"ok\":false,\"error\":\"invalid_request\"}");
  }
  if (state_ == PrinterState::Printing) {
    // A status query must not interleave with print traffic.
    http_status = 409;
    return std::string("{\"ok\":false,\"error\":\"printer_busy\"}");
  }

  const uint32_t started = millis();
  Endpoint target;
  target.address = address;
  if (profile_ != nullptr) target = prepareConnectEndpoint(target, *profile_);

  wifi_.setBleBusy(true);
  const Result connected = active_->connect(target);
  if (connected != Result::Ok) {
    wifi_.setBleBusy(false);
    std::string out = "{\"ok\":true,\"safe_test_id\":\"" + jsonEscape(safe_test_id.c_str()) + "\"";
    out += ",\"status\":\"failed\",\"reason\":\"connect_failed\"";
    out += ",\"error\":\"" + std::string(resultName(connected)) + "\"";
    out += ",\"elapsed_ms\":" + std::to_string(millis() - started);
    out += "}";
    return out;
  }

  ProtocolContext ctx;
  if (profile_ != nullptr) ctx = makeContext(*profile_);
  SessionConfig config;
  config.row_delay_ms = 10;
  config.command_delay_ms = 10;
  config.status_poll_ms = 30000;

  PrintSession session(*active_, tiny_, ctx, config);
  const Result begun = session.begin(millis());
  PrinterStatus status;
  bool received = false;
  const uint32_t deadline = millis() + 1500;
  while (begun == Result::Ok && static_cast<int32_t>(deadline - millis()) > 0) {
    const PrinterStatus& live = session.status();
    if (live.valid && strcmp(live.source_opcode, "A3") == 0) {
      status = live;
      received = true;
      break;
    }
    delay(5);
  }
  session.end();
  active_->disconnect();
  wifi_.setBleBusy(false);

  std::string out = "{\"ok\":true,\"safe_test_id\":\"" + jsonEscape(safe_test_id.c_str()) + "\"";
  out += ",\"opcode\":\"A3\"";
  out += ",\"elapsed_ms\":" + std::to_string(millis() - started);
  if (!received) {
    out += ",\"status\":\"failed\",\"reason\":\"";
    out += (begun == Result::Ok ? "no_status_response" : resultName(begun));
    out += "\"}";
    return out;
  }
  out += ",\"status\":\"" + std::string(status.ready ? "passed" : "failed") + "\"";
  out += ",\"ready\":" + std::string(status.ready ? "true" : "false");
  out += ",\"raw_status\":" + std::to_string(status.raw);
  out += ",\"paper\":\"" + std::string(status.no_paper ? "empty" : "ok") + "\"";
  out += ",\"cover\":\"" + std::string(status.cover_open ? "open" : "closed") + "\"";
  out += ",\"thermal\":\"" + std::string(status.overheat ? "hot" : "ok") + "\"";
  out += "}";
  return out;
}

BridgeApp::PrintDefaults BridgeApp::printDefaults() const {
  PrintDefaults defaults;
  defaults.row_bytes =
      profile_ ? static_cast<size_t>((profile_->width_dots + 7) / 8) : 48u;
  defaults.feed_dots =
      profile_ ? profile_->feed_dots : static_cast<uint16_t>(64);
  return defaults;
}

Result BridgeApp::enqueueJob(PrintJob& job, const char*& reason) {
  // Zero-copy: the queue takes the bitmap, the caller keeps the identifiers.
  return queue_.enqueueMove(std::move(job), millis(), reason);
}

std::string BridgeApp::submitPrintJson(const char* body, size_t body_len,
                                       int& http_status) {
  http_status = 200;

  // Defaults come from the resolved profile (48 bytes / 64 dots when unknown).
  const PrintDefaults defaults = printDefaults();

  // Logical ceilings (8 KB body / 64 KB bitmap) plus the LIVE heap budget:
  // a legal payload that does not fit right now answers 503, not 413.
  PrintLimits limits;
  limits.heap_budget_bytes = httpPrintHeapBudget();

  logHeap("print_request_received");

  // The parse + enqueue + response live in PrintSubmission.cpp so the host tests
  // run the exact same code; only the heap sample and millis() are Arduino-side.
  const PrintSubmitOutcome outcome =
      submitJsonPrint(body, body_len, limits, defaults.row_bytes, defaults.feed_dots,
                      queue_, millis());

  logHeap("print_request_enqueued");

  http_status = outcome.http_status;
  return outcome.body;
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
