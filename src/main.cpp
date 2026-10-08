// Blink + pull-OTA with rollback.
//
// Every boot:
//   1. connect WiFi, fetch versions.txt from GitHub            (= self-test)
//   2. if this image is a freshly installed update, the self-test decides:
//        pass -> mark image valid          fail -> roll back to previous image
//      a crash/hang before that point is rolled back by the bootloader itself
//   3. if versions.txt lists a newer release: download, verify size + SHA-256,
//      switch boot partition, reboot
//   4. blink
//
// WiFi credentials are stored in NVS, never compiled in (the .bin is public).
// Set them once over serial:  ssid <name>   pass <password>   reboot

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>

#include "ota_config.h"
#include "ota_root_ca.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif
#ifndef BLINK_MS
#define BLINK_MS 1000
#endif

// Tell the Arduino core NOT to mark the image valid on its own at startup;
// we do it ourselves once the self-test has passed.
extern "C" bool verifyRollbackLater() { return true; }

struct Release {
  char version[16];
  char file[64];
  uint32_t size;
  char sha256[65];
};

static Preferences prefs;
static bool pendingVerify = false;  // running image still awaits validation

// ---------------------------------------------------------------- helpers --

// Parses "major.minor.patch" (anything after is ignored). Returns false if malformed.
static bool parseVersion(const char *s, int v[3]) {
  return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

// <0, 0, >0 like strcmp. Unparseable versions sort lowest.
static int compareVersions(const char *a, const char *b) {
  int va[3] = {-1, -1, -1}, vb[3] = {-1, -1, -1};
  parseVersion(a, va);
  parseVersion(b, vb);
  for (int i = 0; i < 3; i++) {
    if (va[i] != vb[i]) return va[i] - vb[i];
  }
  return 0;
}

static const char *otaStateName(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:            return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:          return "VALID";
    case ESP_OTA_IMG_INVALID:        return "INVALID";
    case ESP_OTA_IMG_ABORTED:        return "ABORTED";
    default:                         return "UNDEFINED";
  }
}

// getString() logs an error for a missing key; this stays quiet.
static String prefStr(const char *key) {
  return prefs.isKey(key) ? prefs.getString(key, "") : String();
}

static void printStatus() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  Serial.printf("[status] version=%s partition=%s state=%s wifi=%s ip=%s\n",
                FW_VERSION, running->label, otaStateName(state),
                WiFi.isConnected() ? "up" : "down",
                WiFi.localIP().toString().c_str());
  Serial.printf("[status] pending='%s' bad='%s' attempts=%u\n",
                prefStr("pend").c_str(),
                prefStr("bad").c_str(),
                prefs.getUChar("tries", 0));
}

// ------------------------------------------------------- rollback tracking --

// Compares the version we *tried* to install (saved before the reboot) with
// what is actually running, so a rolled-back release is not retried forever.
static void reconcilePendingUpdate() {
  String pend = prefStr("pend");
  if (pend.isEmpty()) return;

  if (pend == FW_VERSION) {
    Serial.printf("[ota] first boot of update %s\n", FW_VERSION);
    return;  // still has to pass the self-test
  }

  uint8_t tries = prefs.getUChar("tries", 0);
  Serial.printf("[ota] ROLLBACK DETECTED: %s did not validate, still running %s (attempt %u/%u)\n",
                pend.c_str(), FW_VERSION, tries, MAX_UPDATE_ATTEMPTS);
  if (tries >= MAX_UPDATE_ATTEMPTS) {
    Serial.printf("[ota] blacklisting %s\n", pend.c_str());
    prefs.putString("bad", pend);
    prefs.putUChar("tries", 0);
  }
  prefs.remove("pend");
}

static void markValid() {
  if (!pendingVerify) return;
  esp_ota_mark_app_valid_cancel_rollback();
  pendingVerify = false;
  prefs.remove("pend");
  prefs.putUChar("tries", 0);
  Serial.printf("[ota] self-test passed, %s marked VALID\n", FW_VERSION);
}

