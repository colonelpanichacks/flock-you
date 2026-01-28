// ============================================================================
// FLOCK-YOU T-BEAM SUPREME - LoRa Mesh Detection System
// ============================================================================
// Surveillance camera detection with LoRa transmission
// Hardware: LILYGO T-Beam Supreme (ESP32-S3 + SX1262 + GNSS)
//
// Instead of WebUI, detections are broadcast over LoRa mesh.
// Receive with another T-Beam, Meshtastic device, or custom receiver.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>
#include <RadioLib.h>
#include <TinyGPSPlus.h>
#include <SPI.h>
#include <Wire.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"

#include "boards/tbeam_supreme.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

// LoRa transmission interval (don't spam the channel)
#define LORA_TX_INTERVAL_MS     5000    // Minimum ms between transmissions
#define LORA_TX_QUEUE_SIZE      10      // Max queued detections

// WiFi Promiscuous Mode Configuration
#define MAX_CHANNEL             13
#define CHANNEL_HOP_INTERVAL    500     // milliseconds

// BLE Scanning Configuration
#define BLE_SCAN_DURATION       1       // Seconds
#define BLE_SCAN_INTERVAL       5000    // Milliseconds between scans

// GPS Configuration
#define GPS_UPDATE_INTERVAL     1000    // ms between GPS reads

// ============================================================================
// DETECTION PATTERNS
// ============================================================================

static const char* wifi_ssid_patterns[] = {
    "flock", "Flock", "FLOCK",
    "FS Ext Battery",
    "Penguin", "Pigvision"
};

static const char* mac_prefixes[] = {
    "58:8e:81", "cc:cc:cc", "ec:1b:bd", "90:35:ea", "04:0d:84",
    "f0:82:c0", "1c:34:f1", "38:5b:44", "94:34:69", "b4:e3:f9",
    "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "14:5a:fc",
    "74:4c:a1", "08:3a:88", "9c:2f:9d", "94:08:53", "e4:aa:ea"
};

static const char* device_name_patterns[] = {
    "FS Ext Battery", "Penguin", "Flock", "Pigvision"
};

// Raven Service UUIDs
#define RAVEN_DEVICE_INFO_SERVICE   "0000180a-0000-1000-8000-00805f9b34fb"
#define RAVEN_GPS_SERVICE           "00003100-0000-1000-8000-00805f9b34fb"
#define RAVEN_POWER_SERVICE         "00003200-0000-1000-8000-00805f9b34fb"
#define RAVEN_NETWORK_SERVICE       "00003300-0000-1000-8000-00805f9b34fb"
#define RAVEN_UPLOAD_SERVICE        "00003400-0000-1000-8000-00805f9b34fb"
#define RAVEN_ERROR_SERVICE         "00003500-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_HEALTH_SERVICE    "00001809-0000-1000-8000-00805f9b34fb"
#define RAVEN_OLD_LOCATION_SERVICE  "00001819-0000-1000-8000-00805f9b34fb"

static const char* raven_service_uuids[] = {
    RAVEN_DEVICE_INFO_SERVICE, RAVEN_GPS_SERVICE, RAVEN_POWER_SERVICE,
    RAVEN_NETWORK_SERVICE, RAVEN_UPLOAD_SERVICE, RAVEN_ERROR_SERVICE,
    RAVEN_OLD_HEALTH_SERVICE, RAVEN_OLD_LOCATION_SERVICE
};

// ============================================================================
// HARDWARE OBJECTS
// ============================================================================

// SX1262 LoRa Radio (using dedicated SPI)
SPIClass radioSPI(HSPI);
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, radioSPI);

// GPS
HardwareSerial gpsSerial(1);  // Use UART1
TinyGPSPlus gps;

// BLE Scanner
NimBLEScan* pBLEScan;

// ============================================================================
// GLOBAL STATE
// ============================================================================

// WiFi state
static uint8_t current_channel = 1;
static unsigned long last_channel_hop = 0;

// BLE state
static unsigned long last_ble_scan = 0;

// GPS state
static unsigned long last_gps_read = 0;
static double current_lat = 0.0;
static double current_lon = 0.0;
static bool gps_valid = false;

// LoRa TX state
static unsigned long last_lora_tx = 0;
static bool radio_ready = false;

