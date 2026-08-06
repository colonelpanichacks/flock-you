// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>

// flock-you-esp32  —  Passive Flock Safety ALPR detector
// Based on field research by @NitekryDPaul (OUI / addr1),
// Michael / DeFlockJoplin (wildcard-probe + 82:6b:f2),
// Will Greenberg (BLE mfr-ID), GainSec (Raven BLE UUID).
//
// Improvements over v1 (July 2026):
//   • SSID patterns: "Flock Camera net.", "Flock-XXXXXX", "FLOCK-XXXXXX"
//   • Locally-administered MAC + Flock SSID detection (issue #43 camera class)
//   • Sequential-MAC heuristic: :DE/:DF pair on adjacent channels → +bonus
//   • Per-detection confidence score 0–100 stored + emitted in JSON
//   • Optional BLE scan phase (ENABLE_BLE_SCAN=1): mfr-ID, Raven UUID, names
//   • BLE cross-correlation: BLE Flock hit within 60 s of WiFi hit → +20 pts
//   • addr3 CHECK_ADDR3 now ON by default (was opt-in)
//   • 5 GHz note: ESP32/S3 are 2.4 GHz-only hardware; channels 149/157 are
//     defined but guarded — enable only on ESP32-C5 hardware (dual-band).
//   • Protocol field in JSON now reflects actual band ("wifi_2_4ghz" vs future
//     "wifi_5ghz") so the Flask app can filter correctly.
//   • ALERT_LAA_SSID: new alert type for locally-administered MAC w/ Flock SSID
//   • emitDetectionJSON now includes "confidence":%u field

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <SPIFFS.h>
#include "esp_log.h"
#include "fy_detect.h"   // PR#39: detection patterns + pure matching functions

// M5Stack Core2 For AWS has the same 320×240 ILI9342C display and M5Unified
// button/speaker API as the M5Stack Basic. Map USE_M5CORE2_AWS → USE_M5BASIC at
// compile time so all existing display guards work transparently.
#if defined(USE_M5CORE2_AWS) && !defined(USE_M5BASIC)
  #define USE_M5BASIC 1
#endif

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  #include <NimBLEDevice.h>
  #include <NimBLEScan.h>
  #include <NimBLEAdvertisedDevice.h>
#endif

// NeoPixel for M5Atom Lite / Voice
#if defined(USE_M5ATOM_LITE) || defined(USE_M5ATOM_VOICE)
  #include <Adafruit_NeoPixel.h>
#endif

// T-Dongle C5 TFT display + RGB LED
#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
  #include "c5_display.h"
#endif

// M5Stack Basic Core v2.7 / Core2 For AWS — ILI9342C 320×240 IPS display
#if defined(USE_M5BASIC)
  #include "m5basic_display.h"
#endif

// M5StickC Plus SE — ST7789v2 1.14" display (240×135 landscape)
#if defined(USE_M5STICKC_PLUS_SE)
  #include "m5stickc_display.h"
#endif

// M5Atom LED support — GPIO27 SK6812, GPIO39 button
#if defined(USE_M5ATOM_LITE) || defined(USE_M5ATOM_VOICE)
  #define USE_M5ATOM 1
  #define LED_PIN 27
  #define NUM_LEDS 1
  #define BUTTON_PIN 39
  Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
#endif

#if defined(USE_M5ATOM_ECHO)
  #define BUTTON_PIN 39
#endif

#if defined(USE_M5ATOM_VOICES3R) || defined(USE_M5ATOM_VOICE) || defined(USE_M5BASIC)
  #include <M5Unified.h>
#endif
#if defined(USE_M5ATOM_VOICES3R)
  #define BUTTON_PIN 41
#endif

// ============================================================
// CONFIG
// ============================================================

#ifndef TESTING_MODE
  #define TESTING_MODE 0
#endif

#if defined(USE_M5ATOM_ECHO)
  #define BUZZER_PIN 25
  #define USE_BUZZER 1
  #define USE_M5_SPEAKER 0
  #define USE_LED 0
  #define LED_FLASH_MS 0
#elif defined(USE_M5ATOM_LITE)
  #define BUZZER_PIN 25
  #define USE_BUZZER 0
  #define USE_LED 1
  #define USE_LED_MATRIX 1
  #define LED_FLASH_MS 30000  // hold red 30 s after detection
#elif defined(USE_M5ATOM_VOICES3R)
  #define USE_BUZZER 0
  #define USE_M5_SPEAKER 1
  #define USE_LED 0
  #define LED_FLASH_MS 0
#elif defined(USE_M5ATOM_VOICE)
  #define USE_BUZZER 0
  #define USE_M5_SPEAKER 1
  #define USE_LED 1
  #define USE_LED_MATRIX 1
  #define LED_FLASH_MS 30000  // hold red 30 s after detection
#elif defined(USE_LILYGO_T_DONGLE_C5)
  // LILYGO T-Dongle C5 — ESP32-C5 RISC-V, dual-band WiFi 6 + BT5
  // ST7735S TFT (80×160) + WS2812B RGB LED via c5_display.h
  #define USE_BUZZER     0
  #define USE_LED        0
  #define USE_C5_DISPLAY 1
  #define LED_FLASH_MS   30000  // hold red 30 s after detection
#elif defined(USE_M5BASIC)
  // M5Stack Basic Core v2.7 — ILI9342C 320×240 IPS + 1W speaker + 3 buttons
  // Display handled via M5Unified in m5basic_display.h.
  // No NeoPixel — display replaces LED status indication entirely.
  #define USE_BUZZER      0
  #define USE_M5_SPEAKER  1
  #define USE_LED         0
  #define LED_FLASH_MS    0
  // Note: USE_M5CORE2_AWS is aliased to USE_M5BASIC above — no separate block needed.
#elif defined(USE_M5STICKC_PLUS_SE)
  // M5StickC Plus SE — passive buzzer G2 (tone/noTone), no NeoPixel.
  // M5Unified speaker disabled (cfg.internal_spk=false) to prevent GPIO2 conflict.
  // Display via m5stickc_display.h (240×135 landscape ST7789v2).
  #define BUZZER_PIN      2
  #define USE_BUZZER      1
  #define USE_M5_SPEAKER  0
  #define USE_LED         0
  #define LED_FLASH_MS    0
#else
  #define BUZZER_PIN 25
  #define USE_BUZZER 1
  #define LED_PIN 2
  #define USE_LED 1
  #define LED_ACTIVE_HIGH 1
  #define LED_FLASH_MS 30000  // hold red 30 s after detection
#endif

#define MIRROR_SERIAL    1
#define MIRROR_TX_PIN    17
#define MIRROR_BAUD      115200

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
#define CHANNEL_DWELL_MS 350
#define SINGLE_CHANNEL 1

// ── 2.4 GHz channels — always scanned ──────────────────────────────────────
// Flock primaries: 1, 6, 11.  "Flock Camera net." observed on ch.1 (2.4 GHz).
static const uint8_t customChannels[]   = {1, 6, 11};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

static const uint8_t fullHopChannels[]  = {1,2,3,4,5,6,7,8,9,10,11};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

// ── 5 GHz channel note ───────────────────────────────────────────────────────
// Field observation (issue #43): "Flock Camera net." hotspot transmits on
// 5 GHz ch.157 (5785 MHz) simultaneously with 2.4 GHz ch.1.
// TRADE-OFF: The ESP32 (PICO-D4), ESP32-S3, and all M5Atom variants use a
// 2.4 GHz-only radio.  Calling esp_wifi_set_channel(149, …) or (157, …) on
// these parts returns ESP_ERR_INVALID_ARG and does nothing — the chip simply
// cannot tune to 5 GHz.  DO NOT add 149/157 to the hop list on 2.4 GHz-only
// hardware; it wastes dwell time and produces no captures.
//
// To scan 5 GHz you need:
//   a) ESP32-C5 (dual-band, 2.4 + 5 GHz) — compile with -DESP32C5_DUALBAND=1
//      and un-comment the 5 GHz block below, or
//   b) A separate 5 GHz sniffer (e.g. a laptop/RPi with an 802.11ac NIC in
//      monitor mode) feeding detections to the same Flask dashboard.
//
// When using the ESP32-C5, ch.149 + ch.157 should be added to the custom
// channel list.  Each 5 GHz channel also needs a separate band call:
//   esp_wifi_set_channel(157, WIFI_SECOND_CHAN_NONE);   // 5 GHz on C5
// The C5 uses the same esp_wifi_set_channel() API but the driver will accept
// ch > 14 because the hardware supports the 5 GHz OFDM sub-band.
//
// NOTE: The "wifi_2_4ghz" / "wifi_5ghz" distinction in emitted JSON is
// important for the Flask app so detections can be tagged by band.
// channelFreqMhz() below returns the correct MHz for both ranges.
#if defined(ESP32C5_DUALBAND) && ESP32C5_DUALBAND
  static const uint8_t fiveGhzChannels[] = {149, 157};
  static const size_t  fiveGhzChannelCount = 2;
