#pragma once

// ---- Where releases live -------------------------------------------------
// versions.txt lists every published image, one per line:
//   <version> <file> <size-bytes> <sha256>
// Files are fetched relative to OTA_BASE_URL.
#define OTA_HOST       "raw.githubusercontent.com"
#define OTA_BASE_URL   "https://" OTA_HOST "/P2mSoft-Embadded-System/live-zkt-ota-gw-fw/main/"
#define OTA_MANIFEST   "versions.txt"

// ---- Board ---------------------------------------------------------------
#ifndef LED_PIN
#define LED_PIN 2            // built-in LED on ESP32 DevKit
#endif

// ---- Timing --------------------------------------------------------------
#define WIFI_TIMEOUT_MS      30000   // per boot, to get an IP
#define HTTP_TIMEOUT_MS      15000   // connect / read timeout
#define DOWNLOAD_STALL_MS    20000   // abort download if no bytes for this long
#define SELFTEST_WDT_S       120     // a new image that hangs is reset -> rolled back

// A release that fails to validate this many times is never tried again.
#define MAX_UPDATE_ATTEMPTS  2
