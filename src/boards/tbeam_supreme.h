// ============================================================================
// T-BEAM SUPREME S3 BOARD CONFIGURATION
// ============================================================================
// LILYGO T-Beam Supreme with ESP32-S3 + SX1262 LoRa + GNSS
// Reference: https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series

#pragma once

#define BOARD_NAME "T-Beam Supreme S3"

// ============================================================================
// SX1262 LoRa Radio Configuration
// ============================================================================
#define RADIO_SCLK_PIN          12
#define RADIO_MISO_PIN          13
#define RADIO_MOSI_PIN          11
#define RADIO_CS_PIN            10
#define RADIO_RST_PIN           5
#define RADIO_DIO1_PIN          1
#define RADIO_BUSY_PIN          4

// SX1262 specific settings
#define SX126X_DIO2_AS_RF_SWITCH    true
#define SX126X_DIO3_TCXO_VOLTAGE    1.8

// LoRa frequency (change based on region)
// US: 915MHz, EU: 868MHz, etc.
#ifndef LORA_FREQUENCY
#define LORA_FREQUENCY          915.0   // MHz - US frequency
#endif

#define LORA_BANDWIDTH          125.0   // kHz
#define LORA_SPREADING_FACTOR   10      // SF10 - good range/speed balance
#define LORA_CODING_RATE        5       // 4/5
#define LORA_SYNC_WORD          0x12    // Private network (Meshtastic uses 0x2B)
#define LORA_TX_POWER           22      // dBm (max for SX1262)
#define LORA_PREAMBLE_LENGTH    8

// ============================================================================
// GPS/GNSS Configuration (U-blox MAX-M10S or Quectel L76K)
// ============================================================================
#define GPS_RX_PIN              9
#define GPS_TX_PIN              8
#define GPS_EN_PIN              7       // GPS enable/power control
#define GPS_PPS_PIN             6       // Pulse per second
#define GPS_BAUD_RATE           9600

// ============================================================================
// I2C Buses
// ============================================================================
// Primary I2C (OLED display, sensors)
#define I2C_SDA                 17
#define I2C_SCL                 18

// Secondary I2C (PMU - AXP2101)
#define I2C1_SDA                42
#define I2C1_SCL                41

// ============================================================================
// OLED Display (SH1106 128x64)
// ============================================================================
#define OLED_SDA                I2C_SDA
#define OLED_SCL                I2C_SCL
#define OLED_RST                -1      // No reset pin
#define OLED_ADDR               0x3C

// ============================================================================
// Power Management (AXP2101)
// ============================================================================
#define PMU_IRQ                 40
#define PMU_WIRE                Wire1   // Uses secondary I2C

// ============================================================================
// User Button
// ============================================================================
#define BUTTON_PIN              0       // Boot button

// ============================================================================
// Onboard Sensors
// ============================================================================
// QMI8658 - 6-axis IMU (I2C primary bus)
#define IMU_ADDR                0x6B

// BME280 - Temp/Humidity/Pressure (I2C primary bus)
#define BME280_ADDR             0x76

// ============================================================================
// LED (optional, some variants have it)
// ============================================================================
#define LED_PIN                 -1      // No dedicated LED on Supreme

// ============================================================================
// Buzzer (external, optional)
// ============================================================================
#define BUZZER_PIN              3       // GPIO3 if you add an external buzzer

// ============================================================================
// SD Card (optional, some variants)
// ============================================================================
#define SD_CS                   -1      // Not present on all variants

// ============================================================================
// Battery Monitoring
// ============================================================================
#define BATTERY_ADC_PIN         -1      // Handled by AXP2101 PMU

// ============================================================================
// Compact Message Format for LoRa Transmission
// ============================================================================
// To fit within LoRa's bandwidth constraints, we use a compact format:
//
// Detection Message (max ~60 bytes):
// [TYPE:1][MAC:6][RSSI:1][LAT:4][LON:4][FLAGS:1] = 17 bytes minimum
//
// TYPE: 0x01=Flock WiFi, 0x02=Flock BLE, 0x03=Raven, 0x04=Penguin
// FLAGS: bit0=has_name, bit1=strong_signal, bit2=gps_valid
//
// Extended format with name (if needed):
// [TYPE:1][MAC:6][RSSI:1][LAT:4][LON:4][FLAGS:1][NAME_LEN:1][NAME:N]

#define MSG_TYPE_FLOCK_WIFI     0x01
#define MSG_TYPE_FLOCK_BLE      0x02
#define MSG_TYPE_RAVEN          0x03
#define MSG_TYPE_PENGUIN        0x04
#define MSG_TYPE_PIGVISION      0x05

#define FLAG_HAS_NAME           0x01
#define FLAG_STRONG_SIGNAL      0x02
#define FLAG_GPS_VALID          0x04
#define FLAG_MAC_MATCH          0x08
#define FLAG_NAME_MATCH         0x10

#endif // TBEAM_SUPREME_H
