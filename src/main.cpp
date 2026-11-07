#include <Arduino.h>

#include "app/BridgeApp.h"

namespace {
rtb::BridgeApp* g_bridge = nullptr;
}

void setup() {
  Serial.begin(115200);
  static rtb::BridgeApp bridge;
  g_bridge = &bridge;
  bridge.begin();
}

void loop() {
  if (g_bridge) g_bridge->tick();
  delay(2);
}
