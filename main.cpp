
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
  // NimBLE-Arduino 2.x (required for ESP32-C5, h2zero/NimBLE-Arduino@^2.1.0)
  // renamed NimBLEAdvertisedDeviceCallbacks -> NimBLEScanCallbacks and
  // NimBLEScan::setAdvertisedDeviceCallbacks() -> setScanCallbacks(), and the
  // onResult() device pointer became `const`.  NIMBLE_CPP_VERSION_MAJOR is
  // only defined starting in 2.x (via NimBLECppVersion.h, pulled in by
  // NimBLEDevice.h) so its absence reliably signals the 1.x API.
  #if defined(NIMBLE_CPP_VERSION_MAJOR) && (NIMBLE_CPP_VERSION_MAJOR >= 2)
    #define FY_NIMBLE_V2 1
  #else
    #define FY_NIMBLE_V2 0
  #endif
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
//
// M5Atom Echo has the SAME onboard SK6812 addressable RGB LED on GPIO27 as
// Atom Lite/Voice (all three share the same reference PCB — confirmed by the
// original board-support commit, which drove Echo and Lite off one shared
// NeoPixel `strip` object). A later refactor ("v2", commit 185abc1) flipped
// USE_LED to 0 for Echo and a subsequent LED-modularization pass excluded it
// from led_neopixel.h's guard entirely — an unintentional regression, not a
// hardware limitation. Restored here.
//
// USE_M5ATOM (a separate flag used elsewhere to gate ledMatrixBootSequence()/
// button pinMode() calls in setup()) intentionally stays OFF for Echo: Echo's
// buzzer pinMode() init further down is itself gated on "!defined(USE_M5ATOM)",
// and Echo — unlike Lite/Voice — actually uses that GPIO25 buzzer. Echo gets
// its own explicit ledMatrixBootSequence() call in setup() instead (search
// USE_M5ATOM_ECHO there) so its buzzer init path is left undisturbed.
#if defined(USE_M5ATOM_LITE) || defined(USE_M5ATOM_VOICE)
  #define USE_M5ATOM 1
#endif
#if defined(USE_M5ATOM_LITE) || defined(USE_M5ATOM_VOICE) || defined(USE_M5ATOM_ECHO)
  #define LED_PIN 27
  #define NUM_LEDS 1
  #define BUTTON_PIN 39
#endif
#include "led_neopixel.h"

#if defined(USE_M5ATOM_ECHO)
  #define USE_M5ATOM_ECHO_BTN 1
#endif



#if defined(USE_M5ATOM_VOICES3R) || defined(USE_M5ATOM_VOICE) || defined(USE_M5BASIC)
  #include <M5Unified.h>
#endif
#if defined(USE_M5ATOM_VOICES3R)
  #define BUTTON_PIN 41
#endif

// ── Simple single-GPIO button support (Atom series + T-Dongle C5) ───────────
// M5Stack Basic/Core2 and M5StickC Plus SE get button handling through
// M5Unified (M5.BtnA/B/C) inside their respective display headers.  The
// Atom Lite/Echo/Voice/VoiceS3R boards and the LILYGO T-Dongle C5 only have
// a single bare GPIO button with no M5Unified Button_Class — previously
// BUTTON_PIN/C5_BTN_PIN were #defined but *never read*, so the button did
// nothing.  This block wires up a simple debounced digitalRead() handler.
#if defined(USE_M5ATOM_LITE) || defined(USE_M5ATOM_ECHO) || \
    defined(USE_M5ATOM_VOICE) || defined(USE_M5ATOM_VOICES3R)
  #define HAS_SIMPLE_BUTTON 1
  #define SIMPLE_BUTTON_PIN BUTTON_PIN
#elif defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
  #define HAS_SIMPLE_BUTTON 1
  #define SIMPLE_BUTTON_PIN C5_BTN_PIN
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
#include "led_gpio.h"

