#pragma once

#include <stdint.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

namespace rtb {

// Pacing is a runtime concern, not a protocol concern.
inline void sleepMs(uint32_t ms) {
#if defined(ARDUINO)
  delay(ms);
#else
  (void)ms;  // host Mock tests run without wall-clock delays
#endif
}

}  // namespace rtb
