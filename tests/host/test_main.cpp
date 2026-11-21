// Host-native Mock tests. Compiled with `zig c++` (no ESP32 required).
// These tests exercise the REAL protocol / catalog / queue / transport code,
// not a Python re-implementation of it.

#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/Base64.h"
#include "app/Discovery.h"
#include "app/CompatibilityReport.h"
#include "app/JsonMini.h"
#include "app/JobId.h"
#include "app/OtaService.h"
#include "app/PrintQueue.h"
#include "app/PrintRequestParser.h"
#include "app/PrintSubmission.h"
#include "app/RawPrintAssembler.h"
#include "app/SafeTestRegistry.h"
#include "app/SerialControl.h"
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
  CHECK(DeviceCatalog::count() == 6);
  CHECK(DeviceCatalog::findByModelId("gb01") != nullptr);
  CHECK(DeviceCatalog::findByModelId("does_not_exist") == nullptr);

  // X18 / 爱记星: advertised AF30 is NOT the GATT print service (AE30).
  const DeviceProfile* x18 = DeviceCatalog::findByModelId("x18_aijixing");
  CHECK(x18 != nullptr);
  CHECK(std::string(x18->service_uuid) == "AE30");
  CHECK(std::string(x18->advertised_service_uuid) == "AF30");
  CHECK(std::string(x18->write_uuid) == "AE01");
  CHECK(std::string(x18->notify_uuid) == "AE02");
  CHECK(x18->protocol_family == ProtocolFamily::TinyIPrint);
  CHECK(profileSupports(*x18, TransportKind::Ble));
  // 384 dots + implemented=true confirmed by the real-hardware power-cycle
  // recovery test; X18 must be selectable by automatic discovery.
  CHECK(x18->implemented);
  CHECK(x18->width_dots == 384);
  Endpoint x18_ep;
  x18_ep.kind = TransportKind::Ble;
  x18_ep.name = "X18-47E3";
  x18_ep.service_uuid = "0000AF30-0000-1000-8000-00805F9B34FB";
  const CatalogMatch x18_match = DeviceCatalog::match(x18_ep);
  CHECK(x18_match.profile != nullptr);
  CHECK(std::string(x18_match.profile->model_id) == "x18_aijixing");
  std::string x18_json;
  DeviceCatalog::toJson(x18_json);
  CHECK(x18_json.find("\"advertised_service_uuid\":\"AF30\"") != std::string::npos);

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

static bool sameBytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

static std::string printBody(const std::string& rows_b64, const std::string& row_bytes = "48",
                             const std::string& feed = "64") {
  std::string body = "{\"job_id\":\"x18-test\",\"row_bytes\":";
  body += row_bytes;
  body += ",\"feed\":";
  body += feed;
  body += ",\"rows_b64\":\"";
  body += rows_b64;
  body += "\"}";
  return body;
}

