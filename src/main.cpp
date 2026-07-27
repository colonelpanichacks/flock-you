// ============================================================================
// FLOCK-YOU: Surveillance Device Detector with Web Dashboard
// ============================================================================
// Detection methods (BLE only - WiFi radio used for AP):
//   1. BLE MAC prefix matching (known Flock Safety OUIs)
//   2. BLE device name pattern matching (case-insensitive substring)
//   3. BLE manufacturer company ID matching (0x09C8 XUNTONG) [from wgreenberg]
//   4. Raven gunshot detector service UUID matching
//   5. Raven firmware version estimation from service UUID patterns
//
// WiFi AP "flockyou" / "flockyou123" serves web dashboard at 192.168.4.1
// All detections stored in memory, exportable as JSON or CSV
// Optional WiFi STA connection for future features
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_wifi.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define BUZZER_PIN 3

// Audio
#define LOW_FREQ 200
#define HIGH_FREQ 800
#define DETECT_FREQ 1000
#define HEARTBEAT_FREQ 600
#define BOOT_BEEP_DURATION 300
#define DETECT_BEEP_DURATION 150
#define HEARTBEAT_DURATION 100

// BLE scanning (mutable — companion mode increases scan duty cycle)
static int fyBleScanDuration = 2;              // seconds per scan
static unsigned long fyBleScanInterval = 3000; // ms between scans

// Detection storage
#define MAX_DETECTIONS 200

// WiFi AP credentials
#define FY_AP_SSID "flockyou"
#define FY_AP_PASS "flockyou123"

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

// MAC address prefixes (OUIs)

// Flock Safety — high-confidence OUIs (direct registration or exclusive use)
static const char* flock_mac_prefixes[] = {
    // FS Ext Battery devices
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    // Flock WiFi devices
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea",
    // Flock Safety (direct IEEE registration)
    "b4:1e:52",
    // Field-observed via the DeFlock community. Same confidence tier as the
    // entries above: seen on Flock deployments rather than inferred from a
    // registry.
    "b8:35:32", "c0:35:32", "24:b2:b9", "e0:4f:43", "b8:1e:a4",
    "70:08:94", "3c:71:bf", "58:00:e3", "5c:93:a2", "64:6e:69",
    "48:27:ea", "a4:cf:12",
    // Contributed by Michael / DeFlockJoplin, attributed to a Raven acoustic
    // sensor. Locally administered (bit 1 of the first byte is set), so it is
    // not IEEE-registered and will not appear in oui.txt. Kept here rather
    // than under soundthinking_mac_prefixes: Raven is Flock hardware, not
    // ShotSpotter/SoundThinking.
    "82:6b:f2"
};

// Flock Safety contract manufacturers — lower confidence alone.
// These OUIs belong to Liteon Technology and USI (Universal Scientific
// Industrial), which produce Flock hardware but also ship unrelated
// consumer/enterprise devices. MAC match alone may be a false positive.
static const char* flock_mfr_mac_prefixes[] = {
    "f4:6a:dd", "f8:a2:d6", "e0:0a:f6", "00:f4:8d", "d0:39:57",
    "e8:d0:fc"
};

// SoundThinking (formerly ShotSpotter) — acoustic gunshot detection sensors.
// d4:11:d6 is registered to SoundThinking in the IEEE OUI database.
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
// Source: wgreenberg/flock-you - XUNTONG ID associated with Flock Safety devices
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
    // GPS from phone (wardriving)
    double gpsLat;
    double gpsLon;
    float gpsAcc;
    bool hasGPS;
};

static FYDetection fyDet[MAX_DETECTIONS];
static int fyDetCount = 0;
static SemaphoreHandle_t fyMutex = NULL;      // guards fyDet[] + fyDetCount
static SemaphoreHandle_t fyGPSMutex = NULL;   // guards fyGPS* globals

// ============================================================================
// GLOBALS
// ============================================================================

static bool fyBuzzerOn = true;
static unsigned long fyLastBleScan = 0;
static bool fyTriggered = false;
static bool fyDeviceInRange = false;
static unsigned long fyLastDetTime = 0;
static unsigned long fyLastHB = 0;
static NimBLEScan* fyBLEScan = NULL;
static AsyncWebServer fyServer(80);

// BLE GATT server (DeFlock app connectivity)
#define FY_SERVICE_UUID     "a1b2c3d4-e5f6-7890-abcd-ef0123456789"
#define FY_TX_CHAR_UUID     "a1b2c3d4-e5f6-7890-abcd-ef01234567aa"
static NimBLEServer*         fyBLEServer = NULL;
static NimBLECharacteristic* fyTxChar    = NULL;
static volatile bool         fyBLEClientConnected = false;
static volatile uint16_t     fyNegotiatedMTU = 23;

// Serial host detection (USB heartbeat from DeFlock desktop)
static volatile bool         fySerialHostConnected = false;
static unsigned long         fyLastSerialHeartbeat = 0;
#define FY_SERIAL_TIMEOUT_MS 5000

// Deferred companion mode switch — BLE callbacks set this flag,
// loop() applies the WiFi/scan changes in the Arduino task context.
static volatile bool         fyCompanionChangePending = false;

// Phone GPS state (updated via browser Geolocation API -> /api/gps)
static double fyGPSLat = 0;
static double fyGPSLon = 0;
static float  fyGPSAcc = 0;
static bool   fyGPSValid = false;
static unsigned long fyGPSLastUpdate = 0;
#define GPS_STALE_MS 30000  // GPS considered stale after 30s without update

// Session persistence (SPIFFS)
#define FY_SESSION_FILE  "/session.json"
#define FY_PREV_FILE     "/prev_session.json"
#define FY_SAVE_INTERVAL 15000  // Auto-save every 15 seconds (prevent data loss on quick power-cycle)
static unsigned long fyLastSave = 0;
static int fyLastSaveCount = 0;  // Track changes to avoid unnecessary writes
static bool fySpiffsReady = false;

// ============================================================================
// AUDIO SYSTEM
// ============================================================================

static void fyBeep(int freq, int dur) {
    if (!fyBuzzerOn) return;
    tone(BUZZER_PIN, freq, dur);
    delay(dur + 50);
}

// Crow caw: harsh descending sweep with warble texture
static void fyCaw(int startFreq, int endFreq, int durationMs, int warbleHz) {
    if (!fyBuzzerOn) return;
    int steps = durationMs / 8;  // 8ms per step
    float fStep = (float)(endFreq - startFreq) / steps;
    for (int i = 0; i < steps; i++) {
        int f = startFreq + (int)(fStep * i);
        // Add warble: oscillate frequency +/- for raspy texture
        if (warbleHz > 0 && (i % 3 == 0)) {
            f += ((i % 6 < 3) ? warbleHz : -warbleHz);
        }
        if (f < 100) f = 100;
        tone(BUZZER_PIN, f, 10);
        delay(8);
    }
    noTone(BUZZER_PIN);
}

