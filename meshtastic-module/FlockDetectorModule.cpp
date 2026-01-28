// ============================================================================
// FLOCK DETECTOR MODULE FOR MESHTASTIC - Implementation
// ============================================================================

#include "configuration.h"

#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_FLOCKDETECTOR

#include "FlockDetectorModule.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "main.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

#include <WiFi.h>
#include <esp_wifi.h>

// Module instance
FlockDetectorModule *flockDetectorModule = nullptr;

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

// Known Flock Safety MAC prefixes
static const char *MAC_PREFIXES[] = {
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea"
};
static const size_t MAC_PREFIX_COUNT = sizeof(MAC_PREFIXES) / sizeof(MAC_PREFIXES[0]);

// WiFi SSID patterns
static const char *SSID_PATTERNS[] = {
    "flock", "Flock", "FLOCK",
    "FS Ext Battery",
    "Penguin", "Pigvision"
};
static const size_t SSID_PATTERN_COUNT = sizeof(SSID_PATTERNS) / sizeof(SSID_PATTERNS[0]);

// BLE device name patterns
static const char *NAME_PATTERNS[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision"
};
static const size_t NAME_PATTERN_COUNT = sizeof(NAME_PATTERNS) / sizeof(NAME_PATTERNS[0]);

// Raven gunshot detector service UUIDs
static const char *RAVEN_UUIDS[] = {
    "0000180a-0000-1000-8000-00805f9b34fb", // Device Info
    "00003100-0000-1000-8000-00805f9b34fb", // GPS Service
    "00003200-0000-1000-8000-00805f9b34fb", // Power Service
    "00003300-0000-1000-8000-00805f9b34fb", // Network Service
    "00003400-0000-1000-8000-00805f9b34fb", // Upload Service
    "00003500-0000-1000-8000-00805f9b34fb", // Error Service
    "00001809-0000-1000-8000-00805f9b34fb", // Health (legacy)
    "00001819-0000-1000-8000-00805f9b34fb"  // Location (legacy)
};
static const size_t RAVEN_UUID_COUNT = sizeof(RAVEN_UUIDS) / sizeof(RAVEN_UUIDS[0]);

// ============================================================================
// CONFIGURATION
// ============================================================================

#define CHANNEL_HOP_INTERVAL_MS     500
#define BLE_SCAN_INTERVAL_MS        10000
#define BLE_SCAN_DURATION_SEC       3
#define REPORT_INTERVAL_MS          5000  // Min time between mesh broadcasts
#define SEEN_TIMEOUT_MS             60000 // Don't re-report same device for 1 min
#define MAX_CHANNEL                 13

// ============================================================================
// STATIC CALLBACK CONTEXT
// ============================================================================

// Need static access for WiFi callback
static FlockDetectorModule *callbackInstance = nullptr;

// ============================================================================
// CONSTRUCTOR
// ============================================================================

FlockDetectorModule::FlockDetectorModule()
    : SinglePortModule("FlockDetector", meshtastic_PortNum_TEXT_MESSAGE_APP),
      concurrency::OSThread("FlockDetector")
{
    callbackInstance = this;

    // Zero out buffers
    memset(detectionQueue, 0, sizeof(detectionQueue));
    memset(seenMacs, 0, sizeof(seenMacs));
    memset(seenTimestamps, 0, sizeof(seenTimestamps));
}

// ============================================================================
// PATTERN MATCHING
// ============================================================================

