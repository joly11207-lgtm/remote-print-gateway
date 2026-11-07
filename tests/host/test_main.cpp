// Host-native Mock tests. Compiled with `zig c++` (no ESP32 required).
// These tests exercise the REAL protocol / catalog / queue / transport code,
// not a Python re-implementation of it.

#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "app/Base64.h"
#include "app/Discovery.h"
#include "app/CompatibilityReport.h"
#include "app/JsonMini.h"
#include "app/OtaService.h"
#include "app/PrintQueue.h"
#include "bitmap/BitmapPipeline.h"
#include "boards/BoardCapabilities.h"
#include "config/RtbConfig.h"
#include "printers/catalog/DeviceCatalog.h"
#include "protocols/base/ProtocolFingerprint.h"
#include "protocols/escpos/EscPosProtocol.h"
#include "protocols/peripage/PeriPageProtocol.h"
#include "protocols/paperang/PaperangProtocol.h"
#include "protocols/tiny_iprint/TinyProtocol.h"
#include "runtime/PrintSession.h"
#include "transport/ble/BleTransport.h"
#include "transport/bt_classic/BluetoothClassicTransport.h"
#include "transport/bt_classic/BtClassicMockTransport.h"
#include "transport/mock/MockTransport.h"

using namespace rtb;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(expr)                                                          \
  do {                                                                       \
    ++g_checks;                                                               \
    if (!(expr)) {                                                            \
      ++g_failed;                                                             \
      printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);               \
    }                                                                        \
  } while (0)

static void section(const char* name) { printf("[ RUN ] %s\n", name); }

static std::vector<uint8_t> makeFrame(uint8_t opcode,
                                      const std::vector<uint8_t>& payload) {
  uint8_t buffer[300];
  size_t len = 0;
  const Result r = TinyProtocol::buildFrame(opcode, payload.empty() ? nullptr : payload.data(),
                                            payload.size(), buffer, sizeof(buffer), len);
  if (r != Result::Ok) return std::vector<uint8_t>();
  return std::vector<uint8_t>(buffer, buffer + len);
}

static void collectFrames(const std::vector<uint8_t>& bytes,
                          std::vector<uint8_t>& opcodes, bool& all_valid) {
  opcodes.clear();
  all_valid = true;
  size_t pos = 0;
  while (pos + TinyProtocol::kFrameOverhead <= bytes.size()) {
    const size_t payload_len =
        static_cast<size_t>(bytes[pos + 4]) | (static_cast<size_t>(bytes[pos + 5]) << 8);
    const size_t total = payload_len + TinyProtocol::kFrameOverhead;
    if (pos + total > bytes.size()) {
      all_valid = false;
      return;
    }
    uint8_t opcode = 0;
    const uint8_t* payload = nullptr;
    size_t parsed_len = 0;
    if (TinyProtocol::validateFrame(bytes.data() + pos, total, opcode, payload,
                                    parsed_len) != Result::Ok) {
      all_valid = false;
      return;
    }
    opcodes.push_back(opcode);
    pos += total;
  }
  all_valid = all_valid && pos == bytes.size();
}

static size_t countOpcode(const std::vector<uint8_t>& opcodes, uint8_t opcode) {
  size_t n = 0;
  for (size_t i = 0; i < opcodes.size(); ++i) if (opcodes[i] == opcode) ++n;
  return n;
}

static Endpoint gb01Endpoint(TransportKind kind) {
  Endpoint ep;
  ep.kind = kind;
  ep.name = "GB01";
  ep.address = "AA:BB:CC:DD:EE:FF";
  ep.rssi = -60;
  ep.service_uuid = "0000AE30-0000-1000-8000-00805F9B34FB";
  ep.write_uuid = "AE01";
  ep.notify_uuid = "AE02";
  return ep;
}

static PrintJob makePrintJob(const std::string& job_id, const std::vector<uint8_t>& rows) {
  PrintJob job;
  job.job_id = job_id;
  job.rows = rows;
  job.row_bytes = 48;
  job.row_count = rows.size() / 48;
  return job;
}

// ---- Regression helpers --------------------------------------------------

// Counts every delivery through the transport's stored callback.
class CountingTransport : public MockTransport {
 public:
  int callback_invocations = 0;
  Result subscribe(NotificationCallback cb) override {
    if (!cb) return MockTransport::subscribe(cb);
    return MockTransport::subscribe(
        [this, cb](const uint8_t* d, size_t l) {
          ++callback_invocations;
          cb(d, l);
        });
  }
};

// Deliberately hostile transport: keeps a copy of the callback even after an
// unsubscribe, simulating a BLE stack that retains a registered notify fn.
class StaleCallbackTransport : public MockTransport {
 public:
  NotificationCallback retained;
  int retained_calls = 0;
  Result subscribe(NotificationCallback cb) override {
    if (cb) {
      retained = [this, cb](const uint8_t* d, size_t l) {
        ++retained_calls;
        cb(d, l);
      };
    }
    // Intentionally ignores subscribe({}): retained_ stays alive.
    return MockTransport::subscribe(cb);
  }
  void fireRetained(const uint8_t* d, size_t l) {
    if (retained) retained(d, l);
  }
};

// Counts status parsing calls so we can prove a dead session is never entered.
class SpyProtocol : public PrinterProtocol {
 public:
  mutable int parse_calls = 0;
  ProtocolFamily family() const override { return ProtocolFamily::TinyIPrint; }
  const char* name() const override { return "spy"; }
  ProtocolCapabilities capabilities() const override { return ProtocolCapabilities(); }
  Result init(const ProtocolContext&, PrinterTransport&) override { return Result::Ok; }
  Result printRow(const ProtocolContext&, const uint8_t*, size_t,
                  PrinterTransport&) override { return Result::Ok; }
  Result finish(const ProtocolContext&, PrinterTransport&) override { return Result::Ok; }
  Result feed(const ProtocolContext&, uint16_t, PrinterTransport&) override { return Result::Ok; }
  Result statusRequest(PrinterTransport&) override { return Result::Ok; }
  Result parseStatus(const uint8_t*, size_t, PrinterStatus& out) const override {
    ++parse_calls;
    out = PrinterStatus();
    out.valid = true;
    out.source_opcode = "A3";
    return Result::Ok;
  }
};