// Detection queue (circular buffer)
struct DetectionMessage {
    uint8_t type;           // MSG_TYPE_*
    uint8_t mac[6];         // MAC address
    int8_t rssi;            // Signal strength
    float lat;              // Latitude
    float lon;              // Longitude
    uint8_t flags;          // Detection flags
    char name[16];          // Device name (truncated)
    bool pending;           // Waiting to be sent
};

static DetectionMessage tx_queue[LORA_TX_QUEUE_SIZE];
static uint8_t tx_queue_head = 0;
static uint8_t tx_queue_tail = 0;

// ============================================================================
// LORA FUNCTIONS
// ============================================================================

bool init_lora() {
    Serial.println("[LoRa] Initializing SX1262...");

    // Initialize dedicated SPI for radio
    radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);

    // Initialize radio
    int state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                            LORA_CODING_RATE, LORA_SYNC_WORD, LORA_TX_POWER,
                            LORA_PREAMBLE_LENGTH);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Init failed, code %d\n", state);
        return false;
    }

    // Configure DIO2 as RF switch control
    radio.setDio2AsRfSwitch(true);

    // Set TCXO voltage
    radio.setTCXO(1.8);

    // Use explicit header mode for variable length packets
    radio.explicitHeader();

    // Set CRC on
    radio.setCRC(true);

    Serial.printf("[LoRa] Ready on %.1f MHz, SF%d, BW%.0f kHz\n",
                  LORA_FREQUENCY, LORA_SPREADING_FACTOR, LORA_BANDWIDTH);

    return true;
}

// Pack detection into compact binary format
int pack_detection_message(DetectionMessage* det, uint8_t* buffer, size_t max_len) {
    if (max_len < 17) return -1;  // Minimum message size

    int pos = 0;

    // Header byte: type
    buffer[pos++] = det->type;

    // MAC address (6 bytes)
    memcpy(&buffer[pos], det->mac, 6);
    pos += 6;

    // RSSI (1 byte, signed)
    buffer[pos++] = (uint8_t)det->rssi;

    // Latitude (4 bytes, float)
    memcpy(&buffer[pos], &det->lat, 4);
    pos += 4;

    // Longitude (4 bytes, float)
    memcpy(&buffer[pos], &det->lon, 4);
    pos += 4;

    // Flags (1 byte)
    buffer[pos++] = det->flags;

    // Optional: device name if present
    if ((det->flags & FLAG_HAS_NAME) && strlen(det->name) > 0) {
        uint8_t name_len = strlen(det->name);
        if (name_len > 15) name_len = 15;
        buffer[pos++] = name_len;
        memcpy(&buffer[pos], det->name, name_len);
        pos += name_len;
    }

    return pos;
}

// Queue a detection for transmission
void queue_detection(uint8_t type, const uint8_t* mac, int8_t rssi, const char* name) {
    // Find next slot in circular buffer
    uint8_t next_head = (tx_queue_head + 1) % LORA_TX_QUEUE_SIZE;

    // If queue is full, drop oldest
    if (next_head == tx_queue_tail) {
        tx_queue_tail = (tx_queue_tail + 1) % LORA_TX_QUEUE_SIZE;
    }

    DetectionMessage* msg = &tx_queue[tx_queue_head];
    msg->type = type;
    memcpy(msg->mac, mac, 6);
    msg->rssi = rssi;
    msg->lat = (float)current_lat;
    msg->lon = (float)current_lon;
    msg->flags = 0;

    if (gps_valid) msg->flags |= FLAG_GPS_VALID;
    if (rssi > -50) msg->flags |= FLAG_STRONG_SIGNAL;

    if (name && strlen(name) > 0) {
        msg->flags |= FLAG_HAS_NAME;
        strncpy(msg->name, name, 15);
        msg->name[15] = '\0';
    } else {
        msg->name[0] = '\0';
    }

    msg->pending = true;
    tx_queue_head = next_head;

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[Queue] Detection: type=%d, MAC=%s, RSSI=%d\n", type, mac_str, rssi);
}