// MIRROR_SERIAL (secondary UART1 log mirror) — DISABLED BY DEFAULT.
//
// ROOT CAUSE FOUND (verified on real M5Atom Echo hardware, 2 physical
// units): calling Serial1.begin(MIRROR_BAUD, SERIAL_8N1, -1, MIRROR_TX_PIN)
// in setup() reliably corrupts the ESP32's ability to read its own SPI
// flash on every subsequent boot, producing an unrecoverable (until a full
// chip erase) boot loop:
//   rst:0x7/0x8 (TG0/TG1WDT_SYS_RESET) ... flash read err, 1000
// This was isolated via exhaustive on-device bisection: a blank sketch
// (and even one with a 900KB+ padding array) boots perfectly forever on
// the same hardware/toolchain; disabling ONLY this Serial1.begin() call
// (with everything else in the real firmware left fully intact) restores
// fully reliable, sustained boot/run behavior. Re-enabling the call with
// an explicit (non -1) RX pin still eventually crashed, just later in the
// boot sequence — so the problem is UART1 usage on this hardware/pin
// combination in general, not merely the "-1 = auto pin" argument.
//
// UART1's default GPIO-matrix pins on the classic ESP32 overlap with the
// SPI flash's data lines when not both explicitly reassigned elsewhere;
// enabling UART1 here appears to disturb the flash controller's pin
// wiring badly enough that flash reads fail on every reset thereafter.
//
// MIRROR_SERIAL was only ever an optional secondary-log-output feature
// (mirroring detection output to a second UART for external hardware) —
// not required for core detector functionality. Given it's now proven to
// reliably brick boot on affected boards (ESP32 DevKit and M5Atom Echo —
// see the guard on the Serial1.begin() call below), it defaults OFF.
// Set to 1 only if you have verified on YOUR specific unit that it is safe.
#define MIRROR_SERIAL    0
#define MIRROR_TX_PIN    17
#define MIRROR_BAUD      115200


#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
// ── Dwell time + hop DIRECTION (August 2026 field-reliability fix) ─────────
// Root-caused a user-reported "hit and miss" pattern (beeps while driving
// through an area but silence while stopped right next to a confirmed
// camera; some cameras only alert while dwelling at a stop light, never on
// a drive-by) against colonelpanichacks/flock-you upstream, which carries
// this exact fix credited to field observation by nsm_barri: Flock cameras
// themselves hop 1 → 6 → 11 (ASCENDING) at ~125 ms per channel. The old
// defaults here — ASCENDING scan order + a slower 350 ms dwell — meant our
// scanner and a camera's hop cycle could fall into a persistent "lockstep"
// phase relationship where their dwell windows never overlap, for as long
// as both keep repeating the same fixed-length cycle (which is exactly the
// "silent while stationary" scenario). Reversing our scan to DESCENDING
// (11 → 6 → 1) makes the two radios sweep toward each other each cycle
// instead of chasing each other in the same direction, which converges on
// an overlap far more reliably regardless of starting phase. Dwell is
// dropped to 250 ms (2 x the camera's observed ~125 ms hop) to also shrink
// the worst-case time-to-overlap. Both changes are direct, low-risk ports
// of upstream's already field-tested fix — see also DETECTION_IMPROVEMENTS.md.
#define CHANNEL_DWELL_MS 250
#define SINGLE_CHANNEL 1

// ── 2.4 GHz channels — always scanned ──────────────────────────────────────
// Flock primaries: 1, 6, 11.  "Flock Camera net." observed on ch.1 (2.4 GHz).
// NOTE: order is intentionally DESCENDING (11, 6, 1) — see the
// CHANNEL_DWELL_MS comment above for why. Do not "fix" this back to
// ascending order without re-reading that comment.
static const uint8_t customChannels[]   = {11, 6, 1};
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

// ui_task.h — decoupled UI/display FreeRTOS task (see file for full
// rationale). Included here, right after the audio/heartbeat macros it
// needs (NEW_CHIRP_*/HB_BEEP_*), and before AlertType/board display headers
// use its publish functions.
#include "ui_task.h"

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

// Confidence weights, OUI byte tables, and sequential-MAC tracking moved to
// fy_confidence.h (included further below, after AlertType/isFcnSsid are
// defined — see the "CONFIDENCE SCORE COMPUTATION" section).

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