#endif

#define HEARTBEAT_MS    30000
#define RSSI_MIN        -95
#define ALERT_COOLDOWN_MS 5000

#define HB_DEVICE_ACTIVE_MS    120000  // keep beeping for 2 min after last detection
#define HB_BEEP_INTERVAL_MS    10000
#define REDISCOVER_MS          30000
#define NEW_CHIRP_LO_HZ        2000
#define NEW_CHIRP_HI_HZ        2800
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             1500
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

// SSID detection — all patterns known to appear on Flock cameras.
// ENABLE_SSID_MATCH must be 1 for the SSID check in the promiscuous callback
// to run.  It is ON by default now because "Flock Camera net." is our only
// handle for locally-administered-MAC cameras (issue #43).
#define ENABLE_SSID_MATCH 1
#define CHECK_ADDR1 1   // dst/rx — catches Flock STAs receiving probe responses
#define CHECK_ADDR3 1   // bssid fallback for randomised addr2  (was 0)

// Full SSID keyword list.  Lower-case; matched case-insensitively.
// "flock"          → bare deployed cameras, provisioning "Flock-XXXXXX"
// "flock camera"   → issue-43 hotspot ("Flock Camera net.")
// "flocksafety"    → variant brand string sometimes advertised
static const char* target_ssid_keywords[] = {
  "flock",          // matches "Flock", "Flock-XXXXXX", "FLOCK-XXXXXX", "Flock Camera net."
  "flocksafety",
  "penguin",        // internal Flock product codename
  "pigvision"       // PigVision / Raven variant
};
static const size_t SSID_KEYWORD_COUNT = sizeof(target_ssid_keywords) / sizeof(target_ssid_keywords[0]);

// Exact SSID strings for higher-confidence matching — scored separately.
// "Flock Camera net." is the issue-#43 pattern and gets a bigger boost
// because it is highly specific and uses a locally-administered MAC that
// will never match any IEEE OUI.
static const char* ssid_exact_flock_cam_net = "Flock Camera net.";

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define AUTOSAVE_INTERVAL_MS 60000

// ============================================================
// CONFIDENCE SCORE WEIGHTS  (additive, capped at 100)
// ============================================================
//
// These weights were tuned against Michael / DeFlockJoplin's field dataset
// (12 cameras in Joplin, 2 false-positives in wildcard-probe-only mode).
//
// A score ≥ 60 is HIGH CONFIDENCE (almost certainly Flock).
// A score 30–59 is PROBABLE (worth logging, flag for review).
// A score < 30 is LOW CONFIDENCE (background noise probable).

#define CS_OUI_ADDR2            40  // transmitter is in the 31-OUI table
#define CS_OUI_ADDR1            18  // camera is the *receiver* — weaker but real
#define CS_OUI_ADDR3            12  // BSSID fallback when addr2 is randomised
#define CS_WILDCARD_PROBE       22  // empty-SSID probe req from known OUI src
                                    //   (stacks with CS_OUI_ADDR2 → 62)
#define CS_SSID_FLOCK           32  // SSID contains "flock" (any case)
#define CS_SSID_FLOCK_CAM_NET   45  // exact "Flock Camera net." — very specific
#define CS_LAA_MAC              12  // locally-administered MAC + Flock SSID
                                    //   (bit 1 set, can't match any OUI)
#define CS_SEQ_MAC_PAIR         10  // sequential :XX/:YY last-byte pair on this
                                    //   camera cluster (e.g. :DE/:DF on ch.1/157)
#define CS_BLE_CORR             20  // BLE Flock signal seen within
                                    //   BLE_CORR_WINDOW_MS of this WiFi hit
#define CS_STRONG_RSSI           5  // RSSI > -70 dBm (camera physically close)
// PR#39: split-confidence OUIs
#define CS_OUI_MFR              20  // contract-mfr OUI (Liteon/USI) — shared
                                    //   hardware; don't chirp alone
#define CS_SOUNDTHINKING        35  // SoundThinking/ShotSpotter OUI — separate
                                    //   device class, still warrants alert

// Minimum confidence for a detection to trigger the chirp/beep + LED flash.
// Detections below this threshold are still logged and emitted in JSON.
// OUI_MFR alone scores 20 (< 30) → silent log only.
// OUI_MFR + BLE corr = 40 (>= 30) → chirps.
#define CHIRP_MIN_CONFIDENCE    30

// BLE cross-correlation window — if a BLE Flock hit occurred within this
// many ms before the WiFi hit, add CS_BLE_CORR to the confidence score.
#define BLE_CORR_WINDOW_MS      60000UL

// ============================================================
// OUI BYTE TABLES  (compiled from fy_detect.h string arrays at boot)
// ============================================================
// Pattern data lives in fy_detect.h (shared with native unit tests).
// Three tables correspond to three confidence tiers (PR#39):
//   HIGH  (FY_OUI_HIGH_COUNT) — direct Flock / exclusively observed
//   MFR   (FY_OUI_MFR_COUNT)  — Liteon/USI contract mfr (shared hardware)
//   ST    (FY_OUI_ST_COUNT)   — SoundThinking / ShotSpotter sensors

static uint8_t oui_high_bytes[FY_OUI_HIGH_COUNT][3];
static uint8_t oui_mfr_bytes[FY_OUI_MFR_COUNT][3];
static uint8_t oui_st_bytes[FY_OUI_ST_COUNT][3];

// Backward-compat count for heartbeat log (high + mfr combined)
#define OUI_COUNT (FY_OUI_HIGH_COUNT + FY_OUI_MFR_COUNT)

// ============================================================
// SEQUENTIAL MAC TRACKING  (locally-administered pairs)
// ============================================================
//
// Issue-#43 "Flock Camera net." cameras use locally-administered MACs with
// sequential last bytes for their dual-band radios, e.g.:
//   XX:XX:XX:9F:A2:DE  (2.4 GHz radio)
//   XX:XX:XX:9F:A2:DF  (5 GHz radio — xx:xx differs by exactly +1)
//
// We track pairs keyed by the first 5 bytes of the MAC.  When the same
// 5-byte prefix appears on two different channels with last bytes that
// differ by exactly 1, we fire the CS_SEQ_MAC_PAIR bonus.
//
// This only applies to locally-administered MACs (bit 1 of byte 0 set),
// because globally-administered MACs are handled by OUI matching.

#define SEQ_MAC_TABLE_SIZE 32

typedef struct {
  uint8_t  prefix[5];   // first 5 bytes (bytes 0–4)
  uint8_t  lastByte;    // 6th byte we observed
  uint8_t  channel;     // channel it was seen on
  uint32_t ts;          // millis() of last observation
} SeqMacEntry;

static SeqMacEntry seqMacTable[SEQ_MAC_TABLE_SIZE];
static size_t      seqMacCount = 0;
#define SEQ_MAC_EXPIRE_MS 30000UL  // forget older than 30 s

// Returns true + sets *pairChannel if a sequential-last-byte pair is found.
// Inserts/updates the entry regardless.
static bool IRAM_ATTR checkSeqMac(const uint8_t* mac, uint8_t ch,
                                   uint8_t* pairChannel) {
  // Only for locally-administered MACs
  if (!(mac[0] & 0x02)) return false;

  uint32_t now = millis();

  // Expire stale entries
  size_t w = 0;
  for (size_t i = 0; i < seqMacCount; i++) {
    if ((now - seqMacTable[i].ts) < SEQ_MAC_EXPIRE_MS)
      seqMacTable[w++] = seqMacTable[i];
  }
  seqMacCount = w;

  // Check for existing entry with same 5-byte prefix
  for (size_t i = 0; i < seqMacCount; i++) {
    if (memcmp(seqMacTable[i].prefix, mac, 5) == 0) {
      uint8_t diff = (mac[5] > seqMacTable[i].lastByte)
                       ? (mac[5] - seqMacTable[i].lastByte)
                       : (seqMacTable[i].lastByte - mac[5]);
      if (diff == 1 && seqMacTable[i].channel != ch) {
        if (pairChannel) *pairChannel = seqMacTable[i].channel;
        seqMacTable[i].lastByte = mac[5];
        seqMacTable[i].channel  = ch;
        seqMacTable[i].ts       = now;
        return true;
      }
      // Same prefix but not sequential — update with latest
      seqMacTable[i].lastByte = mac[5];
      seqMacTable[i].channel  = ch;
      seqMacTable[i].ts       = now;
      return false;
    }
  }

  // Insert new entry (evict oldest if full)
  if (seqMacCount >= SEQ_MAC_TABLE_SIZE) {
    // Find and overwrite oldest
    size_t oldest = 0;
    for (size_t i = 1; i < seqMacCount; i++)
      if (seqMacTable[i].ts < seqMacTable[oldest].ts) oldest = i;
    w = oldest;
  } else {
    w = seqMacCount++;
  }
  memcpy(seqMacTable[w].prefix, mac, 5);
  seqMacTable[w].lastByte = mac[5];
  seqMacTable[w].channel  = ch;
  seqMacTable[w].ts       = now;
  return false;
}