// Transmit next queued detection
void process_lora_tx() {
    if (!radio_ready) return;

    unsigned long now = millis();
    if (now - last_lora_tx < LORA_TX_INTERVAL_MS) return;

    // Check if there's anything to send
    if (tx_queue_tail == tx_queue_head) return;

    DetectionMessage* msg = &tx_queue[tx_queue_tail];
    if (!msg->pending) {
        tx_queue_tail = (tx_queue_tail + 1) % LORA_TX_QUEUE_SIZE;
        return;
    }

    // Pack message
    uint8_t buffer[64];
    int len = pack_detection_message(msg, buffer, sizeof(buffer));

    if (len > 0) {
        Serial.printf("[LoRa] TX %d bytes...\n", len);

        int state = radio.transmit(buffer, len);

        if (state == RADIOLIB_ERR_NONE) {
            Serial.println("[LoRa] TX success!");
        } else {
            Serial.printf("[LoRa] TX failed, code %d\n", state);
        }

        last_lora_tx = now;
    }

    msg->pending = false;
    tx_queue_tail = (tx_queue_tail + 1) % LORA_TX_QUEUE_SIZE;
}

// ============================================================================
// GPS FUNCTIONS
// ============================================================================

void init_gps() {
    Serial.println("[GPS] Initializing...");

    // Enable GPS module
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, HIGH);
    delay(100);

    // Start GPS serial
    gpsSerial.begin(GPS_BAUD_RATE, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    Serial.println("[GPS] Ready, waiting for fix...");
}

void update_gps() {
    while (gpsSerial.available() > 0) {
        char c = gpsSerial.read();
        gps.encode(c);
    }

    if (gps.location.isUpdated() && gps.location.isValid()) {
        current_lat = gps.location.lat();
        current_lon = gps.location.lng();
        gps_valid = true;
    }
}

// ============================================================================
// DETECTION HELPER FUNCTIONS
// ============================================================================

bool check_mac_prefix(const uint8_t* mac) {
    char mac_str[9];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x", mac[0], mac[1], mac[2]);

    for (size_t i = 0; i < sizeof(mac_prefixes)/sizeof(mac_prefixes[0]); i++) {
        if (strncasecmp(mac_str, mac_prefixes[i], 8) == 0) {
            return true;
        }
    }
    return false;
}

bool check_ssid_pattern(const char* ssid) {
    if (!ssid) return false;

    for (size_t i = 0; i < sizeof(wifi_ssid_patterns)/sizeof(wifi_ssid_patterns[0]); i++) {
        if (strcasestr(ssid, wifi_ssid_patterns[i])) {
            return true;
        }
    }
    return false;
}

bool check_device_name_pattern(const char* name) {
    if (!name) return false;

    for (size_t i = 0; i < sizeof(device_name_patterns)/sizeof(device_name_patterns[0]); i++) {
        if (strcasestr(name, device_name_patterns[i])) {
            return true;
        }
    }
    return false;
}