bool FlockDetectorModule::checkMacPrefix(const uint8_t *mac)
{
    char prefix[9];
    snprintf(prefix, sizeof(prefix), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);

    for (size_t i = 0; i < MAC_PREFIX_COUNT; i++) {
        if (strcasecmp(prefix, MAC_PREFIXES[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool FlockDetectorModule::checkSsidPattern(const char *ssid)
{
    if (!ssid || strlen(ssid) == 0) return false;

    for (size_t i = 0; i < SSID_PATTERN_COUNT; i++) {
        if (strcasestr(ssid, SSID_PATTERNS[i])) {
            return true;
        }
    }
    return false;
}

bool FlockDetectorModule::checkDeviceNamePattern(const char *name)
{
    if (!name || strlen(name) == 0) return false;

    for (size_t i = 0; i < NAME_PATTERN_COUNT; i++) {
        if (strcasestr(name, NAME_PATTERNS[i])) {
            return true;
        }
    }
    return false;
}

bool FlockDetectorModule::checkRavenServiceUuid(NimBLEAdvertisedDevice *device)
{
    if (!device || !device->haveServiceUUID()) return false;

    int count = device->getServiceUUIDCount();
    for (int i = 0; i < count; i++) {
        std::string uuid = device->getServiceUUID(i).toString();
        for (size_t j = 0; j < RAVEN_UUID_COUNT; j++) {
            if (strcasecmp(uuid.c_str(), RAVEN_UUIDS[j]) == 0) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// DEDUPLICATION
// ============================================================================

bool FlockDetectorModule::wasRecentlySeen(const uint8_t *mac)
{
    uint32_t now = millis();
    for (size_t i = 0; i < SEEN_SIZE; i++) {
        if (memcmp(seenMacs[i], mac, 6) == 0) {
            if (now - seenTimestamps[i] < SEEN_TIMEOUT_MS) {
                return true;
            }
        }
    }
    return false;
}

void FlockDetectorModule::markAsSeen(const uint8_t *mac)
{
    memcpy(seenMacs[seenIndex], mac, 6);
    seenTimestamps[seenIndex] = millis();
    seenIndex = (seenIndex + 1) % SEEN_SIZE;
}

// ============================================================================
// DETECTION QUEUE
// ============================================================================

void FlockDetectorModule::queueDetection(FlockDetectionType type, const uint8_t *mac,
                                          int8_t rssi, const char *name)
{
    // Skip if recently seen
    if (wasRecentlySeen(mac)) return;
    markAsSeen(mac);

    // Add to queue
    size_t nextHead = (queueHead + 1) % QUEUE_SIZE;
    if (nextHead == queueTail) {
        // Queue full, drop oldest
        queueTail = (queueTail + 1) % QUEUE_SIZE;
    }

    FlockDetection *det = &detectionQueue[queueHead];
    det->type = type;
    memcpy(det->mac, mac, 6);
    det->rssi = rssi;
    det->timestamp = millis();
    det->reported = false;

    if (name && strlen(name) > 0) {
        strncpy(det->name, name, sizeof(det->name) - 1);
        det->name[sizeof(det->name) - 1] = '\0';
    } else {
        det->name[0] = '\0';
    }

    queueHead = nextHead;

    LOG_INFO("FlockDetector: Queued detection type=%d MAC=%02x:%02x:%02x:%02x:%02x:%02x RSSI=%d\n",
             type, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
}

// ============================================================================
// WIFI SCANNING
// ============================================================================

// WiFi packet structure
typedef struct {
    unsigned frame_ctrl : 16;
    unsigned duration_id : 16;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    unsigned sequence_ctrl : 16;
    uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

void IRAM_ATTR FlockDetectorModule::wifiPacketHandler(void *buff, wifi_promiscuous_pkt_type_t type)
{
    if (!callbackInstance) return;

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buff;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    const wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;

    // Only process probe requests (0x40) and beacons (0x80)
    uint8_t frameType = hdr->frame_ctrl & 0xFC;
    if (frameType != 0x40 && frameType != 0x80) return;

    // Extract SSID from tagged params
    char ssid[33] = {0};
    uint8_t *payload = (uint8_t *)ipkt + 24;

    // Skip fixed params for beacons
    if (frameType == 0x80) payload += 12;

    // First tag should be SSID (tag 0)
    if (payload[0] == 0 && payload[1] <= 32) {
        memcpy(ssid, &payload[2], payload[1]);
        ssid[payload[1]] = '\0';
    }

    // Check for matches
    bool matched = false;
    const uint8_t *mac = hdr->addr2;

    if (strlen(ssid) > 0 && checkSsidPattern(ssid)) {
        matched = true;
    } else if (checkMacPrefix(mac)) {
        matched = true;
    }

    if (matched) {
        callbackInstance->queueDetection(DETECT_FLOCK_WIFI, mac, pkt->rx_ctrl.rssi, ssid);
    }
}

void FlockDetectorModule::initWifiScanner()
{
    LOG_INFO("FlockDetector: Initializing WiFi promiscuous mode\n");

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&FlockDetectorModule::wifiPacketHandler);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    wifiInitialized = true;
    LOG_INFO("FlockDetector: WiFi scanner ready on channel %d\n", currentChannel);
}

void FlockDetectorModule::hopChannel()
{
    uint32_t now = millis();
    if (now - lastChannelHop < CHANNEL_HOP_INTERVAL_MS) return;

    currentChannel++;
    if (currentChannel > MAX_CHANNEL) currentChannel = 1;

    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    lastChannelHop = now;
}

// ============================================================================
// BLE SCANNING
// ============================================================================

// BLE callback class
class FlockBLECallbacks : public NimBLEAdvertisedDeviceCallbacks
{
  public:
    void onResult(NimBLEAdvertisedDevice *device) override
    {
        if (!callbackInstance) return;

        NimBLEAddress addr = device->getAddress();
        std::string addrStr = addr.toString();
        uint8_t mac[6];
        sscanf(addrStr.c_str(), "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);

        int rssi = device->getRSSI();
        std::string name = device->haveName() ? device->getName() : "";

        // Check for Raven first (service UUID match)
        if (FlockDetectorModule::checkRavenServiceUuid(device)) {
            callbackInstance->queueDetection(DETECT_RAVEN, mac, rssi, name.c_str());
            return;
        }

        // Check MAC prefix
        if (FlockDetectorModule::checkMacPrefix(mac)) {
            callbackInstance->queueDetection(DETECT_FLOCK_BLE, mac, rssi, name.c_str());
            return;
        }

        // Check device name
        if (!name.empty() && FlockDetectorModule::checkDeviceNamePattern(name.c_str())) {
            FlockDetectionType type = DETECT_FLOCK_BLE;
            if (strcasestr(name.c_str(), "Penguin")) type = DETECT_PENGUIN;
            if (strcasestr(name.c_str(), "Pigvision")) type = DETECT_PIGVISION;

            callbackInstance->queueDetection(type, mac, rssi, name.c_str());
        }
    }
};

void FlockDetectorModule::initBleScanner()
{
    LOG_INFO("FlockDetector: Initializing BLE scanner\n");

    // NimBLE should already be initialized by Meshtastic
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new FlockBLECallbacks(), false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(99);

    bleInitialized = true;
    LOG_INFO("FlockDetector: BLE scanner ready\n");
}

void FlockDetectorModule::runBleScan()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (!scan->isScanning()) {
        scan->start(BLE_SCAN_DURATION_SEC, false);
    }
}

// ============================================================================
// MESH TRANSMISSION
// ============================================================================

void FlockDetectorModule::sendDetectionAlert(FlockDetection *detection)
{
    // Format detection as human-readable text message
    char msg[200];
    const char *typeStr;

    switch (detection->type) {
        case DETECT_FLOCK_WIFI:  typeStr = "FLOCK-WiFi"; break;
        case DETECT_FLOCK_BLE:   typeStr = "FLOCK-BLE"; break;
        case DETECT_RAVEN:       typeStr = "RAVEN"; break;
        case DETECT_PENGUIN:     typeStr = "PENGUIN"; break;
        case DETECT_PIGVISION:   typeStr = "PIGVISION"; break;
        default:                 typeStr = "UNKNOWN"; break;
    }

    // Include GPS if available
    if (GPS_COORD_VALID) {
        snprintf(msg, sizeof(msg),
                 "ALERT %s\nMAC:%02X:%02X:%02X:%02X:%02X:%02X\nRSSI:%ddBm\nLoc:%.6f,%.6f%s%s",
                 typeStr,
                 detection->mac[0], detection->mac[1], detection->mac[2],
                 detection->mac[3], detection->mac[4], detection->mac[5],
                 detection->rssi,
                 gpsStatus->getLatitude() * 1e-7,
                 gpsStatus->getLongitude() * 1e-7,
                 strlen(detection->name) > 0 ? "\nName:" : "",
                 detection->name);
    } else {
        snprintf(msg, sizeof(msg),
                 "ALERT %s\nMAC:%02X:%02X:%02X:%02X:%02X:%02X\nRSSI:%ddBm%s%s",
                 typeStr,
                 detection->mac[0], detection->mac[1], detection->mac[2],
                 detection->mac[3], detection->mac[4], detection->mac[5],
                 detection->rssi,
                 strlen(detection->name) > 0 ? "\nName:" : "",
                 detection->name);
    }

    // Allocate and send mesh packet
    meshtastic_MeshPacket *p = allocDataPacket();
    if (p) {
        p->to = NODENUM_BROADCAST;
        p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        p->want_ack = false;
        p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

        memcpy(p->decoded.payload.bytes, msg, strlen(msg));
        p->decoded.payload.size = strlen(msg);

        service->sendToMesh(p, RX_SRC_LOCAL, true);

        LOG_INFO("FlockDetector: Sent alert to mesh - %s\n", typeStr);
    }

    detection->reported = true;
}

void FlockDetectorModule::processDetectionQueue()
{
    uint32_t now = millis();

    // Rate limit broadcasts
    if (now - lastReport < REPORT_INTERVAL_MS) return;

    // Process pending detections
    while (queueTail != queueHead) {
        FlockDetection *det = &detectionQueue[queueTail];

        if (!det->reported) {
            sendDetectionAlert(det);
            lastReport = now;
            queueTail = (queueTail + 1) % QUEUE_SIZE;
            return; // Only send one per interval
        }

        queueTail = (queueTail + 1) % QUEUE_SIZE;
    }
}

// ============================================================================
// MAIN LOOP (OSThread)
// ============================================================================

int32_t FlockDetectorModule::runOnce()
{
    // First run - initialize scanners
    if (!wifiInitialized) {
        initWifiScanner();
    }

    if (!bleInitialized) {
        initBleScanner();
    }

    // Hop WiFi channels
    hopChannel();

    // Run BLE scan periodically
    uint32_t now = millis();
    if (now - lastBleScan >= BLE_SCAN_INTERVAL_MS) {
        runBleScan();
        lastBleScan = now;
    }

    // Process detection queue
    processDetectionQueue();

    // Run again in 100ms
    return 100;
}

#endif // ARCH_ESP32 && !MESHTASTIC_EXCLUDE_FLOCKDETECTOR