static void fyBootBeep() {
    printf("[FLOCK-YOU] Boot sound (buzzer %s)\n", fyBuzzerOn ? "ON" : "OFF");
    if (!fyBuzzerOn) return;

    // === CROW CALL SEQUENCE ===
    // Caw 1: sharp descending caw
    fyCaw(850, 380, 180, 40);
    delay(100);

    // Caw 2: slightly lower, shorter
    fyCaw(780, 350, 150, 50);
    delay(100);

    // Caw 3: longer trailing caw with more rasp
    fyCaw(820, 280, 220, 60);
    delay(80);

    // Quick staccato ending "kk-kk"
    tone(BUZZER_PIN, 600, 25); delay(40);
    tone(BUZZER_PIN, 550, 25); delay(40);
    noTone(BUZZER_PIN);

    printf("[FLOCK-YOU] *caw caw caw*\n");
}

static void fyDetectBeep() {
    printf("[FLOCK-YOU] Detection alert!\n");
    if (!fyBuzzerOn) return;
    // Alarm crow: two sharp ascending chirps then a caw
    fyCaw(400, 900, 100, 30);   // rising alarm chirp
    delay(60);
    fyCaw(450, 950, 100, 30);   // second chirp, higher
    delay(60);
    fyCaw(900, 350, 200, 50);   // descending caw
}

static void fyHeartbeat() {
    if (!fyBuzzerOn) return;
    // Soft double coo - like a distant crow
    fyCaw(500, 400, 80, 20);
    delay(120);
    fyCaw(480, 380, 80, 20);
}

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
// RAVEN UUID DETECTION
// ============================================================================

static bool checkRavenUUID(NimBLEAdvertisedDevice* device, char* out_uuid = nullptr) {
    if (!device || !device->haveServiceUUID()) return false;
    int count = device->getServiceUUIDCount();
    if (count == 0) return false;
    for (int i = 0; i < count; i++) {
        NimBLEUUID svc = device->getServiceUUID(i);
        std::string str = svc.toString();
        for (size_t j = 0; j < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); j++) {
            if (strcasecmp(str.c_str(), raven_service_uuids[j]) == 0) {
                if (out_uuid) strncpy(out_uuid, str.c_str(), 40);
                return true;
            }
        }
    }
    return false;
}

static const char* estimateRavenFW(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return "?";
    bool has_new_gps = false, has_old_loc = false, has_power = false;
    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string u = device->getServiceUUID(i).toString();
        if (strcasecmp(u.c_str(), RAVEN_GPS_SERVICE) == 0)          has_new_gps = true;
        if (strcasecmp(u.c_str(), RAVEN_OLD_LOCATION_SERVICE) == 0) has_old_loc = true;
        if (strcasecmp(u.c_str(), RAVEN_POWER_SERVICE) == 0)        has_power = true;
    }
    if (has_old_loc && !has_new_gps) return "1.1.x";
    if (has_new_gps && !has_power)   return "1.2.x";
    if (has_new_gps && has_power)    return "1.3.x";
    return "?";
}

// ============================================================================
// GPS HELPERS (mutex-protected snapshot pattern)
// ============================================================================

// Fast advisory check — safe lock-free (for UI/stats only, don't trust for writes)
static bool fyGPSIsFresh() {
    return fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
}

// Atomic snapshot: returns true and fills out-params if GPS is fresh & valid.
// Safe to call from BLE callback context — never races with producer.
static bool fyGPSSnapshot(double& lat, double& lon, float& acc) {
    if (!fyGPSMutex) return false;
    if (xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
    bool fresh = fyGPSValid && (millis() - fyGPSLastUpdate < GPS_STALE_MS);
    if (fresh) { lat = fyGPSLat; lon = fyGPSLon; acc = fyGPSAcc; }
    xSemaphoreGive(fyGPSMutex);
    return fresh;
}

// Atomic GPS publish (from phone via /api/gps or from companion app)
static void fyGPSUpdate(double lat, double lon, float acc) {
    if (!fyGPSMutex) return;
    if (xSemaphoreTake(fyGPSMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    fyGPSLat = lat;
    fyGPSLon = lon;
    fyGPSAcc = acc;
    fyGPSValid = true;
    fyGPSLastUpdate = millis();
    xSemaphoreGive(fyGPSMutex);
}

// Stamp a detection with current GPS if available (used at first-sight and re-sight)
static void fyAttachGPS(FYDetection& d) {
    double lat, lon;
    float acc;
    if (fyGPSSnapshot(lat, lon, acc)) {
        d.hasGPS = true;
        d.gpsLat = lat;
        d.gpsLon = lon;
        d.gpsAcc = acc;
    }
}

// Periodic: back-fill GPS on detections recorded before a fix was available.
// Runs in main loop — MUST NOT be called from BLE callback (takes fyMutex).
static void fyBackfillGPS() {
    double lat, lon;
    float acc;
    if (!fyGPSSnapshot(lat, lon, acc)) return;
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
    int filled = 0;
    for (int i = 0; i < fyDetCount; i++) {
        if (!fyDet[i].hasGPS) {
            fyDet[i].hasGPS = true;
            fyDet[i].gpsLat = lat;
            fyDet[i].gpsLon = lon;
            fyDet[i].gpsAcc = acc;
            filled++;
        }
    }
    xSemaphoreGive(fyMutex);
    if (filled) printf("[FLOCK-YOU] GPS backfilled %d detection(s)\n", filled);
}

// ============================================================================
// CRC32 (IEEE 802.3) — for session file integrity
// ============================================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *data++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
    return ~crc;
}

// ============================================================================
// DETECTION MANAGEMENT
// ============================================================================

static int fyAddDetection(const char* mac, const char* name, int rssi,
                          const char* method, bool isRaven = false,
                          const char* ravenFW = "") {
    if (!fyMutex || xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

    // Update existing by MAC
    for (int i = 0; i < fyDetCount; i++) {
        if (strcasecmp(fyDet[i].mac, mac) == 0) {
            fyDet[i].count++;
            fyDet[i].lastSeen = millis();
            fyDet[i].rssi = rssi;
            if (name && name[0]) {
                strncpy(fyDet[i].name, name, sizeof(fyDet[i].name) - 1);
            }
            // Update GPS on every re-sighting (captures movement)
            fyAttachGPS(fyDet[i]);
            xSemaphoreGive(fyMutex);
            return i;
        }
    }

    // Add new
    if (fyDetCount < MAX_DETECTIONS) {
        FYDetection& d = fyDet[fyDetCount];
        memset(&d, 0, sizeof(d));
        strncpy(d.mac, mac, sizeof(d.mac) - 1);
        // Sanitize name for JSON safety
        if (name) {
            for (int j = 0; j < (int)sizeof(d.name) - 1 && name[j]; j++) {
                d.name[j] = (name[j] == '"' || name[j] == '\\') ? '_' : name[j];
            }
        }
        d.rssi = rssi;
        strncpy(d.method, method, sizeof(d.method) - 1);
        d.firstSeen = millis();
        d.lastSeen = millis();
        d.count = 1;
        d.isRaven = isRaven;
        strncpy(d.ravenFW, ravenFW ? ravenFW : "", sizeof(d.ravenFW) - 1);
        // Attach GPS from phone
        fyAttachGPS(d);
        int idx = fyDetCount++;
        xSemaphoreGive(fyMutex);
        return idx;
    }

    xSemaphoreGive(fyMutex);
    return -1;
}

// ============================================================================
// BLE GATT SERVER (DeFlock companion connectivity)
// ============================================================================

class FYServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        fyBLEClientConnected = true;
        fyCompanionChangePending = true;
    }
    void onDisconnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        fyBLEClientConnected = false;
        fyNegotiatedMTU = 23;
        NimBLEDevice::startAdvertising();
        fyCompanionChangePending = true;
    }
    void onMTUChange(uint16_t mtu, ble_gap_conn_desc* desc) override {
        fyNegotiatedMTU = mtu;
        printf("[FLOCK-YOU] MTU negotiated: %u\n", mtu);
    }
};

