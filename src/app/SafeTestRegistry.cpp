#include "app/SafeTestRegistry.h"

#include <string.h>

namespace rtb {
namespace {

// Ordered, immutable, and deliberately tiny. Adding an entry is a product and
// safety decision, not a configuration option.
const SafeTestSpec kRegistry[] = {
    {"SAFE_TEST_TINY_STATUS_V1", "Tiny/iPrint read-only status query (A3 00)",
     "tiny_iprint", "ble", false},
};

const size_t kRegistrySize = sizeof(kRegistry) / sizeof(kRegistry[0]);

}  // namespace

const SafeTestSpec* findSafeTest(const std::string& safe_test_id) {
  for (size_t i = 0; i < kRegistrySize; ++i) {
    if (safe_test_id == kRegistry[i].safe_test_id) return &kRegistry[i];
  }
  return nullptr;
}

bool isSafeTestAllowed(const std::string& safe_test_id) {
  return findSafeTest(safe_test_id) != nullptr;
}

size_t safeTestCount() { return kRegistrySize; }

}  // namespace rtb