class RecordingScanTransport : public MockTransport {
 public:
  size_t scan_calls = 0;
  std::vector<std::string> filters;
  Result scan(const ScanRequest& req, ScanResult& out) override {
    ++scan_calls;
    filters.push_back(req.service_uuid_filter);
    return MockTransport::scan(req, out);
  }
};

// Fails a write once N writes have succeeded (mid-job failure simulation).
class FailAfterWritesTransport : public MockTransport {
 public:
  explicit FailAfterWritesTransport(size_t fail_after) : fail_after_(fail_after) {}
  Result write(const uint8_t* data, size_t len, size_t& written) override {
    if (writeCount() >= fail_after_) {
      written = 0;
      return Result::IoError;
    }
    return MockTransport::write(data, len, written);
  }

 private:
  size_t fail_after_;
};

static void testBoardCapabilities() {
  section("BoardCapabilities / capability detection");
  const BoardCapabilities& c = boardCapabilities();
  CHECK(c.wifi);
  CHECK(c.ble);
  CHECK(c.flash_size > 0);
  CHECK(c.ota);
#if defined(RTB_HOST_EMULATE_WROOM32E)
  CHECK(c.bluetooth_classic);      // ESP32 dual-mode chip
  CHECK(!c.psram);
  CHECK(std::string(c.target_id) == "host-test-emulating-wroom32e");
#else
  CHECK(!c.bluetooth_classic);     // ESP32-S3: BLE-only silicon
  CHECK(c.psram);
  CHECK(std::string(c.target_id) == "host-test-emulating-s3");
#endif
}

static void testClassicGate() {
  section("Bluetooth Classic capability gate / NOT_SUPPORTED_ON_THIS_TARGET");
  BluetoothClassicTransport classic;
  ScanRequest req;
  ScanResult res;
  const Result scan_result = classic.scan(req, res);
  size_t written = 0;
  uint8_t byte = 0;
  const Result write_result = classic.write(&byte, 1, written);
  const TransportCapabilities caps = classic.getCapabilities();
  CHECK(!caps.available);
  CHECK(!classic.isConnected());
  CHECK(resultName(Result::NotSupportedOnThisTarget) ==
        std::string("NOT_SUPPORTED_ON_THIS_TARGET"));
#if defined(RTB_HOST_EMULATE_WROOM32E)
  CHECK(classic.hardwareSupported());
  CHECK(std::string(resultName(scan_result)) == "NOT_IMPLEMENTED");
  CHECK(std::string(caps.unavailable_reason) == "NOT_IMPLEMENTED");
#else
  CHECK(!classic.hardwareSupported());
  CHECK(scan_result == Result::NotSupportedOnThisTarget);
  CHECK(write_result == Result::NotSupportedOnThisTarget);
  CHECK(std::string(caps.unavailable_reason) == "NOT_SUPPORTED_ON_THIS_TARGET");
#endif
  CHECK(!classic.implementationEnabled());  // V0.1 never enables SPP/RFCOMM
}

static void testCrcAndFraming() {
  section("TinyProtocol CRC-8 and frame validation");
  const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
  CHECK(TinyProtocol::crc8(nullptr, 0) == 0x00);
  CHECK(TinyProtocol::crc8(check, sizeof(check)) == 0xF4);
  CHECK(TinyProtocol::mirrorByte(0x01) == 0x80);
  CHECK(TinyProtocol::mirrorByte(0x80) == 0x01);
  CHECK(TinyProtocol::mirrorByte(0xAA) == 0x55);

  std::vector<uint8_t> feed_payload;
  feed_payload.push_back(30);
  feed_payload.push_back(0x00);
  const std::vector<uint8_t> frame = makeFrame(TinyProtocol::kFeed, feed_payload);
  CHECK(frame.size() == 10);
  CHECK(frame[0] == 0x51 && frame[1] == 0x78 && frame[2] == 0xA1);
  CHECK(frame[3] == 0x00 && frame[4] == 0x02 && frame[5] == 0x00);
  CHECK(frame[6] == 30 && frame[7] == 0x00);
  CHECK(frame[8] == 0x81);  // CRC-8(poly 0x07) of {0x1E,0x00}
  CHECK(frame[9] == 0xFF);

  uint8_t opcode = 0;
  const uint8_t* payload = nullptr;
  size_t payload_len = 0;
  CHECK(TinyProtocol::validateFrame(frame.data(), frame.size(), opcode, payload,
                                    payload_len) == Result::Ok);
  CHECK(opcode == TinyProtocol::kFeed);
  CHECK(payload_len == 2 && payload[0] == 30);

  std::vector<uint8_t> bad_crc = frame;
  bad_crc[8] ^= 0xFF;
  CHECK(TinyProtocol::validateFrame(bad_crc.data(), bad_crc.size(), opcode, payload,
                                    payload_len) == Result::IoError);
  std::vector<uint8_t> bad_magic = frame;
  bad_magic[0] = 0x00;
  CHECK(TinyProtocol::validateFrame(bad_magic.data(), bad_magic.size(), opcode, payload,
                                    payload_len) == Result::InvalidArgument);
  CHECK(TinyProtocol::validateFrame(frame.data(), frame.size() - 1, opcode, payload,
                                    payload_len) == Result::InvalidArgument);
}