// ============================================================
// BLE CROSS-CORRELATION STATE
// ============================================================
//
// When ENABLE_BLE_SCAN=1, a periodic BLE scan runs.  It looks for:
//   1. Manufacturer-specific data with Flock's BLE mfr-ID (Will Greenberg)
//   2. Raven/Flock service UUID 0x1B7E (GainSec), 0xFD60 (Raven telemetry)
//   3. Device names: "Flock", "Penguin", "Pigvision", "FS Ext Battery",
//                    "Raven", "raven"
//
// When a BLE match is found, g_bleFlockLastSeen is set to millis().
// The WiFi callback checks this timestamp; if within BLE_CORR_WINDOW_MS,
// it adds CS_BLE_CORR to the confidence score for that detection event.
//
// BLE and WiFi share the 2.4 GHz radio on ESP32.  Time-multiplexing strategy:
//   - Every BLE_SCAN_INTERVAL_MS, pause promiscuous mode for BLE_SCAN_DWELL_MS
//   - Run NimBLE passive scan during the pause
//   - Resume promiscuous mode immediately after
// Typical loss: 5 s out of every 60 s = ~8% of WiFi capture time — acceptable.

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN

#define BLE_SCAN_INTERVAL_MS   60000UL  // how often to run a BLE scan
#define BLE_SCAN_DWELL_MS       5000UL  // how long the BLE scan runs
// PR#39 correction: 0x09C8 is the XUNTONG BT company ID per wgreenberg/flock-you.
// Pre-PR#39 firmware used 0x05A7 (incorrect — that ID belongs to Assa Abloy).
#define BLE_FLOCK_MFR_ID       0x09C8   // XUNTONG Technology Co., Ltd

// Raven UUIDs are checked via fyCheckRavenUUIDFromStrings() from fy_detect.h
// using full 128-bit UUID strings.  The old short-form defines are gone.

static const char* ble_flock_names[] = {
  "flock", "penguin", "pigvision", "fs ext battery",
  "raven",  // Raven variant
  nullptr
};

static volatile uint32_t g_bleFlockLastSeen = 0;  // millis() of last BLE Flock hit
static volatile int8_t   g_bleFlockRssi     = -127;
static unsigned long     g_bleNextScan      = 0;
static NimBLEScan*       g_pBLEScan         = nullptr;

// Case-insensitive substring search (BLE name is typically short)
static bool bleNameContains(const char* name, const char* needle) {
  if (!name || !needle || !*name || !*needle) return false;
  // lowercase copy of name
  char low[64]; size_t i = 0;
  while (i < 63 && name[i]) { low[i] = (char)tolower((unsigned char)name[i]); i++; }
  low[i] = '\0';
  return strstr(low, needle) != nullptr;
}

class FlockBLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* adv) override {
    if (!adv) return;
    int8_t rssi = (int8_t)adv->getRSSI();
    bool matched = false;

    // 1. Manufacturer ID check (Flock Safety BLE mfr-ID per Will Greenberg)
    {
      std::string mfr = adv->getManufacturerData();
      if (mfr.size() >= 2) {
        const uint8_t* m = (const uint8_t*)mfr.data();
        // BLE mfr data is LE: low byte first
        uint16_t mfrId = (uint16_t)m[0] | ((uint16_t)m[1] << 8);
        if (mfrId == BLE_FLOCK_MFR_ID) matched = true;
      }
    }

    // 2. Raven / Flock BLE service UUID check — full 128-bit UUIDs (GainSec/PR#39).
    // Iterates the device's advertised service list and delegates to fy_detect.h's
    // hardware-independent fyCheckRavenUUIDFromStrings() for the actual comparison.
    if (!matched && adv->haveServiceUUID()) {
      int nsvc = adv->getServiceUUIDCount();
      const char* strs[16];
      std::string bufs[16];
      int n = (nsvc < 16) ? nsvc : 16;
      for (int si = 0; si < n; si++) {
        bufs[si] = adv->getServiceUUID(si).toString();
        strs[si] = bufs[si].c_str();
      }
      char matchedUUID[41] = {0};
      if (fyCheckRavenUUIDFromStrings(strs, n, matchedUUID)) {
        matched = true;
        // Log which UUID matched (matchedUUID is populated by the helper)
        (void)matchedUUID;
      }
    }

    // 3. Device name match
    if (!matched) {
      std::string name = adv->getName();
      if (!name.empty()) {
        for (const char** kw = ble_flock_names; *kw; kw++) {
          if (bleNameContains(name.c_str(), *kw)) { matched = true; break; }
        }
      }
    }

    if (matched) {
      g_bleFlockLastSeen = (uint32_t)millis();
      g_bleFlockRssi     = rssi;
      // Log immediately from BLE task — Serial is safe here because we're not
      // in the WiFi promiscuous callback (different task context).
      Serial.printf("[flockyou] BLE-Flock rssi=%d addr=%s\n",
                    (int)rssi, adv->getAddress().toString().c_str());
    }
  }
};

static FlockBLECallbacks g_bleCallbacks;

static void bleScanStart() {
  if (!g_pBLEScan) return;
  if (g_pBLEScan->isScanning()) return;
  g_pBLEScan->clearResults();
  // Passive (false = no scan-request packets — avoids alerting the camera)
  g_pBLEScan->start((uint32_t)(BLE_SCAN_DWELL_MS / 1000), false);
}

static void bleScanStop() {
  if (!g_pBLEScan) return;
  if (g_pBLEScan->isScanning()) g_pBLEScan->stop();
}

static void initBLE() {
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  g_pBLEScan = NimBLEDevice::getScan();
  g_pBLEScan->setAdvertisedDeviceCallbacks(&g_bleCallbacks, false);
  g_pBLEScan->setActiveScan(false);
  g_pBLEScan->setInterval(100);
  g_pBLEScan->setWindow(99);
}

// ── BLE_COEX_MODE=1: true simultaneous WiFi + BLE ────────────────────────────
// When BLE_COEX_MODE is set, the ESP-IDF software coexistence scheduler
// (CONFIG_SW_COEXIST_ENABLE, enabled by default in arduino-esp32) automatically
// time-slices the shared 2.4 GHz radio between WiFi promiscuous mode and BLE
// without any application-level pausing.  Both stacks run continuously.
//
// Trade-off: ~10–20% of WiFi frames are missed during BLE TX/RX scheduler
// windows, but the radio NEVER goes fully dark.  This is strictly better than
// the manual 5 s pause every 60 s strategy for real-time walking detections.
//
// Implementation: start a continuous NimBLE scan (duration = 0) once after
// WiFi init completes.  If the scan stops for any reason (BLE stack reset,
// coexistence forced-stop) bleScanTick() restarts it.
//
// BLE_COEX_MODE=0 (default): original manual time-multiplexing — WiFi
// promiscuous is paused for BLE_SCAN_DWELL_MS every BLE_SCAN_INTERVAL_MS.
// Use this on boards where you have confirmed the auto-coexistence causes
// excessive WiFi frame loss.

#if defined(BLE_COEX_MODE) && BLE_COEX_MODE

// Single-shot: called from setup() after esp_wifi_set_promiscuous(true)
static void bleCoexStart() {
  if (!g_pBLEScan) return;
  if (g_pBLEScan->isScanning()) return;
  g_pBLEScan->clearResults();
  // duration = 0 → scan runs indefinitely until stop() is called
  g_pBLEScan->start(0, false);
  Serial.println("[flockyou] BLE coex-scan started (continuous, promisc always ON)");
}

// Called from loop() — just keeps the continuous scan alive.
// No promiscuous pause/resume needed; the coexistence module handles it.
static void bleScanTick(bool& /*promiscPaused*/) {
  if (!g_pBLEScan) return;
  if (!g_pBLEScan->isScanning()) {
    // Scan stopped unexpectedly (BLE stack reset, etc.) — restart it
    g_pBLEScan->clearResults();
    g_pBLEScan->start(0, false);
    Serial.println("[flockyou] BLE coex-scan restarted");
  }
}