static void fySendBLE(const char* data, size_t len) {
    if (!fyBLEClientConnected || !fyTxChar) return;
    uint16_t chunkSize = fyNegotiatedMTU - 3;
    if (chunkSize < 1) chunkSize = 1;
    if (len <= chunkSize) {
        fyTxChar->setValue((const uint8_t*)data, len);
        fyTxChar->notify();
    } else {
        size_t offset = 0;
        while (offset < len) {
            size_t remaining = len - offset;
            size_t send = remaining < chunkSize ? remaining : chunkSize;
            fyTxChar->setValue((const uint8_t*)(data + offset), send);
            fyTxChar->notify();
            offset += send;
        }
    }
}

// ============================================================================
// COMPANION MODE (WiFi AP vs BLE/serial)
// ============================================================================

static void fyOnCompanionChange() {
    if (fyBLEClientConnected || fySerialHostConnected) {
        // Companion mode — disable WiFi AP, boost BLE scanning
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        fyBleScanDuration = 3;
        printf("[FLOCK-YOU] Companion mode: WiFi AP OFF, scan duration %ds\n",
               fyBleScanDuration);
    } else {
        // Standalone mode — re-enable WiFi AP and web dashboard
        WiFi.mode(WIFI_AP);
        delay(100);
        WiFi.softAP(FY_AP_SSID, FY_AP_PASS);
        fyBleScanDuration = 2;
        printf("[FLOCK-YOU] Standalone mode: WiFi AP ON (%s), scan duration %ds\n",
               FY_AP_SSID, fyBleScanDuration);
    }
}

// ============================================================================
// BLE SCANNING
// ============================================================================

class FYBLECallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        NimBLEAddress addr = dev->getAddress();
        std::string addrStr = addr.toString();

        // Extract MAC prefix string for OUI checks
        char macPrefix[9];
        snprintf(macPrefix, sizeof(macPrefix), "%.8s", addrStr.c_str());

        int rssi = dev->getRSSI();
        std::string name = dev->haveName() ? dev->getName() : "";

        bool detected = false;
        bool highConfidence = true;
        const char* method = "";
        bool isRaven = false;
        const char* ravenFW = "";

        // 1. Check Flock Safety direct OUIs (high confidence)
        if (checkFlockMAC(macPrefix)) {
            detected = true;
            method = "mac_prefix";
        }

        // 2. Check SoundThinking/ShotSpotter OUIs (high confidence)
        if (!detected && checkSoundThinkingMAC(macPrefix)) {
            detected = true;
            method = "mac_prefix_soundthinking";
        }

        // 3. Check Flock contract manufacturer OUIs (low confidence)
        if (!detected && checkFlockMfrMAC(macPrefix)) {
            detected = true;
            method = "mac_prefix_mfr";
            highConfidence = false;
        }

        // 4. Check BLE device name patterns
        if (!detected && !name.empty() && checkDeviceName(name.c_str())) {
            detected = true;
            method = "device_name";
        }

        // 5. Check BLE manufacturer company IDs (from wgreenberg/flock-you)
        if (!detected) {
            for (int i = 0; i < (int)dev->getManufacturerDataCount(); i++) {
                std::string data = dev->getManufacturerData(i);
                if (data.size() >= 2) {
                    uint16_t code = ((uint16_t)(uint8_t)data[1] << 8) |
                                     (uint16_t)(uint8_t)data[0];
                    if (checkManufacturerID(code)) {
                        detected = true;
                        method = "ble_mfr_id";
                        break;
                    }
                }
            }
        }

        // 6. Check Raven gunshot detector service UUIDs
        if (!detected) {
            char detUUID[41] = {0};
            if (checkRavenUUID(dev, detUUID)) {
                detected = true;
                method = "raven_uuid";
                isRaven = true;
                ravenFW = estimateRavenFW(dev);
            }
        }

        if (detected) {
            int idx = fyAddDetection(addrStr.c_str(), name.c_str(), rssi,
                                     method, isRaven, ravenFW);

            // Human-readable log
            printf("[FLOCK-YOU] DETECTED: %s %s RSSI:%d [%s] count:%d\n",
                   addrStr.c_str(), name.c_str(), rssi, method,
                   idx >= 0 ? fyDet[idx].count : 0);

            // JSON output — build into buffer for serial + BLE
            // Use atomic snapshot to avoid races with /api/gps writer
            char gpsBuf[80] = "";
            {
                double sLat, sLon; float sAcc;
                if (fyGPSSnapshot(sLat, sLon, sAcc)) {
                    snprintf(gpsBuf, sizeof(gpsBuf),
                        ",\"gps\":{\"latitude\":%.8f,\"longitude\":%.8f,\"accuracy\":%.1f}",
                        sLat, sLon, sAcc);
                }
            }
            char jsonBuf[512];
            int jsonLen = snprintf(jsonBuf, sizeof(jsonBuf),
                "{\"event\":\"detection\",\"detection_method\":\"%s\","
                "\"protocol\":\"bluetooth_le\",\"mac_address\":\"%s\","
                "\"device_name\":\"%s\",\"rssi\":%d,"
                "\"is_raven\":%s,\"raven_fw\":\"%s\"%s}",
                method, addrStr.c_str(), name.c_str(), rssi,
                isRaven ? "true" : "false", isRaven ? ravenFW : "", gpsBuf);
            printf("%s\n", jsonBuf);
            // Append newline for BLE framing and send
            if (jsonLen > 0 && jsonLen < (int)sizeof(jsonBuf) - 1) {
                jsonBuf[jsonLen] = '\n';
                fySendBLE(jsonBuf, jsonLen + 1);
            }

            if (!fyTriggered && highConfidence) {
                fyTriggered = true;
                fyDetectBeep();
            }
            if (highConfidence) {
                fyDeviceInRange = true;
                fyLastDetTime = millis();
                fyLastHB = millis();
            }
        }
    }
};

// ============================================================================
// JSON HELPER
// ============================================================================