static void runTinyOverTransport(PrinterTransport& transport, const char* label) {
  printf("  [%s] transport=%s\n", label, transport.name());
  CHECK(transport.getCapabilities().available);
  CHECK(transport.connect(gb01Endpoint(transport.kind())) == Result::Ok);
  CHECK(transport.isConnected());

  TinyProtocol protocol;
  const DeviceProfile* profile = DeviceCatalog::findByModelId("gb01");
  CHECK(profile != nullptr);
  ProtocolContext ctx = makeContext(*profile);
  CHECK(ctx.width_dots == 384 && ctx.width_bytes == 48);

  CHECK(protocol.init(ctx, transport) == Result::Ok);
  std::vector<uint8_t> row(ctx.width_bytes, 0xAA);
  CHECK(protocol.printRow(ctx, row.data(), row.size(), transport) == Result::Ok);
  CHECK(protocol.printRow(ctx, row.data(), row.size(), transport) == Result::Ok);
  CHECK(protocol.feed(ctx, 130, transport) == Result::Ok);

  std::vector<uint8_t> opcodes;
  bool all_valid = false;
  collectFrames(static_cast<MockTransport&>(transport).writtenBytes(), opcodes, all_valid);
  CHECK(all_valid);
  CHECK(opcodes.size() >= 9);
  CHECK(countOpcode(opcodes, TinyProtocol::kGetStatus) == 1);
  CHECK(countOpcode(opcodes, TinyProtocol::kQuality) == 1);
  CHECK(countOpcode(opcodes, TinyProtocol::kSpeed) == 1);
  CHECK(countOpcode(opcodes, TinyProtocol::kEnergy) == 1);
  CHECK(countOpcode(opcodes, TinyProtocol::kDrawingMode) == 1);
  CHECK(countOpcode(opcodes, TinyProtocol::kLattice) == 1);
  CHECK(countOpcode(opcodes, TinyProtocol::kPrintRow) == 2);
  CHECK(countOpcode(opcodes, TinyProtocol::kFeed) == 2);  // 130 dots -> 100 + 30

  transport.disconnect();
  CHECK(!transport.isConnected());
}

static void testCatalogAndFingerprint() {
  section("Device catalog / protocol fingerprint");
  CHECK(DeviceCatalog::count() == 5);
  CHECK(DeviceCatalog::findByModelId("gb01") != nullptr);
  CHECK(DeviceCatalog::findByModelId("does_not_exist") == nullptr);

  Endpoint ble_ep = gb01Endpoint(TransportKind::Ble);
  CatalogMatch m = DeviceCatalog::match(ble_ep);
  CHECK(m.profile != nullptr);
  CHECK(std::string(m.profile->model_id) == "gb01");
  CHECK(m.fingerprint.family == ProtocolFamily::TinyIPrint);
  CHECK(m.fingerprint.confidence >= 90);
  CHECK(profileSupports(*m.profile, TransportKind::Ble));
  CHECK(!profileSupports(*m.profile, TransportKind::BluetoothClassic));

  Endpoint classic_ep;
  classic_ep.kind = TransportKind::BluetoothClassic;
  classic_ep.name = "POS-Printer";
  classic_ep.address = "11:22:33:44:55:66";
  CatalogMatch cm = DeviceCatalog::match(classic_ep);
  CHECK(cm.profile != nullptr);
  CHECK(std::string(cm.profile->model_id) == "escpos_spp_future");
  CHECK(cm.profile->protocol_family == ProtocolFamily::EscPos);
  CHECK(!cm.profile->implemented);
  CHECK(profileSupports(*cm.profile, TransportKind::BluetoothClassic));

  Endpoint unknown;
  unknown.kind = TransportKind::Ble;
  unknown.name = "Random Speaker";
  CHECK(DeviceCatalog::match(unknown).profile == nullptr);

  std::string json;
  DeviceCatalog::toJson(json);
  CHECK(json.find("\"model_id\":\"gb01\"") != std::string::npos);
  CHECK(json.find("\"supported_transports\":[\"bluetooth_classic\"]") != std::string::npos);
}

static void testQueueDedupAndRecovery() {
  section("Print queue / dedup / offline waiting / auto recovery");
  PrintQueue queue(8, 64 * 1024);
  std::vector<uint8_t> rows(48 * 2, 0x55);
  PrintJob a;
  a.job_id = "JOB-1";
  a.rows = rows;
  a.row_bytes = 48;
  a.row_count = 2;
  const char* reason = "";
  CHECK(queue.enqueue(a, 0, reason) == Result::Ok);
  CHECK(std::string(a.dedup_key) == "JOB-1");

  PrintJob duplicate = a;
  CHECK(queue.enqueue(duplicate, 10, reason) == Result::Busy);
  CHECK(std::string(reason) == "duplicate_job");

  // Content-hash dedup for a DIFFERENT job_id is no longer allowed to block
  // submission; full coverage lives in testDedupSemanticsById().

  PrintJob* next = queue.nextPending(0);
  CHECK(next != nullptr);
  queue.markPrinting(next->dedup_key);
  queue.markWaitingPrinter(next->dedup_key, "printer_offline", 100, 2000);
  CHECK(queue.nextPending(1500) == nullptr);   // offline: job waits
  CHECK(queue.stats().waiting == 1);
  PrintJob* retried = queue.nextPending(2100);
  CHECK(retried != nullptr);                   // auto recovery window
  CHECK(retried->attempts == 1);
  CHECK(retried->last_error == "printer_offline");
  queue.markPrinting(retried->dedup_key);
  queue.markDone(retried->dedup_key);
  CHECK(queue.stats().done == 1);
  CHECK(!queue.hasPending());

  PrintJob again = a;
  CHECK(queue.enqueue(again, 3000, reason) == Result::Busy);  // recent-history dedup
  std::string json;
  queue.toJson(json);
  CHECK(json.find("\"state\":\"done\"") != std::string::npos);

  PrintQueue bad_queue;
  PrintJob invalid;
  invalid.rows = rows;
  invalid.row_bytes = 48;
  invalid.row_count = 100;  // inconsistent bitmap
  CHECK(bad_queue.enqueue(invalid, 0, reason) == Result::InvalidArgument);
}

