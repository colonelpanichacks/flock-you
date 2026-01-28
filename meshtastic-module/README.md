# Flock Detector Module for Meshtastic

Surveillance camera detection module that integrates with Meshtastic firmware. Detects Flock Safety ALPR cameras, Raven gunshot detectors, and other surveillance devices, then broadcasts alerts over the Meshtastic mesh network.

## What It Detects

| Device | Detection Method |
|--------|------------------|
| **Flock Safety** | WiFi SSID patterns, MAC prefixes, BLE device names |
| **Raven** (ShotSpotter) | BLE service UUIDs (firmware 1.1.7 - 1.3.x) |
| **Penguin** | WiFi/BLE device name matching |
| **Pigvision** | WiFi/BLE device name matching |

## How It Works

```
┌─────────────────────────────────────────────────────────┐
│              T-Beam Supreme + Meshtastic                │
│                                                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐ │
│  │   WiFi      │  │    BLE      │  │   Meshtastic    │ │
│  │ Promiscuous │  │   Scanner   │  │   LoRa Mesh     │ │
│  │   Mode      │  │             │  │                 │ │
│  └──────┬──────┘  └──────┬──────┘  └────────▲────────┘ │
│         │                │                  │          │
│         └───────┬────────┘                  │          │
│                 │                           │          │
│         ┌───────▼───────┐                   │          │
│         │ FlockDetector │                   │          │
│         │    Module     │───────────────────┘          │
│         └───────────────┘                              │
│              Detections sent as TEXT_MESSAGE           │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼
              ┌───────────────────────┐
              │  Any Meshtastic Node  │
              │  - Phone App          │
              │  - Another T-Beam     │
              │  - Web Client         │
              └───────────────────────┘
```

## Alert Format

Detections are broadcast as text messages that any Meshtastic device can receive:

```
ALERT FLOCK-WiFi
MAC:58:8E:81:XX:XX:XX
RSSI:-65dBm
Loc:37.774900,-122.419400
Name:Flock_Camera
```

```
ALERT RAVEN
MAC:12:34:56:78:9A:BC
RSSI:-72dBm
Loc:37.774900,-122.419400
```

**Note:** Alerts are sent to **channel 1** (secondary channel), not the primary channel. Configure channel 1 on your Meshtastic devices to receive alerts.

## Channel Setup

Before using, configure channel 1 on all devices that should receive alerts:

```bash
# Using meshtastic CLI
meshtastic --ch-index 1 --ch-set name "FLOCK"
meshtastic --ch-index 1 --ch-set psk random
```

Or in the Meshtastic app: **Settings → Channels → Channel 1** → Set name and PSK.

## Integration Instructions

### 1. Clone Meshtastic Firmware

```bash
git clone https://github.com/meshtastic/firmware.git
cd firmware
git submodule update --init --recursive
```

### 2. Copy Module Files

```bash
# Copy module files to Meshtastic source
cp FlockDetectorModule.h firmware/src/modules/esp32/
cp FlockDetectorModule.cpp firmware/src/modules/esp32/
```

### 3. Register Module in Modules.cpp

Edit `firmware/src/modules/Modules.cpp`:

```cpp
// Add include at top
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_FLOCKDETECTOR
#include "modules/esp32/FlockDetectorModule.h"
#endif

// Add in setupModules() function, in the ESP32 section
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_FLOCKDETECTOR
    new FlockDetectorModule();
#endif
```

### 4. (Optional) Disable Unused Modules

To save flash space and avoid conflicts, you can disable PaxcounterModule since FlockDetector does similar scanning. Edit your `platformio.ini` variant or add build flags:

```ini
build_flags =
    -DMESHTASTIC_EXCLUDE_PAXCOUNTER=1
    -DMESHTASTIC_EXCLUDE_STOREFORWARD=1  ; optional
```

### 5. Build for T-Beam Supreme

```bash
# Build for T-Beam S3 Supreme
pio run -e tbeam-s3-core
```

### 6. Flash Your Device

```bash
pio run -e tbeam-s3-core -t upload
```

## Configuration

The module runs automatically after flashing. Default settings:

| Parameter | Value | Description |
|-----------|-------|-------------|
| `CHANNEL_HOP_INTERVAL_MS` | 500 | WiFi channel hop rate |
| `BLE_SCAN_INTERVAL_MS` | 10000 | Time between BLE scans |
| `BLE_SCAN_DURATION_SEC` | 3 | BLE scan duration |
| `REPORT_INTERVAL_MS` | 5000 | Min time between mesh broadcasts |
| `SEEN_TIMEOUT_MS` | 60000 | Don't re-report same device for 1 min |

To modify, edit the `#define` values in `FlockDetectorModule.cpp`.

## Receiving Alerts

Alerts appear as regular text messages on any Meshtastic device connected to the same mesh:

- **Meshtastic Android/iOS App** - Shows in message list
- **Meshtastic Web Client** - Shows in chat
- **Another T-Beam** - Shows on OLED display (if equipped)
- **Serial Console** - `meshtastic --port /dev/ttyUSB0 --listen`

## Hardware Requirements

- **T-Beam Supreme S3** (recommended) - ESP32-S3, SX1262 LoRa, GPS
- Or any ESP32-based Meshtastic device

Note: This module requires ESP32 for WiFi promiscuous mode and NimBLE support.

## Disabling the Module

To build Meshtastic without the FlockDetector module:

```ini
build_flags =
    -DMESHTASTIC_EXCLUDE_FLOCKDETECTOR=1
```

## License

Same license as flock-you and Meshtastic (GPL-3.0).
