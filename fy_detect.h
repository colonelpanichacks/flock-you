// fy_detect.h — Detection pattern data and pure matching logic for flock-you-esp32
//
// This header is shared between the firmware (main.cpp, compiled with the ESP32
// Arduino toolchain) and the native unit-test build (pio test -e native, compiled
// with the host GCC/Clang).  It must not include any Arduino or ESP-IDF headers.
//
// Sources:
//   OUI lists   — @NitekryDPaul (original 30), Michael/DeFlockJoplin (82:6b:f2),
//                 dougborg/PR#39 (b4:1e:52 direct reg, FS Ext Battery, e0:0a:f6 mfr)
//   BLE mfr-ID  — wgreenberg/flock-you (0x09C8 XUNTONG)
//   Raven UUIDs — GainSec research (8 full 128-bit service UUIDs)
//   SoundThinking — avenstewart/PR#39 (d4:11:d6 / formerly ShotSpotter)

#ifndef FY_DETECT_H
#define FY_DETECT_H

#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifndef ARDUINO
  // POSIX host build (pio test -e native) — need strcasecmp / strncasecmp
  #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
  #endif
  #include <strings.h>   // strncasecmp, strcasecmp
  #include <stdio.h>
#endif

// ============================================================================
// OUI TABLE A — HIGH-CONFIDENCE Flock Safety
// ============================================================================
// These OUIs are either directly registered to Flock Safety or have been
// exclusively observed on confirmed Flock ALPR hardware through field testing.
// An OUI-A match alone warrants a loud alert and a confidence score of ≥40.

static const char* fy_oui_high[] = {
  // Flock WiFi cameras — @NitekryDPaul promiscuous-mode dataset (30 OUIs)
  "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
  "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
  "94:08:53", "e4:aa:ea", "24:b2:b9",
  "b8:1e:a4", "70:08:94", "58:8e:81", "ec:1b:bd", "3c:71:bf",
  "58:00:e3", "90:35:ea", "5c:93:a2", "64:6e:69", "48:27:ea",
  "a4:cf:12", "e0:4f:43",
  // DeFlockJoplin — 12th camera found via wildcard-probe field test (PR#39)
  "82:6b:f2",
  // Flock Safety direct IEEE assignment (dougborg/PR#39)
  "b4:1e:52",
  // FS Ext Battery device series (dougborg/PR#39)
  "04:0d:84", "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9"
};
#define FY_OUI_HIGH_COUNT (sizeof(fy_oui_high)/sizeof(fy_oui_high[0]))

// ============================================================================
// OUI TABLE B — CONTRACT-MANUFACTURER OUIs  (Liteon Technology / USI)
// ============================================================================
// These OUIs belong to contract manufacturers that produce Flock hardware but
// ALSO ship unrelated consumer and enterprise devices.  An OUI-B match alone
// is LOW confidence (~20 pts); do not chirp or flash without a corroborating
// signal (SSID, BLE correlation, wildcard probe, or sequential-MAC pair).

static const char* fy_oui_mfr[] = {
  "f4:6a:dd",   // Liteon Technology
  "f8:a2:d6",   // Liteon Technology
  "00:f4:8d",   // Universal Scientific Industrial (USI)
  "d0:39:57",   // USI
  "e8:d0:fc",   // USI
  "e0:0a:f6"    // USI (added dougborg/PR#39)
};
#define FY_OUI_MFR_COUNT (sizeof(fy_oui_mfr)/sizeof(fy_oui_mfr[0]))

// ============================================================================
// OUI TABLE C — SOUNDTHINKING / SHOTSPOTTER
// ============================================================================
// SoundThinking (formerly ShotSpotter) manufactures acoustic gunshot-detection
// sensors that are frequently co-deployed with Flock ALPR systems.  Detecting
// one indicates a surveillance-capable installation; method = "soundthinking".

static const char* fy_oui_soundthinking[] = {
  "d4:11:d6"
};
#define FY_OUI_ST_COUNT (sizeof(fy_oui_soundthinking)/sizeof(fy_oui_soundthinking[0]))

// ============================================================================
// BLE DEVICE NAME PATTERNS  (case-insensitive substring match)
// ============================================================================

static const char* fy_ble_names[] = {
  "FS Ext Battery",
  "Penguin",
  "Flock",
  "Pigvision",
  "Raven",
  nullptr
};

// ============================================================================
// BLE MANUFACTURER COMPANY ID
// ============================================================================
// Source: wgreenberg/flock-you — 0x09C8 is the XUNTONG BT company ID observed
// in Flock Safety BLE advertisement packets during field testing.
// NOTE: Earlier firmware used 0x05A7 (incorrect).  0x09C8 is the confirmed ID.

static const uint16_t fy_ble_mfr_ids[] = {
  0x09C8   // XUNTONG Technology Co., Ltd  (confirmed Flock Safety BLE)
};
#define FY_BLE_MFR_COUNT (sizeof(fy_ble_mfr_ids)/sizeof(fy_ble_mfr_ids[0]))

// ============================================================================
// RAVEN SURVEILLANCE DEVICE SERVICE UUIDs  (full 128-bit, GainSec research)
// ============================================================================
// Raven is a combined ALPR + gunshot-detection platform sometimes co-deployed
// with Flock cameras.  These GATT service UUIDs were identified by GainSec.

