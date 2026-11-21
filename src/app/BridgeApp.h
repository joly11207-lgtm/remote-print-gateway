#pragma once

#if defined(ARDUINO)
#include <WebServer.h>

#include <string>
#include <vector>

#include "app/CompatibilityReport.h"
#include "app/HttpDebugApi.h"
#include "app/PrintQueue.h"
#include "app/SerialControl.h"
#include "app/WifiService.h"
#include "printers/catalog/DeviceProfile.h"
#include "protocols/escpos/EscPosProtocol.h"
#include "protocols/tiny_iprint/TinyProtocol.h"
#include "transport/base/PrinterTransport.h"
#include "transport/ble/BleTransport.h"
#include "transport/bt_classic/BluetoothClassicTransport.h"
#include "transport/bt_classic/BtClassicMockTransport.h"
#include "transport/mock/MockTransport.h"

namespace rtb {

enum class PrinterState : uint8_t {
  Offline = 0,
  Scanning = 1,
  Connecting = 2,
  Connected = 3,
  Printing = 4,
  Recovering = 5,
};

const char* printerStateName(PrinterState state);

class BridgeApp {
 public:
  BridgeApp();
  void begin();
  void tick();

  // ---- HTTP debug API surface ---------------------------------------------
  std::string statusJson() const;
  std::string compatibilityJson() const;
  std::string queueJson() const;
  // Legacy GET /api/scan: discovery list only (name/address/rssi/fingerprint).
  // No connection is made, so it cannot report GATT evidence.
  std::string scanJson();

  // V0.2 gateway scan for the website: discovery + a GATT confirmation of the
  // best catalog-matched candidate. The confirmation is a real connect(), which
  // verifies the profile's service/write/notify characteristics exist - that is
  // the evidence the compatibility engine needs. No printing, no writes.
  // `body` accepts {"duration_ms": N, "inspect": bool}.
  std::string scanDetailJson(const std::string& body);

  // V0.2 safe test: executes ONE registry entry (SafeTestRegistry.h), never
  // bytes from the caller. `http_status` is 200 for an executed test (passed or
  // failed), 422 when the id is not in the registry, 400 for a malformed body.
  std::string safeTestJson(const std::string& body, int& http_status);

  // `body` is taken as a buffer, NOT as a std::string: the caller's HTTP body is
  // already in RAM and must never be copied (that copy was the allocation that
  // failed and aborted the chip).
  // SMALL / LEGACY endpoint: RTB_HTTP_MAX_BODY_BYTES (8 KB) is its ceiling.
  // Large payloads MUST use POST /api/print/raw.
  // `http_status` is an OUT parameter: 200 success, 400 malformed/invalid,
  // 409 duplicate job, 413 over the configured ceiling, 503 legal payload that
  // does not fit in the current heap. The caller maps it to the response.
  std::string submitPrintJson(const char* body, size_t body_len, int& http_status);

  // Defaults resolved from the active DeviceProfile (48 bytes / 64 dots when no
  // profile is known yet). Used by both print entry points so /api/print and
  // /api/print/raw cannot drift apart.
  struct PrintDefaults {
    size_t row_bytes;
    uint16_t feed_dots;
  };
  PrintDefaults printDefaults() const;

  // Single enqueue path for every entry point. The queue takes ownership of
  // job.rows, so the caller's bitmap is gone after a successful call.
  Result enqueueJob(PrintJob& job, const char*& reason);
  std::string setMockJson(const std::string& body);
  std::string connectJson(const std::string& body);
  std::string otaJson() const;
  void disconnectPrinter();

  PrinterState printerState() const { return state_; }
  const char* lastError() const { return last_error_.c_str(); }
  bool mockMode() const { return mock_mode_; }

 private:
  void attemptConnect(uint32_t now_ms);
  void printerOffline(const char* reason, uint32_t now_ms);
  void runPendingJob(uint32_t now_ms);
  void setMockMode(bool enabled);
  CompatibilityInput makeCompatibilityInput() const;

  // V1.1/V1.2 USB control channel. Bounded and non-blocking; the only commands
  // it answers are gateway_info and scan (see app/SerialControl.h).
  void pollSerialControl();
  void printSerialLine(const std::string& line);
  // Runs one bounded BLE scan for the serial `scan` command and answers it.
  // It never connects, never tests and never prints.
  void runSerialScan(uint32_t duration_ms);

  WebServer server_;
  WifiService wifi_;
  HttpDebugApi api_;

  BleTransport ble_;
  MockTransport mock_;
  BtClassicMockTransport classic_mock_;
  BluetoothClassicTransport classic_stub_;
  PrinterTransport* active_ = nullptr;

  TinyProtocol tiny_;
  EscPosProtocol escpos_;
  PrintQueue queue_;

  const DeviceProfile* profile_ = nullptr;
  Endpoint endpoint_;
  PrinterState state_ = PrinterState::Offline;
  std::string last_error_;
  uint32_t next_retry_ms_ = 0;
  uint32_t backoff_ms_ = 2000;
  uint32_t last_keepalive_ms_ = 0;
  bool mock_mode_ = false;

  // Serial control state. The line buffer is fixed size and lives in the
  // object: a client cannot make the firmware allocate per request.
  SerialControl serial_control_;
  char serial_line_[SerialControl::kMaxLineBytes + 1];
  size_t serial_line_len_ = 0;
  bool serial_discard_ = false;
};

}  // namespace rtb
#endif
