# Flock-You ESP32 - Complete Build Package

**WiFi promiscuous-mode detector for Flock Safety surveillance cameras**

Ported to standard ESP32 hardware for maximum accessibility and cost savings.

---

## 🚀 Quick Links

- **[Setup Instructions](SETUP_INSTRUCTIONS.md)** - Get started in 3 steps
- **[Solderless Build Guide](SOLDERLESS_BUILD_GUIDE.md)** - No soldering required! ($9-11 total)
- **[3D Printable Case](CASE_DESIGN.md)** - Professional enclosure design
- **[Business Analysis](../BUSINESS_ANALYSIS.md)** - Market opportunity & financials
- **[Porting Guide](../ESP32_PORTING_GUIDE.md)** - Technical documentation

---

## ✨ What's Included

This package contains everything you need to build and deploy your own Flock-You detector:

### 📁 Firmware (`/firmware`)
- **main.cpp** - Modified for ESP32 (GPIO 25, 2, 17)
- **platformio.ini** - ESP32 DevKit configuration
- **partitions_4mb.csv** - Optimized for 4MB flash
- **api/** - Flask dashboard for GPS wardriving
- **datasets/** - OUI lists & research data

### 🔧 Hardware (`/hardware`)
- **openscad/** - Parametric case source files
- **stl/** - Ready-to-print STL files (coming soon)
- **assembly_photos/** - Step-by-step build photos (coming soon)

### 📚 Documentation
- Complete user manuals
- Troubleshooting guides
- Business planning resources
- Technical specifications

---

## 💰 Cost Breakdown

| Build Type | Components | Total Cost | Detection Accuracy |
|------------|------------|------------|-------------------|
| **Minimal** | ESP32 + USB cable | **$5** | ✅ 100% |
| **Breadboard** | + Buzzer + breadboard | **$9-11** | ✅ 100% |
| **With Case** | + 3D printed enclosure | **$10-12** | ✅ 100% |
| **OUI-SPY** | Pre-built board | **$85** | ✅ 100% |

**Same detection performance, 85% cost savings!**

---

## 🎯 Three Ways to Build

### Option 1: LED-Only (Cheapest)
**Cost:** $5 | **Time:** 5 minutes | **Difficulty:** ⭐☆☆☆☆

- ESP32 DevKit + USB cable
- Onboard LED provides visual feedback
- Perfect for testing or silent operation
- [Instructions](SETUP_INSTRUCTIONS.md#minimal-led-only---5)

### Option 2: Breadboard Build (Recommended)
**Cost:** $9-11 | **Time:** 10 minutes | **Difficulty:** ⭐⭐☆☆☆

- Add passive buzzer module + breadboard
- Audio chirps on detection
- No soldering required
- [Full Guide](SOLDERLESS_BUILD_GUIDE.md)

### Option 3: Enclosed Build (Professional)
**Cost:** $10-12 | **Time:** 15 minutes + 3hr print | **Difficulty:** ⭐⭐⭐☆☆

- 3D printed case with snap-fit lid
- LED light pipe
- USB strain relief
**[Case Design](CASE_DESIGN.md)**

---

## 🛠️ Quick Start

### 1. Get Hardware
**Minimum:**
- [ESP32 DevKit](https://amazon.com/s?k=ESP32+DevKit) ($5-6)
- USB Micro cable ($1)

**Recommended:**
- [ESP32 Breadboard Kit](https://amazon.com/s?k=ESP32+breadboard+kit) ($15-20)
- Includes everything: ESP32 + breadboard + jumpers + buzzer

### 2. Flash Firmware
```bash
# Install PlatformIO
pip install platformio

# Clone / enter the repo
cd flock-you-esp32

# WiFi-only (recommended first flash — works on any ESP32 DevKit)
pio run -e esp32dev -t upload && pio device monitor

# WiFi + BLE coexistence (continuous BLE scan + WiFi simultaneously)
pio run -e esp32dev-ble -t upload && pio device monitor

# M5Atom variants — use the unified flasher script
./flash.sh          # interactively identifies your device
./flash.sh --once   # flash one device and exit
```

**All supported environments:**

| Environment | Board | BLE |
|-------------|-------|-----|
| `esp32dev` | ESP32 DevKit | — |
| `esp32dev-ble` | ESP32 DevKit | ✅ COEX |
| `m5atom-lite` | M5Atom Lite | — |
| `m5atom-lite-ble` | M5Atom Lite | ✅ COEX |
| `m5atom-echo` | M5Atom Echo | — |
| `m5atom-echo-ble` | M5Atom Echo | ✅ COEX |
| `m5atom-voice` | M5Atom Voice | — |
| `m5atom-voice-ble` | M5Atom Voice | ✅ COEX |
| `m5atom-voices3r` | Atom VoiceS3R (S3) | — |
| `m5atom-voices3r-ble` | Atom VoiceS3R (S3) | ✅ COEX |

### 3. Test Detection
- Device boots with Super Mario 1-2 startup tune
- LED flashes on WiFi traffic
- Buzzer chirps on Flock camera detection
- Drive near known camera locations to verify

**That's it!** You're detecting.

---

## 📊 Detection Methodology

This firmware uses **five research-proven techniques** with a confidence score (0–100):

### 1. WiFi Promiscuous Sniffing (@NitekryDPaul)
- Monitors 2.4 GHz management & data frames
- **Three OUI confidence tiers** (PR#39):
  - **HIGH** (32 OUIs) — exclusively Flock Safety registered → score 40, always alerts
  - **MFR** (6 OUIs) — Liteon/USI contract manufacturer → score 20, silent log only
  - **SoundThinking** (1 OUI) — acoustic sensor co-deployed with Flock → score 35, alerts
- **addr1 receiver-side detection** (catches sleeping cameras)
- **addr3 BSSID fallback** for randomized addr2 frames (now ON by default)

### 2. Wildcard Probe Signature (DeFlockJoplin)
- Flock cameras send **probe requests with empty SSID**
- Combined score OUI+probe = 62 → HIGH CONFIDENCE on first match
- Field-tested: 11/12 cameras detected, only 2 false positives

### 3. SSID Pattern Matching — including LAA-MAC cameras (issue #43)
- Patterns: `"Flock Camera net."`, `"Flock-XXXXXX"`, `"FLOCK-XXXXXX"`, `"penguin"`, `"pigvision"`
- `"Flock Camera net."` cameras use **locally-administered MACs** (OUI matching won't work)
- `ALERT_LAA_SSID` type detects these — SSID is the sole WiFi handle
- Sequential-MAC heuristic: `:DE`/`:DF` last-byte pair on adjacent channels → +10 pts

### 4. BLE Cross-Correlation (`ENABLE_BLE_SCAN=1`)
- Passive NimBLE scan for Flock BLE advertisements
- Checks: mfr-ID `0x09C8` (XUNTONG/Flock), Raven 128-bit service UUIDs (GainSec), device names
- **BLE_COEX_MODE=1** (default for all `-ble` environments): ESP-IDF SW coexistence scheduler
  runs WiFi promiscuous + BLE simultaneously — no promiscuous pause needed
- BLE hit within 60 s of WiFi hit → +20 confidence bonus

### 5. Multi-Address Matching
- **addr2** (transmitter) — standard detection
- **addr1** (receiver) — catches cameras receiving probe responses
- **addr3** (BSSID) — fallback for randomized MACs

**Confidence tiers:** < 30 = LOW (log only) · 30–59 = PROBABLE · ≥ 60 = HIGH (alert)

See [DETECTION_IMPROVEMENTS.md](DETECTION_IMPROVEMENTS.md) for full scoring tables and examples.

---

## 🧪 Native Unit Tests

The detection pattern library (`fy_detect.h`) is fully tested via a host-side
Unity test suite — no ESP32 hardware needed:

```bash
cd flock-you-esp32
pio test -e native                         # run all 38 tests
pio test -e native -f test_ble_matching    # MAC / BLE name / mfr-ID tests (22)
pio test -e native -f test_uuid_matching   # Raven UUID / firmware version (16)
```

All **38 tests pass** against the current `fy_detect.h`.  The test suite covers:
- All 32 high-confidence Flock OUI prefixes (case-insensitive)
- All 6 contract-manufacturer OUIs (Liteon/USI)
- SoundThinking OUI isolation (not in high or mfr lists)
- BLE device name substring matching (case-insensitive)
- BLE mfr-ID `0x09C8` match + rejection of the old incorrect `0x05A7`
- All 8 Raven 128-bit GATT service UUIDs (case-insensitive)
- Raven firmware version estimation from UUID categories

---

## 🎵 Audio Feedback

### Startup Sound
**Super Mario Bros. World 1-2** (underground theme)
- 6 notes: C5 → C4 → A4 → A3 → G#4 → G#3
- Confirms buzzer is working

### New Detection
**Two fast ascending beeps** (2000 Hz → 2800 Hz)
- First time seeing a camera MAC
- Or camera reappears after 30+ seconds

### Heartbeat
**Two monotone beeps** (1500 Hz), every 10 seconds
- At least one camera detected in last 3 seconds
- Confirms active tracking

### Visual
**Onboard LED flashes** on every detection
- Works even without buzzer
- Visible through case light pipe

---

## 📱 Flask Dashboard (GPS Wardriving)

### Features
- Real-time detection visualization
- GPS coordinate tagging (USB puck or browser)
- Export formats: JSON, CSV, KML (Google Earth)
- Multi-device support
- Historical tracking

### Quick Setup
```bash
cd firmware/api
pip install -r requirements.txt
python flockyou.py
```

Open `http://localhost:5000` and select your serial port.

---

## 🔬 Technical Specs

### Detection
- **Channels:** 1, 6, 11 (customizable)
- **Dwell time:** 350ms per channel
- **RSSI threshold:** -95 dBm (configurable)
- **Range:** 50-100m typical, 300m with external antenna
- **Latency:** <10ms from RF frame to alert

### Hardware
- **MCU:** ESP32-WROOM-32 (dual-core 240 MHz)
- **RAM:** 520KB (uses ~85KB)
- **Flash:** 4MB (uses ~1.2MB)
- **Power:** ~180mA @ 3.3V (WiFi active)
- **Battery:** 6-8 hours on 3,000mAh 18650

### Storage
- **SPIFFS:** 1MB partition
- **Capacity:** 200 unique detections with full metadata
- **Persistence:** CRC32-validated, atomic writes
- **Recovery:** Survives power loss mid-save

---

## 📦 What Makes This Special?

### vs. Original Flock-You (XIAO ESP32-S3)
✅ **85% cheaper** ($6 vs $85 for OUI-SPY)  
✅ **Same detection** (identical WiFi chipset)  
✅ **More available** (ESP32 everywhere, XIAO only Seeed)  
✅ **Easier to prototype** (breadboard-friendly)  
✅ **Larger community** (ESP32 has huge support)  

### vs. Other Solutions
✅ **Passive detection** (no transmission, legal)  
✅ **Proven accuracy** (field-tested research)  
✅ **Open source** (modify freely)  
✅ **Portable** (pocket-sized with case)  
✅ **Expandable** (add GPS, batteries, external antenna)  

---

## 🚗 Use Cases

### Privacy Awareness
- Know when you're being surveilled
- Document camera locations
- Share data with DeFlock community
- Raise awareness in your area

### Security Research
- Test detection algorithms
- Map surveillance infrastructure
- Contribute to open research
- Develop counter-measures

### Wardriving
- GPS-tagged detection mapping
- Export to Google Earth (KML)
- Build community databases
- Identify high-surveillance zones

### Vehicle Integration
- Dashboard mount (case design included)
- USB power from car
- Audio alerts while driving
- Optional battery for portability

---

## 📋 Complete BOM

### Electronics
| Part | Qty | Unit Price | Total |
|------|-----|------------|-------|
| ESP32 DevKit | 1 | $5-6 | $5-6 |
| KY-006 Passive Buzzer | 1 | $1-2 | $1-2 |
| 400-pt Breadboard | 1 | $2 | $2 |
| Male-Male Jumpers (3) | 1 | <$1 | <$1 |
| USB Micro Cable | 1 | $1 | $1 |
| **Subtotal** | | | **$9-11** |

### 3D Printed Case (Optional)
| Part | Material | Cost |
|------|----------|------|
| Case Base | 15g PLA | $0.30-0.50 |
| Case Lid | 8g PLA | $0.15-0.25 |
| LED Light Pipe | 2g Clear | $0.05 |
| Mounting Bracket | 12g PLA | $0.25 |
| **Subtotal** | | **$0.75-1.00** |

**Grand Total:** $10-12

---

## 🐛 Troubleshooting

### No startup sound?
- Check passive (not active) buzzer
- Verify GPIO 25 connection
- Try swapping buzzer polarity
- Disable in code: `#define USE_BUZZER 0`

### No detections?
- No cameras nearby (drive to known locations)
- Check serial output (should show channel hopping)
- Lower RSSI threshold: `#define RSSI_MIN -100`
- Verify WiFi promiscuous mode enabled

### Compilation errors?
- Update PlatformIO: `pio upgrade`
- Check board definition: `esp32dev`
- Verify partition file exists
- Clean build: `pio run -t clean`

### Case doesn't fit?
- Scale STL by 101% for looser fit
- Sand snap-fit tabs if too tight
- Check component dimensions against specs
- Use OpenSCAD to customize

**[Full Troubleshooting Guide](SOLDERLESS_BUILD_GUIDE.md#troubleshooting)**

---

## 🤝 Contributing

### Ways to Contribute
- 📸 Share your build photos
- 🐛 Report bugs & issues
- 💡 Suggest features
- 📝 Improve documentation
- 🎨 Design case variants
- 🧪 Field-test and report accuracy
- 🗺️ Submit camera locations to DeFlock

### Remix Culture
This project is licensed **CC-BY-SA 4.0**:
- ✅ Use commercially
- ✅ Modify and remix
- ✅ Share freely
- 📝 Credit original authors
- 🔄 Share-alike license

---

## 🏆 Credits

### Original Firmware
- **colonelpanichacks** - Original Flock-You creator
- **ØяĐöØцяöЪöяцฐ (@NitekryDPaul)** - WiFi research, 30 OUIs, addr1 technique
- **Michael / DeFlockJoplin** - Wildcard-probe signature, 31st OUI
- **Will Greenberg** - BLE manufacturer ID detection
- **DeFlock / FoggedLens** - Crowdsourced ALPR data
- **GainSec** - Raven BLE service UUIDs

### This ESP32 Port
- Modified for standard ESP32 (4MB flash, UART)
- Solderless assembly guide
- 3D printable case design
- Business analysis & documentation
- Community testing & feedback

---

## ⚖️ Legal & Disclaimer

### What This Device Does
- **Passively receives** publicly-broadcast WiFi frames
- **Does not transmit** any signals
- **Does not authenticate** to networks
- **Does not decrypt** any data
- **Educational/research** purposes

### Legality
- Passive WiFi reception is **legal in most jurisdictions**
- Equivalent to listening to public radio broadcasts
- No different from WiFi analyzers or network sniffers
- **Always comply with local laws**

### Use Responsibly
- Respect privacy and property rights
- Use for legitimate security research
- Contribute findings to public good (DeFlock)
- Don't use to enable illegal activity

**The authors assume no liability for misuse.**

---

## 🔗 Resources

### Community
- **Original Repo:** [colonelpanichacks/flock-you](https://github.com/colonelpanichacks/flock-you)
- **De-Flock:** [deflock.me](https://deflock.me) - Crowdsourced camera maps
- **Research:** `firmware/datasets/` - Full methodology

### Hardware
- **ESP32:** [espressif.com](https://www.espressif.com/en/products/socs/esp32)
- **PlatformIO:** [platformio.org](https://platformio.org/)
- **OpenSCAD:** [openscad.org](https://openscad.org/)

### Learn More
- **WiFi Sniffing:** [ESP32 Promiscuous Mode](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/network/esp_wifi.html)
- **3D Printing:** [All3DP Guides](https://all3dp.com/tag/3d-printing-guides/)
- **Privacy Tech:** [EFF Surveillance Self-Defense](https://ssd.eff.org/)

---

## 📈 Project Stats

- **Hardware Cost:** $5-12 (vs $85 OUI-SPY)
- **Build Time:** 5-15 minutes
- **Detection Accuracy:** Same as premium hardware
- **Supported Boards:** Any ESP32 with 4MB+ flash
- **Community:** Growing!

---

## 🎉 Get Started!

**You're 3 steps away from detecting surveillance:**

1. **[Buy hardware](https://amazon.com/s?k=ESP32+DevKit)** → $5-11
2. **[Flash firmware](SETUP_INSTRUCTIONS.md)** → 10 minutes
3. **[Build case](CASE_DESIGN.md)** → Optional

**Questions?** Check the docs or open an issue!

**Ready?** [Start Building →](SETUP_INSTRUCTIONS.md)

---

*Built with love for privacy, security, and open knowledge.*  
*Detect. Document. DeFlock.*