static void testDedupSemanticsById() {
  section("Dedup semantics: job_id idempotency / content hash for no-ID only");
  const std::vector<uint8_t> content_x(48 * 2, 0x55);
  const char* reason = "";

  // (1) job_id=A + content X -> OK; repeat A + X -> duplicate_job.
  {
    PrintQueue q;
    PrintJob a = makePrintJob("A", content_x);
    CHECK(q.enqueue(a, 0, reason) == Result::Ok);
    CHECK(a.dedup_key == "A");
    CHECK(a.dedup_by_id);
    PrintJob again = makePrintJob("A", content_x);
    CHECK(q.enqueue(again, 1, reason) == Result::Busy);
    CHECK(std::string(reason) == "duplicate_job");
  }

  // (2) job_id=A + content X -> OK; job_id=B + SAME content X -> MUST be OK.
  {
    PrintQueue q;
    PrintJob a = makePrintJob("A", content_x);
    CHECK(q.enqueue(a, 0, reason) == Result::Ok);
    PrintJob b = makePrintJob("B", content_x);
    CHECK(q.enqueue(b, 1, reason) == Result::Ok);
    CHECK(b.dedup_key == "B");
    CHECK(b.dedup_by_id);
    CHECK(b.content_hash == a.content_hash);
    CHECK(b.content_key == a.content_key);  // same bytes, but NOT a dedup key
    CHECK(q.size() == 2);
  }

  // (2b) Same after A is DONE: recent history must not leak the content hash.
  {
    PrintQueue q;
    PrintJob a = makePrintJob("A", content_x);
    CHECK(q.enqueue(a, 0, reason) == Result::Ok);
    q.markPrinting(a.dedup_key);
    q.markDone(a.dedup_key);
    PrintJob b = makePrintJob("B", content_x);
    CHECK(q.enqueue(b, 3000, reason) == Result::Ok);
    CHECK(q.stats().done == 1 && q.stats().queued == 1);
  }

  // (3) no job_id + content X -> OK; repeat no-ID + X -> duplicate_job.
  {
    PrintQueue q;
    PrintJob n1 = makePrintJob("", content_x);
    CHECK(q.enqueue(n1, 0, reason) == Result::Ok);
    CHECK(!n1.dedup_by_id);
    CHECK(n1.dedup_key == n1.content_key);
    PrintJob n2 = makePrintJob("", content_x);
    CHECK(q.enqueue(n2, 1, reason) == Result::Busy);
    CHECK(std::string(reason) == "duplicate_job");
  }

  // (3b) The two namespaces must not block each other.
  {
    PrintQueue q;
    PrintJob n = makePrintJob("", content_x);
    CHECK(q.enqueue(n, 0, reason) == Result::Ok);
    PrintJob idjob = makePrintJob("B", content_x);
    CHECK(q.enqueue(idjob, 1, reason) == Result::Ok);
  }
  {
    PrintQueue q;
    PrintJob idjob = makePrintJob("A", content_x);
    CHECK(q.enqueue(idjob, 0, reason) == Result::Ok);
    PrintJob n = makePrintJob("", content_x);
    CHECK(q.enqueue(n, 1, reason) == Result::Ok);
  }
  {
    // Even a job_id that happens to equal a content-hash string is isolated.
    PrintQueue q;
    PrintJob n = makePrintJob("", content_x);
    CHECK(q.enqueue(n, 0, reason) == Result::Ok);
    PrintJob idjob = makePrintJob(n.content_key, content_x);
    CHECK(q.enqueue(idjob, 1, reason) == Result::Ok);
  }

  // (4) uncertain_partial A, then manual reprint B + same content -> MUST be OK.
  {
    PrintQueue q;
    PrintJob a = makePrintJob("A", content_x);
    CHECK(q.enqueue(a, 0, reason) == Result::Ok);
    q.markPrinting(a.dedup_key);
    q.markUncertainPartial(a.dedup_key, "uncertain_partial:IO_ERROR", 2, 1);
    CHECK(q.stats().uncertain == 1);
    PrintJob b = makePrintJob("B", content_x);
    CHECK(q.enqueue(b, 10, reason) == Result::Ok);
    CHECK(q.stats().uncertain == 1);
    CHECK(q.stats().queued == 1);
    CHECK(q.stats().pending() == 1);
    PrintJob* reprint = q.nextPending(10);
    CHECK(reprint != nullptr);
    CHECK(reprint->dedup_key == "B");
    // Same job_id is still rejected, forcing a conscious new id.
    PrintJob a_again = makePrintJob("A", content_x);
    CHECK(q.enqueue(a_again, 20, reason) == Result::Busy);
    CHECK(std::string(reason) == "duplicate_job");
  }
}

