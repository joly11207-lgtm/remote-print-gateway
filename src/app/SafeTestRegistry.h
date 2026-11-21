#pragma once

#include <stdint.h>

#include <string>

namespace rtb {

// The only tests the firmware will ever execute on a printer.
//
// This is the firmware half of the safe-test whitelist (the gateway has the
// other half in gateway/safe_tests.py). A request names an ID; it can never
// carry bytes. If an ID is not in this table the firmware refuses - there is no
// "raw command" escape hatch anywhere in the HTTP surface.
//
// V0.2 ships exactly one entry: the read-only Tiny/iPrint status query that has
// already been validated on the X18 (A3 [0x00], no paper move, no print data).
struct SafeTestSpec {
  const char* safe_test_id;
  const char* description;
  const char* protocol;
  const char* transport;
  bool moves_paper;  // must stay false: a compatibility test never prints
};

// Returns nullptr for anything not in the registry.
const SafeTestSpec* findSafeTest(const std::string& safe_test_id);
bool isSafeTestAllowed(const std::string& safe_test_id);

// Registry size (used by the host tests to prove there is no hidden entry).
size_t safeTestCount();

// The one command SAFE_TEST_TINY_STATUS_V1 issues: A3 [0x00]. Exposed for tests
// so "no arbitrary hex" is an assertion, not a promise.
constexpr uint8_t kSafeTestStatusOpcode = 0xA3;
constexpr uint8_t kSafeTestStatusPayload = 0x00;

}  // namespace rtb