#else  // BLE_COEX_MODE == 0 — original manual pause/resume time-multiplexing

// Called from loop() — handles BLE scan scheduling around WiFi promiscuous mode.
// The WiFi promiscuous mode must be paused before BLE can use the shared radio.
static void bleScanTick(bool& promiscPaused) {
  unsigned long now = millis();
  if (now < g_bleNextScan) return;

  if (!promiscPaused) {
    // Pause promiscuous mode for the BLE scan window
    esp_wifi_set_promiscuous(false);
    promiscPaused = true;
    bleScanStart();
    Serial.println("[flockyou] BLE scan start (promisc paused)");
  }

  // Wait until BLE scan completes, then resume
  if (g_pBLEScan && !g_pBLEScan->isScanning()) {
    esp_wifi_set_promiscuous(true);
    promiscPaused = false;
    g_bleNextScan = now + BLE_SCAN_INTERVAL_MS;
    Serial.println("[flockyou] BLE scan done (promisc resumed)");
  }
}

#endif  // BLE_COEX_MODE

#endif  // ENABLE_BLE_SCAN

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 32

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,
  ALERT_WILDCARD_PROBE  = 4,
  // Locally-administered MAC + Flock SSID (issue-#43 "Flock Camera net." class).
  // These cameras will never match any OUI — SSID is the only WiFi handle.
  ALERT_LAA_SSID        = 5,
  // PR#39: contract-manufacturer OUI (Liteon/USI) — lower confidence, no chirp alone.
  // Score CS_OUI_MFR=20 < CHIRP_MIN_CONFIDENCE=30 → logged but silent.
  ALERT_OUI_MFR         = 6,
  // PR#39: SoundThinking/ShotSpotter acoustic sensor co-deployed with Flock cameras.
  // Score CS_SOUNDTHINKING=35 ≥ CHIRP_MIN_CONFIDENCE → audible alert, "soundthinking" method.
  ALERT_SOUNDTHINKING   = 7,
} AlertType;

typedef struct {
  AlertType type;
  uint8_t   mac[6];
  int8_t    rssi;
  uint8_t   channel;
  char      ssid[33];
  char      frameKind[12];
  uint8_t   confidence;   // 0–100 computed in callback, emitted in JSON
} AlertEntry;

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;
static volatile size_t alertTail = 0;
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac, int8_t rssi,
                                    uint8_t ch, const char* ssid, const char* kind,
                                    uint8_t confidence) {
  portENTER_CRITICAL_ISR(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) { portEXIT_CRITICAL_ISR(&queueMux); return; }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type       = type;
  e->rssi       = rssi;
  e->channel    = ch;
  e->confidence = confidence;
  memcpy((void*)e->mac, mac, 6);

  if (ssid) { strncpy((char*)e->ssid,      ssid, 32); ((char*)e->ssid)[32] = '\0'; }
  else       { ((char*)e->ssid)[0] = '\0'; }

  if (kind) { strncpy((char*)e->frameKind, kind, 11); ((char*)e->frameKind)[11] = '\0'; }
  else       { ((char*)e->frameKind)[0] = '\0'; }

  alertHead = next;
  portEXIT_CRITICAL_ISR(&queueMux);
}

// ============================================================
// DETECTION TABLE  (on-device storage, persisted to SPIFFS)
// ============================================================

typedef struct {
  char     mac[18];
  char     method[16];
  int8_t   rssi;
  uint8_t  channel;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint16_t count;
  char     ssid[33];
  uint8_t  maxConfidence;   // highest confidence score seen for this MAC
} FYDetection;

static FYDetection fyDet[MAX_DETECTIONS];
static int           fyDetCount       = 0;
static bool          fySpiffsReady    = false;
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;

// ============================================================
// STATE
// ============================================================

static uint8_t  currentChannel = 1;
static size_t   customChannelIndex = 0;
static size_t   fullHopIndex = 0;
static unsigned long lastHop = 0;
static unsigned long lastHeartbeat = 0;
static volatile bool sniffingStopped = false;

#define DEDUPE_SLOTS 8
static struct {
  char mac[18];
  unsigned long ts;
} dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

static volatile unsigned long ledOffAt = 0;

static unsigned long fyLastTargetSeen  = 0;
static unsigned long fyLastHeartbeatAt = 0;

// Tracks whether promiscuous mode is currently paused for BLE
static bool fyPromiscPaused = false;

// ============================================================
// 802.11 HEADER
// ============================================================

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ============================================================
// HELPERS
// ============================================================

static char _dualBuf[384];

static void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dualPrintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(_dualBuf, sizeof(_dualBuf), fmt, args);
  va_end(args);
  if (n > 0) {
    Serial.write(_dualBuf, n);
#if MIRROR_SERIAL && !defined(USE_M5ATOM_VOICES3R)
    Serial1.write(_dualBuf, n);
#endif
  }
}

static void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL && !defined(USE_M5ATOM_VOICES3R)
  Serial1.println(str);
#endif
}

static inline void ledSet(bool on) {
#if USE_LED
#if defined(USE_LED_MATRIX)
  if (on) { strip.setPixelColor(0, strip.Color(180, 0, 0)); }
  else     { strip.setPixelColor(0, strip.Color(0, 60, 0)); }
  strip.show();
#elif LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW  : HIGH);
#endif
#endif
}

static void ledFlash(unsigned ms) {
#if USE_LED
  ledSet(true);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;
#endif
}

static void ledTick() {
#if USE_LED
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(false);
    ledOffAt = 0;
  }
#endif
}

static void buzzerBeep(unsigned int ms) {
#if USE_BUZZER
  digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
#endif
}

static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#elif defined(USE_M5_SPEAKER) && USE_M5_SPEAKER
  M5.Speaker.tone(NEW_CHIRP_LO_HZ, NEW_CHIRP_NOTE_MS);
  delay(NEW_CHIRP_NOTE_MS + NEW_CHIRP_GAP_MS);
  M5.Speaker.tone(NEW_CHIRP_HI_HZ, NEW_CHIRP_NOTE_MS);
  delay(NEW_CHIRP_NOTE_MS);
  M5.Speaker.stop();
#endif
}

static void heartbeatBeep() {
#if USE_BUZZER
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
  delay(HB_BEEP_GAP_MS);
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
#elif defined(USE_M5_SPEAKER) && USE_M5_SPEAKER
  M5.Speaker.tone(HB_BEEP_HZ, HB_BEEP_NOTE_MS);
  delay(HB_BEEP_NOTE_MS + HB_BEEP_GAP_MS);
  M5.Speaker.tone(HB_BEEP_HZ, HB_BEEP_NOTE_MS);
  delay(HB_BEEP_NOTE_MS);
  M5.Speaker.stop();
#endif
}

static void startupBeep() {
#if USE_BUZZER
  static const uint16_t notes[6] = { 523, 262, 440, 220, 415, 208 };
  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#elif defined(USE_M5_SPEAKER) && USE_M5_SPEAKER
  static const uint16_t notes[] = { 659, 659, 659, 523, 659, 784, 392 };
  static const uint16_t durs[]  = { 100, 100, 100, 100, 100, 300, 300 };
  static const uint8_t  gaps[]  = {  80,  80,  80,   0,   0,  80,   0 };
  for (size_t i = 0; i < sizeof(notes)/sizeof(notes[0]); i++) {
    M5.Speaker.tone(notes[i], durs[i]);
    delay(durs[i]);
    if (gaps[i]) { M5.Speaker.stop(); delay(gaps[i]); }
  }
  M5.Speaker.stop();
#endif
}

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

