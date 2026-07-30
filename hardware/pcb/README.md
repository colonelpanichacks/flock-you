# Flock-You Custom PCB

A professional, compact, USB-C powered surveillance detection device on a custom PCB. Designed for easy ordering via JLCPCB or PCBWay assembly services.

---

## 📋 Overview

This custom PCB design integrates all components onto a single compact board:

- **ESP32-WROOM-32E** module (WiFi + microcontroller)
- **USB-C** connector for power and programming
- **CH340C** USB-to-UART bridge (auto-programming support)
- **Integrated piezo buzzer** (12mm SMD)
- **Status LED** (blue, 0805 SMD)
- **Auto-flash circuit** (DTR/RTS method, no button pressing needed)
- **Boot and Reset buttons** for manual control

**Board Size:** 50mm × 35mm (credit card sized, pocket-friendly)

---

## ✨ Key Features

### Plug-and-Play Design
- USB-C connector (reversible, modern standard)
- No breadboard or wiring required
- No soldering needed for end users
- Works with any USB-C cable

### Easy Firmware Updates
- Auto-programming circuit means you just run `pio run -t upload`
- No need to press Boot+Reset buttons
- CH340C driver widely supported (Windows/Mac/Linux)

### Professional Quality
- 2-layer PCB with proper ground planes
- Decoupling capacitors for stable operation
- Voltage regulation for clean 3.3V power
- Test points for debugging

### Cost-Effective Production
- All components in JLCPCB Basic parts library
- Full assembly service available
- Volume pricing scales down to $5-6 per board

---

## 📦 What's Included

This folder contains:

1. **SCHEMATIC.md** - Complete circuit schematic with component descriptions
2. **BOM_JLCPCB.csv** - Bill of Materials with LCSC part numbers
3. **ASSEMBLY_GUIDE.md** - Step-by-step ordering instructions for JLCPCB/PCBWay
4. **README.md** - This file

**Note:** KiCad PCB layout files are not included. You can either:
- Design the PCB yourself using the schematic ($0, requires PCB design skills)
- Hire a PCB designer on Fiverr/Upwork ($50-100, 2-3 days)
- Many designers will create PCB files from a schematic for low cost

---

## 💰 Cost Breakdown

### Prototype Batch (5 boards from JLCPCB)
| Item | Cost |
|------|------|
| PCB Fabrication | $2 |
| SMT Assembly Service | $8 |
| Components (all 5 boards) | $30-40 |
| Shipping (DHL) | $15-20 |
| **TOTAL** | **$55-70** |
| **Per Board** | **$11-14** |

### Production Batch (100 boards)
| Item | Cost |
|------|------|
| PCB Fabrication | $50 |
| SMT Assembly Service | $80 |
| Components (all 100 boards) | $300-350 |
| Shipping (DHL) | $50 |
| **TOTAL** | **$480-530** |
| **Per Board** | **$5-6** |

### Comparison to Alternatives
| Option | Cost | Assembly | Quality |
|--------|------|----------|---------|
| **Breadboard DIY** | $10 | Manual | Fragile |
| **Custom PCB (This)** | $11 (5x) or $6 (100x) | Professional | Production-ready |
| **Flock OUI-SPY** | $149 | Professional | Commercial |

---

## 🚀 Quick Start

### Option 1: Order Pre-Assembled (Easiest)

1. **Get PCB Files:**
   - Have the schematic converted to KiCad Gerber files (~$50-100)
   - Or design yourself in KiCad (free software)

2. **Order from JLCPCB:**
   - Upload Gerber files
   - Enable SMT Assembly
   - Upload BOM and CPL files
   - Wait 7-10 days for delivery

3. **Flash Firmware:**
   ```bash
   cd flock-you-esp32
   source venv/bin/activate
   pio run -t upload
   ```

4. **Done!** Device is ready to use.

See **ASSEMBLY_GUIDE.md** for detailed instructions.

### Option 2: Hand-Assemble (Advanced)

If you have a hot air station and soldering skills:

1. Order bare PCBs ($2 for 5 boards from JLCPCB)
2. Buy components from LCSC/AliExpress (~$8 per board)
3. Assemble using hot air station + soldering iron
4. Total cost: ~$10 per board

See **SCHEMATIC.md** for pinout and assembly notes.

---

## 📐 Technical Specifications

### Electrical
- **Input:** USB-C 5V @ 300mA max
- **Voltage Regulator:** AMS1117-3.3 (800mA capable)
- **MCU:** ESP32-WROOM-32E (240MHz dual-core, 4MB flash)
- **USB Bridge:** CH340C (480 Mbps)
- **Power Consumption:**
  - Idle: 80mA @ 5V
  - Scanning: 150mA @ 5V
  - Peak (WiFi TX): 300mA @ 5V

### Mechanical
- **Board Size:** 50mm × 35mm × 1.6mm
- **Weight:** ~8g (assembled)
- **Mounting Holes:** 4× M2.5 (optional)
- **Fits:** 3D printed case (see `/hardware/openscad/`)

### Environmental
- **Operating Temp:** 0°C to 50°C (ESP32 spec)
- **Storage Temp:** -40°C to 85°C
- **Humidity:** 10% to 90% non-condensing

### Interfaces
- **USB-C:** Power + Serial (115200 baud)
- **Status LED:** GPIO2 (blue, active high)
- **Piezo Buzzer:** GPIO25 (passive, tone() function)
- **Serial Mirror:** GPIO17 (TX-only, 115200 baud for debugging)

### Compliance
- **CE/FCC:** Not certified (hobbyist device)
- **RoHS:** Yes (lead-free HASL or ENIG finish)

