// fy_detect.h — Detection logic and pattern data for Flock-You
// Shared between firmware (main.cpp) and native unit tests.
// All functions are static to avoid linker conflicts.

#ifndef FY_DETECT_H
#define FY_DETECT_H

#include <string.h>
#include <stdint.h>

#ifndef ARDUINO
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <strings.h>  // strncasecmp, strcasecmp on POSIX
#include <stdio.h>
#endif

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

// Flock Safety — high-confidence OUIs (direct registration or exclusive use)
static const char* flock_mac_prefixes[] = {
    // FS Ext Battery devices
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    // Flock WiFi devices
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea",
    // Flock Safety (direct IEEE registration)
    "b4:1e:52"
};

// Flock Safety contract manufacturers — lower confidence alone.
// These OUIs belong to Liteon Technology and USI, which produce Flock
// hardware but also ship unrelated consumer/enterprise devices.
static const char* flock_mfr_mac_prefixes[] = {
    "f4:6a:dd", "f8:a2:d6", "e0:0a:f6", "00:f4:8d", "d0:39:57",
    "e8:d0:fc"
};

// SoundThinking (formerly ShotSpotter) — acoustic gunshot detection sensors.
static const char* soundthinking_mac_prefixes[] = {
    "d4:11:d6"
};

// BLE device name patterns (matched case-insensitive substring)
static const char* device_name_patterns[] = {
    "FS Ext Battery",
    "Penguin",
    "Flock",
    "Pigvision"
};

// BLE Manufacturer Company IDs
// Source: wgreenberg/flock-you — XUNTONG ID associated with Flock Safety devices
static const uint16_t ble_manufacturer_ids[] = {
    0x09C8   // XUNTONG
};

// ============================================================================
// RAVEN SURVEILLANCE DEVICE UUID PATTERNS
// ============================================================================

#define RAVEN_DEVICE_INFO_SERVICE   "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE           "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE         "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE       "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE        "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE         "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE    "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE  "00001819-0000-1000-8000-00805f9b34fb"

static const char* raven_service_uuids[] = {
    RAVEN_DEVICE_INFO_SERVICE,
    RAVEN_GPS_SERVICE,
    RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE,
    RAVEN_UPLOAD_SERVICE,
    RAVEN_ERROR_SERVICE,
    RAVEN_OLD_HEALTH_SERVICE,
    RAVEN_OLD_LOCATION_SERVICE
};

// ============================================================================
// DETECTION STORAGE
// ============================================================================

#define MAX_DETECTIONS 200

struct FYDetection {
    char mac[18];
    char name[48];
    int rssi;
    char method[32];
    unsigned long firstSeen;
    unsigned long lastSeen;
    int count;
    bool isRaven;
    char ravenFW[16];
    double gpsLat;
    double gpsLon;
    float gpsAcc;
    bool hasGPS;
};

// ============================================================================
// DETECTION HELPERS
// ============================================================================

static bool checkFlockMAC(const char* mac_str) {
    for (size_t i = 0; i < sizeof(flock_mac_prefixes)/sizeof(flock_mac_prefixes[0]); i++) {
        if (strncasecmp(mac_str, flock_mac_prefixes[i], 8) == 0) return true;
    }
    return false;
}

static bool checkFlockMfrMAC(const char* mac_str) {
    for (size_t i = 0; i < sizeof(flock_mfr_mac_prefixes)/sizeof(flock_mfr_mac_prefixes[0]); i++) {
        if (strncasecmp(mac_str, flock_mfr_mac_prefixes[i], 8) == 0) return true;
    }
    return false;
}

static bool checkSoundThinkingMAC(const char* mac_str) {
    for (size_t i = 0; i < sizeof(soundthinking_mac_prefixes)/sizeof(soundthinking_mac_prefixes[0]); i++) {
        if (strncasecmp(mac_str, soundthinking_mac_prefixes[i], 8) == 0) return true;
    }
    return false;
}

static bool checkDeviceName(const char* name) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
        if (strcasestr(name, device_name_patterns[i])) return true;
    }
    return false;
}

static bool checkManufacturerID(uint16_t id) {
    for (size_t i = 0; i < sizeof(ble_manufacturer_ids)/sizeof(ble_manufacturer_ids[0]); i++) {
        if (ble_manufacturer_ids[i] == id) return true;
    }
    return false;
}

// ============================================================================
// RAVEN DETECTION (hardware-independent)
// ============================================================================

// Check a list of UUID strings against known Raven service UUIDs.
static bool checkRavenUUIDFromStrings(const char** uuids, int count, char* out_uuid) {
    for (int i = 0; i < count; i++) {
        for (size_t j = 0; j < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); j++) {
            if (strcasecmp(uuids[i], raven_service_uuids[j]) == 0) {
                if (out_uuid) strncpy(out_uuid, uuids[i], 40);
                return true;
            }
        }
    }
    return false;
}

// Estimate Raven firmware version from which service categories are present.
static const char* estimateRavenFWFromFlags(bool has_new_gps, bool has_old_loc, bool has_power) {
    if (has_old_loc && !has_new_gps) return "1.1.x";
    if (has_new_gps && !has_power)   return "1.2.x";
    if (has_new_gps && has_power)    return "1.3.x";
    return "?";
}

#endif // FY_DETECT_H