// Shared onResult() logic — identical for NimBLE-Arduino 1.x and 2.x.  The
// getters used here (getRSSI/getManufacturerData/haveServiceUUID/
// getServiceUUID/getName/getAddress) exist with the same signatures in both
// major versions, but NimBLE 1.x declares them non-const while 2.x declares
// them on a `const NimBLEAdvertisedDevice*`.  This helper takes a non-const
// pointer (the 1.x-native shape); the 2.x wrapper below const_casts its
// `const` parameter away before calling in — safe, since the scan callback
// never actually receives a truly-const object, only a const-qualified view.
static void fyProcessBLEAdvertisedDevice(NimBLEAdvertisedDevice* adv) {

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

// Thin per-API-version wrapper class — only the base class, method name/
// signature differ; all real logic lives in fyProcessBLEAdvertisedDevice().
#if FY_NIMBLE_V2
class FlockBLECallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    // 2.x hands us a const-qualified view; fyProcessBLEAdvertisedDevice()
    // takes non-const because NimBLE 1.x's getters are non-const (see
    // comment above the function).  Safe to cast away: the underlying
    // object is never truly const, only the pointer type differs by API
    // version.
    fyProcessBLEAdvertisedDevice(const_cast<NimBLEAdvertisedDevice*>(adv));
  }
};
#else
class FlockBLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* adv) override {
    fyProcessBLEAdvertisedDevice(adv);
  }
};
#endif

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
  // NimBLE 1.x's NimBLEDevice::setPower() ONLY has the
  // esp_power_level_t-enum overload (setPower(esp_power_level_t, ...)) — it
  // does NOT accept a plain int/dBm value.  NimBLE 2.x dropped that enum
  // overload in favor of a dBm-based int overload.  Must version-gate;
  // 3 dBm ≈ ESP_PWR_LVL_P3.
#if FY_NIMBLE_V2
  NimBLEDevice::setPower(3);               // dBm-based (2.x only)
#else
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);  // enum-based (1.x only)
#endif
  g_pBLEScan = NimBLEDevice::getScan();

#if FY_NIMBLE_V2
  g_pBLEScan->setScanCallbacks(&g_bleCallbacks, false);
#else
  g_pBLEScan->setAdvertisedDeviceCallbacks(&g_bleCallbacks, false);
#endif
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
// SIMPLE SINGLE-GPIO BUTTON  (Atom Lite/Echo/Voice/VoiceS3R + T-Dongle C5)
// ============================================================
// These boards have exactly one bare GPIO button with no M5Unified
// Button_Class helper.  BUTTON_PIN / C5_BTN_PIN were previously #defined
// but never actually read anywhere — the button did nothing.  This adds a
// minimal debounced active-low (INPUT_PULLUP) press detector.  A press
// forces an immediate channel hop (mirrors the M5Basic "Btn C" / StickC
// "Btn B" action) plus a short audible/visual acknowledgement.
#if defined(HAS_SIMPLE_BUTTON)
static bool          simpleBtnLastState     = true;  // idle = HIGH (pulled up)
static unsigned long simpleBtnLastChangeMs  = 0;
#define SIMPLE_BUTTON_DEBOUNCE_MS 50

// Returns true exactly once per physical press (debounced falling edge).
static bool simpleButtonPressed() {
  bool cur = digitalRead(SIMPLE_BUTTON_PIN);
  unsigned long now = millis();
  if (cur != simpleBtnLastState && (now - simpleBtnLastChangeMs) > SIMPLE_BUTTON_DEBOUNCE_MS) {
    simpleBtnLastChangeMs = now;
    simpleBtnLastState    = cur;
    if (!cur) return true;   // LOW == pressed
  }
  return false;
}
#endif


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
#if defined(USE_M5BASIC)
    mb_logAdd(_dualBuf);
#endif
  }
}

static void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL && !defined(USE_M5ATOM_VOICES3R)
  Serial1.println(str);
#endif
#if defined(USE_M5BASIC)
  mb_logAdd(str);
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
#elif defined(USE_M5BASIC)
  // M5.Speaker shares the M5Unified singleton the UI task (ui_task.h) now
  // exclusively owns for display access — never call it directly from the
  // scan/main task. Hand off instead; the UI task plays the identical
  // tone/delay/tone sequence itself, off the WiFi/BLE-scanning critical path.
  uiRequestAudio(1);
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
#elif defined(USE_M5BASIC)
  // See newDetectChirp() above — hand off to the UI task instead of
  // touching M5.Speaker directly from the scan/main task.
  uiRequestAudio(2);
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

// precompileOuis()/isMulticast()/matchFlockHighOui()/matchFlockMfrOui()/
// matchSoundThinkingOui()/matchOuiRaw() moved to fy_confidence.h.

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
    // C5's periodic scanning-screen redraw now happens inside the UI task's
    // own HEARTBEAT_MS gate (ui_task.h) — no direct display call here.
  }
}