static void rollbackNow(const char *why) {
  Serial.printf("[ota] self-test FAILED (%s) -> rolling back\n", why);
  Serial.flush();
  esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
  Serial.println("[ota] rollback not possible, restarting");
  ESP.restart();
}

// ------------------------------------------------------------------- WiFi --

static bool connectWifi() {
  String ssid = prefStr("ssid");
  if (ssid.isEmpty()) {
    Serial.println("[wifi] no credentials. Send:  ssid <name>  /  pass <password>  /  reboot");
    return false;
  }
  Serial.printf("[wifi] connecting to '%s'", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), prefStr("pass").c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(100);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] connect timeout");
    return false;
  }
  Serial.printf("[wifi] connected, ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// --------------------------------------------------------------- manifest --

// Downloads versions.txt and picks the highest listed version.
// Returns false only on transport failure; `found` tells whether a line parsed.
static bool fetchLatestRelease(Release &latest, bool &found) {
  found = false;
  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);

  // query string only defeats intermediate caches; GitHub ignores it
  String url = String(OTA_BASE_URL OTA_MANIFEST "?t=") + String(esp_random());
  if (!http.begin(client, url)) {
    Serial.println("[ota] manifest: bad url");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] manifest: HTTP %d %s\n", code, code < 0 ? http.errorToString(code).c_str() : "");
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  int pos = 0, count = 0;
  while (pos < (int)body.length()) {
    int eol = body.indexOf('\n', pos);
    if (eol < 0) eol = body.length();
    String line = body.substring(pos, eol);
    pos = eol + 1;
    line.trim();
    if (line.isEmpty() || line[0] == '#') continue;

    Release r;
    int v[3];
    unsigned long size = 0;
    if (sscanf(line.c_str(), "%15s %63s %lu %64s", r.version, r.file, &size, r.sha256) != 4 ||
        !parseVersion(r.version, v) || size == 0 || strlen(r.sha256) != 64) {
      Serial.printf("[ota] manifest: skipping malformed line '%s'\n", line.c_str());
      continue;
    }
    r.size = size;
    count++;
    if (!found || compareVersions(r.version, latest.version) > 0) {
      latest = r;
      found = true;
    }
  }
  Serial.printf("[ota] manifest: %d release(s), latest=%s\n", count, found ? latest.version : "none");
  return true;
}

// ---------------------------------------------------------------- install --

static bool installRelease(const Release &rel) {
  Serial.printf("[ota] downloading %s (%u bytes)\n", rel.file, rel.size);

  WiFiClientSecure client;
  client.setCACert(OTA_ROOT_CA);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, String(OTA_BASE_URL) + rel.file)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] download: HTTP %d\n", code);
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len != (int)rel.size) {
    Serial.printf("[ota] download: size mismatch (server %d, manifest %u)\n", len, rel.size);
    http.end();
    return false;
  }
  if (!Update.begin(rel.size)) {
    Serial.printf("[ota] Update.begin: %s\n", Update.errorString());
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);

  WiFiClient *stream = http.getStreamPtr();
  static uint8_t buf[4096];
  uint32_t written = 0, lastData = millis();
  int lastPct = -1;
  bool ok = true;
  while (written < rel.size) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (!stream->connected() || millis() - lastData > DOWNLOAD_STALL_MS) {
        Serial.println("[ota] download: connection lost / stalled");
        ok = false;
        break;
      }
      delay(5);
      continue;
    }
    int n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (n <= 0) continue;
    lastData = millis();
    mbedtls_sha256_update_ret(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) {
      Serial.printf("[ota] flash write: %s\n", Update.errorString());
      ok = false;
      break;
    }
    written += n;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    int pct = written * 100ULL / rel.size;
    if (pct / 10 != lastPct / 10) {
      Serial.printf("[ota] %d%%\n", pct);
      lastPct = pct;
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);

  if (ok) {
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
    if (strcasecmp(hex, rel.sha256) != 0) {
      Serial.printf("[ota] SHA-256 mismatch\n  got      %s\n  expected %s\n", hex, rel.sha256);
      ok = false;
    }
  }
  if (!ok) {
    Update.abort();  // boot partition untouched, old image keeps running
    return false;
  }
  if (!Update.end()) {  // validates the image and switches the boot partition
    Serial.printf("[ota] Update.end: %s\n", Update.errorString());
    return false;
  }
  Serial.println("[ota] image verified and written");
  return true;
}