static void precompileOuis() {
  // Populate high-confidence Flock OUI byte table from fy_detect.h strings
  for (size_t i = 0; i < FY_OUI_HIGH_COUNT; i++) {
    const char* o  = fy_oui_high[i];
    oui_high_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_high_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_high_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
  // Populate contract-manufacturer OUI byte table
  for (size_t i = 0; i < FY_OUI_MFR_COUNT; i++) {
    const char* o  = fy_oui_mfr[i];
    oui_mfr_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_mfr_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_mfr_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
  // Populate SoundThinking OUI byte table
  for (size_t i = 0; i < FY_OUI_ST_COUNT; i++) {
    const char* o  = fy_oui_soundthinking[i];
    oui_st_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_st_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_st_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
}

static inline bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

// High-confidence Flock Safety OUIs (direct registration / exclusively Flock).
// NOTE: 82:6b:f2 (DeFlockJoplin, 12th camera) has the locally-administered bit
// set (0x82 & 0x02 = 2), but it IS a confirmed Flock OUI — do NOT filter it here.
// The LAA-bit check belongs in the SSID path (ALERT_LAA_SSID), not here.
static bool IRAM_ATTR matchFlockHighOui(const uint8_t* mac) {
  for (size_t i = 0; i < FY_OUI_HIGH_COUNT; i++) {
    if (mac[0] == oui_high_bytes[i][0] &&
        mac[1] == oui_high_bytes[i][1] &&
        mac[2] == oui_high_bytes[i][2]) return true;
  }
  return false;
}

// Contract-manufacturer OUIs (Liteon/USI) — Flock hardware but also other products.
static bool IRAM_ATTR matchFlockMfrOui(const uint8_t* mac) {
  if (mac[0] & 0x02) return false;
  for (size_t i = 0; i < FY_OUI_MFR_COUNT; i++) {
    if (mac[0] == oui_mfr_bytes[i][0] &&
        mac[1] == oui_mfr_bytes[i][1] &&
        mac[2] == oui_mfr_bytes[i][2]) return true;
  }
  return false;
}

// SoundThinking / ShotSpotter acoustic gunshot detection sensors.
static bool IRAM_ATTR matchSoundThinkingOui(const uint8_t* mac) {
  if (mac[0] & 0x02) return false;
  for (size_t i = 0; i < FY_OUI_ST_COUNT; i++) {
    if (mac[0] == oui_st_bytes[i][0] &&
        mac[1] == oui_st_bytes[i][1] &&
        mac[2] == oui_st_bytes[i][2]) return true;
  }
  return false;
}

// Backward-compat wrapper: covers high + mfr — used by addr1 / addr3 checks
// where distinguishing between tables is not needed.
static bool IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  if (mac[0] & 0x02) return false;
  return matchFlockHighOui(mac) || matchFlockMfrOui(mac);
}

static char* strcasestr_local(const char* haystack, const char* needle) {
  if (!*needle) return (char*)haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack; const char* n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
    if (!*n) return (char*)haystack;
  }
  return nullptr;
}

static bool matchSsidKeyword(const char* ssid) {
  for (size_t i = 0; i < SSID_KEYWORD_COUNT; i++)
    if (strcasestr_local(ssid, target_ssid_keywords[i])) return true;
  return false;
}

// Returns true if ssid is the exact "Flock Camera net." string (case-sensitive
// match because the real SSID is consistent per field reports).
static bool IRAM_ATTR isFcnSsid(const char* ssid) {
  return ssid && (strcmp(ssid, ssid_exact_flock_cam_net) == 0);
}

static const char* channelModeName() {
  switch (CHANNEL_MODE) {
    case CHANNEL_MODE_FULL_HOP: return "FULL_HOP";
    case CHANNEL_MODE_CUSTOM:   return "CUSTOM";
    case CHANNEL_MODE_SINGLE:   return "SINGLE";
    default:                    return "UNKNOWN";
  }
}

// Returns frequency in MHz for any channel:
//   2.4 GHz: ch 1–14 → 2407 + 5*ch  (exactly per 802.11 spec)
//   5 GHz:   ch 36–165 → 5000 + 5*ch (UNII-3: ch 149=5745, 157=5785)
static inline uint16_t channelFreqMhz(uint8_t ch) {
  if (ch >= 1  && ch <= 14)  return (uint16_t)(2407 + 5 * ch);
  if (ch >= 36 && ch <= 165) return (uint16_t)(5000 + 5 * ch);
  return 0;
}

static const char* channelBand(uint8_t ch) {
  if (ch >= 1  && ch <= 14)  return "wifi_2_4ghz";
  if (ch >= 36 && ch <= 165) return "wifi_5ghz";
  return "wifi_unknown";
}

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  strlcpy(dedupeTable[dedupeIdx].mac, macStr, 18);
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

static void stopSniffing(const char* reason) {
  if (sniffingStopped) return;
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  dualPrintf("[flockyou] sniffing stopped: %s\n", reason);
}

static void applyInitialChannel() {
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  currentChannel = SINGLE_CHANNEL;
#elif CHANNEL_MODE == CHANNEL_MODE_CUSTOM
  currentChannel = customChannels[0];
#else
  currentChannel = fullHopChannels[0];
#endif
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
}

static void updateChannelMode() {
  if (sniffingStopped || fyPromiscPaused) return;
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  if (currentChannel != SINGLE_CHANNEL) {
    currentChannel = SINGLE_CHANNEL;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  }
  return;
#else
  if (millis() - lastHop < CHANNEL_DWELL_MS) return;
  #if CHANNEL_MODE == CHANNEL_MODE_CUSTOM
    customChannelIndex = (customChannelIndex + 1) % customChannelCount;
    currentChannel = customChannels[customChannelIndex];
  #else
    fullHopIndex = (fullHopIndex + 1) % fullHopChannelCount;
    currentChannel = fullHopChannels[fullHopIndex];
  #endif
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
#endif
}

static void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    dualPrintf("[flockyou] scanning (ch=%u mode=%s det=%d)\n",
                  currentChannel, channelModeName(), fyDetCount);
    lastHeartbeat = millis();
#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
    c5DisplayScanning(currentChannel, fyDetCount);
#endif
#if defined(USE_M5BASIC)
    m5basicScanning(currentChannel, channelModeName(), fyDetCount,
                    millis(), fySpiffsReady,
                    (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
#endif
#if defined(USE_M5STICKC_PLUS_SE)
    m5stickcScanning(currentChannel, channelModeName(), fyDetCount,
                     millis(), fySpiffsReady,
                     (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
#endif
  }
}

// ============================================================
// CONFIDENCE SCORE COMPUTATION  (called from promiscuous callback)
// ============================================================
//
// Called from the WiFi ISR context — must be IRAM-resident and fast.
// Returns a value 0–100.  The BLE correlation check reads
// g_bleFlockLastSeen which is written by a different task, but since it's
// a single uint32_t write it's effectively atomic on Xtensa.

static uint8_t IRAM_ATTR computeConfidence(AlertType type, const uint8_t* mac,
                                            int8_t rssi, const char* ssid) {
  int score = 0;

  switch (type) {
    case ALERT_OUI_ADDR2:
      score += CS_OUI_ADDR2;
      break;
    case ALERT_WILDCARD_PROBE:
      // OUI already verified by caller before emitting this type
      score += CS_OUI_ADDR2 + CS_WILDCARD_PROBE;
      break;
    case ALERT_OUI_ADDR1:
      score += CS_OUI_ADDR1;
      break;
    case ALERT_OUI_ADDR3:
      score += CS_OUI_ADDR3;
      break;
    case ALERT_SSID:
      // SSID hit from a globally-administered MAC — may also have OUI match
      if (matchOuiRaw(mac)) score += CS_OUI_ADDR2;
      if (ssid && isFcnSsid(ssid)) score += CS_SSID_FLOCK_CAM_NET;
      else                          score += CS_SSID_FLOCK;
      break;
    case ALERT_LAA_SSID:
      // Locally-administered MAC — can't match OUI, SSID is sole WiFi handle
      if (ssid && isFcnSsid(ssid)) score += CS_SSID_FLOCK_CAM_NET;
      else                          score += CS_SSID_FLOCK;
      score += CS_LAA_MAC;
      break;
    case ALERT_OUI_MFR:
      // Contract-manufacturer OUI (Liteon/USI) — shared hardware, lower confidence
      score += CS_OUI_MFR;
      break;
    case ALERT_SOUNDTHINKING:
      // SoundThinking/ShotSpotter acoustic sensor
      score += CS_SOUNDTHINKING;
      break;
    default:
      break;
  }

  // Strong RSSI bonus
  if (rssi > -70) score += CS_STRONG_RSSI;

  // BLE cross-correlation bonus
#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  {
    uint32_t now = (uint32_t)millis();
    uint32_t bts = g_bleFlockLastSeen;  // volatile read — atomic uint32
    if (bts != 0 && (now - bts) < (uint32_t)BLE_CORR_WINDOW_MS)
      score += CS_BLE_CORR;
  }
#endif

  // Clamp to 0–100
  if (score > 100) score = 100;
  if (score < 0)   score = 0;
  return (uint8_t)score;
}

// Sequential-MAC-pair bonus: add to an existing queued alert's confidence
// or include in a new alert.  Called from the WiFi callback after checkSeqMac().
// NOTE: Since we can't easily retroactively update an already-enqueued item,
// we instead compute this before enqueueing (see wifiSniffer below).
static uint8_t IRAM_ATTR applySeqMacBonus(uint8_t base) {
  int s = (int)base + CS_SEQ_MAC_PAIR;
  if (s > 100) s = 100;
  return (uint8_t)s;
}

// ============================================================
// DETECTION TABLE OPS
// ============================================================

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:           return "ssid";
    case ALERT_WILDCARD_PROBE: return "wildcard_probe";
    case ALERT_LAA_SSID:       return "laa_ssid";
    case ALERT_OUI_MFR:        return "oui_mfr";       // PR#39 contract-mfr
    case ALERT_SOUNDTHINKING:  return "soundthinking";  // PR#39 SoundThinking
    default:                   return "unknown";
  }
}