static void testPrintRequestParser() {
  section("Print request parser: in-place base64 + bounded allocation");

  // ---- 1. the real X18 payload shape: 64 rows x 48 B = 3072 B -> 4096 chars
  std::vector<uint8_t> bitmap(48 * 64, 0);
  for (size_t i = 0; i < bitmap.size(); ++i) {
    bitmap[i] = static_cast<uint8_t>((i * 7) & 0xFF);
  }
  const std::string encoded = base64Encode(bitmap.data(), bitmap.size());
  CHECK(encoded.size() == 4096);
  CHECK(base64MaxDecodedSize(encoded.size()) >= bitmap.size());

  const std::string body = printBody(encoded);

  // The field must be REFERENCED, not copied: the pointer lives inside `body`.
  const JsonFieldSlice slice = jsonStringSlice(body, "rows_b64");
  CHECK(slice.found);
  CHECK(!slice.escaped);
  CHECK(slice.size == encoded.size());
  CHECK(slice.data >= body.data() && slice.data + slice.size <= body.data() + body.size());
  CHECK(static_cast<size_t>(slice.data - body.data()) == body.find(encoded));

  PrintLimits limits;
  ParsedPrintRequest parsed;
  CHECK(parsePrintRequest(body, limits, 48, 64, parsed) == PrintParseError::None);
  CHECK(parsed.rows.size() == bitmap.size());
  CHECK(sameBytes(parsed.rows, bitmap));
  CHECK(parsed.row_bytes == 48);
  CHECK(parsed.row_count == 64);
  CHECK(parsed.feed_dots == 64);
  CHECK(parsed.job_id == "x18-test");
  CHECK(!parsed.test_pattern);

  // ---- 2. same request parsed twice: same result, still no copy of the field
  ParsedPrintRequest again;
  CHECK(parsePrintRequest(body, limits, 48, 64, again) == PrintParseError::None);
  CHECK(sameBytes(again.rows, parsed.rows));
  CHECK(jsonStringSlice(body, "rows_b64").data == slice.data);

  // ---- 3. empty field
  ParsedPrintRequest empty;
  CHECK(parsePrintRequest(printBody(""), limits, 48, 64, empty) ==
        PrintParseError::MissingRows);
  CHECK(empty.rows.empty());

  // ---- 4. field absent entirely
  ParsedPrintRequest absent;
  CHECK(parsePrintRequest("{\"job_id\":\"a\"}", limits, 48, 64, absent) ==
        PrintParseError::MissingRows);
  CHECK(printParseErrorStatus(PrintParseError::MissingRows) == 400);

  // ---- 5. invalid base64 alphabet
  ParsedPrintRequest bad;
  CHECK(parsePrintRequest(printBody("!!!!"), limits, 48, 64, bad) ==
        PrintParseError::InvalidBase64);
  CHECK(bad.rows.empty());

  // ---- 6. base64 with a backslash escape cannot be referenced in place
  ParsedPrintRequest escaped;
  CHECK(parsePrintRequest("{\"rows_b64\":\"AA\\/==\",\"row_bytes\":48}", limits, 48, 64,
                          escaped) == PrintParseError::InvalidBase64);

  // ---- 7. truncated JSON (no closing quote)
  ParsedPrintRequest truncated;
  CHECK(parsePrintRequest("{\"row_bytes\":48,\"rows_b64\":\"AAAA", limits, 48, 64,
                          truncated) == PrintParseError::InvalidJson);

  // ---- 8. test pattern still works through the same entry point
  ParsedPrintRequest pattern;
  CHECK(parsePrintRequest("{\"test\":true,\"row_bytes\":48}", limits, 48, 64, pattern) ==
        PrintParseError::None);
  CHECK(pattern.test_pattern);
  CHECK(pattern.rows.size() == 48 * 24);
  CHECK(pattern.row_count == 24);

  // ---- 9. exactly the maximum bitmap size is accepted (64 B rows -> 1024 rows)
  // The default body limit is the SMALL / LEGACY 8 KB, so a 64 KB bitmap cannot
  // travel through /api/print at all; lift only the body ceiling here to test
  // the logical bitmap limit on its own.
  PrintLimits bitmap_limits;
  bitmap_limits.max_body_bytes = 256 * 1024;
  std::vector<uint8_t> max_bitmap(RTB_MAX_JOB_BITMAP_BYTES, 0x5A);
  const std::string max_encoded = base64Encode(max_bitmap.data(), max_bitmap.size());
  ParsedPrintRequest at_limit;
  CHECK(parsePrintRequest(printBody(max_encoded, "64"), bitmap_limits, 64, 64, at_limit) ==
        PrintParseError::None);
  CHECK(at_limit.rows.size() == RTB_MAX_JOB_BITMAP_BYTES);
  CHECK(at_limit.row_count == RTB_MAX_JOB_BITMAP_BYTES / 64);

  // ---- 10. one byte over the limit is rejected before enqueueing
  std::vector<uint8_t> over_bitmap(RTB_MAX_JOB_BITMAP_BYTES + 1, 0x5A);
  const std::string over_encoded = base64Encode(over_bitmap.data(), over_bitmap.size());
  ParsedPrintRequest over;
  CHECK(parsePrintRequest(printBody(over_encoded, "64"), bitmap_limits, 64, 64, over) ==
        PrintParseError::PayloadTooLarge);
  CHECK(printParseErrorStatus(PrintParseError::PayloadTooLarge) == 413);
  CHECK(over.rows.empty());

  // ---- 10b. the same 64 KB request against the DEFAULT (small) limits is
  // rejected on the body ceiling first: /api/print is not a bulk transport.
  ParsedPrintRequest bulk;
  CHECK(parsePrintRequest(printBody(max_encoded, "64"), limits, 64, 64, bulk) ==
        PrintParseError::PayloadTooLarge);
  CHECK(printParseErrorStatus(PrintParseError::PayloadTooLarge) == 413);

  // ---- 11. oversized HTTP body is rejected without parsing anything
  std::string huge(RTB_HTTP_MAX_BODY_BYTES + 1, 'x');
  huge += "{\"rows_b64\":\"\"}";
  ParsedPrintRequest huge_out;
  CHECK(parsePrintRequest(huge, limits, 48, 64, huge_out) ==
        PrintParseError::PayloadTooLarge);

  // ---- 12. decoder primitives: capacity guard and truncated final group
  uint8_t small[2];
  size_t written = 0;
  CHECK(!base64DecodeInto("aGVsbG8=", 8, small, 2, written));
  uint8_t exact[5];
  CHECK(base64DecodeInto("aGVsbG8=", 8, exact, 5, written));
  CHECK(written == 5);
  CHECK(!base64DecodeInto("aGVsb", 5, exact, 5, written));

  // ---- 13. the queue limit the parser mirrors
  CHECK(RTB_MAX_JOB_BITMAP_BYTES == 64u * 1024u);
  PrintQueue q;
  PrintJob job;
  job.row_bytes = 48;
  job.row_count = 1;
  job.rows.assign(48, 0);
  const char* reason = "";
  CHECK(q.enqueue(job, 0, reason) == Result::Ok);

  // ---- 14. a finished job releases its bitmap: the heap must not drift 3 KB
  // per print, and the reported counters must survive the release.
  PrintJob done_job;
  done_job.job_id = "release-check";
  done_job.row_bytes = 48;
  done_job.row_count = 64;
  done_job.rows.assign(48 * 64, 0x33);
  CHECK(q.enqueue(done_job, 1, reason) == Result::Ok);
  PrintJob* queued = q.find(done_job.job_id);
  CHECK(queued != nullptr);
  CHECK(queued->rows.size() == 48 * 64);
  q.markDone(done_job.dedup_key);
  PrintJob* finished = q.find(done_job.job_id);
  CHECK(finished != nullptr);
  CHECK(finished->state == JobState::Done);
  CHECK(finished->rows.empty());
  CHECK(finished->rows.capacity() == 0);  // the block really went back
  CHECK(finished->row_count == 64);       // still reported by /api/queue
}

// Regression for a real UBSan finding: jsonIntFieldIn() used to accumulate
// "value = value * 10 + digit" and check the range afterwards, so any input
// above INT32_MAX was a signed overflow (UB) before the check ran.
static void testJsonIntOverflow() {
  section("JSON integer parsing: overflow rejected before it happens");

  const int fallback = -12345;
  const char* long_digits = "{\"n\":99999999999999999999999999999999999999999999}";

  // The exact boundary and the three values from the report.
  CHECK(jsonIntField("{\"n\":2147483647}", "n", fallback) == 2147483647);
  CHECK(jsonIntField("{\"n\":2147483648}", "n", fallback) == fallback);
  CHECK(jsonIntField("{\"n\":3000000000}", "n", fallback) == fallback);
  CHECK(jsonIntField(long_digits, "n", fallback) == fallback);
  CHECK(jsonIntField("{\"n\":-3000000000}", "n", fallback) == fallback);
  CHECK(jsonIntField("{\"n\":-2147483647}", "n", fallback) == -2147483647);

  // Positive control around the boundary: one below must still parse exactly.
  CHECK(jsonIntField("{\"n\":2147483646}", "n", fallback) == 2147483646);

  // Same inputs through the in-place entry point (no std::string overload).
  const std::string in_place = "{\"feed\":3000000000}";
  CHECK(jsonIntFieldIn(in_place.data(), in_place.size(), "feed", fallback) == fallback);
  const std::string in_place_max = "{\"feed\":2147483647}";
  CHECK(jsonIntFieldIn(in_place_max.data(), in_place_max.size(), "feed", fallback) ==
        2147483647);

  // Real entry points must fall back to the profile defaults, not to a wrapped
  // negative row_bytes / feed.
  PrintLimits limits;
  ParsedPrintRequest parsed;
  CHECK(parsePrintRequest("{\"test\":true,\"row_bytes\":3000000000}", limits, 48, 64,
                          parsed) == PrintParseError::None);
  CHECK(parsed.row_bytes == 48);   // profile default, not a wrapped value
  CHECK(parsed.feed_dots == 64);

  CHECK(parsePrintRequest("{\"test\":true,\"row_bytes\":48,\"feed\":3000000000}", limits,
                          48, 64, parsed) == PrintParseError::None);
  CHECK(parsed.row_bytes == 48);
  CHECK(parsed.feed_dots == 64);   // default, not a wrapped value

  // INT32_MAX itself must still be parsed (and then rejected by the size limit),
  // which proves the value is not silently replaced by the fallback.
  CHECK(parsePrintRequest("{\"test\":true,\"row_bytes\":2147483647}", limits, 48, 64,
                          parsed) == PrintParseError::PayloadTooLarge);
  CHECK(parsed.rows.empty());
}

