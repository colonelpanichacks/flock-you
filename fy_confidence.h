// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// fy_confidence.h — OUI matching + confidence-score computation +
// sequential-MAC-pair tracking for flock-you-esp32.
//
// WHY THIS FILE EXISTS (bug-isolation refactor):
// This is the fragile core of the detection engine — it decides HOW
// CONFIDENT the firmware is that a given WiFi frame came from a Flock
// device, and that confidence score is what ultimately gates the audible
// chirp and the ledFlash()/LED-hold behavior. A subtle bug here (conflating
// the "high" and "mfr" OUI confidence tiers for the ALERT_WILDCARD_PROBE
// case) was the actual root cause of the M5Atom Lite's status LED appearing
// permanently stuck red — NOT the OUI table data itself. Pulling this logic
// into its own single-purpose file:
//   1. Shrinks the context needed to fully understand main.cpp.
//   2. Makes this exact class of scoring bug easier to find, review, and
//      (eventually) unit-test in isolation, without wading through the
//      promiscuous-callback / alert-queue / persistence code that used to
//      surround it in main.cpp.
//
// NOTE ON NATIVE UNIT TESTS: unlike fy_detect.h, this file intentionally
// uses ESP-IDF/Arduino-only symbols (IRAM_ATTR, millis()) and is therefore
// firmware-only — it is #include-d directly into main.cpp's translation
// unit, exactly like the rest of main.cpp's logic was before this refactor.
// It is NOT part of the native/host `pio test -e native` build. The pure,
// hardware-independent OUI/BLE/UUID pattern data and matching primitives
// that ARE host-testable continue to live in fy_detect.h and are used here.
//
// Include-order requirement: this header must be #include-d from main.cpp
// AFTER the AlertType enum (defined in main.cpp's "ALERT QUEUE" section) and
// after the optional g_bleFlockLastSeen BLE-correlation state (defined in
// main.cpp's "BLE CROSS-CORRELATION STATE" section, guarded by
// ENABLE_BLE_SCAN) — both are referenced by computeConfidence() below.
// It must also come before any code that calls precompileOuis(),
// matchFlockHighOui()/matchFlockMfrOui()/matchSoundThinkingOui()/matchOuiRaw(),
// checkSeqMac(), computeConfidence(), or applySeqMacBonus() — in practice,
// before setup() and before the promiscuous wifiSniffer() callback.

#pragma once

#include <string.h>
#include <stdint.h>
#include <stdlib.h>      // strtol (precompileOuis)
#include "fy_detect.h"   // FY_OUI_HIGH_COUNT / FY_OUI_MFR_COUNT / FY_OUI_ST_COUNT + string tables


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

// Standalone BLE-only confidence tiers (fix: these previously never fired —
// a BLE-only match only recorded a timestamp for later WiFi correlation and
// NEVER produced a real alert on its own). All three are set above
// CHIRP_MIN_CONFIDENCE=30 so a lone BLE match now chirps/flashes/logs just
// like a WiFi OUI hit does, mirroring that existing tiered-confidence design.
#define CS_BLE_MFR_ID_STANDALONE 45  // BLE mfr-ID 0x09C8 (XUNTONG/Flock) — high
                                    //   confidence, this ID is Flock-specific
#define CS_BLE_UUID_STANDALONE   45  // Raven/Flock 128-bit service UUID — high
                                    //   confidence, GainSec-confirmed UUIDs
#define CS_BLE_NAME_STANDALONE   35  // BLE device-name substring — good but
                                    //   slightly less specific than mfr-ID/UUID

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
// CONFIDENCE SCORE COMPUTATION  (called from promiscuous callback)
// ============================================================
//
// Called from the WiFi ISR context — must be IRAM-resident and fast.
// Returns a value 0–100.  The BLE correlation check reads
// g_bleFlockLastSeen which is written by a different task, but since it's
// a single uint32_t write it's effectively atomic on Xtensa.
//
// Requires: AlertType enum (main.cpp "ALERT QUEUE" section) and, when
// ENABLE_BLE_SCAN=1, g_bleFlockLastSeen (main.cpp "BLE CROSS-CORRELATION
// STATE" section) to already be declared — see include-order note at the
// top of this file.

static uint8_t IRAM_ATTR computeConfidence(AlertType type, const uint8_t* mac,
                                            int8_t rssi, const char* ssid) {
  int score = 0;

  switch (type) {
    case ALERT_OUI_ADDR2:
      score += CS_OUI_ADDR2;
      break;
    case ALERT_WILDCARD_PROBE:
      // OUI tier already verified by caller before emitting this type.
      // High-tier (exclusive Flock) OUI + wildcard probe is a strong,
      // specific signal — score it the full 62 (was always the intent).
      // Mfr-tier (shared Liteon/USI contract-manufacturer chipset) is a
      // different story: wildcard/broadcast probe requests (empty SSID)
      // are baseline behavior emitted constantly by huge numbers of
      // ordinary, unrelated Liteon/USI-chipset laptops/phones/IoT devices
      // whenever WiFi is on and not yet associated. Treating that combo
      // as high-confidence (62) caused frequent false "detections" that
      // kept re-triggering ledFlash(LED_FLASH_MS) faster than it could
      // expire, making status LEDs appear permanently stuck red. Score
      // mfr-tier + wildcard the same as a lone mfr OUI hit (20, silent)
      // so it still logs but can't single-handedly cross
      // CHIRP_MIN_CONFIDENCE without real corroboration (BLE corr, seq-mac).
      if (matchFlockHighOui(mac)) {
        score += CS_OUI_ADDR2 + CS_WILDCARD_PROBE;
      } else {
        score += CS_OUI_MFR;
      }
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
    case ALERT_BLE_MFR_ID:
      // Standalone BLE mfr-ID match — no corroborating WiFi frame needed.
      score += CS_BLE_MFR_ID_STANDALONE;
      break;
    case ALERT_BLE_RAVEN_UUID:
      // Standalone Raven/Flock 128-bit BLE service UUID match.
      score += CS_BLE_UUID_STANDALONE;
      break;
    case ALERT_BLE_NAME:
      // Standalone BLE device-name substring match.
      score += CS_BLE_NAME_STANDALONE;
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
// we instead compute this before enqueueing (see wifiSniffer in main.cpp).
static uint8_t IRAM_ATTR applySeqMacBonus(uint8_t base) {
  int s = (int)base + CS_SEQ_MAC_PAIR;
  if (s > 100) s = 100;
  return (uint8_t)s;
}