static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch, const char* ssid,
                          uint8_t confidence, bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      if (confidence > fyDet[i].maxConfidence)
        fyDet[i].maxConfidence = confidence;
      if (ssid && ssid[0] && !fyDet[i].ssid[0])
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    if (outChirpWorthy) *outChirpWorthy = false;
    return -1;
  }
  FYDetection& d = fyDet[fyDetCount];
  strlcpy(d.mac,    mac,                  sizeof(d.mac));
  strlcpy(d.method, method ? method : "", sizeof(d.method));
  d.rssi          = rssi;
  d.channel       = ch;
  d.firstSeen     = now;
  d.lastSeen      = now;
  d.count         = 1;
  d.maxConfidence = confidence;
  if (ssid && ssid[0]) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else                 d.ssid[0] = '\0';
  fyDetCount++;
  fyDirty = true;
  if (outChirpWorthy) *outChirpWorthy = true;
  return fyDetCount - 1;
}

// ============================================================
// JSON ESCAPE
// ============================================================

static size_t jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) break;
      dst[o++] = '\\'; dst[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      if (o + 6 >= cap) break;
      int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
      if (n <= 0 || (size_t)n >= cap - o) break;
      o += (size_t)n;
    } else {
      if (o + 1 >= cap) break;
      dst[o++] = c;
    }
  }
  dst[o] = '\0';
  return o;
}

// ============================================================
// CRC32
// ============================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

// ============================================================
// SPIFFS SESSION PERSISTENCE
// ============================================================

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"method\":\"%s\",\"rssi\":%d,\"channel\":%u,"
      "\"first\":%lu,\"last\":%lu,\"count\":%u,\"ssid\":\"%s\","
      "\"confidence\":%u}",
      d.mac, d.method, d.rssi, (unsigned)d.channel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen,
      (unsigned)d.count, ssidEsc, (unsigned)d.maxConfidence);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static uint32_t fyComputePayloadCRC(size_t& outBytes) {
  char line[384];
  uint32_t crc = 0;
  outBytes = 0;
  crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    crc = fyCRC32Update(crc, (const uint8_t*)line, n);
    outBytes += n;
  }
  crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
  return crc;
}

static bool fyParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
  const char* b = strstr(hdr, "\"bytes\":");
  const char* c = strstr(hdr, "\"crc\":\"0x");
  if (!b || !c) return false;
  b += 8;
  long long bv = 0;
  if (sscanf(b, "%lld", &bv) != 1 || bv < 0) return false;
  c += 9;
  unsigned cv = 0;
  if (sscanf(c, "%x", &cv) != 1) return false;
  outBytes = (size_t)bv;
  outCrc   = (uint32_t)cv;
  return true;
}

static bool fyValidateSessionFile(const char* path) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  String hdr = f.readStringUntil('\n');
  if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }
  size_t   expectedBytes = 0;
  uint32_t expectedCRC   = 0;
  if (!fyParseEnvelope(hdr.c_str(), expectedBytes, expectedCRC)) {
    f.close(); return false;
  }
  size_t bodyOffset = hdr.length() + 1;
  size_t fileSize   = f.size();
  if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
  if ((fileSize - bodyOffset) != expectedBytes) { f.close(); return false; }
  uint8_t buf[256];
  uint32_t crc = 0;
  size_t remaining = expectedBytes;
  while (remaining > 0) {
    int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) break;
    crc = fyCRC32Update(crc, buf, (size_t)n);
    remaining -= (size_t)n;
  }
  f.close();
  return (remaining == 0 && crc == expectedCRC);
}

static bool fySpiffsCopy(const char* src, const char* dst) {
  File s = SPIFFS.open(src, "r");
  if (!s) return false;
  File d = SPIFFS.open(dst, "w");
  if (!d) { s.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  s.close();
  d.close();
  return ok;
}

static bool fyAtomicPromote(const char* src, const char* dst) {
  if (SPIFFS.rename(src, dst)) return true;
  if (!fySpiffsCopy(src, dst)) return false;
  SPIFFS.remove(src);
  return true;
}

static void fySaveSession() {
  if (!fySpiffsReady) return;
  if (!fyDirty && fyDetCount == fyLastSaveCount) return;
  size_t   payloadBytes = 0;
  uint32_t crc          = fyComputePayloadCRC(payloadBytes);
  int      savedCount   = fyDetCount;
  File f = SPIFFS.open(FY_SESSION_TMP, "w");
  if (!f) {
    dualPrintf("[flockyou] save failed: cannot open %s\n", FY_SESSION_TMP);
    return;
  }
  f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);
  char line[384];
  size_t wrote = 0;
  f.write((uint8_t*)"[", 1); wrote++;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    f.write((uint8_t*)line, n);
    wrote += n;
  }
  f.write((uint8_t*)"]", 1); wrote++;
  f.close();
  if (wrote != payloadBytes) {
    dualPrintf("[flockyou] save WARNING: wrote %u expected %u — aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }
  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintf("[flockyou] save verify FAILED — old session preserved\n");
    return;
  }
  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[flockyou] promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
    return;
  }
  fyLastSaveAt    = millis();
  fyLastSaveCount = savedCount;
  fyDirty         = false;
  dualPrintf("[flockyou] session saved: %d det, %u bytes, crc=0x%08lX\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

static void fyPromotePrevSession() {
  if (!fySpiffsReady) return;
  const char* source = nullptr;
  if      (fyValidateSessionFile(FY_SESSION_FILE)) source = FY_SESSION_FILE;
  else if (fyValidateSessionFile(FY_SESSION_TMP))  source = FY_SESSION_TMP;
  if (!source) {
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
    dualPrintln("[flockyou] no valid prior session to promote");
    return;
  }
  if (!fySpiffsCopy(source, FY_PREV_FILE)) {
    dualPrintf("[flockyou] failed to promote %s → %s\n", source, FY_PREV_FILE);
    return;
  }
  if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
  if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
  File v = SPIFFS.open(FY_PREV_FILE, "r");
  size_t sz = v ? v.size() : 0;
  if (v) v.close();
  dualPrintf("[flockyou] prior session promoted from %s (%u bytes)\n",
             source, (unsigned)sz);
}

// ============================================================
// FLASK-COMPATIBLE JSON EMISSION
// ============================================================
// Now includes "confidence":%u and "protocol" is band-aware.

static void emitDetectionJSON(const char* mac, const char* method,
                               int8_t rssi, uint8_t ch, const char* ssid,
                               uint8_t confidence) {
  char ssidEsc[sizeof(((FYDetection*)0)->ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
  char oui[9];
  uint8_t mbytes[6] = {0};
  sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
  ouiFromMac(mbytes, oui, sizeof(oui));

  // Locally-administered MAC: OUI field is meaningless, tag it clearly
  const char* ouiStr = (mbytes[0] & 0x02) ? "laa" : oui;

  dualPrintf(
      "{\"event\":\"detection\","
      "\"detection_method\":\"wifi_%s\","
      "\"protocol\":\"%s\","
      "\"mac_address\":\"%s\","
      "\"oui\":\"%s\","
      "\"device_name\":\"\","
      "\"rssi\":%d,"
      "\"channel\":%u,"
      "\"frequency\":%u,"
      "\"ssid\":\"%s\","
      "\"confidence\":%u}\n",
      method, channelBand(ch), mac, ouiStr, rssi,
      (unsigned)ch, (unsigned)channelFreqMhz(ch),
      ssidEsc, (unsigned)confidence);
}

// ============================================================
// PROMISCUOUS CALLBACK  — keep it fast, no Serial, no malloc
// ============================================================

static bool IRAM_ATTR extractSsidFromMgmtBody(const uint8_t* body, int len,
                                     char* outSsid, size_t outLen) {
  if (!body || len <= 0 || !outSsid || outLen == 0) return false;
  while (len >= 2) {
    uint8_t id = body[0], elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) {
      size_t n = (elen < (outLen - 1)) ? elen : (outLen - 1);
      memcpy(outSsid, body + 2, n);
      outSsid[n] = '\0';
      return true;
    }
    body += elen + 2; len -= elen + 2;
  }
  return false;
}

static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

static void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped) return;

#if PROCESS_MGMT_FRAMES && PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif PROCESS_MGMT_FRAMES
  if (type != WIFI_PKT_MGMT) return;
#elif PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_DATA) return;
#else
  return;
#endif

  wifi_promiscuous_pkt_t*   pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;
  wifi_ieee80211_mac_hdr_t* hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;
  if (rssi < RSSI_MIN) return;
  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;

#if TESTING_MODE
  enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, rssi, ch, nullptr, "test", 50);
  return;
