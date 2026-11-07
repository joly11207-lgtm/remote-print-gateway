#include "printers/catalog/BuiltinProfiles.h"

namespace rtb {
namespace {

const char* const kGb01Names[] = {"GB01"};
const char* const kGb02Names[] = {"GB02"};
const char* const kMx06Names[] = {"MX06", "MX05", "MX10", "YT01"};
const char* const kSc03hNames[] = {"SC03h", "iPrint", "Pocket Printer", "D1", "FC02"};
const char* const kEscPosSppNames[] = {"POS", "ESC", "Printer"};

}  // namespace

const DeviceProfile kBuiltinProfiles[] = {
    // V0.1 target family: BLE + Tiny / iPrint / CatPrinter AE30.
    {"gb01", "GB01 (58mm Cat Printer)", ProtocolFamily::TinyIPrint, kSupportBle,
     kGb01Names, 1, "AE30", "AE01", "AE02", 384, true, true, 16000, 51, 35, 64,
     10, 20, true},

    {"gb02", "GB02 (58mm Cat Printer)", ProtocolFamily::TinyIPrint, kSupportBle,
     kGb02Names, 1, "AE30", "AE01", "AE02", 384, true, true, 12000, 51, 26, 64,
     10, 20, true},

    {"mx06", "MX06 / MX05 / YT01", ProtocolFamily::TinyIPrint, kSupportBle,
     kMx06Names, 4, "AE30", "AE01", "AE02", 384, true, true, 12000, 51, 30, 64,
     10, 20, true},

    // iPrint SC03h family: same framing, but 0xA6 lattice is known to crash it.
    {"sc03h", "iPrint SC03h / Pocket Printer", ProtocolFamily::TinyIPrint, kSupportBle,
     kSc03hNames, 5, "AE30", "AE01", "AE02", 384, false, false, 17520, 51, 35,
     130, 10, 180, true},

    // LATER: example of a Classic SPP ESC/POS device. Architecture only.
    {"escpos_spp_future", "Future ESC/POS SPP printer (NOT implemented)",
     ProtocolFamily::EscPos, kSupportBluetoothClassic, kEscPosSppNames, 3,
     "", "SPP", "", 384, false, false, 0, 0, 0, 0, 0, 0, false},
};

const size_t kBuiltinProfileCount = sizeof(kBuiltinProfiles) / sizeof(kBuiltinProfiles[0]);

}  // namespace rtb
