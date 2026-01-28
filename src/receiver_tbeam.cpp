// ============================================================================
// FLOCK-YOU LORA RECEIVER - T-BEAM SUPREME
// ============================================================================
// Receives detection broadcasts from flock-you detector nodes
// Displays on OLED and outputs to Serial
//
// Build with: pio run -e tbeam_supreme_receiver
// ============================================================================

#include <Arduino.h>
#include <RadioLib.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <SPI.h>

#include "boards/tbeam_supreme.h"

// ============================================================================
// HARDWARE OBJECTS
// ============================================================================

// SX1262 LoRa Radio
SPIClass radioSPI(HSPI);
SX1262 radio = new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN, radioSPI);

// OLED Display (SH1106 128x64)
U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// ============================================================================
// STATISTICS
// ============================================================================
static uint32_t rx_count = 0;
static uint32_t flock_count = 0;
static uint32_t raven_count = 0;
static int last_rssi = 0;
static float last_snr = 0;

// ============================================================================
// MESSAGE PARSING
// ============================================================================

const char* get_type_name(uint8_t type) {
    switch (type) {
        case MSG_TYPE_FLOCK_WIFI: return "FLOCK-WiFi";
        case MSG_TYPE_FLOCK_BLE:  return "FLOCK-BLE";
        case MSG_TYPE_RAVEN:      return "RAVEN";
        case MSG_TYPE_PENGUIN:    return "PENGUIN";
        case MSG_TYPE_PIGVISION:  return "PIGVISION";
        default:                  return "UNKNOWN";
    }
}

void parse_detection_message(uint8_t* data, int len) {
    if (len < 17) {
        Serial.println("[RX] Message too short");
        return;
    }

    int pos = 0;

    // Type
    uint8_t type = data[pos++];

    // MAC
    uint8_t mac[6];
    memcpy(mac, &data[pos], 6);
    pos += 6;

    // RSSI
    int8_t det_rssi = (int8_t)data[pos++];

    // Latitude
    float lat;
    memcpy(&lat, &data[pos], 4);
    pos += 4;

    // Longitude
    float lon;
    memcpy(&lon, &data[pos], 4);
    pos += 4;

    // Flags
    uint8_t flags = data[pos++];

    // Optional name
    char name[17] = {0};
    if ((flags & FLAG_HAS_NAME) && pos < len) {
        uint8_t name_len = data[pos++];
        if (name_len > 15) name_len = 15;
        if (pos + name_len <= len) {
            memcpy(name, &data[pos], name_len);
            name[name_len] = '\0';
        }
    }

    // Update stats
    rx_count++;
    if (type == MSG_TYPE_RAVEN) raven_count++;
    else flock_count++;

    // Format MAC
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Print to Serial
    Serial.println();
    Serial.println("========== DETECTION RECEIVED ==========");
    Serial.printf("Type:   %s\n", get_type_name(type));
    Serial.printf("MAC:    %s\n", mac_str);
    Serial.printf("RSSI:   %d dBm\n", det_rssi);
    if (flags & FLAG_GPS_VALID) {
        Serial.printf("Loc:    %.6f, %.6f\n", lat, lon);
    } else {
        Serial.println("Loc:    No GPS fix");
    }
    if (strlen(name) > 0) {
        Serial.printf("Name:   %s\n", name);
    }
    Serial.printf("Signal: %s\n", (flags & FLAG_STRONG_SIGNAL) ? "STRONG" : "NORMAL");
    Serial.printf("LoRa:   RSSI=%d, SNR=%.1f\n", last_rssi, last_snr);
    Serial.println("========================================");
    Serial.println();

    // Update display
    display.clearBuffer();

    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, "FLOCK-YOU RECEIVER");
    display.drawHLine(0, 12, 128);

    display.setFont(u8g2_font_5x7_tf);

    char buf[32];
    snprintf(buf, sizeof(buf), "Type: %s", get_type_name(type));
    display.drawStr(0, 24, buf);

    // Show truncated MAC
    snprintf(buf, sizeof(buf), "MAC: %02X:%02X:..:%02X", mac[0], mac[1], mac[5]);
    display.drawStr(0, 34, buf);

    snprintf(buf, sizeof(buf), "Det RSSI: %d dBm", det_rssi);
    display.drawStr(0, 44, buf);

    if (flags & FLAG_GPS_VALID) {
        snprintf(buf, sizeof(buf), "%.4f,%.4f", lat, lon);
        display.drawStr(0, 54, buf);
    }

    snprintf(buf, sizeof(buf), "RX:%lu F:%lu R:%lu", rx_count, flock_count, raven_count);
    display.drawStr(0, 64, buf);

    display.sendBuffer();
}

// ============================================================================
// SETUP & LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("============================================");
    Serial.println(" FLOCK-YOU LORA RECEIVER");
    Serial.println("============================================");

    // Initialize I2C and display
    Wire.begin(I2C_SDA, I2C_SCL);
    display.begin();
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 20, "FLOCK-YOU RX");
    display.drawStr(0, 35, "Initializing...");
    display.sendBuffer();

    // Initialize LoRa
    Serial.println("[LoRa] Initializing SX1262...");
    radioSPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN, RADIO_CS_PIN);

    int state = radio.begin(LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SPREADING_FACTOR,
                            LORA_CODING_RATE, LORA_SYNC_WORD, LORA_TX_POWER,
                            LORA_PREAMBLE_LENGTH);

    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] Init failed: %d\n", state);
        display.clearBuffer();
        display.drawStr(0, 30, "LoRa FAILED!");
        display.sendBuffer();
        while (1) delay(1000);
    }

    radio.setDio2AsRfSwitch(true);
    radio.setTCXO(1.8);
    radio.explicitHeader();
    radio.setCRC(true);

    Serial.printf("[LoRa] Ready on %.1f MHz\n", LORA_FREQUENCY);

    // Update display
    display.clearBuffer();
    display.setFont(u8g2_font_6x10_tf);
    display.drawStr(0, 10, "FLOCK-YOU RECEIVER");
    display.drawHLine(0, 12, 128);
    display.setFont(u8g2_font_5x7_tf);

    char buf[32];
    snprintf(buf, sizeof(buf), "Freq: %.1f MHz", LORA_FREQUENCY);
    display.drawStr(0, 26, buf);
    snprintf(buf, sizeof(buf), "SF%d BW%.0f", LORA_SPREADING_FACTOR, LORA_BANDWIDTH);
    display.drawStr(0, 36, buf);
    display.drawStr(0, 50, "Waiting for");
    display.drawStr(0, 60, "detections...");
    display.sendBuffer();

    Serial.println();
    Serial.println("Listening for detection broadcasts...");
    Serial.println();
}

void loop() {
    // Check for incoming packet
    uint8_t buffer[64];
    int len = radio.receive(buffer, sizeof(buffer));

    if (len > 0) {
        last_rssi = radio.getRSSI();
        last_snr = radio.getSNR();
        parse_detection_message(buffer, len);
    } else if (len == RADIOLIB_ERR_RX_TIMEOUT) {
        // Normal timeout, continue
    } else if (len != RADIOLIB_ERR_NONE) {
        Serial.printf("[LoRa] RX error: %d\n", len);
    }
}