// Shared job_id contract (JobId.h): one rule set for /api/print and
// /api/print/raw.
static void testJobIdContract() {
  section("job_id contract shared by JSON and raw endpoints");

  CHECK(kMaxJobIdLength == 64);
  CHECK(isValidJobId(""));                      // empty = content-hash namespace
  CHECK(isValidJobId("X18-1_a.b:c"));
  CHECK(isValidJobId(std::string(64, 'a')));    // at the limit
  CHECK(!isValidJobId(std::string(65, 'a')));   // one over
  CHECK(!isValidJobId("bad id"));               // space
  CHECK(!isValidJobId("bad/id"));               // slash
  CHECK(!isValidJobId("bad\"id"));              // quote (would break JSON)
  CHECK(!isValidJobId("bad\nid"));

  // JSON endpoint: rejected by the parser, 400 invalid_job_id.
  PrintLimits limits;
  ParsedPrintRequest parsed;
  CHECK(parsePrintRequest("{\"job_id\":\"bad id\",\"test\":true}", limits, 48, 64,
                          parsed) == PrintParseError::InvalidJobId);
  CHECK(std::string(printParseErrorName(PrintParseError::InvalidJobId)) ==
        "invalid_job_id");
  CHECK(printParseErrorStatus(PrintParseError::InvalidJobId) == 400);
  CHECK(parsePrintRequest("{\"test\":true}", limits, 48, 64, parsed) ==
        PrintParseError::None);  // empty job_id still allowed

  // Raw endpoint: same rule, same error name, same status.
  RawPrintMeta meta;
  meta.row_bytes = 48;
  meta.job_id = "bad id";
  RawPrintAssembler assembler;
  CHECK(assembler.begin(48, meta, limits, 0) == RawBeginResult::InvalidArgument);
  CHECK(std::string(assembler.errorName()) == "invalid_job_id");
  CHECK(rawBeginResultStatus(RawBeginResult::InvalidArgument) == 400);

  meta.job_id.clear();
  CHECK(assembler.begin(48, meta, limits, 0) == RawBeginResult::Ok);  // empty ok
  assembler.abort();
}