#define FY_RAVEN_DEVICE_INFO  "0000180a-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_GPS          "00003100-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_POWER        "00003200-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_NETWORK      "00003300-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_UPLOAD       "00003400-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_ERROR        "00003500-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_OLD_HEALTH   "00001809-0000-1000-8000-00805f9b34fb"
#define FY_RAVEN_OLD_LOCATION "00001819-0000-1000-8000-00805f9b34fb"

static const char* fy_raven_uuids[] = {
  FY_RAVEN_DEVICE_INFO,
  FY_RAVEN_GPS,
  FY_RAVEN_POWER,
  FY_RAVEN_NETWORK,
  FY_RAVEN_UPLOAD,
  FY_RAVEN_ERROR,
  FY_RAVEN_OLD_HEALTH,
  FY_RAVEN_OLD_LOCATION
};
#define FY_RAVEN_UUID_COUNT (sizeof(fy_raven_uuids)/sizeof(fy_raven_uuids[0]))

// ============================================================================
// PURE MATCHING FUNCTIONS  (no Arduino / ESP-IDF deps — testable on host)
// ============================================================================

// Returns true if mac_str (e.g. "70:c9:4e:xx:xx:xx") starts with a known
// high-confidence Flock Safety OUI.  First 8 bytes (xx:xx:xx) are compared
// case-insensitively.
static inline bool fyCheckFlockHighMAC(const char* mac_str) {
  if (!mac_str) return false;
  for (size_t i = 0; i < FY_OUI_HIGH_COUNT; i++) {
    if (strncasecmp(mac_str, fy_oui_high[i], 8) == 0) return true;
  }
  return false;
}

// Returns true if mac_str starts with a contract-manufacturer OUI.
static inline bool fyCheckFlockMfrMAC(const char* mac_str) {
  if (!mac_str) return false;
  for (size_t i = 0; i < FY_OUI_MFR_COUNT; i++) {
    if (strncasecmp(mac_str, fy_oui_mfr[i], 8) == 0) return true;
  }
  return false;
}

// Returns true if mac_str starts with a SoundThinking/ShotSpotter OUI.
static inline bool fyCheckSoundThinkingMAC(const char* mac_str) {
  if (!mac_str) return false;
  for (size_t i = 0; i < FY_OUI_ST_COUNT; i++) {
    if (strncasecmp(mac_str, fy_oui_soundthinking[i], 8) == 0) return true;
  }
  return false;
}

// Returns true if 'name' (case-insensitive substring search) matches any
// known Flock/Raven BLE device name pattern.
static inline bool fyCheckBLEName(const char* name) {
  if (!name || !name[0]) return false;
  // Build a lower-case copy of 'name' (max 64 chars)
  char low[64]; size_t i = 0;
  for (; i < 63 && name[i]; i++) {
    char c = name[i];
    low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  low[i] = '\0';
  for (const char** kw = fy_ble_names; *kw; kw++) {
    // Walk the keyword in lower-case and use strstr on the lowered name
    char kwl[64]; size_t j = 0;
    for (; j < 63 && (*kw)[j]; j++) {
      char c = (*kw)[j];
      kwl[j] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    kwl[j] = '\0';
    if (strstr(low, kwl)) return true;
  }
  return false;
}

// Returns true if 'id' matches a known Flock Safety BLE manufacturer company ID.
static inline bool fyCheckBLEMfrID(uint16_t id) {
  for (size_t i = 0; i < FY_BLE_MFR_COUNT; i++) {
    if (fy_ble_mfr_ids[i] == id) return true;
  }
  return false;
}

// ============================================================================
// RAVEN UUID MATCHING  (hardware-independent, string-based)
// ============================================================================

// Check an array of UUID strings against the known Raven service UUID list.
// Returns true on first match; sets out_uuid (up to 40 chars) if provided.
// uuids[] must be lowercase or the comparison will still work because
// strcasecmp is used.
static inline bool fyCheckRavenUUIDFromStrings(const char** uuids, int count,
                                               char* out_uuid) {
  if (!uuids || count <= 0) return false;
  for (int i = 0; i < count; i++) {
    if (!uuids[i]) continue;
    for (size_t j = 0; j < FY_RAVEN_UUID_COUNT; j++) {
      if (strcasecmp(uuids[i], fy_raven_uuids[j]) == 0) {
        if (out_uuid) strncpy(out_uuid, uuids[i], 40);
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// RAVEN FIRMWARE VERSION ESTIMATION
// ============================================================================
// Estimate Raven firmware version from which service UUID categories are present.
// has_new_gps  = FY_RAVEN_GPS (0x3100) was advertised
// has_old_loc  = FY_RAVEN_OLD_LOCATION (0x1819) was advertised
// has_power    = FY_RAVEN_POWER (0x3200) was advertised

static inline const char* fyEstimateRavenFW(bool has_new_gps,
                                             bool has_old_loc,
                                             bool has_power) {
  if (has_old_loc && !has_new_gps) return "1.1.x";
  if (has_new_gps && !has_power)   return "1.2.x";
  if (has_new_gps && has_power)    return "1.3.x";
  return "?";
}

#endif // FY_DETECT_H