// M5Stack Basic/Core2/StickC "scanning" screen — shows runtime, log-mirror
// strip, and idle status.  IMPORTANT: this is called every loop() iteration
// (NOT gated behind the 30 s printHeartbeat() interval above).
// m5basicScanning()/m5stickcScanning() each already contain their own
// internal staleness check (mb_lastDrawMs/msc_lastDrawMs) that cheaply
// no-ops most calls, so calling them unconditionally here is what actually
// makes the screen redraw several times per second in real time instead of
// once every 30 s.
static void screenTick() {
  // Publish the current scanning status to the UI task (ui_task.h) instead
  // of calling m5basicScanning()/m5stickcScanning() directly — those touch
  // M5Unified, which only the UI task may access now. Cheap even on boards
  // with no display (Atom/esp32dev) since nothing ever reads it there.
  uiPublishScan(currentChannel, channelModeName(), fyDetCount, fySpiffsReady);
}

// Confidence weights, OUI byte tables, sequential-MAC tracking, and
// computeConfidence()/applySeqMacBonus() all live in fy_confidence.h now.
// Included here (not earlier) because computeConfidence() needs AlertType
// (defined above in "ALERT QUEUE"), isFcnSsid() (defined above in
// "HELPERS"), and — when ENABLE_BLE_SCAN=1 — g_bleFlockLastSeen (defined
// above in "BLE CROSS-CORRELATION STATE"), all of which must already be
// visible to the preprocessor at this point in the file.
#include "fy_confidence.h"

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
    // Publish this detection to the UI task (ui_task.h) instead of calling
    // c5DisplayDetection()/m5basicDetection()/m5stickcDetection() directly —
    // those touch M5Unified/the C5 display object, which only the UI task
    // may access now. The UI task renders the alert screen on its next poll
    // (~50 ms), independent of WiFi/BLE scanning.
    {
      const char* dispType = (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                             ? "SSID" : "OUI";
      const char* ssidArg  = (e.type == ALERT_SSID || e.type == ALERT_LAA_SSID)
                             ? e.ssid : "";
      uiPublishAlert(method, macStr, e.confidence, e.rssi, e.channel, ssidArg,
                     fyDetCount,
                     (fyLastTargetSeen > 0) ? millis() - fyLastTargetSeen : 0UL,
                     dispType);
    }

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

#if defined(HAS_SIMPLE_BUTTON)
  // Single bare-GPIO button (Atom Lite/Echo/Voice/VoiceS3R, T-Dongle C5).
  // Safe to call even where USE_M5ATOM's block below also sets this pin.
  pinMode(SIMPLE_BUTTON_PIN, INPUT_PULLUP);
  simpleBtnLastState = digitalRead(SIMPLE_BUTTON_PIN);
#endif

#if defined(USE_M5ATOM) || defined(USE_M5ATOM_ECHO)
  ledMatrixBootSequence();
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
  // Immediately replace the splash with the scanning screen.  SPIFFS/BLE/WiFi
  // init below can take a while (or, on some StickC Plus SE / Basic units,
  // stall) — drawing the real UI *now* means the display never appears
  // "stuck at Init..." even if a later step is slow.  It gets redrawn with
  // real data once init finishes (see bottom of setup()).
  m5basicScanning(currentChannel, channelModeName(), 0,
                  millis(), false,
                  (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
#endif
// M5StickC Plus SE: M5Unified inits in m5stickcInit() (display + AXP192, no I2S).
#if defined(USE_M5STICKC_PLUS_SE)
  m5stickcInit();
  // Same rationale as above — replace "Init..." splash before any
  // potentially slow/blocking SPIFFS/BLE/WiFi setup runs.
  m5stickcScanning(currentChannel, channelModeName(), 0,
                   millis(), false,
                   (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
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
  // Boot-confirmation flash: briefly show red, then settle to idle (dim
  // green on NeoPixel boards, off on plain-GPIO boards) BEFORE the slow
  // SPIFFS/BLE/WiFi init below runs.  ledTick() — the only thing that
  // normally clears a flash back to idle — isn't called until loop()
  // starts, so without this explicit delay+tick the LED would sit in its
  // "red" state for the *entire* remaining duration of setup() (BLE init
  // alone can take the better part of a second), making it look
  // permanently red at boot instead of promptly turning green.
  ledFlash(200);
  delay(200);
  ledTick();
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

  // ------------------------------------------------------------------
  // WiFi promiscuous init MUST happen BEFORE BLE controller init.
  //
  // This used to be reversed (BLE init before esp_wifi_start()) based on a
  // comment claiming shared-radio coexistence required it. That was never
  // verified on hardware and matches a known ESP-IDF failure mode: bringing
  // up the BT controller before esp_wifi_start() can leave the coexistence
  // arbiter in a state where esp_wifi_start() hangs forever. Espressif's own
  // WiFi/BT coex examples always init+start WiFi FIRST, then bring up the BT
  // controller. This matches a field report of the device hanging right
  // after printing "BLE scanner init OK" and never reaching "v2 WiFi
  // detector started" -- i.e. setup() never finishes, loop() never runs, and
  // the LED gets stuck on its boot-flash color forever (that is NOT a
  // separate LED bug -- ledTick()/drainAlertQueue() only run from loop()).
  // Checkpoint prints below pinpoint exactly which call hangs if this
  // recurs.
  // ------------------------------------------------------------------
  dualPrintln("[flockyou] wifi init...");
  WiFi.mode(WIFI_MODE_NULL);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  dualPrintln("[flockyou] wifi start...");
  esp_wifi_start();
  dualPrintln("[flockyou] wifi start OK");

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
  dualPrintln("[flockyou] wifi promiscuous ON");

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  dualPrintln("[flockyou] BLE init...");
  initBLE();
  g_bleNextScan = millis() + 5000;  // first BLE scan 5 s after boot
  dualPrintln("[flockyou] BLE scanner init OK");
#endif

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN && defined(BLE_COEX_MODE) && BLE_COEX_MODE
  // In coex mode, start the continuous BLE scan NOW -- after WiFi promiscuous
  // is already ON.  The ESP-IDF coexistence scheduler (SW_COEXIST) handles
  // time-sharing automatically; no application-level pause/resume needed.
  bleCoexStart();
  dualPrintln("[flockyou] BLE coex scan started");
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

  // Start the decoupled UI/display task (ui_task.h) now that every
  // board/display header it depends on has been initialised above. From
  // this point on, M5Unified/display-object access (drawing, M5.update(),
  // button reads, vibration, M5.Speaker on M5Basic) happens ONLY on that
  // task — never again from loop()/scan code.
  startUiTask();
}

void loop() {
  updateChannelMode();
  drainAlertQueue();
  autosaveTick();
  heartbeatTick();
  ledTick();
  printHeartbeat();
  screenTick();

#if defined(ENABLE_BLE_SCAN) && ENABLE_BLE_SCAN
  bleScanTick(fyPromiscPaused);
#endif


#if defined(USE_M5BASIC) || defined(USE_M5STICKC_PLUS_SE)
  // Button presses are now detected on the UI task (ui_task.h) — it is the
  // only task allowed to touch M5Unified (M5.update()/M5.BtnX) since that
  // object is shared with the display and is not thread-safe. loop() just
  // consumes whichever action (if any) the UI task recorded since the last
  // check. Action codes: 1 = Btn A (save session), 3 = Btn C/B (force
  // channel hop). M5Basic's brightness cycle (Btn B) and Core2's vibration
  // tick are handled entirely inside the UI task and need no feedback here.
  {
    uint8_t btn = uiTakeButtonAction();
    if (btn == 1) {
      fySaveSession(); Serial.println("[flockyou] Manual save (button)");
    } else if (btn == 3) {
      customChannelIndex = (customChannelIndex + 1) % customChannelCount;
      currentChannel = customChannels[customChannelIndex];
      esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
      lastHop = millis();
      Serial.printf("[flockyou] Manual ch hop -> %u (button)\n", currentChannel);
    }
  }
#endif


#if defined(HAS_SIMPLE_BUTTON)
  // Single bare-GPIO button (Atom Lite/Echo/Voice/VoiceS3R, T-Dongle C5).
  // Press = force immediate channel hop + save session + short ack
  // (beep on buzzer/speaker-equipped boards, LED flash on LED-equipped boards).
  if (simpleButtonPressed()) {
    fySaveSession();
    customChannelIndex = (customChannelIndex + 1) % customChannelCount;
    currentChannel = customChannels[customChannelIndex];
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastHop = millis();
    Serial.printf("[flockyou] Manual save + ch hop -> %u (button)\n", currentChannel);
#if USE_BUZZER || (defined(USE_M5_SPEAKER) && USE_M5_SPEAKER)
    heartbeatBeep();
#endif
#if USE_LED
    ledFlash(400);
#endif
#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
    uiForceC5Redraw();
#endif
  }
#endif

  delay(1);
}