#endif

  // ── addr2 (transmitter) OUI match — three confidence tiers (PR#39) ─────────
  // High confidence (exclusive Flock):  ALERT_OUI_ADDR2,    score≥40, chirps.
  // Contract-mfr (shared Liteon/USI):   ALERT_OUI_MFR,      score=20, silent.
  // SoundThinking/ShotSpotter:          ALERT_SOUNDTHINKING, score=35, chirps.
  {
    bool isHigh = matchFlockHighOui(hdr->addr2);
    bool isMfr  = !isHigh && matchFlockMfrOui(hdr->addr2);
    bool isST   = !isHigh && !isMfr && matchSoundThinkingOui(hdr->addr2);

    if (isHigh || isMfr || isST) {
      bool emitted = false;
      // Wildcard probe check: Flock cameras (high + mfr OUI) emit empty-SSID probes.
      // SoundThinking sensors do not send this probe pattern — skip for them.
      if ((isHigh || isMfr) && type == WIFI_PKT_MGMT) {
        uint8_t fc0     = hdr->frame_ctrl & 0xFF;
        uint8_t ftype   = (fc0 >> 2) & 0x03;
        uint8_t subtype = (fc0 >> 4) & 0x0F;
        if (ftype == 0 && subtype == 4) {   // Probe Request
          int sigLen  = (int)pkt->rx_ctrl.sig_len;
          int bodyLen = sigLen - (int)sizeof(wifi_ieee80211_mac_hdr_t);
          const uint8_t* body = pkt->payload + sizeof(wifi_ieee80211_mac_hdr_t);
          int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
          if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
          if (r == 1) {
            uint8_t conf = computeConfidence(ALERT_WILDCARD_PROBE, hdr->addr2, rssi, nullptr);
            uint8_t pairCh = 0;
            if (checkSeqMac(hdr->addr2, ch, &pairCh))
              conf = applySeqMacBonus(conf);
            enqueueAlert(ALERT_WILDCARD_PROBE, hdr->addr2, rssi, ch,
                         nullptr, "probe_req", conf);
            emitted = true;
          }
        }
      }
      if (!emitted) {
        AlertType atype = isST  ? ALERT_SOUNDTHINKING :
                          isMfr ? ALERT_OUI_MFR       : ALERT_OUI_ADDR2;
        uint8_t conf = computeConfidence(atype, hdr->addr2, rssi, nullptr);
        uint8_t pairCh = 0;
        if (checkSeqMac(hdr->addr2, ch, &pairCh))
          conf = applySeqMacBonus(conf);
        enqueueAlert(atype, hdr->addr2, rssi, ch, nullptr, "addr2", conf);
      }
    }
  }

  // ── addr1 (receiver/destination) OUI match ─────────────────────────────────
  // Catches cameras in burst-sleep: they appear as *dst* of probe responses
  // even when not transmitting.  Multicast guard is mandatory.
#if CHECK_ADDR1
  if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1)) {
    uint8_t conf = computeConfidence(ALERT_OUI_ADDR1, hdr->addr1, rssi, nullptr);
    enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, rssi, ch, nullptr, "addr1", conf);
  }
#endif

  // ── addr3 (BSSID) OUI match — fallback for addr2-randomised frames ─────────
#if CHECK_ADDR3
  if (type == WIFI_PKT_MGMT && !isMulticast(hdr->addr3) && matchOuiRaw(hdr->addr3)) {
    uint8_t conf = computeConfidence(ALERT_OUI_ADDR3, hdr->addr3, rssi, nullptr);
    enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, rssi, ch, nullptr, "addr3", conf);
  }
#endif

  // ── SSID match (beacon / probe response / probe request) ───────────────────
  //
  // NOTE ON "Flock Camera net." and LOCALLY-ADMINISTERED MACs:
  // This hotspot uses LAA MACs (bit 1 of first byte set).  matchOuiRaw()
  // returns false for these by design (LAA MACs cannot match IEEE OUIs).
  // The SSID is therefore the SOLE detection handle for this camera class.
  //
  // We check BOTH addr2 (transmitter of beacons/probe-responses) AND the
  // locally-administered check below so either path catches the camera.
  //
  // Frame types checked:
  //   subtype 8  = Beacon         (camera's hotspot advertising itself)
  //   subtype 5  = Probe Response (reply to our 2.4/5 GHz probes)
  //   subtype 4  = Probe Request  (camera scanning for an upstream AP)