// POST /api/print is the SMALL / LEGACY JSON endpoint: 8 KB body ceiling,
// logical bitmap ceiling, plus a runtime heap budget.
static void testJsonPrintEndpointPolicy() {
  section("/api/print JSON endpoint: 8 KB legacy ceiling + heap budget + 409");

  // The policy itself: bulk bitmaps must use /api/print/raw.
  CHECK(RTB_HTTP_MAX_BODY_BYTES == 8u * 1024u);
  CHECK(RTB_MAX_JOB_BITMAP_BYTES == 64u * 1024u);  // logical limit unchanged

  PrintLimits limits;  // defaults: 8 KB body, 64 KB bitmap, no heap check
  const std::vector<uint8_t> bitmap(48 * 8, 0x11);  // 384 B, typical small job
  const std::string encoded = base64Encode(bitmap.data(), bitmap.size());

  std::string body = "{\"job_id\":\"json-small\",\"row_bytes\":48,\"rows_b64\":\"";
  body += encoded;
  body += "\"}";
  CHECK(body.size() < RTB_HTTP_MAX_BODY_BYTES);

  PrintQueue queue;

  // ---- 1. small valid request
  PrintSubmitOutcome ok =
      submitJsonPrint(body.data(), body.size(), limits, 48, 64, queue, 0);
  CHECK(ok.http_status == 200);
  CHECK(ok.body.find("\"ok\":true") != std::string::npos);
  CHECK(queue.size() == 1);

  // ---- 2. duplicate job_id -> 409, not 400, and nothing is added
  PrintSubmitOutcome dup =
      submitJsonPrint(body.data(), body.size(), limits, 48, 64, queue, 1);
  CHECK(dup.http_status == 409);
  CHECK(dup.body.find("\"ok\":false") != std::string::npos);
  CHECK(dup.body.find("duplicate_job") != std::string::npos);
  CHECK(queue.size() == 1);

  // ---- 3. body above the configured ceiling -> 413
  std::string too_long = body;
  too_long.append(RTB_HTTP_MAX_BODY_BYTES, ' ');
  PrintSubmitOutcome over_body =
      submitJsonPrint(too_long.data(), too_long.size(), limits, 48, 64, queue, 2);
  CHECK(over_body.http_status == 413);
  CHECK(over_body.body.find("payload_too_large") != std::string::npos);
  CHECK(queue.size() == 1);

  // ---- 4. bitmap above the LOGICAL limit -> 413 (body ceiling lifted here so
  // the bitmap limit is the one that fires)
  PrintLimits big_body;
  big_body.max_body_bytes = 256 * 1024;
  std::vector<uint8_t> huge_bitmap(RTB_MAX_JOB_BITMAP_BYTES + 48, 0x22);
  const std::string huge_encoded = base64Encode(huge_bitmap.data(), huge_bitmap.size());
  std::string huge_body = "{\"job_id\":\"json-huge\",\"row_bytes\":48,\"rows_b64\":\"";
  huge_body += huge_encoded;
  huge_body += "\"}";
  PrintSubmitOutcome logical =
      submitJsonPrint(huge_body.data(), huge_body.size(), big_body, 48, 64, queue, 3);
  CHECK(logical.http_status == 413);
  CHECK(logical.body.find("payload_too_large") != std::string::npos);
  CHECK(queue.size() == 1);

  // ---- 5. logically legal, but the heap budget is one byte short -> 503
  PrintLimits tight = limits;
  tight.heap_budget_bytes = bitmap.size() - 1;
  PrintSubmitOutcome oom =
      submitJsonPrint(body.data(), body.size(), tight, 48, 64, queue, 4);
  CHECK(oom.http_status == 503);
  CHECK(oom.body.find("insufficient_memory") != std::string::npos);
  CHECK(queue.size() == 1);  // nothing enqueued

  // ---- 6. exactly enough budget is accepted (different job_id: the queue must
  // not dedup on content, this is the id namespace)
  PrintLimits exact = limits;
  exact.heap_budget_bytes = bitmap.size();
  std::string body_fit = "{\"job_id\":\"json-fit\",\"row_bytes\":48,\"rows_b64\":\"";
  body_fit += encoded;
  body_fit += "\"}";
  PrintSubmitOutcome fits =
      submitJsonPrint(body_fit.data(), body_fit.size(), exact, 48, 64, queue, 5);
  CHECK(fits.http_status == 200);
  CHECK(queue.size() == 2);

  // ---- 7. invalid job_id -> 400 invalid_job_id (same rule as raw)
  std::string bad_id = "{\"job_id\":\"bad id!\",\"row_bytes\":48,\"rows_b64\":\"";
  bad_id += encoded;
  bad_id += "\"}";
  PrintSubmitOutcome bad =
      submitJsonPrint(bad_id.data(), bad_id.size(), limits, 48, 64, queue, 6);
  CHECK(bad.http_status == 400);
  CHECK(bad.body.find("invalid_job_id") != std::string::npos);
  CHECK(queue.size() == 2);

  // ---- 8. dedup namespaces stay separate:
  //         no job_id -> content hash dedup (second attempt 409)
  //         identical content with a NEW job_id -> still accepted
  std::string no_id_body = "{\"row_bytes\":48,\"rows_b64\":\"";
  no_id_body += encoded;
  no_id_body += "\"}";
  PrintSubmitOutcome first_no_id =
      submitJsonPrint(no_id_body.data(), no_id_body.size(), limits, 48, 64, queue, 7);
  CHECK(first_no_id.http_status == 200);
  PrintSubmitOutcome again_no_id =
      submitJsonPrint(no_id_body.data(), no_id_body.size(), limits, 48, 64, queue, 8);
  CHECK(again_no_id.http_status == 409);
  CHECK(again_no_id.body.find("duplicate_job") != std::string::npos);

  std::string reid_body = "{\"job_id\":\"json-reid\",\"row_bytes\":48,\"rows_b64\":\"";
  reid_body += encoded;
  reid_body += "\"}";
  PrintSubmitOutcome reid =
      submitJsonPrint(reid_body.data(), reid_body.size(), limits, 48, 64, queue, 9);
  CHECK(reid.http_status == 200);
  CHECK(queue.size() == 4);  // small, fit, no-id, reid - no cross-namespace dedup

  // ---- 9. the queue mapper is shared with the raw handler
  CHECK(printEnqueueStatus(Result::Ok) == 200);
  CHECK(printEnqueueStatus(Result::Busy) == 409);
  CHECK(printEnqueueStatus(Result::InvalidArgument) == 400);
  CHECK(printEnqueueStatus(Result::IoError) == 400);
}

// The firmware half of the safe-test whitelist: an HTTP request names an id and
// never carries bytes. These checks are what "no arbitrary HEX" means.
static void testSafeTestRegistry() {
  section("safe test registry: ids only, no raw command surface");

  CHECK(safeTestCount() == 1);
  const SafeTestSpec* spec = findSafeTest("SAFE_TEST_TINY_STATUS_V1");
  CHECK(spec != nullptr);
  CHECK(std::string(spec->protocol) == "tiny_iprint");
  CHECK(std::string(spec->transport) == "ble");
  CHECK(spec->moves_paper == false);  // a compatibility test never prints
  CHECK(kSafeTestStatusOpcode == 0xA3);
  CHECK(kSafeTestStatusPayload == 0x00);

  // The verified X18 command is exactly A3 [0x00], and it round-trips through
  // the same parser the firmware uses for the printer's answer.
  uint8_t frame[16];
  size_t frame_len = 0;
  const uint8_t payload[1] = {kSafeTestStatusPayload};
  CHECK(TinyProtocol::buildFrame(kSafeTestStatusOpcode, payload, 1, frame, sizeof(frame),
                                 frame_len) == Result::Ok);
  TinyProtocol tiny;
  PrinterStatus parsed;
  CHECK(tiny.parseStatus(frame, frame_len, parsed) == Result::Ok);
  CHECK(parsed.valid);
  CHECK(std::string(parsed.source_opcode) == "A3");

  // Anything that is not the registry id is refused - including hex strings.
  const char* rejected[] = {
      "", "SAFE_TEST_TINY_STATUS_V1 ", "safe_test_tiny_status_v1",
      "A3", "A300", "0xA3", "A3 00", "\xA3\x00", "SAFE_TEST_RAW_HEX",
      "SAFE_TEST_PRINT_DEMO", "SAFE_TEST_TINY_STATUS_V2"};
  for (const char* value : rejected) {
    CHECK(!isSafeTestAllowed(std::string(value)));
    CHECK(findSafeTest(std::string(value)) == nullptr);
  }
}