---

## 🛠️ Design Choices

### Why ESP32-WROOM-32E?
- Widely available, low cost ($2-3)
- Integrated PCB antenna (no external antenna needed)
- 4MB flash (plenty for firmware + SPIFFS)
- Well-supported in Arduino ecosystem

### Why CH340C USB Bridge?
- Cheaper than CP2102 or FTDI ($0.50 vs $2-4)
- Auto-programming circuit support (DTR/RTS)
- Good driver support across platforms
- SOIC-16 package (easy to assemble)

### Why USB-C?
- Modern standard (future-proof)
- Reversible connector (user-friendly)
- Higher current capability (up to 3A with proper config)
- Same cost as micro-USB in 2026

### Why 12mm SMD Piezo?
- Loud enough for alerts (~80dB)
- Flat profile (fits in slim case)
- Low current (<20mA)
- Available in JLCPCB parts library

### Why 50mm × 35mm Size?
- Credit card sized (easy to carry)
- Fits common project boxes
- Enough space for all components
- Cost is based on 10cm² chunks ($2 flat rate up to 100×100mm)

---

## 🔧 Firmware Compatibility

This PCB uses the same GPIO pin assignments as the firmware:
- **GPIO25** → Piezo buzzer (BUZZER_PIN)
- **GPIO2** → Status LED (LED_PIN, active HIGH)
- **GPIO17** → Serial mirror TX (MIRROR_TX_PIN)
- **GPIO1/GPIO3** → USB serial (TXD0/RXD0)

No code changes needed from the main firmware!

---

## 📝 Bill of Materials

Total: **24 components** per board

### ICs (3)
- 1× ESP32-WROOM-32E module
- 1× CH340C USB-to-UART bridge
- 1× AMS1117-3.3 voltage regulator

### Passives (16)
- 2× 10µF tantalum caps (power filtering)
- 7× 100nF-10µF ceramic caps (decoupling)
- 5× Resistors (pull-ups, LED current limit, USB-C config)
- 1× 12MHz crystal (CH340C clock)

### Other (5)
- 1× USB-C 16-pin connector
- 1× 12mm SMD piezo buzzer
- 1× Blue 0805 LED
- 1× SS34 Schottky diode (USB protection)
- 2× 6×6mm tactile switches (Boot/Reset)

See **BOM_JLCPCB.csv** for complete list with LCSC part numbers.

---

## 🧪 Testing & Validation

After receiving assembled boards:

1. ✅ **Visual inspection** - Check component placement, no solder bridges
2. ✅ **Power test** - Measure 3.3V at test point TP1
3. ✅ **USB enumeration** - Device appears as CH340 serial port
4. ✅ **Flash test** - Upload firmware via USB
5. ✅ **Functional test** - LED blinks, buzzer chirps, detects WiFi targets

See **ASSEMBLY_GUIDE.md** troubleshooting section for common issues.

---

## 🎨 Case Design

A 3D-printable case is available in `/hardware/openscad/flock-you-case.scad`

**Note:** The original case was designed for breadboard assembly. The PCB version will need a modified case design with:
- USB-C cutout on side
- Smaller internal volume (no breadboard)
- Button access holes (Boot/Reset)
- LED light pipe
- Buzzer acoustic port

Case redesign is TODO. Current PCB can be used without case for testing.

---

## 🤝 Contributing

Improvements welcome:

- **PCB Layout:** Create KiCad files from schematic
- **Case Design:** Update OpenSCAD model for PCB version
- **Testing:** Report assembly issues, component replacements
- **Optimization:** Suggest cheaper/better components
- **Documentation:** Improve guides, add photos

---

## 📄 License

Open-source hardware. You may manufacture and sell these boards.

Attribution appreciated but not required. Consider contributing improvements back to the project.

---

## 🔗 Resources

- **JLCPCB:** https://jlcpcb.com
- **PCBWay:** https://www.pcbway.com
- **LCSC Parts:** https://lcsc.com (component supplier)
- **KiCad:** https://www.kicad.org (free PCB design software)
- **ESP32 Datasheet:** https://www.espressif.com/sites/default/files/documentation/esp32-wroom-32e_datasheet_en.pdf
- **CH340 Datasheet:** https://cdn.sparkfun.com/datasheets/Dev/Arduino/Other/CH340DS1.PDF

---

## ❓ FAQ

### Do I need PCB design skills?
No! You can hire someone on Fiverr to convert the schematic to KiCad for $50-100. Provide them with SCHEMATIC.md and they'll create the Gerber files.

### Can I sell assembled devices?
Yes! This is open-source hardware. Consider pricing at $49-69 retail to compete with the $149 commercial OUI-SPY while making good profit.

### How many should I order for first batch?
Order 5 boards ($60 total) to test. If they work well, order 20-50 for volume pricing (~$7-8 per board).

### What if parts go out of stock?
The BOM includes JLCPCB Basic parts (highly stocked). If something is unavailable, the schematic has notes on compatible replacements.

### Can I modify the design?
Absolutely! Fork it, improve it, share your changes. Consider adding:
- Battery power (LiPo + charger IC)
- External antenna (u.FL connector)
- SD card slot (for GPS logging)
- RGB LED (for fancy status indicators)

### Will this pass FCC/CE certification?
Not without additional testing and design changes (proper shielding, filtering). This is a hobbyist/research device.

---

## 📧 Support

For PCB-specific questions, see ASSEMBLY_GUIDE.md or SCHEMATIC.md.

For firmware issues, see the main project README.md.

**Good luck with your builds! 🚀**
