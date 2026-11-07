#pragma once

#if defined(ARDUINO)
#include <WebServer.h>

#include <string>
#include <vector>

#include "app/CompatibilityReport.h"
#include "app/HttpDebugApi.h"
#include "app/PrintQueue.h"
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
  std::string scanJson();
  std::string submitPrintJson(const std::string& body);
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
};

}  // namespace rtb
#endif