static void checkForUpdate(const Release &latest) {
  if (compareVersions(latest.version, FW_VERSION) <= 0) {
    Serial.printf("[ota] up to date (running %s)\n", FW_VERSION);
    return;
  }
  if (prefStr("bad") == latest.version) {
    Serial.printf("[ota] %s is blacklisted after failed installs, staying on %s\n", latest.version, FW_VERSION);
    return;
  }
  Serial.printf("[ota] update available: %s -> %s\n", FW_VERSION, latest.version);
  if (!installRelease(latest)) {
    Serial.println("[ota] install failed, will retry on next boot");
    return;
  }
  // Remember what we are about to boot so the next boot can tell
  // "update succeeded" from "bootloader rolled us back".
  prefs.putString("pend", latest.version);
  prefs.putUChar("tries", prefs.getUChar("tries", 0) + 1);
  Serial.printf("[ota] rebooting into %s\n", latest.version);
  Serial.flush();
  ESP.restart();
}

// ----------------------------------------------------------------- serial --

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 128) line += c;
      continue;
    }
    line.trim();
    if (line.startsWith("ssid ")) {
      prefs.putString("ssid", line.substring(5));
      Serial.println("[cmd] ssid saved");
    } else if (line.startsWith("pass ")) {
      prefs.putString("pass", line.substring(5));
      Serial.println("[cmd] password saved");
    } else if (line == "reboot") {
      Serial.println("[cmd] rebooting");
      Serial.flush();
      ESP.restart();
    } else if (line == "status") {
      printStatus();
    } else if (line == "forget") {  // allow a blacklisted release to be retried
      prefs.remove("bad");
      prefs.remove("pend");
      prefs.putUChar("tries", 0);
      Serial.println("[cmd] update history cleared");
    } else if (!line.isEmpty()) {
      Serial.println("[cmd] commands: ssid <name> | pass <password> | reboot | status | forget");
    }
    line = "";
  }
}

// ------------------------------------------------------------------- main --

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n\n=== OTA blink firmware v%s (blink %d ms) ===\n", FW_VERSION, BLINK_MS);

  prefs.begin("ota", false);

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  pendingVerify = (state == ESP_OTA_IMG_PENDING_VERIFY);
  Serial.printf("[boot] partition=%s state=%s\n", running->label, otaStateName(state));

  reconcilePendingUpdate();

  if (pendingVerify) {
    // A new image that hangs anywhere below gets reset by the watchdog, and the
    // bootloader then falls back to the previous image.
    esp_task_wdt_init(SELFTEST_WDT_S, true);
    esp_task_wdt_add(NULL);
  }

#ifdef SIMULATE_BAD_FW
  Serial.println("[test] SIMULATED BAD IMAGE: crashing before validation");
  Serial.flush();
  delay(500);
  abort();
#endif

  bool online = connectWifi();
  Release latest;
  bool found = false;
  bool manifestOk = online && fetchLatestRelease(latest, found);

  if (pendingVerify) {
    // Self-test: an image that cannot reach the update server could never be
    // fixed remotely, so that is the bar for keeping it.
    if (!manifestOk) rollbackNow(online ? "update server unreachable" : "no WiFi");
    esp_task_wdt_delete(NULL);
    markValid();
  }

  if (manifestOk && found) checkForUpdate(latest);
  printStatus();
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= BLINK_MS) {
    last = millis();
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  handleSerial();
}