static void writeDetectionsJSON(AsyncResponseStream *resp) {
    resp->print("[");
    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < fyDetCount; i++) {
            if (i > 0) resp->print(",");
            resp->printf(
                "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
                "\"first\":%lu,\"last\":%lu,\"count\":%d,"
                "\"raven\":%s,\"fw\":\"%s\"",
                fyDet[i].mac, fyDet[i].name, fyDet[i].rssi, fyDet[i].method,
                fyDet[i].firstSeen, fyDet[i].lastSeen, fyDet[i].count,
                fyDet[i].isRaven ? "true" : "false", fyDet[i].ravenFW);
            // Append GPS if present
            if (fyDet[i].hasGPS) {
                resp->printf(",\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}",
                    fyDet[i].gpsLat, fyDet[i].gpsLon, fyDet[i].gpsAcc);
            }
            resp->print("}");
        }
        xSemaphoreGive(fyMutex);
    }
    resp->print("]");
}

// ============================================================================
// SESSION PERSISTENCE (SPIFFS) — bulletproof envelope format
// ============================================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},{"mac":...},...]    (exactly B bytes, CRC32 == X)
//
// Atomic write procedure:
//   1. Compute size+CRC over the detections payload (pass 1, under fyMutex)
//   2. Write envelope header + payload to /session.tmp (pass 2, under same lock)
//   3. Remove /session.json
//   4. Rename /session.tmp → /session.json (with copy+delete fallback)
//
// Recovery: if /session.json is missing or CRC-invalid, fall back to /session.tmp.

#define FY_SESSION_TMP "/session.tmp"

// Serialize a single detection to `dst`. Returns bytes written (0 on overflow).
static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
    int n;
    if (d.hasGPS) {
        n = snprintf(dst, cap,
            "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
            "\"first\":%lu,\"last\":%lu,\"count\":%d,"
            "\"raven\":%s,\"fw\":\"%s\","
            "\"gps\":{\"lat\":%.8f,\"lon\":%.8f,\"acc\":%.1f}}",
            d.mac, d.name, d.rssi, d.method,
            d.firstSeen, d.lastSeen, d.count,
            d.isRaven ? "true" : "false", d.ravenFW,
            d.gpsLat, d.gpsLon, d.gpsAcc);
    } else {
        n = snprintf(dst, cap,
            "{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"method\":\"%s\","
            "\"first\":%lu,\"last\":%lu,\"count\":%d,"
            "\"raven\":%s,\"fw\":\"%s\"}",
            d.mac, d.name, d.rssi, d.method,
            d.firstSeen, d.lastSeen, d.count,
            d.isRaven ? "true" : "false", d.ravenFW);
    }
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// Pass 1: compute exact payload size + CRC32 without allocating.
// Caller MUST hold fyMutex.
static uint32_t fyComputePayloadCRC(size_t& outBytes) {
    char line[512];
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

// Validate a session file envelope and its payload CRC. Returns true if intact.
static bool fyValidateSessionFile(const char* path) {
    if (!SPIFFS.exists(path)) return false;
    File f = SPIFFS.open(path, "r");
    if (!f) return false;

    String hdr = f.readStringUntil('\n');
    if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

    JsonDocument doc;
    if (deserializeJson(doc, hdr) != DeserializationError::Ok) { f.close(); return false; }
    if ((int)(doc["v"] | 0) != 1) { f.close(); return false; }
    size_t expectedBytes = (size_t)(doc["bytes"] | 0);
    uint32_t expectedCRC = 0;
    const char* crcStr = doc["crc"] | "";
    if (sscanf(crcStr, "%x", &expectedCRC) != 1) { f.close(); return false; }

    size_t bodyOffset = hdr.length() + 1;
    size_t fileSize = f.size();
    if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
    size_t actualBytes = fileSize - bodyOffset;
    if (actualBytes != expectedBytes) { f.close(); return false; }

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

// Copy src→dst in chunks. Returns true on success.
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

// Atomic rename: try SPIFFS rename first, fall back to copy+delete if rename fails.
static bool fyAtomicPromote(const char* src, const char* dst) {
    if (SPIFFS.rename(src, dst)) return true;
    if (!fySpiffsCopy(src, dst)) return false;
    SPIFFS.remove(src);
    return true;
}

static void fySaveSession() {
    if (!fySpiffsReady || !fyMutex) return;
    if (xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("[FLOCK-YOU] Save skipped: fyMutex busy\n");
        return;
    }

    // Pass 1: compute CRC + byte count
    size_t payloadBytes = 0;
    uint32_t crc = fyComputePayloadCRC(payloadBytes);
    int savedCount = fyDetCount;

    // Pass 2: write envelope + payload to tmp
    File f = SPIFFS.open(FY_SESSION_TMP, "w");
    if (!f) {
        xSemaphoreGive(fyMutex);
        printf("[FLOCK-YOU] Save failed: cannot open %s\n", FY_SESSION_TMP);
        return;
    }
    f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);

    char line[512];
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
    xSemaphoreGive(fyMutex);

    if (wrote != payloadBytes) {
        printf("[FLOCK-YOU] Save WARNING: wrote %u expected %u — aborting promote\n",
               (unsigned)wrote, (unsigned)payloadBytes);
        return;
    }

    if (!fyValidateSessionFile(FY_SESSION_TMP)) {
        printf("[FLOCK-YOU] Save verify FAILED — aborting promote (old session preserved)\n");
        return;
    }

    SPIFFS.remove(FY_SESSION_FILE);
    if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
        printf("[FLOCK-YOU] Promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
        return;
    }

    fyLastSaveCount = savedCount;
    printf("[FLOCK-YOU] Session saved: %d det, %u bytes, crc=0x%08lX\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

static void fyPromotePrevSession() {
    if (!fySpiffsReady) return;

    const char* source = nullptr;
    if (fyValidateSessionFile(FY_SESSION_FILE)) {
        source = FY_SESSION_FILE;
    } else if (fyValidateSessionFile(FY_SESSION_TMP)) {
        printf("[FLOCK-YOU] Main session corrupt/missing — recovering from tmp\n");
        source = FY_SESSION_TMP;
    } else {
        // Legacy fallback: old format (raw array, no envelope)
        if (SPIFFS.exists(FY_SESSION_FILE)) {
            File f = SPIFFS.open(FY_SESSION_FILE, "r");
            if (f && f.size() > 2) {
                int first = f.peek();
                f.close();
                if (first == '[') {
                    source = FY_SESSION_FILE;
                    printf("[FLOCK-YOU] Legacy-format session detected — promoting\n");
                }
            } else if (f) { f.close(); }
        }
    }

    if (!source) {
        if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
        if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
        printf("[FLOCK-YOU] No valid prior session to promote\n");
        return;
    }

    if (!fySpiffsCopy(source, FY_PREV_FILE)) {
        printf("[FLOCK-YOU] Failed to promote %s → %s\n", source, FY_PREV_FILE);
        return;
    }

    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

    File v = SPIFFS.open(FY_PREV_FILE, "r");
    size_t sz = v ? v.size() : 0;
    if (v) v.close();
    printf("[FLOCK-YOU] Prior session promoted from %s (%u bytes)\n", source, (unsigned)sz);
}

// Read prev_session as a raw detection JSON array (strips envelope header if present).
static void fyStreamPrevSessionBody(AsyncResponseStream* resp) {
    if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) { resp->print("[]"); return; }
    File f = SPIFFS.open(FY_PREV_FILE, "r");
    if (!f) { resp->print("[]"); return; }

    int first = f.peek();
    if (first == '{') f.readStringUntil('\n');

    uint8_t buf[256];
    int n;
    size_t streamed = 0;
    while ((n = f.read(buf, sizeof(buf))) > 0) {
        resp->write(buf, (size_t)n);
        streamed += (size_t)n;
    }
    f.close();
    if (streamed == 0) resp->print("[]");
}

// ============================================================================
// KML EXPORT
// ============================================================================

static void writeDetectionsKML(AsyncResponseStream *resp) {
    resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                "<name>Flock-You Detections</name>\n"
                "<description>Surveillance device detections with GPS</description>\n");

    // Detection pin style
    resp->print("<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                "<scale>1.0</scale></IconStyle></Style>\n"
                "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                "<scale>1.2</scale></IconStyle></Style>\n");

    if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < fyDetCount; i++) {
            FYDetection& d = fyDet[i];
            if (!d.hasGPS) continue;  // Skip detections without GPS
            resp->print("<Placemark>\n");
            resp->printf("<name>%s</name>\n", d.mac);
            resp->printf("<styleUrl>#%s</styleUrl>\n", d.isRaven ? "raven" : "det");
            resp->print("<description><![CDATA[");
            if (d.name[0]) resp->printf("<b>Name:</b> %s<br/>", d.name);
            resp->printf("<b>Method:</b> %s<br/>"
                         "<b>RSSI:</b> %d dBm<br/>"
                         "<b>Count:</b> %d<br/>",
                         d.method, d.rssi, d.count);
            if (d.isRaven) resp->printf("<b>Raven FW:</b> %s<br/>", d.ravenFW);
            resp->printf("<b>Accuracy:</b> %.1f m", d.gpsAcc);
            resp->print("]]></description>\n");
            resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                         d.gpsLon, d.gpsLat);
            resp->print("</Placemark>\n");
        }
        xSemaphoreGive(fyMutex);
    }
    resp->print("</Document>\n</kml>");
}