// OOM containment contract for the raw request lifecycle: a failed allocation
// must never leave an active assembler or a queued half-job behind.
static void testRawPrintOomContainment() {
  section("Raw print OOM containment: no abort, no residue, next job works");

  PrintLimits limits;
  RawPrintMeta meta;
  meta.row_bytes = 48;
  meta.job_id = "RAW-OOM-1";

  const std::vector<uint8_t> body(48 * 4, 0x5A);

  // ---- 1. begin() refused by the heap budget -> 503, nothing active
  {
    RawPrintAssembler a;
    const RawBeginResult r = a.begin(body.size(), meta, limits, 100);
    CHECK(r == RawBeginResult::InsufficientMemory);
    CHECK(rawBeginResultStatus(r) == 503);
    CHECK(!a.active());
    CHECK(a.expected() == 0);
    CHECK(a.received() == 0);

    // ---- 2. the same assembler recovers for the next request
    CHECK(a.begin(body.size(), meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), body.size()));
    PrintJob job;
    CHECK(a.finish(job));
    CHECK(job.rows.size() == body.size());
  }

  // ---- 3. a body that never completes is dropped, and the next request works
  {
    RawPrintAssembler a;
    CHECK(a.begin(body.size(), meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), 48));
    PrintJob job;
    CHECK(!a.finish(job));
    CHECK(job.rows.empty());      // no half job leaves the assembler
    CHECK(!a.active());
    CHECK(a.expected() == 0);     // the block really went back

    CHECK(a.begin(body.size(), meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), body.size()));
    CHECK(a.finish(job));
  }

  // ---- 4. queue-side rejection keeps ownership with the caller and leaves no
  // residue: the duplicate is discarded, the next distinct job is accepted
  {
    PrintQueue q;
    RawPrintAssembler a;
    const char* reason = "";

    CHECK(a.begin(body.size(), meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), body.size()));
    PrintJob first;
    CHECK(a.finish(first));
    CHECK(q.enqueueMove(std::move(first), 0, reason) == Result::Ok);
    CHECK(q.size() == 1);

    CHECK(a.begin(body.size(), meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), body.size()));
    PrintJob dup;
    CHECK(a.finish(dup));
    CHECK(q.enqueueMove(std::move(dup), 1, reason) == Result::Busy);
    CHECK(std::string(reason) == "duplicate_job");
    CHECK(printEnqueueStatus(Result::Busy) == 409);
    CHECK(q.size() == 1);   // no half task stored

    RawPrintMeta next = meta;
    next.job_id = "RAW-OOM-2";
    CHECK(a.begin(body.size(), next, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), body.size()));
    PrintJob recovered;
    CHECK(a.finish(recovered));
    CHECK(q.enqueueMove(std::move(recovered), 2, reason) == Result::Ok);
    CHECK(q.size() == 2);
  }
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

  // (5) X18: advertises AF30 (lowercase from the BLE stack), GATT AA/services
  //     unknown until connect. Must be found in PHASE 1, not after a fallback.
  {
    RecordingScanTransport transport;
    Endpoint ep;
    ep.kind = TransportKind::Ble;
    ep.address = "e3:47:88:2e:e1:2c";
    ep.name = "X18-47E3";
    ep.rssi = -33;
    ep.service_uuid = "0000af30-0000-1000-8000-00805f9b34fb";
    transport.setScanEndpoints(std::vector<Endpoint>(1, ep));

    const DiscoveryResult d = discoverPrinter(transport, 1000, 1000);
    CHECK(d.result == Result::Ok);
    CHECK(d.stage == DiscoveryStage::Ae30Filtered);   // phase 1, not fallback
    CHECK(d.scan_rounds == 1 && transport.scan_calls == 1);
    CHECK(transport.filters.size() == 1);
    CHECK(transport.filters[0] == "AE30");            // primary unchanged
    CHECK(d.match.profile != nullptr);
    CHECK(std::string(d.match.profile->model_id) == "x18_aijixing");
    CHECK(d.match.fingerprint.family == ProtocolFamily::TinyIPrint);
    CHECK(std::string(d.match.fingerprint.evidence) == "advertised_service_af30");
    CHECK(d.match.score >= 50);
  }

  // (6) A crowded desk must not truncate the result list (old cap was 8).
  {
    ScanRequest request;
    CHECK(request.max_results >= 32);
    RecordingScanTransport transport;
    std::vector<Endpoint> crowd;
    for (int i = 0; i < 12; ++i) {
      Endpoint ep = gb01Endpoint(TransportKind::Ble);
      ep.name = "GB01-" + std::to_string(i);
      ep.address = "aa:bb:cc:dd:ee:" + std::to_string(10 + i);
      crowd.push_back(ep);
    }
    transport.setScanEndpoints(crowd);
    ScanResult out;
    CHECK(transport.scan(request, out) == Result::Ok);
    CHECK(out.endpoints.size() == 12);
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

// ---- POST /api/print/raw: streaming assembler -------------------------------
static void testRawPrintAssembler() {
  section("Raw print assembler: streaming, limits, abort, single bitmap");
  PrintLimits limits;  // RTB_MAX_JOB_BITMAP_BYTES = 64 KB
  const size_t row_bytes = 48;

  RawPrintMeta meta;
  meta.row_bytes = row_bytes;
  meta.job_id = "RAW-JOB-1";
  meta.feed_dots = 64;

  // Deliberately not a whole number of 1436-byte chunks: the tail case is the
  // one that used to cost 5 s on the wire.
  const size_t total = row_bytes * 86;  // 4128 bytes
  std::vector<uint8_t> body(total);
  for (size_t i = 0; i < total; ++i) body[i] = static_cast<uint8_t>(i * 31 + 7);

  // ---- 1. one chunk
  {
    RawPrintAssembler a;
    CHECK(a.begin(row_bytes * 2, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes * 2));
    CHECK(a.received() == row_bytes * 2);
    PrintJob job;
    CHECK(a.finish(job));
    CHECK(job.rows.size() == row_bytes * 2);
    CHECK(job.rows == std::vector<uint8_t>(body.begin(), body.begin() + row_bytes * 2));
    CHECK(job.row_count == 2);
    CHECK(job.row_bytes == row_bytes);
    CHECK(job.feed_dots == 64);
    CHECK(job.job_id == "RAW-JOB-1");
    CHECK(!a.active());
    CHECK(a.expected() == 0);
  }

  // ---- 2. chunk sizes 1 / 64 / 512 / 1436 reassemble byte-exactly
  {
    RawPrintAssembler a;
    CHECK(a.begin(total, meta, limits, 0) == RawBeginResult::Ok);
    const size_t pattern[] = {1, 64, 512, 1436, 1, 64, 512, 1436};
    const size_t pattern_len = sizeof(pattern) / sizeof(pattern[0]);
    size_t offset = 0;
    size_t step = 0;
    while (offset < total) {
      size_t n = pattern[step++ % pattern_len];
      if (n > total - offset) n = total - offset;
      CHECK(a.write(body.data() + offset, n));
      offset += n;
    }
    CHECK(a.received() == total);
    PrintJob job;
    CHECK(a.finish(job));
    CHECK(job.rows == body);  // no loss, no duplication, no reordering
    CHECK(job.row_count == 86);
  }

  // ---- 3. Content-Length over the limit is rejected before allocation
  {
    RawPrintAssembler a;
    const RawBeginResult r =
        a.begin(RTB_MAX_JOB_BITMAP_BYTES + row_bytes, meta, limits, 0);
    CHECK(r == RawBeginResult::PayloadTooLarge);
    CHECK(rawBeginResultStatus(r) == 413);
    CHECK(std::string(rawBeginResultName(r)) == "payload_too_large");
    CHECK(!a.active());
  }

  // ---- 4. malformed metadata -> 400 (and nothing allocated)
  {
    RawPrintAssembler a;

    RawPrintMeta bad = meta;
    bad.row_bytes = 0;
    CHECK(a.begin(row_bytes * 2, bad, limits, 0) == RawBeginResult::InvalidArgument);

    bad = meta;
    bad.row_bytes = 4096;  // above the 512-byte row cap
    CHECK(a.begin(8192, bad, limits, 0) == RawBeginResult::InvalidArgument);

    bad = meta;
    bad.width_dots = 400;  // 400 dots does not match 48 row bytes
    CHECK(a.begin(row_bytes * 2, bad, limits, 0) == RawBeginResult::InvalidArgument);

    bad = meta;
    bad.width_dots = 384;  // 384 dots == 48 bytes: accepted
    CHECK(a.begin(row_bytes * 2, bad, limits, 0) == RawBeginResult::Ok);
    a.abort();

    bad = meta;
    bad.job_id = "bad id!";  // space and '!' are not job_id characters
    CHECK(a.begin(row_bytes * 2, bad, limits, 0) == RawBeginResult::InvalidArgument);

    bad = meta;
    bad.job_id = std::string(65, 'a');
    CHECK(a.begin(row_bytes * 2, bad, limits, 0) == RawBeginResult::InvalidArgument);

    CHECK(a.begin(0, meta, limits, 0) == RawBeginResult::InvalidArgument);
    CHECK(rawBeginResultStatus(RawBeginResult::InvalidArgument) == 400);
  }

  // ---- 5. Content-Length that is not a whole number of rows
  {
    RawPrintAssembler a;
    CHECK(a.begin(row_bytes * 2 + 1, meta, limits, 0) == RawBeginResult::InvalidArgument);
    CHECK(std::string(a.errorName()) == "bitmap_not_row_aligned");
  }

  // ---- 6. a short body never enqueues
  {
    RawPrintAssembler a;
    CHECK(a.begin(row_bytes * 2, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes));  // half of what was promised
    PrintJob job;
    CHECK(!a.finish(job));
    CHECK(std::string(a.errorName()) == "body_truncated");
    CHECK(job.rows.empty());
    CHECK(!a.active());
    CHECK(a.expected() == 0);  // the buffer really went back to the heap
  }

  // ---- 7. an oversized body is cut off, never written past the bitmap
  {
    RawPrintAssembler a;
    CHECK(a.begin(row_bytes * 2, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes * 2));
    CHECK(!a.write(body.data(), 1));
    CHECK(std::string(a.errorName()) == "body_overflow");
    CHECK(a.received() == row_bytes * 2);
    PrintJob job;
    CHECK(!a.finish(job));
    CHECK(job.rows.empty());
  }

  // ---- 8. RAW_ABORTED drops everything; the next request is unaffected
  {
    RawPrintAssembler a;
    CHECK(a.begin(row_bytes * 4, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes));
    a.abort();
    CHECK(!a.active());
    CHECK(a.received() == 0);
    PrintJob job;
    CHECK(!a.finish(job));

    CHECK(a.begin(row_bytes * 2, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes * 2));
    CHECK(a.finish(job));
    CHECK(job.rows.size() == row_bytes * 2);
  }

  // ---- 9. allocation failure is a clean 503, not a crash
  {
    RawPrintAssembler a;
    const RawBeginResult r = a.begin(row_bytes * 20, meta, limits, 128);
    CHECK(r == RawBeginResult::InsufficientMemory);
    CHECK(rawBeginResultStatus(r) == 503);
    CHECK(std::string(rawBeginResultName(r)) == "insufficient_memory");
    CHECK(!a.active());
  }

  // ---- 10. the queue becomes the single owner of the bitmap
  {
    PrintQueue q;
    RawPrintAssembler a;
    CHECK(a.begin(total, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), total));
    PrintJob job;
    CHECK(a.finish(job));
    CHECK(job.rows.size() == total);

    const char* reason = "";
    CHECK(q.enqueueMove(std::move(job), 0, reason) == Result::Ok);
    CHECK(job.rows.empty());             // moved into the queue, not copied
    CHECK(job.rows.capacity() == 0);
    CHECK(job.dedup_key == "RAW-JOB-1"); // identifiers survive the move
    CHECK(job.row_count == 86);

    PrintJob* stored = q.find("RAW-JOB-1");
    CHECK(stored != nullptr);
    CHECK(stored->rows == body);
    CHECK(stored->rows.size() == total);
  }

  // ---- 11. duplicate job_id: rejected, no second job, no half job
  {
    PrintQueue q;
    RawPrintAssembler a;
    RawPrintMeta m = meta;
    m.job_id = "DUP-1";
    const char* reason = "";

    CHECK(a.begin(row_bytes, m, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes));
    PrintJob first;
    CHECK(a.finish(first));
    CHECK(q.enqueueMove(std::move(first), 0, reason) == Result::Ok);

    CHECK(a.begin(row_bytes, m, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes));
    PrintJob dup;
    CHECK(a.finish(dup));
    CHECK(q.enqueueMove(std::move(dup), 1, reason) == Result::Busy);
    CHECK(std::string(reason) == "duplicate_job");
    CHECK(q.size() == 1);
    CHECK(q.stats().pending() == 1);
  }

  // ---- 12. a failed request leaves the queue completely untouched
  {
    PrintQueue q;
    RawPrintAssembler a;
    CHECK(a.begin(row_bytes * 2, meta, limits, 0) == RawBeginResult::Ok);
    CHECK(a.write(body.data(), row_bytes));  // short
    PrintJob job;
    CHECK(!a.finish(job));
    CHECK(q.size() == 0);
    CHECK(q.stats().pending() == 0);
  }
}

