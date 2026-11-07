#pragma once

// Optional local credentials. Create src/app/secrets.h (gitignored) with:
//   #define RTB_WIFI_SSID "your-ssid"
//   #define RTB_WIFI_PASS "your-password"
// With no credentials the bridge starts a local AP for the HTTP debug API.

#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef RTB_WIFI_SSID
#define RTB_WIFI_SSID ""
#endif
#ifndef RTB_WIFI_PASS
#define RTB_WIFI_PASS ""
#endif

#define RTB_AP_SSID "RTB-V0.1-AP"