static void testSessionEndToEnd() {
  section("PrintSession over MockPrinter: init / bitmap / feed / status / flow control");
  MockTransport mock;
  CHECK(mock.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
  mock.setMaxWriteChunk(20);

  const DeviceProfile* profile = DeviceCatalog::findByModelId("gb01");
  ProtocolContext ctx = makeContext(*profile);
  SessionConfig cfg;
  cfg.row_delay_ms = 10;
  cfg.command_delay_ms = 5;
  TinyProtocol protocol;
  PrintSession session(mock, protocol, ctx, cfg);
  CHECK(session.begin(0) == Result::Ok);
  CHECK(mock.notifySubscribed());

  PrintJob job;
  job.job_id = "MOCK-JOB";
  job.row_bytes = 48;
  job.row_count = 4;
  job.rows.assign(48 * 4, 0xAA);
  CHECK(session.execute(job) == Result::Ok);

  std::vector<uint8_t> opcodes;
  bool all_valid = false;
  collectFrames(mock.writtenBytes(), opcodes, all_valid);
  CHECK(all_valid);
  CHECK(countOpcode(opcodes, TinyProtocol::kGetStatus) == 2);  // begin + init
  CHECK(countOpcode(opcodes, TinyProtocol::kPrintRow) == 4);
  CHECK(countOpcode(opcodes, TinyProtocol::kLattice) == 2);    // start + end
  CHECK(countOpcode(opcodes, TinyProtocol::kFeed) == 1);
  CHECK(mock.writtenChunks().size() > 0);                       // 20-byte GATT chunks

  std::vector<uint8_t> status_payload;
  status_payload.push_back(0x80);
  const std::vector<uint8_t> status_frame =
      makeFrame(TinyProtocol::kGetStatus, status_payload);
  mock.pushNotification(status_frame.data(), status_frame.size());
  CHECK(session.status().valid);
  CHECK(session.status().ready);
  CHECK(std::string(session.status().source_opcode) == "A3");

  std::vector<uint8_t> no_paper;
  no_paper.push_back(0x01);
  const std::vector<uint8_t> no_paper_frame = makeFrame(TinyProtocol::kGetStatus, no_paper);
  mock.pushNotification(no_paper_frame.data(), no_paper_frame.size());
  CHECK(session.status().no_paper);

  std::vector<uint8_t> pause_payload;
  pause_payload.push_back(0x10);
  const std::vector<uint8_t> pause_frame = makeFrame(TinyProtocol::kFlowControl, pause_payload);
  mock.pushNotification(pause_frame.data(), pause_frame.size());
  CHECK(session.paused());
  std::vector<uint8_t> resume_payload;
  resume_payload.push_back(0x00);
  const std::vector<uint8_t> resume_frame = makeFrame(TinyProtocol::kFlowControl, resume_payload);
  mock.pushNotification(resume_frame.data(), resume_frame.size());
  CHECK(!session.paused());

  CHECK(session.keepAlive(0) == Result::Ok);
  CHECK(session.keepAlive(40000) == Result::Ok);
}

static void testOfflineSimulatedMock() {
  section("MockPrinter offline / recover");
  MockTransport mock;
  mock.setOffline(true);
  ScanRequest req;
  ScanResult res;
  CHECK(mock.scan(req, res) == Result::NotFound);
  CHECK(mock.connect(gb01Endpoint(TransportKind::Mock)) == Result::NotConnected);
  size_t written = 0;
  uint8_t b = 0;
  CHECK(mock.write(&b, 1, written) == Result::NotConnected);

  mock.setOffline(false);
  mock.setScanEndpoints(std::vector<Endpoint>(1, gb01Endpoint(TransportKind::Mock)));
  CHECK(mock.scan(req, res) == Result::Ok);
  CHECK(res.endpoints.size() == 1);
  CHECK(mock.connect(res.endpoints[0]) == Result::Ok);
  CHECK(mock.write(&b, 1, written) == Result::Ok);
  mock.setDropWrites(true);
  CHECK(mock.write(&b, 1, written) == Result::IoError);
}

static void testBitmapAndHelpers() {
  section("Bitmap pipeline / Base64 / JSON helpers / stubs / OTA / report");
  const uint8_t gray[16] = {0,0,0,0,0,0,0,0, 255,255,255,255,255,255,255,255};
  std::vector<uint8_t> rows;
  size_t row_bytes = 0;
  CHECK(packGrayscale(gray, 8, 2, 128, rows, row_bytes) == Result::Ok);
  CHECK(row_bytes == 1);
  CHECK(rows[0] == 0xFF && rows[1] == 0x00);
  std::vector<uint8_t> pattern;
  makeTestPattern(48, 3, pattern);
  CHECK(pattern.size() == 48 * 3);
  CHECK(pattern[0] == 0xFF && pattern[48] != 0xFF);

  const std::string encoded = base64Encode(reinterpret_cast<const uint8_t*>("hello"), 5);
  CHECK(encoded == "aGVsbG8=");
  std::vector<uint8_t> decoded;
  CHECK(base64Decode(encoded, decoded));
  CHECK(std::string(decoded.begin(), decoded.end()) == "hello");
  CHECK(!base64Decode("!!!", decoded));

  const std::string body =
      "{\"job_id\":\"abc\",\"test\":true,\"feed\":64,\"rows_b64\":\"AA==\"}";
  CHECK(jsonStringField(body, "job_id") == "abc");
  CHECK(jsonBoolField(body, "test", false));
  CHECK(jsonIntField(body, "feed", 0) == 64);
  CHECK(jsonStringField(body, "missing").empty());

  EscPosProtocol escpos;
  PeriPageProtocol peripage;
  PaperangProtocol paperang;
  MockTransport mock;
  CHECK(!escpos.capabilities().implemented);
  CHECK(!peripage.capabilities().implemented);
  CHECK(!paperang.capabilities().implemented);
  CHECK(escpos.init(ProtocolContext(), mock) == Result::NotImplemented);
  CHECK(escpos.family() == ProtocolFamily::EscPos);

  OtaService ota;
  CHECK(ota.interfaceReserved());
  CHECK(!ota.available());
  CHECK(ota.begin(1024) == Result::NotImplemented);
  CHECK(ota.write(nullptr, 0) == Result::NotImplemented);
  CHECK(ota.end() == Result::NotImplemented);

  CompatibilityInput in;
  in.board = &boardCapabilities();
  in.ble = mock.getCapabilities();
  BluetoothClassicTransport classic;
  in.bluetooth_classic = classic.getCapabilities();
  BtClassicMockTransport classic_mock;
  in.classic_mock = classic_mock.getCapabilities();
  in.mock = mock.getCapabilities();
  TinyProtocol tiny;
  in.tiny = tiny.capabilities();
  in.escpos = escpos.capabilities();
  in.profile_count = DeviceCatalog::count();
  const std::string report = buildCompatibilityReport(in);
  CHECK(report.find("\"real_printer_tested\":false") != std::string::npos);
  CHECK(report.find("\"real_print_succeeded\":false") != std::string::npos);
  CHECK(report.find("\"classic_bluetooth_verified\":false") != std::string::npos);
  CHECK(report.find("NOT_SUPPORTED_ON_THIS_TARGET") != std::string::npos ||
        report.find("NOT_IMPLEMENTED") != std::string::npos);
}

static void testHostCompileConditions() {
  section("Host build compile conditions");
  BleTransport ble;
  ScanRequest req;
  ScanResult res;
  CHECK(ble.scan(req, res) == Result::NotSupportedOnThisTarget);
  CHECK(!ble.getCapabilities().available);  // no BLE headers compiled on host
  BtClassicMockTransport classic_mock;
#if defined(RTB_HOST_EMULATE_WROOM32E)
  CHECK(classic_mock.realHardwareSupported());
#else
  CHECK(!classic_mock.realHardwareSupported());
#endif
  CHECK(classic_mock.kind() == TransportKind::MockClassic);
  CHECK(std::string(classic_mock.name()) == "mock_bluetooth_classic");
}

static void testNotificationCallbackLifetime() {
  section("Notification callback lifetime / stack-use-after-scope regression");
  const std::vector<uint8_t> status_frame =
      makeFrame(TinyProtocol::kGetStatus, std::vector<uint8_t>(1, 0x80));

  // (1) session destruction must remove the transport's callback entirely.
  {
    CountingTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    SpyProtocol spy;
    ProtocolContext ctx;
    SessionConfig cfg;
    {
      PrintSession session(transport, spy, ctx, cfg);
      CHECK(session.begin(0) == Result::Ok);
      CHECK(transport.notifySubscribed());
      transport.pushNotification(status_frame.data(), status_frame.size());
      CHECK(transport.callback_invocations == 1);
      CHECK(spy.parse_calls == 1);
      CHECK(session.status().valid);
      session.end();
      CHECK(!transport.notifySubscribed());
      transport.pushNotification(status_frame.data(), status_frame.size());
      CHECK(transport.callback_invocations == 1);  // old session not called
    }
    // Session is destroyed here: push again, must be a safe no-op.
    transport.pushNotification(status_frame.data(), status_frame.size());
    CHECK(transport.callback_invocations == 1);
    CHECK(spy.parse_calls == 1);
  }

  // (2) hostile transport that retains a copy even after unsubscribe.
  {
    StaleCallbackTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    SpyProtocol spy;
    ProtocolContext ctx;
    SessionConfig cfg;
    {
      PrintSession session(transport, spy, ctx, cfg);
      CHECK(session.begin(0) == Result::Ok);
      transport.fireRetained(status_frame.data(), status_frame.size());
      CHECK(transport.retained_calls == 1);
      CHECK(spy.parse_calls == 1);
      CHECK(session.status().valid);
    }
    // Deliberately fire the STALE callback after the session is gone.
    // With the old `[this]` lambda this is a stack-use-after-scope (ASAN).
    transport.fireRetained(status_frame.data(), status_frame.size());
    CHECK(transport.retained_calls == 2);
    CHECK(spy.parse_calls == 1);  // dead session was never entered
  }

  // (3) disconnect() must clear the callback (late notify safety).
  {
    CountingTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    SpyProtocol spy;
    ProtocolContext ctx;
    SessionConfig cfg;
    PrintSession session(transport, spy, ctx, cfg);
    CHECK(session.begin(0) == Result::Ok);
    transport.pushNotification(status_frame.data(), status_frame.size());
    CHECK(transport.callback_invocations == 1);
    transport.disconnect();
    CHECK(!transport.notifySubscribed());
    transport.pushNotification(status_frame.data(), status_frame.size());
    CHECK(transport.callback_invocations == 1);
    CHECK(spy.parse_calls == 1);
  }
}

static void testWideRowMirror() {
  section("TinyProtocol::printRow bounded mirror buffer (>64 bytes)");

  // (1) 96-byte row: previously overflowed the fixed uint8_t mirrored[64].
  {
    MockTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    TinyProtocol protocol;
    ProtocolContext ctx;
    ctx.width_bytes = 96;
    ctx.settings.mirror_rows = true;
    std::vector<uint8_t> row(96);
    for (size_t i = 0; i < row.size(); ++i) row[i] = static_cast<uint8_t>(i * 7 + 1);
    CHECK(protocol.printRow(ctx, row.data(), row.size(), transport) == Result::Ok);
    uint8_t opcode = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    const std::vector<uint8_t>& bytes = transport.writtenBytes();
    CHECK(bytes.size() == TinyProtocol::kFrameOverhead + 96);
    CHECK(TinyProtocol::validateFrame(bytes.data(), bytes.size(), opcode, payload,
                                      payload_len) == Result::Ok);
    CHECK(opcode == TinyProtocol::kPrintRow);
    CHECK(payload_len == 96);
    bool mirrored_ok = true;
    for (size_t i = 0; i < 96; ++i) {
      if (payload[i] != TinyProtocol::mirrorByte(row[i])) mirrored_ok = false;
    }
    CHECK(mirrored_ok);
  }

  // (2) Current 384-dot / 48-byte GB01 behaviour must be unchanged.
  {
    MockTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    TinyProtocol protocol;
    ProtocolContext ctx;
    ctx.width_bytes = 48;
    ctx.settings.mirror_rows = true;
    const std::vector<uint8_t> row(48, 0xAA);
    CHECK(protocol.printRow(ctx, row.data(), row.size(), transport) == Result::Ok);
    uint8_t opcode = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    const std::vector<uint8_t>& bytes = transport.writtenBytes();
    CHECK(TinyProtocol::validateFrame(bytes.data(), bytes.size(), opcode, payload,
                                      payload_len) == Result::Ok);
    CHECK(payload_len == 48);
    CHECK(payload[0] == 0x55 && payload[47] == 0x55);
  }

  // (3) Maximum protocol width works; anything wider is rejected, not copied.
  {
    MockTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    TinyProtocol protocol;
    ProtocolContext ctx;
    ctx.width_bytes = static_cast<uint16_t>(TinyProtocol::kMaxRowBytes);
    ctx.settings.mirror_rows = true;
    const std::vector<uint8_t> row(TinyProtocol::kMaxRowBytes, 0x0F);
    CHECK(protocol.printRow(ctx, row.data(), row.size(), transport) == Result::Ok);
    CHECK(transport.writtenBytes().size() ==
          TinyProtocol::kFrameOverhead + TinyProtocol::kMaxRowBytes);

    ctx.width_bytes = static_cast<uint16_t>(TinyProtocol::kMaxRowBytes + 1);
    const std::vector<uint8_t> too_wide(TinyProtocol::kMaxRowBytes + 1, 0x0F);
    const size_t before = transport.writtenBytes().size();
    CHECK(protocol.printRow(ctx, too_wide.data(), too_wide.size(), transport) ==
          Result::InvalidArgument);
    CHECK(transport.writtenBytes().size() == before);  // nothing emitted
  }

  // (4) Non-mirrored wide row passes through unchanged.
  {
    MockTransport transport;
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    TinyProtocol protocol;
    ProtocolContext ctx;
    ctx.width_bytes = 128;
    ctx.settings.mirror_rows = false;
    const std::vector<uint8_t> row(128, 0x3C);
    CHECK(protocol.printRow(ctx, row.data(), row.size(), transport) == Result::Ok);
    uint8_t opcode = 0;
    const uint8_t* payload = nullptr;
    size_t payload_len = 0;
    const std::vector<uint8_t>& bytes = transport.writtenBytes();
    CHECK(TinyProtocol::validateFrame(bytes.data(), bytes.size(), opcode, payload,
                                      payload_len) == Result::Ok);
    CHECK(payload_len == 128 && payload[127] == 0x3C);
  }
}

static void testDiscoveryFallback() {
  section("Two-phase BLE discovery fallback (no AE30 advertisement)");

  // (1) AE30-advertised device: phase 1 only.
  {
    RecordingScanTransport transport;
    transport.setScanEndpoints(std::vector<Endpoint>(1, gb01Endpoint(TransportKind::Ble)));
    const DiscoveryResult d = discoverPrinter(transport, 1000, 1000);
    CHECK(d.result == Result::Ok);
    CHECK(d.stage == DiscoveryStage::Ae30Filtered);
    CHECK(d.scan_rounds == 1 && transport.scan_calls == 1);
    CHECK(d.match.profile != nullptr);
    CHECK(std::string(d.match.profile->model_id) == "gb01");
  }

  // (2) GB01 with NO AE30 in the advertisement: still recognised after fallback.
  {
    RecordingScanTransport transport;
    Endpoint ep = gb01Endpoint(TransportKind::Ble);
    ep.service_uuid.clear();
    ep.write_uuid.clear();
    ep.notify_uuid.clear();
    ep.advertised_services.clear();
    ep.name = "GB01";
    transport.setScanEndpoints(std::vector<Endpoint>(1, ep));

    const DiscoveryResult d = discoverPrinter(transport, 1000, 1000);
    CHECK(d.result == Result::Ok);
    CHECK(d.stage == DiscoveryStage::UnfilteredFallback);
    CHECK(d.scan_rounds == 2 && transport.scan_calls == 2);
    CHECK(transport.filters.size() == 2);
    CHECK(transport.filters[0] == "AE30");
    CHECK(transport.filters[1].empty());
    CHECK(d.match.profile != nullptr);
    CHECK(std::string(d.match.profile->model_id) == "gb01");
    CHECK(d.match.fingerprint.family == ProtocolFamily::TinyIPrint);
    CHECK(std::string(d.match.fingerprint.evidence) == "bluetooth_name_pattern");
    CHECK(d.endpoint.service_uuid.empty());  // discovery invented no evidence

    // Final connect still gets profile-owned UUIDs -> strict GATT verification.
    const Endpoint prepared = prepareConnectEndpoint(d.endpoint, *d.match.profile);
    CHECK(std::string(prepared.service_uuid) == "AE30");
    CHECK(std::string(prepared.write_uuid) == "AE01");
    CHECK(std::string(prepared.notify_uuid) == "AE02");
    CHECK(prepared.address == ep.address);
  }

  // (3) AE30 present only in advertised_services: evidence path also works.
  {
    RecordingScanTransport transport;
    Endpoint ep = gb01Endpoint(TransportKind::Ble);
    ep.service_uuid.clear();
    ep.write_uuid.clear();
    ep.notify_uuid.clear();
    ep.advertised_services.clear();
    ep.advertised_services.push_back("0000AE30-0000-1000-8000-00805F9B34FB");
    transport.setScanEndpoints(std::vector<Endpoint>(1, ep));
    const DiscoveryResult d = discoverPrinter(transport, 1000, 1000);
    CHECK(d.result == Result::Ok);
    CHECK(d.stage == DiscoveryStage::UnfilteredFallback);
    CHECK(d.match.fingerprint.family == ProtocolFamily::TinyIPrint);
    CHECK(std::string(d.match.fingerprint.evidence) == "advertised_service_ae30");
  }

  // (4) No candidate at all: both rounds attempted, no profile.
  {
    RecordingScanTransport transport;
    const DiscoveryResult d = discoverPrinter(transport, 1000, 1000);
    CHECK(d.result == Result::NotFound);
    CHECK(d.stage == DiscoveryStage::NotFound);
    CHECK(d.scan_rounds == 2 && transport.scan_calls == 2);
    CHECK(d.match.profile == nullptr);
    CHECK(std::string(discoveryStageName(d.stage)) == "not_found");
  }
}

static void testPartialPrintSemantics() {
  section("Mid-job retry semantics / uncertain_partial");

  CHECK(classifyJobOutcome(Result::Ok, 0) == JobOutcome::Complete);
  CHECK(classifyJobOutcome(Result::Ok, 5) == JobOutcome::Complete);
  CHECK(classifyJobOutcome(Result::IoError, 0) == JobOutcome::SafeRetry);
  CHECK(classifyJobOutcome(Result::NotConnected, 0) == JobOutcome::SafeRetry);
  CHECK(classifyJobOutcome(Result::IoError, 1) == JobOutcome::UncertainPartial);
  CHECK(classifyJobOutcome(Result::Busy, 7) == JobOutcome::UncertainPartial);

  // (1) Real mid-job failure reports progress and is classified as uncertain.
  {
    FailAfterWritesTransport transport(8);
    CHECK(transport.connect(gb01Endpoint(TransportKind::Mock)) == Result::Ok);
    TinyProtocol protocol;
    const DeviceProfile* profile = DeviceCatalog::findByModelId("gb01");
    ProtocolContext ctx = makeContext(*profile);
    SessionConfig cfg;
    cfg.command_delay_ms = 0;
    cfg.row_delay_ms = 0;
    PrintSession session(transport, protocol, ctx, cfg);
    CHECK(session.begin(0) == Result::Ok);
    PrintJob job;
    job.rows.assign(48 * 4, 0xAA);
    job.row_bytes = 48;
    job.row_count = 4;
    const Result r = session.execute(job);
    CHECK(r == Result::IoError);
    CHECK(session.rowsAttempted() == 2);
    CHECK(session.rowsDispatched() == 1);
    CHECK(classifyJobOutcome(r, session.rowsAttempted()) ==
          JobOutcome::UncertainPartial);
  }

  // (2) Pre-print failure is safe to retry automatically.
  {
    PrintQueue queue;
    PrintJob job;
    job.job_id = "SAFE-RETRY";
    job.rows.assign(48 * 2, 0x11);
    job.row_bytes = 48;
    job.row_count = 2;
    const char* reason = "";
    CHECK(queue.enqueue(job, 0, reason) == Result::Ok);
    queue.markWaitingPrinter(job.dedup_key, "printer_offline", 100, 2000);
    CHECK(queue.nextPending(1500) == nullptr);
    PrintJob* retried = queue.nextPending(2100);
    CHECK(retried != nullptr);
    CHECK(retried->state == JobState::WaitingPrinter);
    CHECK(retried->rows_attempted == 0);
    CHECK(retried->last_error == "printer_offline");
  }

  // (3) uncertain_partial is terminal: no auto retry, progress + error recorded.
  {
    PrintQueue queue;
    PrintJob job;
    job.job_id = "PARTIAL";
    job.rows.assign(48 * 3, 0x22);
    job.row_bytes = 48;
    job.row_count = 3;
    const char* reason = "";
    CHECK(queue.enqueue(job, 0, reason) == Result::Ok);
    queue.markPrinting(job.dedup_key);
    queue.markUncertainPartial(job.dedup_key, "uncertain_partial:IO_ERROR", 2, 1);
    const QueueStats s = queue.stats();
    CHECK(s.uncertain == 1);
    CHECK(s.pending() == 0);
    CHECK(!queue.hasPending());
    CHECK(queue.nextPending(1000000) == nullptr);
    PrintJob* frozen = queue.find("PARTIAL");
    CHECK(frozen != nullptr);
    CHECK(frozen->state == JobState::UncertainPartial);
    CHECK(std::string(jobStateName(frozen->state)) == "uncertain_partial");
    CHECK(frozen->rows_attempted == 2);
    CHECK(frozen->rows_dispatched == 1);
    CHECK(frozen->last_error == "uncertain_partial:IO_ERROR");
    std::string json;
    queue.toJson(json);
    CHECK(json.find("\"state\":\"uncertain_partial\"") != std::string::npos);
    CHECK(json.find("\"rows_attempted\":2") != std::string::npos);
    CHECK(json.find("\"rows_dispatched\":1") != std::string::npos);
    CHECK(json.find("\"uncertain\":1") != std::string::npos);
  }
}

int main() {
#if defined(RTB_HOST_EMULATE_WROOM32E)
  printf("=== RTB V0.1 Mock tests (WROOM-32E capability emulation) ===\n");
#else
  printf("=== RTB V0.1 Mock tests (ESP32-S3 capability emulation) ===\n");
#endif
  testBoardCapabilities();
  testClassicGate();
  testCrcAndFraming();
  testCatalogAndFingerprint();
  testQueueDedupAndRecovery();
  testSessionEndToEnd();
  testOfflineSimulatedMock();
  testBitmapAndHelpers();
  testHostCompileConditions();
  testDedupSemanticsById();
  testNotificationCallbackLifetime();
  testWideRowMirror();
  testDiscoveryFallback();
  testPartialPrintSemantics();

  // Protocol runs unchanged over a BLE-emulating Mock and an SPP-emulating Mock.
  section("Transport / Protocol decoupling: TinyProtocol over BLE Mock and Classic Mock");
  MockTransport ble_mock;
  runTinyOverTransport(ble_mock, "ble-mock");
  BtClassicMockTransport spp_mock;
  runTinyOverTransport(spp_mock, "classic-spp-mock");

  printf("=== %d checks, %d failures ===\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
