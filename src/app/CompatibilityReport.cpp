#include "app/CompatibilityReport.h"

#include "config/RtbConfig.h"

namespace rtb {
namespace {

std::string q(const char* s) { return std::string("\"") + (s ? s : "") + "\""; }

std::string b(bool v) { return v ? "true" : "false"; }

std::string transportJson(const char* key, const TransportCapabilities& c) {
  std::string out = "\"" + std::string(key) + "\":{";
  out += "\"available\":" + b(c.available);
  out += ",\"supports_scan\":" + b(c.supports_scan);
  out += ",\"supports_notifications\":" + b(c.supports_notifications);
  out += ",\"requires_notify_subscription\":" + b(c.requires_notify_subscription);
  out += ",\"max_write_chunk\":" + std::to_string(c.max_write_chunk);
  out += ",\"unavailable_reason\":" +
         (c.unavailable_reason ? q(c.unavailable_reason) : std::string("null"));
  out += ",\"note\":" + q(c.note) + "}";
  return out;
}

std::string protocolJson(const char* key, const ProtocolCapabilities& c) {
  std::string out = "\"" + std::string(key) + "\":{";
  out += "\"implemented\":" + b(c.implemented);
  out += ",\"framing\":" + b(c.framing);
  out += ",\"crc\":" + b(c.crc);
  out += ",\"init_command\":" + b(c.init_command);
  out += ",\"bitmap_command\":" + b(c.bitmap_command);
  out += ",\"feed\":" + b(c.feed);
  out += ",\"status_parsing\":" + b(c.status_parsing);
  out += ",\"note\":" + q(c.note) + "}";
  return out;
}

}  // namespace

std::string buildCompatibilityReport(const CompatibilityInput& input) {
  const BoardCapabilities& caps = input.board ? *input.board : boardCapabilities();

  std::string out = "{";
  out += "\"version\":" + q(RTB_VERSION);
  out += ",\"target\":" + q(RTB_TARGET_ID);
  out += ",\"chip\":" + q(caps.chip_name);
  out += ",\"board_capabilities\":{";
  out += "\"wifi\":" + b(caps.wifi);
  out += ",\"ble\":" + b(caps.ble);
  out += ",\"bluetooth_classic\":" + b(caps.bluetooth_classic);
  out += ",\"psram\":" + b(caps.psram);
  out += ",\"psram_size\":" + std::to_string(caps.psram_size);
  out += ",\"flash_size\":" + std::to_string(caps.flash_size);
  out += ",\"ota\":" + b(caps.ota);
  out += ",\"dual_core\":" + b(caps.dual_core) + "}";

  out += ",\"transports\":{";
  out += transportJson("ble", input.ble) + ",";
  out += transportJson("bluetooth_classic", input.bluetooth_classic) + ",";
  out += transportJson("mock_bluetooth_classic", input.classic_mock) + ",";
  out += transportJson("mock", input.mock) + "}";

  out += ",\"protocols\":{";
  out += protocolJson("tiny_iprint", input.tiny) + ",";
  out += protocolJson("escpos", input.escpos) + "}";

  out += ",\"device_profiles\":" + std::to_string(input.profile_count);
  out += ",\"runtime\":{";
  out += "\"wifi_connected\":" + b(input.wifi_connected);
  out += ",\"printer_state\":" + q(input.printer_state);
  out += ",\"active_transport\":" + q(input.active_transport);
  out += ",\"pending_jobs\":" + std::to_string(input.pending_jobs) + "}";

  // Honesty block: nothing in V0.1 has been verified against a real printer.
  out += ",\"verification\":{";
  out += "\"real_printer_tested\":false";
  out += ",\"real_print_succeeded\":false";
  out += ",\"esp32_s3_n16r8_hardware_flashed\":false";
  out += ",\"esp32_s3_supermini_hardware_flashed\":false";
  out += ",\"wroom32e_board_available\":false";
  out += ",\"classic_bluetooth_verified\":false";
  out += ",\"this_result_is\":\"compile_and_mock_tests_only\"}";

  out += ",\"warnings\":[";
  out += q("No physical printer available: BLE protocol is compile/mock verified only.");
  out += "," + q("ESP32-S3 has no Bluetooth Classic radio; SPP/RFCOMM is architecture-only.");
  out += "," + q("No ESP32-WROOM-32E board on the desk; future target is not hardware verified.");
  out += "," + q("Flash/PSRAM part numbers are intentionally not locked for production.");
  out += "]}";
  return out;
}

}  // namespace rtb