// Website V1.1/V1.2: the USB serial channel answers exactly two commands.
// Every refusal below must also be free of side effects - the parser owns no
// printer, transport or firmware state, and the scan reply is rendered only
// from data the caller measured.
void testSerialControl() {
  section("Serial control: gateway_info + scan only (V1.1/V1.2)");

  SerialControl control;
  control.setIdentity("gw-esp32-a1b2c3", "ESP32-WROOM-32E", "0.2.0");

  const std::string expected =
      "{\"type\":\"gateway_info\",\"gateway_id\":\"gw-esp32-a1b2c3\","
      "\"hardware_family\":\"ESP32-WROOM-32E\",\"firmware_version\":\"0.2.0\","
      "\"protocol_version\":1}";

  // ---- accepted command #1: identity, exact wire shape ----
  CHECK(control.dispatch("{\"type\":\"gateway_info\"}").command ==
        SerialControl::Command::kInfo);
  CHECK(control.dispatch("{\"type\":\"gateway_info\"}").reply == expected);
  CHECK(control.dispatch(std::string("{\"type\":\"gateway_info\"}\r\n")).reply == expected);
  CHECK(control.dispatch(std::string("  {\"type\":\"gateway_info\"}  ")).reply == expected);
  // Extra fields are ignored: the client may add whatever it likes.
  CHECK(control.dispatch("{\"type\":\"gateway_info\",\"nonce\":\"7\"}").reply == expected);
  // A fixed schema: one line, no newline inside, version always present.
  CHECK(expected.find('\n') == std::string::npos);
  CHECK(expected.find("\"protocol_version\":1") != std::string::npos);

  // ---- accepted command #2: scan is handed to the caller, never answered here ----
  {
    const SerialControl::Dispatch scan = control.dispatch("{\"type\":\"scan\"}");
    CHECK(scan.command == SerialControl::Command::kScan);
    CHECK(scan.reply.empty());
    CHECK(control.dispatch(std::string("{\"type\":\"scan\"}\r\n")).command ==
          SerialControl::Command::kScan);
  }

  // ---- scan may ask for a window, but the caller only grants a bounded one ----
  {
    CHECK(control.dispatch("{\"type\":\"scan\",\"duration_ms\":1500}").duration_ms == 1500);
    CHECK(control.dispatch("{\"type\":\"scan\"}").duration_ms == 0);
    CHECK(SerialControl::clampScanDuration(0) == SerialControl::kScanDefaultMs);
    CHECK(SerialControl::clampScanDuration(-5) == SerialControl::kScanDefaultMs);
    CHECK(SerialControl::clampScanDuration(200) == SerialControl::kScanMinMs);
    CHECK(SerialControl::clampScanDuration(1500) == 1500);
    CHECK(SerialControl::clampScanDuration(60000) == SerialControl::kScanMaxMs);
    CHECK(SerialControl::clampScanDuration(-2147483647 - 1) == SerialControl::kScanDefaultMs);
  }

  // ---- human logs on the same UART are not commands ----
  const char* quiet_lines[] = {
      "Remote Thermal Printer Bridge V0.1 - compile/mock verified only",
      "[ctrl] usb control ready",
      "[HEAP] rawprint free=45000",
      "",
      "   ",
      "gateway_info",
  };
  for (const char* log_line : quiet_lines) {
    const SerialControl::Dispatch quiet = control.dispatch(log_line);
    CHECK(quiet.command == SerialControl::Command::kNone);
    CHECK(quiet.reply.empty());
  }

  // ---- refused commands: no identity, no echo ----
  const char* refused[] = {
      "{\"type\":\"print\"}",
      "{\"type\":\"raw\"}",
      "{\"type\":\"firmware_update\"}",
      "{\"type\":\"ble_connect\"}",
      "{\"type\":\"safe_test\"}",
      "{\"type\":\"unknown\"}",
      "{\"type\":\"Gateway_Info\"}",  // case matters
      "{\"type\":\"gateway_info_x\"}",
      "{\"type\":\"scan_all\"}",
  };
  for (const char* command : refused) {
    const SerialControl::Dispatch dispatch = control.dispatch(command);
    CHECK(dispatch.command == SerialControl::Command::kError);
    CHECK(dispatch.reply.find("\"error\":\"unsupported_command\"") != std::string::npos);
    CHECK(dispatch.reply.find("\"type\":\"error\"") != std::string::npos);
    CHECK(dispatch.reply.find("gateway_id") == std::string::npos);
    CHECK(dispatch.reply.find("protocol_version") != std::string::npos);
  }

  // ---- malformed input is rejected, not guessed ----
  const char* malformed[] = {
      "{\"type\":",
      "{}",
      "{\"command\":\"gateway_info\"}",
      "{\"type\":123}",
      "{\"type\":null}",
  };
  for (const char* bad : malformed) {
    const SerialControl::Dispatch dispatch = control.dispatch(bad);
    CHECK(dispatch.command == SerialControl::Command::kError);
    CHECK(dispatch.reply.find("malformed_request") != std::string::npos);
  }

  // ---- oversized line: refused without growing anything ----
  {
    std::string big = "{\"type\":\"";
    big += std::string(300, 'a');
    big += "\"}";
    CHECK(big.size() > SerialControl::kMaxLineBytes);
    const SerialControl::Dispatch dispatch = control.dispatch(big);
    CHECK(dispatch.command == SerialControl::Command::kError);
    CHECK(dispatch.reply.find("line_too_long") != std::string::npos);
  }

  // ---- an injection attempt never reaches the reply ----
  {
    const std::string reply = control.dispatch("{\"type\":\"gateway_info\",\"x\":\"evil\"}").reply;
    CHECK(reply == expected);
    CHECK(reply.find("evil") == std::string::npos);
    const std::string trick =
        control.dispatch("{\"type\":\"gateway_info\\\",\\\"x\\\":\\\"evil\"}").reply;
    CHECK(trick.find("evil") == std::string::npos);
  }

  // ---- identity is reported verbatim, and can be updated ----
  control.setIdentity("gw-esp32-ffffff", "ESP32-S3-WROOM-1", "0.3.1");
  const std::string second = control.dispatch("{\"type\":\"gateway_info\"}").reply;
  CHECK(second.find("\"gateway_id\":\"gw-esp32-ffffff\"") != std::string::npos);
  CHECK(second.find("\"hardware_family\":\"ESP32-S3-WROOM-1\"") != std::string::npos);
  CHECK(second.find("\"firmware_version\":\"0.3.1\"") != std::string::npos);

  // ---- an empty identity is still a valid, parseable line ----
  {
    SerialControl blank;
    const std::string reply = blank.dispatch("{\"type\":\"gateway_info\"}").reply;
    CHECK(reply.find("\"gateway_id\":\"\"") != std::string::npos);
    CHECK(reply.find("\"protocol_version\":1") != std::string::npos);
  }

  // ---- scan_result: fixed schema, and only fields the scan really measured ----
  {
    SerialScanDevice device;
    device.name = "X18";
    device.address = "aa:bb:cc:dd:ee:ff";
    device.rssi = -52;
    device.service_uuid = "0000af30-0000-1000-8000-00805f9b34fb";
    device.fingerprint = "TinyIPrint";
    device.confidence = 70;
    device.evidence = "bluetooth_name_pattern";
    device.model_id = "x18_aijixing";
    device.match_reason = "bluetooth_name_pattern";

    const std::string line = control.scanLine(&device, 1, "OK", 4012, 3);
    CHECK(line.find("{\"type\":\"scan_result\",\"result\":\"OK\",\"transport\":\"ble\"") == 0);
    CHECK(line.find("\"scanned\":3") != std::string::npos);
    CHECK(line.find("\"elapsed_ms\":4012") != std::string::npos);
    CHECK(line.find("\"protocol_version\":1") != std::string::npos);
    CHECK(line.find("\"name\":\"X18\"") != std::string::npos);
    CHECK(line.find("\"address\":\"aa:bb:cc:dd:ee:ff\"") != std::string::npos);
    CHECK(line.find("\"rssi\":-52") != std::string::npos);
    CHECK(line.find("\"services\":[\"0000af30") != std::string::npos);
    CHECK(line.find("\"fingerprint\":\"TinyIPrint\"") != std::string::npos);
    CHECK(line.find("\"confidence\":70") != std::string::npos);
    CHECK(line.find("\"evidence\":\"bluetooth_name_pattern\"") != std::string::npos);
    CHECK(line.find("\"model_id\":\"x18_aijixing\"") != std::string::npos);
    CHECK(line.find("\"match_reason\":\"bluetooth_name_pattern\"") != std::string::npos);
    // One line only, and nothing invented: no GATT enumeration, no manufacturer
    // data (V1.2 never connects to a printer).
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.find("characteristics") == std::string::npos);
    CHECK(line.find("manufacturer") == std::string::npos);
    CHECK(line.find("gatt_verified") == std::string::npos);
  }

  // ---- scan_result: no devices, and an advertiser with no name / no service ----
  {
    const std::string empty = control.scanLine(nullptr, 0, "NOT_FOUND", 4001, 0);
    CHECK(empty.find("\"result\":\"NOT_FOUND\"") != std::string::npos);
    CHECK(empty.find("\"devices\":[]") != std::string::npos);

    SerialScanDevice quiet;
    quiet.name = "";
    quiet.address = "11:22:33:44:55:66";
    quiet.rssi = -90;
    quiet.service_uuid = "";
    quiet.fingerprint = "Unknown";
    quiet.confidence = 0;
    quiet.evidence = "no_matching_evidence";
    quiet.model_id = "unmatched";
    quiet.match_reason = "insufficient_evidence";
    const std::string line = control.scanLine(&quiet, 1, "OK", 4002, 1);
    CHECK(line.find("\"name\":\"\"") != std::string::npos);
    CHECK(line.find("\"services\":[]") != std::string::npos);
    CHECK(line.find("\"model_id\":\"unmatched\"") != std::string::npos);
  }

  // ---- scan_result: two devices stay comma separated on one line ----
  {
    SerialScanDevice pair[2];
    pair[0].name = "GB01";
    pair[0].address = "aa:aa:aa:aa:aa:aa";
    pair[0].rssi = -60;
    pair[0].service_uuid = "ae30";
    pair[0].model_id = "gb01";
    pair[1].name = "X18";
    pair[1].address = "bb:bb:bb:bb:bb:bb";
    pair[1].rssi = -45;
    pair[1].service_uuid = "af30";
    pair[1].model_id = "x18_aijixing";
    const std::string line = control.scanLine(pair, 2, "OK", 4003, 2);
    CHECK(line.find("\"name\":\"GB01\"") < line.find("\"name\":\"X18\""));
    CHECK(line.find("},{") != std::string::npos);
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.substr(line.size() - 2) == "]}");
  }

  // ---- scan_result: a hostile name cannot break the JSON ----
  {
    SerialScanDevice hostile;
    hostile.name = "X18\",\"model_id\":\"evil";
    hostile.address = "cc:cc:cc:cc:cc:cc";
    hostile.rssi = -70;
    hostile.service_uuid = "";
    hostile.model_id = "unmatched";
    const std::string line = control.scanLine(&hostile, 1, "OK", 4004, 1);
    CHECK(line.find("\\\"model_id\\\"") != std::string::npos);
    CHECK(line.find("\"model_id\":\"evil") == std::string::npos);
    CHECK(line.find("\"model_id\":\"unmatched\"") != std::string::npos);
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
  testPrintRequestParser();
  testJsonIntOverflow();
  testJobIdContract();
  testJsonPrintEndpointPolicy();
  testRawPrintOomContainment();
  testSafeTestRegistry();
  testRawPrintAssembler();
  testHostCompileConditions();
  testDedupSemanticsById();
  testNotificationCallbackLifetime();
  testWideRowMirror();
  testDiscoveryFallback();
  testPartialPrintSemantics();
  testSerialControl();

  // Protocol runs unchanged over a BLE-emulating Mock and an SPP-emulating Mock.
  section("Transport / Protocol decoupling: TinyProtocol over BLE Mock and Classic Mock");
  MockTransport ble_mock;
  runTinyOverTransport(ble_mock, "ble-mock");
  BtClassicMockTransport spp_mock;
  runTinyOverTransport(spp_mock, "classic-spp-mock");

  printf("=== %d checks, %d failures ===\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