// ============================================================================
// DASHBOARD HTML
// ============================================================================

static const char FY_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>FLOCK-YOU</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{height:100%;overflow:hidden}
body{font-family:'Courier New',monospace;background:#0a0012;color:#e0e0e0;display:flex;flex-direction:column}
.hd{background:#1a0033;padding:10px 14px;border-bottom:2px solid #ec4899;flex-shrink:0}
.hd h1{font-size:22px;color:#ec4899;letter-spacing:3px}
.hd .sub{font-size:11px;color:#8b5cf6;margin-top:2px}
.st{display:flex;gap:8px;padding:8px 12px;background:rgba(139,92,246,.08);border-bottom:1px solid rgba(139,92,246,.19);flex-shrink:0}
.sc{flex:1;text-align:center;padding:6px;border:1px solid rgba(139,92,246,.25);border-radius:5px}
.sc .n{font-size:22px;font-weight:bold;color:#ec4899}
.sc .l{font-size:10px;color:#8b5cf6;margin-top:2px}
.tb{display:flex;border-bottom:1px solid #8b5cf6;flex-shrink:0}
.tb button{flex:1;padding:9px;text-align:center;cursor:pointer;color:#8b5cf6;border:none;background:none;font-family:inherit;font-size:13px;font-weight:bold;letter-spacing:1px}
.tb button.a{color:#ec4899;border-bottom:2px solid #ec4899;background:rgba(236,72,153,.08)}
.cn{flex:1;overflow-y:auto;padding:10px}
.pn{display:none}.pn.a{display:block}
.det{background:rgba(45,27,105,.4);border:1px solid rgba(139,92,246,.25);border-radius:7px;padding:10px;margin-bottom:8px}
.det .mac{color:#ec4899;font-weight:bold;font-size:14px}
.det .nm{color:#c084fc;font-size:13px;margin-left:4px}
.det .inf{display:flex;flex-wrap:wrap;gap:5px;margin-top:5px;font-size:12px}
.det .inf span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px}
.det .rv{background:rgba(239,68,68,.15)!important;color:#ef4444;font-weight:bold}
.pg{margin-bottom:12px}
.pg h3{color:#ec4899;font-size:14px;margin-bottom:4px;border-bottom:1px solid rgba(139,92,246,.19);padding-bottom:4px}
.pg .it{display:flex;flex-wrap:wrap;gap:4px;font-size:12px}
.pg .it span{background:rgba(139,92,246,.15);padding:3px 6px;border-radius:4px;border:1px solid rgba(139,92,246,.12)}
.btn{display:block;width:100%;padding:10px;margin-bottom:8px;background:#8b5cf6;color:#fff;border:none;border-radius:5px;cursor:pointer;font-family:inherit;font-size:14px;font-weight:bold}
.btn:active{background:#ec4899}
.btn.dng{background:#ef4444}
.empty{text-align:center;color:rgba(139,92,246,.5);padding:28px;font-size:14px}
.sep{border:none;border-top:1px solid rgba(139,92,246,.12);margin:12px 0}
h4{color:#ec4899;font-size:14px;margin-bottom:8px}
</style></head><body>
<div class="hd"><h1>FLOCK-YOU</h1><div class="sub">Surveillance Device Detector &bull; Wardriving + GPS</div></div>
<div class="st">
<div class="sc"><div class="n" id="sT">0</div><div class="l">DETECTED</div></div>
<div class="sc"><div class="n" id="sR">0</div><div class="l">RAVEN</div></div>
<div class="sc"><div class="n" id="sB">ON</div><div class="l">BLE</div></div>
<div class="sc" onclick="reqGPS()" style="cursor:pointer"><div class="n" id="sG" style="font-size:14px">TAP</div><div class="l">GPS</div></div>
</div>
<div class="tb">
<button class="a" onclick="tab(0,this)">LIVE</button>
<button onclick="tab(1,this)">PREV</button>
<button onclick="tab(2,this)">DB</button>
<button onclick="tab(3,this)">TOOLS</button>
</div>
<div class="cn">
<div class="pn a" id="p0">
<div id="dL"><div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div></div>
</div>
<div class="pn" id="p1"><div id="hL"><div class="empty">Loading prior session...</div></div></div>
<div class="pn" id="p2"><div id="pC">Loading patterns...</div></div>
<div class="pn" id="p3">
<h4>EXPORT DETECTIONS</h4>
<p style="font-size:10px;color:#8b5cf6;margin-bottom:8px">Download current session to import into Flask dashboard</p>
<button class="btn" onclick="location.href='/api/export/json'">DOWNLOAD JSON</button>
<button class="btn" onclick="location.href='/api/export/csv'">DOWNLOAD CSV</button>
<button class="btn" onclick="location.href='/api/export/kml'" style="background:#22c55e">DOWNLOAD KML (GPS MAP)</button>
<hr class="sep">
<h4>PRIOR SESSION</h4>
<button class="btn" onclick="location.href='/api/history/json'" style="background:#6366f1">DOWNLOAD PREV JSON</button>
<button class="btn" onclick="location.href='/api/history/kml'" style="background:#22c55e">DOWNLOAD PREV KML</button>
<hr class="sep">
<button class="btn dng" onclick="if(confirm('Clear all detections?'))fetch('/api/clear').then(()=>refresh())">CLEAR ALL DETECTIONS</button>
</div>
</div>
<script>
let D=[],H=[];
function tab(i,el){document.querySelectorAll('.tb button').forEach(b=>b.classList.remove('a'));document.querySelectorAll('.pn').forEach(p=>p.classList.remove('a'));el.classList.add('a');document.getElementById('p'+i).classList.add('a');if(i===1&&!window._hL)loadHistory();if(i===2&&!window._pL)loadPat();}
function refresh(){fetch('/api/detections').then(r=>r.json()).then(d=>{D=d;render();stats();}).catch(()=>{});}
function render(){const el=document.getElementById('dL');if(!D.length){el.innerHTML='<div class="empty">Scanning for surveillance devices...<br>BLE active on all channels</div>';return;}
D.sort((a,b)=>b.last-a.last);el.innerHTML=D.map(card).join('');}
function stats(){document.getElementById('sT').textContent=D.length;document.getElementById('sR').textContent=D.filter(d=>d.raven).length;
fetch('/api/stats').then(r=>r.json()).then(s=>{let g=document.getElementById('sG');if(s.gps_valid){g.textContent=s.gps_tagged+'/'+s.total;g.style.color='#22c55e';}else{g.textContent='OFF';g.style.color='#ef4444';}}).catch(()=>{});}
function card(d){return '<div class="det"><div class="mac">'+d.mac+(d.name?'<span class="nm">'+d.name+'</span>':'')+'</div><div class="inf"><span>RSSI: '+d.rssi+'</span><span>'+d.method+'</span><span style="color:#ec4899;font-weight:bold">&times;'+d.count+'</span>'+(d.raven?'<span class="rv">RAVEN '+d.fw+'</span>':'')+(d.gps?'<span style="color:#22c55e">&#9673; '+d.gps.lat.toFixed(5)+','+d.gps.lon.toFixed(5)+'</span>':'<span style="color:#666">no gps</span>')+'</div></div>';}
function loadHistory(){fetch('/api/history').then(r=>r.json()).then(d=>{H=d;let el=document.getElementById('hL');if(!H.length){el.innerHTML='<div class="empty">No prior session data</div>';return;}
H.sort((a,b)=>b.last-a.last);el.innerHTML='<div style="font-size:11px;color:#8b5cf6;margin-bottom:8px">'+H.length+' detections from prior session</div>'+H.map(card).join('');window._hL=1;}).catch(()=>{document.getElementById('hL').innerHTML='<div class="empty">No prior session data</div>';});}
function loadPat(){fetch('/api/patterns').then(r=>r.json()).then(p=>{let h='';
h+='<div class="pg"><h3>Flock MAC Prefixes ('+p.macs.length+')</h3><div class="it">'+p.macs.map(m=>'<span>'+m+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>Contract Mfr MACs ('+p.macs_mfr.length+')</h3><div class="it">'+p.macs_mfr.map(m=>'<span>'+m+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>SoundThinking MACs ('+p.macs_soundthinking.length+')</h3><div class="it">'+p.macs_soundthinking.map(m=>'<span>'+m+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Device Names ('+p.names.length+')</h3><div class="it">'+p.names.map(n=>'<span>'+n+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>BLE Manufacturer IDs ('+p.mfr.length+')</h3><div class="it">'+p.mfr.map(m=>'<span>0x'+m.toString(16).toUpperCase().padStart(4,'0')+'</span>').join('')+'</div></div>';
h+='<div class="pg"><h3>Raven UUIDs ('+p.raven.length+')</h3><div class="it">'+p.raven.map(u=>'<span style="font-size:8px">'+u+'</span>').join('')+'</div></div>';
document.getElementById('pC').innerHTML=h;window._pL=1;}).catch(()=>{});}
// GPS from phone -> ESP32 (wardriving)
// NOTE: Geolocation API needs secure context (HTTPS) on most browsers.
// HTTP works on: Android Chrome (local IPs), some Android browsers.
// Won't work on: iOS Safari (needs HTTPS always).
// We only request on user tap (gesture) for best permission prompt chance.
let _gW=null,_gOk=false,_gTried=false;
function sendGPS(p){_gOk=true;let g=document.getElementById('sG');g.textContent='OK';g.style.color='#22c55e';
fetch('/api/gps?lat='+p.coords.latitude+'&lon='+p.coords.longitude+'&acc='+(p.coords.accuracy||0)).catch(()=>{});}
function gpsErr(e){_gOk=false;let g=document.getElementById('sG');
var msg='ERR';if(e.code===1){msg='DENIED';g.style.color='#ef4444';alert('GPS permission denied. On iPhone, GPS requires HTTPS which this device cannot provide. On Android Chrome, tap the lock/info icon in the address bar and allow Location.');}
else if(e.code===2){msg='N/A';g.style.color='#ef4444';}
else if(e.code===3){msg='WAIT';g.style.color='#facc15';}
g.textContent=msg;}
function startGPS(){if(!navigator.geolocation){return false;}
if(_gW!==null){navigator.geolocation.clearWatch(_gW);_gW=null;}
let g=document.getElementById('sG');g.textContent='...';g.style.color='#facc15';
_gW=navigator.geolocation.watchPosition(sendGPS,gpsErr,{enableHighAccuracy:true,maximumAge:5000,timeout:15000});return true;}
function reqGPS(){if(!navigator.geolocation){alert('GPS not available in this browser.');return;}
if(_gOk){return;}
if(!window.isSecureContext){alert('GPS requires a secure context (HTTPS). This HTTP page may not get GPS permission.\\n\\nAndroid Chrome: try chrome://flags and enable "Insecure origins treated as secure", add http://192.168.4.1\\n\\niPhone: GPS will not work over HTTP.');}
startGPS();_gTried=true;}
refresh();setInterval(refresh,2500);
</script></body></html>
)rawliteral";

// ============================================================================
// WEB SERVER SETUP
// ============================================================================

static void fySetupServer() {
    // Dashboard
    fyServer.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
        r->send(200, "text/html", FY_HTML);
    });

    // API: Detection list
    fyServer.on("/api/detections", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        writeDetectionsJSON(resp);
        r->send(resp);
    });

    // API: Stats (includes GPS status)
    fyServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest *r) {
        int raven = 0, withGPS = 0;
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                if (fyDet[i].isRaven) raven++;
                if (fyDet[i].hasGPS) withGPS++;
            }
            xSemaphoreGive(fyMutex);
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\"total\":%d,\"raven\":%d,\"ble\":\"active\","
            "\"gps_valid\":%s,\"gps_age\":%lu,\"gps_tagged\":%d}",
            fyDetCount, raven,
            fyGPSIsFresh() ? "true" : "false",
            fyGPSValid ? (millis() - fyGPSLastUpdate) : 0UL,
            withGPS);
        r->send(200, "application/json", buf);
    });

    // API: Receive GPS from phone browser (atomic publish under fyGPSMutex)
    fyServer.on("/api/gps", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (r->hasParam("lat") && r->hasParam("lon")) {
            double lat = r->getParam("lat")->value().toDouble();
            double lon = r->getParam("lon")->value().toDouble();
            float  acc = r->hasParam("acc") ? r->getParam("acc")->value().toFloat() : 0;
            fyGPSUpdate(lat, lon, acc);
            r->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            r->send(400, "application/json", "{\"error\":\"lat,lon required\"}");
        }
    });

    // API: Pattern database
    fyServer.on("/api/patterns", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->print("{\"macs\":[");
        for (size_t i = 0; i < sizeof(flock_mac_prefixes)/sizeof(flock_mac_prefixes[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", flock_mac_prefixes[i]);
        }
        resp->print("],\"macs_mfr\":[");
        for (size_t i = 0; i < sizeof(flock_mfr_mac_prefixes)/sizeof(flock_mfr_mac_prefixes[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", flock_mfr_mac_prefixes[i]);
        }
        resp->print("],\"macs_soundthinking\":[");
        for (size_t i = 0; i < sizeof(soundthinking_mac_prefixes)/sizeof(soundthinking_mac_prefixes[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", soundthinking_mac_prefixes[i]);
        }
        resp->print("],\"names\":[");
        for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", device_name_patterns[i]);
        }
        resp->print("],\"mfr\":[");
        for (size_t i = 0; i < sizeof(ble_manufacturer_ids)/sizeof(ble_manufacturer_ids[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("%u", ble_manufacturer_ids[i]);
        }
        resp->print("],\"raven\":[");
        for (size_t i = 0; i < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); i++) {
            if (i > 0) resp->print(",");
            resp->printf("\"%s\"", raven_service_uuids[i]);
        }
        resp->print("]}");
        r->send(resp);
    });

    // API: Export JSON (downloadable file)
    fyServer.on("/api/export/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.json\"");
        writeDetectionsJSON(resp);
        r->send(resp);
    });

    // API: Export CSV (downloadable file, includes GPS)
    fyServer.on("/api/export/csv", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("text/csv");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.csv\"");
        resp->println("mac,name,rssi,method,first_seen_ms,last_seen_ms,count,is_raven,raven_fw,latitude,longitude,gps_accuracy");
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = 0; i < fyDetCount; i++) {
                FYDetection& d = fyDet[i];
                if (d.hasGPS) {
                    resp->printf("\"%s\",\"%s\",%d,\"%s\",%lu,%lu,%d,%s,\"%s\",%.8f,%.8f,%.1f\n",
                        d.mac, d.name, d.rssi, d.method,
                        d.firstSeen, d.lastSeen, d.count,
                        d.isRaven ? "true" : "false", d.ravenFW,
                        d.gpsLat, d.gpsLon, d.gpsAcc);
                } else {
                    resp->printf("\"%s\",\"%s\",%d,\"%s\",%lu,%lu,%d,%s,\"%s\",,,\n",
                        d.mac, d.name, d.rssi, d.method,
                        d.firstSeen, d.lastSeen, d.count,
                        d.isRaven ? "true" : "false", d.ravenFW);
                }
            }
            xSemaphoreGive(fyMutex);
        }
        r->send(resp);
    });

    // API: Export KML (GPS-tagged detections for Google Earth)
    fyServer.on("/api/export/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_detections.kml\"");
        writeDetectionsKML(resp);
        r->send(resp);
    });

    // API: Prior session history (JSON) — strips envelope header if present
    fyServer.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *r) {
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        fyStreamPrevSessionBody(resp);
        r->send(resp);
    });

    // API: Download prior session as JSON file (body-only, envelope stripped)
    fyServer.on("/api/history/json", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        AsyncResponseStream *resp = r->beginResponseStream("application/json");
        resp->addHeader("Content-Disposition",
            "attachment; filename=\"flockyou_prev_session.json\"");
        fyStreamPrevSessionBody(resp);
        r->send(resp);
    });

    // API: Download prior session as KML (reads JSON body from SPIFFS, converts)
    fyServer.on("/api/history/kml", HTTP_GET, [](AsyncWebServerRequest *r) {
        if (!fySpiffsReady || !SPIFFS.exists(FY_PREV_FILE)) {
            r->send(404, "application/json", "{\"error\":\"no prior session\"}");
            return;
        }
        File f = SPIFFS.open(FY_PREV_FILE, "r");
        if (!f) { r->send(500, "text/plain", "read error"); return; }
        // Strip envelope header if present
        if (f.peek() == '{') f.readStringUntil('\n');
        String content = f.readString();
        f.close();
        if (content.length() == 0) {
            r->send(404, "application/json", "{\"error\":\"prior session empty\"}");
            return;
        }
        AsyncResponseStream *resp = r->beginResponseStream("application/vnd.google-earth.kml+xml");
        resp->addHeader("Content-Disposition", "attachment; filename=\"flockyou_prev_session.kml\"");
        resp->print("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<kml xmlns=\"http://www.opengis.net/kml/2.2\">\n<Document>\n"
                    "<name>Flock-You Prior Session</name>\n"
                    "<description>Surveillance device detections from prior session</description>\n"
                    "<Style id=\"det\"><IconStyle><color>ff4489ec</color>"
                    "<scale>1.0</scale></IconStyle></Style>\n"
                    "<Style id=\"raven\"><IconStyle><color>ff4444ef</color>"
                    "<scale>1.2</scale></IconStyle></Style>\n");
        // Parse JSON array and emit placemarks
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, content);
        if (!err && doc.is<JsonArray>()) {
            int placed = 0;
            for (JsonObject d : doc.as<JsonArray>()) {
                JsonObject gps = d["gps"];
                if (!gps || !gps.containsKey("lat")) continue;
                bool isRaven = d["raven"] | false;
                resp->printf("<Placemark><name>%s</name>\n", d["mac"] | "?");
                resp->printf("<styleUrl>#%s</styleUrl>\n", isRaven ? "raven" : "det");
                resp->print("<description><![CDATA[");
                if (d["name"].is<const char*>() && strlen(d["name"] | "") > 0)
                    resp->printf("<b>Name:</b> %s<br/>", d["name"] | "");
                resp->printf("<b>Method:</b> %s<br/><b>RSSI:</b> %d<br/><b>Count:</b> %d",
                    d["method"] | "?", d["rssi"] | 0, d["count"] | 1);
                if (isRaven && d["fw"].is<const char*>())
                    resp->printf("<br/><b>Raven FW:</b> %s", d["fw"] | "");
                resp->print("]]></description>\n");
                resp->printf("<Point><coordinates>%.8f,%.8f,0</coordinates></Point>\n",
                    (double)(gps["lon"] | 0.0), (double)(gps["lat"] | 0.0));
                resp->print("</Placemark>\n");
                placed++;
            }
            printf("[FLOCK-YOU] Prior session KML: %d placemarks\n", placed);
        } else {
            printf("[FLOCK-YOU] Prior session KML: JSON parse failed\n");
        }
        resp->print("</Document>\n</kml>");
        r->send(resp);
    });

    // API: Clear all detections (saves current session first)
    fyServer.on("/api/clear", HTTP_GET, [](AsyncWebServerRequest *r) {
        fySaveSession();  // Persist before clearing
        if (fyMutex && xSemaphoreTake(fyMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            fyDetCount = 0;
            memset(fyDet, 0, sizeof(fyDet));
            fyTriggered = false;
            fyDeviceInRange = false;
            xSemaphoreGive(fyMutex);
        }
        r->send(200, "application/json", "{\"status\":\"cleared\"}");
        printf("[FLOCK-YOU] All detections cleared (session saved)\n");
    });

    fyServer.begin();
    printf("[FLOCK-YOU] Web server started on port 80\n");
}

// ============================================================================
// MAIN FUNCTIONS
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(500);

    // Standalone mode: buzzer always on by default
    fyBuzzerOn = true;

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    fyMutex    = xSemaphoreCreateMutex();
    fyGPSMutex = xSemaphoreCreateMutex();

    // Init SPIFFS for session persistence
    if (SPIFFS.begin(true)) {
        fySpiffsReady = true;
        printf("[FLOCK-YOU] SPIFFS ready\n");
        // Promote last session to prev_session before we start a new one
        fyPromotePrevSession();
    } else {
        printf("[FLOCK-YOU] SPIFFS init failed - no persistence\n");
    }

    printf("\n========================================\n");
    printf("  FLOCK-YOU Surveillance Detector\n");
    printf("  Buzzer: %s\n", fyBuzzerOn ? "ON" : "OFF");
    printf("========================================\n");

    // Init BLE with device name and large MTU for GATT notifications
    NimBLEDevice::init("flockyou");
    NimBLEDevice::setMTU(512);

    // BLE scanner setup
    fyBLEScan = NimBLEDevice::getScan();
    fyBLEScan->setAdvertisedDeviceCallbacks(new FYBLECallbacks());
    fyBLEScan->setActiveScan(true);
    fyBLEScan->setInterval(100);
    fyBLEScan->setWindow(99);

    // Kick off the first scan right away
    fyBLEScan->start(fyBleScanDuration, false);
    fyLastBleScan = millis();
    printf("[FLOCK-YOU] BLE scanning ACTIVE\n");

    // BLE GATT server — DeFlock app connectivity
    fyBLEServer = NimBLEDevice::createServer();
    fyBLEServer->setCallbacks(new FYServerCallbacks());
    NimBLEService* pService = fyBLEServer->createService(FY_SERVICE_UUID);
    fyTxChar = pService->createCharacteristic(
        FY_TX_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    pService->start();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    pAdv->addServiceUUID(FY_SERVICE_UUID);
    pAdv->setName("flockyou");
    pAdv->setScanResponse(true);
    pAdv->start();
    printf("[FLOCK-YOU] BLE GATT server advertising (service %s)\n", FY_SERVICE_UUID);

    // Crow calls play WHILE BLE is already scanning
    fyBootBeep();

    // Start WiFi AP (no need to connect to anything -- AP only)
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP(FY_AP_SSID, FY_AP_PASS);
    printf("[FLOCK-YOU] AP: %s / %s\n", FY_AP_SSID, FY_AP_PASS);
    printf("[FLOCK-YOU] IP: %s\n", WiFi.softAPIP().toString().c_str());

    // Start web dashboard
    fySetupServer();

    printf("[FLOCK-YOU] Detection methods: MAC prefix, device name, manufacturer ID, Raven UUID\n");
    printf("[FLOCK-YOU] Dashboard: http://192.168.4.1\n");
    printf("[FLOCK-YOU] Ready - BLE GATT + AP mode\n\n");
}

void loop() {
    // Serial host detection (heartbeat from DeFlock desktop app)
    if (Serial.available()) {
        while (Serial.available()) Serial.read();  // drain buffer
        fyLastSerialHeartbeat = millis();
        if (!fySerialHostConnected) {
            fySerialHostConnected = true;
            fyCompanionChangePending = true;
        }
    } else if (fySerialHostConnected &&
               millis() - fyLastSerialHeartbeat >= FY_SERIAL_TIMEOUT_MS) {
        fySerialHostConnected = false;
        fyCompanionChangePending = true;
    }

    // Apply deferred companion mode switch (from BLE callbacks or serial detection)
    if (fyCompanionChangePending) {
        fyCompanionChangePending = false;
        fyOnCompanionChange();
    }

    // BLE scanning cycle
    if (millis() - fyLastBleScan >= fyBleScanInterval && !fyBLEScan->isScanning()) {
        fyBLEScan->start(fyBleScanDuration, false);
        fyLastBleScan = millis();
    }

    if (!fyBLEScan->isScanning() && millis() - fyLastBleScan > (unsigned long)fyBleScanDuration * 1000) {
        fyBLEScan->clearResults();
    }

    // Heartbeat tracking
    if (fyDeviceInRange) {
        if (millis() - fyLastHB >= 10000) {
            fyHeartbeat();
            fyLastHB = millis();
        }
        if (millis() - fyLastDetTime >= 30000) {
            printf("[FLOCK-YOU] Device out of range - stopping heartbeat\n");
            fyDeviceInRange = false;
            fyTriggered = false;
        }
    }

    // Back-fill GPS on any detections captured before the first fix (every 2s)
    static unsigned long lastBackfill = 0;
    if (millis() - lastBackfill >= 2000) {
        fyBackfillGPS();
        lastBackfill = millis();
    }

    // Bulletproof save cadence:
    //  - within 5s of first detection (quick first-save)
    //  - any time fyDetCount increases (new unique device), throttled to 3s minimum
    //  - every FY_SAVE_INTERVAL (15s) as a safety net
    if (fySpiffsReady && fyDetCount > 0) {
        unsigned long now = millis();
        bool countChanged = (fyDetCount != fyLastSaveCount);
        bool minGap       = (now - fyLastSave >= 3000);
        bool firstSave    = (fyLastSaveCount == 0 && now - fyLastSave >= 5000);
        bool periodic     = (now - fyLastSave >= FY_SAVE_INTERVAL);

        if (firstSave || (countChanged && minGap) || periodic) {
            fySaveSession();
            fyLastSave = millis();
        }
    }

    delay(100);
}