#if ENABLE_SSID_MATCH
  if (type == WIFI_PKT_MGMT) {
    uint8_t fc0     = hdr->frame_ctrl & 0xFF;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    uint8_t ftype   = (fc0 >> 2) & 0x03;

    if (ftype == 0) {
      int sigLen = pkt->rx_ctrl.sig_len - 4;   // strip FCS
      if (sigLen < (int)sizeof(wifi_ieee80211_mac_hdr_t)) goto ssid_done;

      const uint8_t* mgmtBody    = nullptr;
      int            mgmtBodyLen = 0;
      const char*    frameKind   = nullptr;

      if (subtype == 8 || subtype == 5) {
        // Beacon / Probe Response: 12-byte fixed params before IEs
        int off = sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        if (sigLen > off) {
          frameKind   = (subtype == 8) ? "beacon" : "probe_resp";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      } else if (subtype == 4) {
        // Probe Request: IEs follow directly after MAC header
        int off = sizeof(wifi_ieee80211_mac_hdr_t);
        if (sigLen > off) {
          frameKind   = "probe_req";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      }

      if (mgmtBody && mgmtBodyLen > 0) {
        char ssid[33] = {0};
        if (extractSsidFromMgmtBody(mgmtBody, mgmtBodyLen, ssid, sizeof(ssid))
            && ssid[0] != '\0') {

          if (matchSsidKeyword(ssid)) {
            bool laa = (hdr->addr2[0] & 0x02) != 0;

            if (laa) {
              // ── ALERT_LAA_SSID: LAA MAC + Flock SSID ───────────────────────
              // This is the primary path for issue-#43 "Flock Camera net."
              // cameras.  Run sequential-MAC check for the :DE/:DF pair bonus.
              uint8_t conf = computeConfidence(ALERT_LAA_SSID, hdr->addr2, rssi, ssid);
              uint8_t pairCh = 0;
              if (checkSeqMac(hdr->addr2, ch, &pairCh))
                conf = applySeqMacBonus(conf);
              enqueueAlert(ALERT_LAA_SSID, hdr->addr2, rssi, ch,
                           ssid, frameKind, conf);
            } else {
              // Globally-administered MAC with Flock SSID (fully deployed cam)
              uint8_t conf = computeConfidence(ALERT_SSID, hdr->addr2, rssi, ssid);
              enqueueAlert(ALERT_SSID, hdr->addr2, rssi, ch,
                           ssid, frameKind, conf);
            }
          }
        }
      }
    }
  }
  ssid_done: ;
#endif
}

// ============================================================
// DRAIN QUEUE
// ============================================================

static void drainAlertQueue() {
  while (true) {
    portENTER_CRITICAL(&queueMux);
    if (alertTail == alertHead) { portEXIT_CRITICAL(&queueMux); break; }
    AlertEntry e;
    memcpy(&e, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
    alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&queueMux);

    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    const char* method = alertTypeToMethod(e.type);

    bool chirpWorthy = false;
    int idx = fyAddDetection(macStr, method, e.rssi, e.channel,
                             (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                               ? e.ssid : nullptr,
                             e.confidence, &chirpWorthy);

    fyLastTargetSeen = millis();

    if (shouldSuppressDuplicate(macStr)) continue;

    char oui[9];
    ouiFromMac(e.mac, oui, sizeof(oui));

    // Human-readable line
    if (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID) {
      const char* tag = (e.type == ALERT_LAA_SSID) ? "DETECT-LAA-SSID" : "DETECT-SSID";
      dualPrintf("[flockyou] %s type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u conf=%u count=%d\n",
                 tag, e.frameKind, macStr, e.ssid, e.rssi, e.channel,
                 (unsigned)e.confidence,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    } else {
      dualPrintf("[flockyou] DETECT-OUI mac=%s oui=%s rssi=%d ch=%u addr=%s conf=%u count=%d\n",
                 macStr, oui, e.rssi, e.channel,
                 e.frameKind[0] ? e.frameKind : "addr2",
                 (unsigned)e.confidence,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    }

    // Flask JSON
    emitDetectionJSON(macStr, method, e.rssi, e.channel,
                      (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                        ? e.ssid : "",
                      e.confidence);

    // PR#39: only chirp and LED flash for detections at or above CHIRP_MIN_CONFIDENCE.
    // Contract-mfr OUI alone (conf=20 < 30) logs silently — no audible/visual noise.
    if (chirpWorthy && e.confidence >= CHIRP_MIN_CONFIDENCE) {
      newDetectChirp();
      fyLastHeartbeatAt = millis();
    }
    if (e.confidence >= CHIRP_MIN_CONFIDENCE) {
      ledFlash(LED_FLASH_MS);
    }
#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
    {
      const char* dt = (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                       ? "SSID" : "OUI";
      c5DisplayDetection(dt, macStr, e.confidence, e.rssi, e.channel);
    }
#endif
#if defined(USE_M5BASIC)
    m5basicDetection(method, macStr, e.confidence, e.rssi, e.channel,
                     (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                       ? e.ssid : "",
                     fyDetCount,
                     (fyLastTargetSeen > 0) ? millis() - fyLastTargetSeen : 0UL);
#endif
#if defined(USE_M5STICKC_PLUS_SE)
    m5stickcDetection(method, macStr, e.confidence, e.rssi, e.channel,
                      (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                        ? e.ssid : "",
                      fyDetCount,
                      (fyLastTargetSeen > 0) ? millis() - fyLastTargetSeen : 0UL);
#endif

#if STOP_ON_OUI_HIT
    if (e.type != ALERT_SSID && e.type != ALERT_LAA_SSID) stopSniffing("OUI hit");
#endif
#if STOP_ON_SSID_HIT
    if (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID) stopSniffing("SSID hit");
#endif
  }
}

// ============================================================
// AUTOSAVE / HEARTBEAT / LED TICKS
// ============================================================

static void autosaveTick() {
  if (!fySpiffsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);

#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
  c5DisplayInit();
#endif

#if defined(USE_M5ATOM)
  strip.begin();
  strip.setBrightness(80);
  strip.show();
  for (int i = 0; i < 256; i++) {
    uint8_t r, g, b;
    if (i < 85)       { r = 255 - i*3; g = i*3;          b = 0; }
    else if (i < 170) { r = 0;          g = 255-(i-85)*3; b = (i-85)*3; }
    else              { r = (i-170)*3;  g = 0;            b = 255-(i-170)*3; }
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
    delay(4);
  }
  strip.setPixelColor(0, strip.Color(0, 60, 0));
  strip.show();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

#if defined(USE_M5ATOM_VOICES3R)
  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  M5.Speaker.setVolume(200);
#endif

#if defined(USE_M5ATOM_VOICE)
  {
    auto m5cfg = M5.config();
    M5.begin(m5cfg);
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.pin_data_out = 22;
    spk_cfg.pin_bck      = 19;
    spk_cfg.pin_ws       = 33;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.begin();
    M5.Speaker.setVolume(220);
  }
#endif

// M5Stack Basic/Core2: M5Unified fully inits inside m5basicInit().
#if defined(USE_M5BASIC)
  m5basicInit();
#endif
// M5StickC Plus SE: M5Unified inits in m5stickcInit() (display + AXP192, no I2S).
#if defined(USE_M5STICKC_PLUS_SE)
  m5stickcInit();
#endif

#if MIRROR_SERIAL && !defined(USE_M5ATOM) && !defined(USE_M5ATOM_VOICES3R) && !defined(USE_M5BASIC) && !defined(USE_M5STICKC_PLUS_SE)
  Serial1.begin(MIRROR_BAUD, SERIAL_8N1, -1, MIRROR_TX_PIN);
#endif

#if USE_BUZZER && !defined(USE_M5ATOM)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif

#if USE_LED && !defined(USE_LED_MATRIX)
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
#endif

  startupBeep();
#if USE_LED
  ledFlash(200);
#endif

  precompileOuis();
  memset(dedupeTable, 0, sizeof(dedupeTable));
  memset(seqMacTable, 0, sizeof(seqMacTable));
  seqMacCount = 0;

  // Suppress expected first-boot format noise: on freshly-erased flash the
  // SPIFFS driver logs "mount failed, -10025" before auto-formatting.
  // The error is cosmetic — SPIFFS.begin(true) handles it silently after this.
  esp_log_level_set("SPIFFS", ESP_LOG_NONE);
  bool spiffsOk = SPIFFS.begin(true);
  esp_log_level_set("SPIFFS", ESP_LOG_WARN);
  if (spiffsOk) {
    fySpiffsReady = true;
    dualPrintln("[flockyou] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[flockyou] SPIFFS init FAILED — running without persistence");
  }

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  // BLE init must happen BEFORE WiFi promiscuous start on shared-radio ESP32.
  // NimBLE takes the radio first; we'll hand it back to WiFi after init.
  initBLE();
  g_bleNextScan = millis() + 5000;  // first BLE scan 5 s after boot
  dualPrintln("[flockyou] BLE scanner init OK");
#endif

  WiFi.mode(WIFI_MODE_NULL);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();

  applyInitialChannel();

  wifi_promiscuous_filter_t filt = {
    .filter_mask = 0
#if PROCESS_MGMT_FRAMES
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if PROCESS_DATA_FRAMES
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
  };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
  esp_wifi_set_promiscuous(true);

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN && defined(BLE_COEX_MODE) && BLE_COEX_MODE
  // In coex mode, start the continuous BLE scan NOW — after WiFi promiscuous
  // is already ON.  The ESP-IDF coexistence scheduler (SW_COEXIST) handles
  // time-sharing automatically; no application-level pause/resume needed.
  bleCoexStart();
#endif

  dualPrintln("[flockyou] v2 WiFi detector started");
  dualPrintf("[flockyou] mode=%s dwell_ms=%u start_ch=%u rssi_min=%d spiffs=%d"
#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  #if defined(BLE_COEX_MODE) && BLE_COEX_MODE
             " ble=COEX(continuous)"
  #else
             " ble=ON(time-mux) corr_win=%lus"
  #endif
#endif
             "\n",
             channelModeName(), CHANNEL_DWELL_MS, currentChannel,
             RSSI_MIN, fySpiffsReady ? 1 : 0
#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN && !(defined(BLE_COEX_MODE) && BLE_COEX_MODE)
             , (unsigned long)(BLE_CORR_WINDOW_MS / 1000)
#endif
             );
  dualPrintf("[flockyou] OUIs: high=%u mfr=%u st=%u | SSID_KW=%u seqSlots=%u chirpMin=%d\n",
             (unsigned)FY_OUI_HIGH_COUNT, (unsigned)FY_OUI_MFR_COUNT,
             (unsigned)FY_OUI_ST_COUNT, (unsigned)SSID_KEYWORD_COUNT,
             (unsigned)SEQ_MAC_TABLE_SIZE, (int)CHIRP_MIN_CONFIDENCE);

  lastHeartbeat = millis();
  fyLastSaveAt  = millis();

  // Force immediate scanning screen — clears splash without waiting 30 s
  // for the first printHeartbeat() heartbeat tick.
#if defined(USE_M5BASIC)
  m5basicScanning(currentChannel, channelModeName(), fyDetCount,
                  millis(), fySpiffsReady,
                  (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
#endif
#if defined(USE_M5STICKC_PLUS_SE)
  m5stickcScanning(currentChannel, channelModeName(), fyDetCount,
                   millis(), fySpiffsReady,
                   (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
#endif
}

void loop() {
  updateChannelMode();
  drainAlertQueue();
  autosaveTick();
  heartbeatTick();
  ledTick();
  printHeartbeat();

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  bleScanTick(fyPromiscPaused);
#endif

#if defined(USE_M5BASIC)
  {
    int btn = m5basicButtonTick();
    if (btn == 1) {
      fySaveSession(); Serial.println("[flockyou] Manual save (Btn A)");
    } else if (btn == 3) {
      customChannelIndex = (customChannelIndex + 1) % customChannelCount;
      currentChannel = customChannels[customChannelIndex];
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
      lastHop = millis();
      Serial.printf("[flockyou] Manual ch hop -> %u (Btn C)\n", currentChannel);
    }
  }
#endif
#if defined(USE_M5STICKC_PLUS_SE)
  {
    int btn = m5stickcButtonTick();
    if (btn == 1) {
      fySaveSession(); Serial.println("[flockyou] Manual save (Btn A)");
    } else if (btn == 3) {
      customChannelIndex = (customChannelIndex + 1) % customChannelCount;
      currentChannel = customChannels[customChannelIndex];
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
      lastHop = millis();
      Serial.printf("[flockyou] Manual ch hop -> %u (Btn B)\n", currentChannel);
    }
  }
#endif

  delay(1);
}