bool check_raven_service_uuid(NimBLEAdvertisedDevice* device) {
    if (!device || !device->haveServiceUUID()) return false;

    int serviceCount = device->getServiceUUIDCount();
    for (int i = 0; i < serviceCount; i++) {
        NimBLEUUID serviceUUID = device->getServiceUUID(i);
        std::string uuidStr = serviceUUID.toString();

        for (size_t j = 0; j < sizeof(raven_service_uuids)/sizeof(raven_service_uuids[0]); j++) {
            if (strcasecmp(uuidStr.c_str(), raven_service_uuids[j]) == 0) {
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// WIFI PROMISCUOUS MODE HANDLER
// ============================================================================

typedef struct {
    unsigned frame_ctrl:16;
    unsigned duration_id:16;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    unsigned sequence_ctrl:16;
    uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

void IRAM_ATTR wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buff;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)ppkt->payload;
    const wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;

    uint8_t frame_type = (hdr->frame_ctrl & 0xFF) >> 2;
    if (frame_type != 0x20 && frame_type != 0x80) return;  // Probe req or beacon

    // Extract SSID
    char ssid[33] = {0};
    uint8_t *payload = (uint8_t *)ipkt + 24;

    if (frame_type == 0x80) {
        payload += 12;  // Skip beacon fixed params
    }

    if (payload[0] == 0 && payload[1] <= 32) {
        memcpy(ssid, &payload[2], payload[1]);
        ssid[payload[1]] = '\0';
    }

    // Check SSID pattern
    if (strlen(ssid) > 0 && check_ssid_pattern(ssid)) {
        queue_detection(MSG_TYPE_FLOCK_WIFI, hdr->addr2, ppkt->rx_ctrl.rssi, ssid);
        return;
    }

    // Check MAC prefix
    if (check_mac_prefix(hdr->addr2)) {
        queue_detection(MSG_TYPE_FLOCK_WIFI, hdr->addr2, ppkt->rx_ctrl.rssi, ssid);
        return;
    }
}

// ============================================================================
// BLE SCANNING
// ============================================================================

class AdvertisedDeviceCallbacks: public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* advertisedDevice) {
        NimBLEAddress addr = advertisedDevice->getAddress();
        std::string addrStr = addr.toString();
        uint8_t mac[6];
        sscanf(addrStr.c_str(), "%02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);

        int rssi = advertisedDevice->getRSSI();
        std::string name = advertisedDevice->haveName() ? advertisedDevice->getName() : "";

        // Check for Raven surveillance device
        if (check_raven_service_uuid(advertisedDevice)) {
            queue_detection(MSG_TYPE_RAVEN, mac, rssi, name.c_str());
            return;
        }

        // Check MAC prefix
        if (check_mac_prefix(mac)) {
            queue_detection(MSG_TYPE_FLOCK_BLE, mac, rssi, name.c_str());
            return;
        }

        // Check device name
        if (!name.empty() && check_device_name_pattern(name.c_str())) {
            uint8_t det_type = MSG_TYPE_FLOCK_BLE;
            if (strcasestr(name.c_str(), "Penguin")) det_type = MSG_TYPE_PENGUIN;
            if (strcasestr(name.c_str(), "Pigvision")) det_type = MSG_TYPE_PIGVISION;

            queue_detection(det_type, mac, rssi, name.c_str());
            return;
        }
    }
};

// ============================================================================
// CHANNEL HOPPING
// ============================================================================

void hop_channel() {
    unsigned long now = millis();
    if (now - last_channel_hop > CHANNEL_HOP_INTERVAL) {
        current_channel++;
        if (current_channel > MAX_CHANNEL) {
            current_channel = 1;
        }
        esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
        last_channel_hop = now;
    }
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("============================================");
    Serial.println(" FLOCK-YOU T-BEAM SUPREME");
    Serial.println(" LoRa Mesh Surveillance Detection");
    Serial.println("============================================");
    Serial.println();

    // Initialize I2C buses
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire1.begin(I2C1_SDA, I2C1_SCL);

    // Initialize GPS
    init_gps();

    // Initialize LoRa
    radio_ready = init_lora();

    // Initialize WiFi promiscuous mode
    Serial.println("[WiFi] Starting promiscuous mode...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[WiFi] Promiscuous mode on channel %d\n", current_channel);

    // Initialize BLE
    Serial.println("[BLE] Starting scanner...");
    NimBLEDevice::init("");
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);

    Serial.println();
    Serial.println("============================================");
    Serial.println(" READY - Scanning for surveillance devices");
    Serial.println(" Detections will be broadcast over LoRa");
    Serial.println("============================================");
    Serial.println();

    last_channel_hop = millis();
    last_ble_scan = millis();
    last_gps_read = millis();
}

void loop() {
    // Update GPS
    if (millis() - last_gps_read >= GPS_UPDATE_INTERVAL) {
        update_gps();
        last_gps_read = millis();

        if (gps_valid) {
            static bool first_fix = true;
            if (first_fix) {
                Serial.printf("[GPS] Fix acquired: %.6f, %.6f\n", current_lat, current_lon);
                first_fix = false;
            }
        }
    }

    // WiFi channel hopping
    hop_channel();

    // BLE scanning
    if (millis() - last_ble_scan >= BLE_SCAN_INTERVAL && !pBLEScan->isScanning()) {
        pBLEScan->start(BLE_SCAN_DURATION, false);
        last_ble_scan = millis();
    }

    if (!pBLEScan->isScanning() && millis() - last_ble_scan > BLE_SCAN_DURATION * 1000) {
        pBLEScan->clearResults();
    }

    // Process LoRa transmissions
    process_lora_tx();

    delay(10);
}
