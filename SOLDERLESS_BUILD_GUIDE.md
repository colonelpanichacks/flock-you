# Solderless Build Guide - Flock-You ESP32

## 🎯 Goal: Build Without Soldering

This guide shows you how to assemble a Flock-You detector using **zero soldering** - perfect for beginners or quick prototypes.

---

## 🛒 Shopping List (Solderless Components)

### Option 1: Breadboard Build with Pre-Wired Buzzer ($8-10) ⭐ **EASIEST**
| Item | Quantity | Price | Where to Buy | Search Terms |
|------|----------|-------|--------------|--------------|
| ESP32 DevKit (30-pin) | 1 | $5-6 | Amazon, AliExpress | "ESP32 DevKit" |
| 400-point Breadboard | 1 | $2 | Amazon, AliExpress | "breadboard 400" |
| **KY-006 Passive Buzzer Module** | 1 | $1-2 | Amazon, AliExpress | **"KY-006 buzzer module"** |
| USB Micro Cable | 1 | $1 | Amazon, local stores | "USB micro cable" |

**Total: $9-11**

**Why KY-006?** It comes with a 3-pin Dupont connector (male pins) that plugs directly into breadboard or ESP32 female headers - **NO separate jumper wires needed!**

### Option 2: Grove System Build ($12-15)
| Item | Quantity | Price | Where to Buy |
|------|----------|-------|--------------|
| ESP32 DevKit with Grove Base | 1 | $8-10 | Seeed Studio, Amazon |
| Grove Buzzer Module | 1 | $3-4 | Seeed Studio, Amazon |
| Grove Cable (4-pin) | 1 | $1 | Usually included |
| USB Micro Cable | 1 | $1 | Amazon, local stores |

**Total: $13-16**

### Option 3: Qwiic/STEMMA System Build ($15-18)
| Item | Quantity | Price | Where to Buy |
|------|----------|-------|--------------|
| ESP32 DevKit (any) | 1 | $5-6 | Amazon, AliExpress |
| Qwiic/STEMMA Buzzer Breakout | 1 | $6-8 | Adafruit, SparkFun |
| Qwiic/STEMMA Cable | 1 | $1-2 | Adafruit, SparkFun |
| Female-to-Female Jumpers | 3 | $1 | Amazon, AliExpress |
| USB Micro Cable | 1 | $1 | Amazon, local stores |

**Total: $14-19**

---

## 🔧 Option 1: Breadboard Build (Recommended)

### Parts You Need

**1. ESP32 DevKit Board**
- Any 30-pin ESP32-WROOM-32 DevKit
- Make sure it has 15 pins on each side
- $5-6 on Amazon/AliExpress

**2. Passive Piezo Buzzer Module**
Look for modules with **3 pins** (VCC, GND, I/O):
- **KY-006** Passive Buzzer Module ~$1-2
- **XY-T** Passive Buzzer Module ~$1-2
- Any "passive piezo buzzer module" with 3 pins

**Important:** Get "passive" not "active"! 
- ✅ **Passive** = can play different tones (what we need)
- ❌ **Active** = only one tone (won't work)

**3. Breadboard & Jumpers**
- 400-point breadboard (standard half-size)
- 3× male-to-male jumper wires
- Any color works, but red/black/yellow helps

### Assembly Steps

**Step 1: Insert ESP32 into Breadboard**
```
   Breadboard Layout (top view)
   ═══════════════════════════════
   ┌─────────────────────────────┐
   │  1  2  3  4  5 ... 15       │
   │  ●  ●  ●  ●  ●  ...  ●      │ ← ESP32 Left Side
   │                             │
   │  ●  ●  ●  ●  ●  ...  ●      │ ← ESP32 Right Side
   │  1  2  3  4  5 ... 15       │
   └─────────────────────────────┘
```

1. Straddle the ESP32 across the center gap
2. Push down gently but firmly
3. All 30 pins should be inserted

**Step 2: Connect Buzzer Module**

Buzzer modules typically have 3 pins:
- **S** or **I/O** = Signal pin
- **VCC** or **+** = Power (3.3V or 5V)
- **GND** or **-** = Ground

**Wiring:**
```
ESP32 DevKit          Buzzer Module
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    GPIO 25  ────────► S (Signal/I/O)
    
    3V3      ────────► VCC (+) or (leave unconnected*)
    
    GND      ────────► GND (-)
```

**Important Note:** Most passive buzzer modules are "self-powered" from the signal pin and don't need VCC. If yours has only 2 pins (S and GND), that's normal - just connect those two.

**Step 3: Insert Jumper Wires**

Using 3 male-to-male jumpers:

1. **GPIO 25 → Buzzer S**
   - Find GPIO 25 on ESP32 (usually labeled)
   - Insert one jumper end in same row as GPIO 25
   - Insert other end in Buzzer S pin row

2. **GND → Buzzer GND**
   - Find any GND pin on ESP32
   - Insert jumper in same row as GND
   - Insert other end in Buzzer GND pin row

3. **3V3 → Buzzer VCC** (optional, see note above)
   - Only if your buzzer has 3 pins
   - Connect 3V3 to VCC if buzzer requires it

**Visual Reference:**
```
Breadboard with ESP32 & Buzzer (side view):

     ESP32 (top down)                Buzzer Module
        
│ 3V3─●  USB  ●─GND │            ┌─────────────┐
│ EN ─●       ●─D23 │            │   ┌─────┐   │
│ D36─●       ●─D22 │  Jumpers   │   │ ))) │   │
│ D39─●       ●─TX  │  ═══════►  │   └─────┘   │
│ D34─●       ●─RX  │            │  S  +  GND  │
│ D35─●       ●─D21 │            └──●──●──●───┘
│ D32─●       ●─D19 │               │  │  │
│ D33─●       ●─D18 │               │  │  └──────┐
│ D25─●       ●─D5  │ ◄═════════════╝  │         │
│ D26─●       ●─D17 │                  │         │
│ D27─●       ●─D16 │                  │         │
│ D14─●       ●─D4  │                  │         │
│ D12─●       ●─D0  │ ◄════════════════╝         │
│ GND─●       ●─D2  │ ◄══════════════════════════╝
│ D13─●       ●─D15 │
```

**Step 4: Connect USB Cable**
- Plug micro USB cable into ESP32
- Connect to computer
- LED on ESP32 should light up

### Testing

1. Power on (ESP32 LED lights up)
2. Flash firmware: `pio run -t upload`
3. Monitor serial: `pio device monitor`
4. Should hear 6-note startup tune (Super Mario 1-2)
5. LED flashes when WiFi traffic detected
6. Buzzer chirps on Flock camera detection

**No sound?** See troubleshooting below.

---

## 🌿 Option 2: Grove System Build

Grove connectors are fool-proof - just plug and play!

### Parts You

 Need

**1. ESP32 with Grove Base**
- "ESP32 DevKit Grove Base Shield" ($8-10)
- Or "ESP32 + Grove Shield" combo
- Available on Seeed Studio, Amazon

**2. Grove Buzzer Module**
- "Grove - Buzzer" (passive) ~$3-4
- Item: 101020082 (Seeed Studio)
- Comes with Grove cable

### Assembly Steps

**Step 1: Attach Grove Base to ESP32**
1. Remove ESP32 from packaging
2. Align Grove Base Shield over ESP32 pins
3. Press down firmly to seat all pins
4. Base should click into place

**Step 2: Connect Grove Buzzer**
1. Take Grove cable (4-pin, 2mm pitch)
2. Plug one end into Buzzer module
3. Plug other end into **D25** port on Grove Base
4. Cable only fits one way - don't force it

**Step 3: Flash & Test**
1. Connect USB cable
2. Flash firmware
3. Should hear startup tune immediately
4. Grove system auto-configures - zero setup needed!

**Why Grove?**
- ✅ No wiring errors possible
- ✅ Professional-looking build
- ✅ Hot-swappable modules
- ✅ Sturdy connections (vibration-proof)

---

## 🔌 Option 3: Qwiic/STEMMA Build

Similar to Grove but uses I2C... except we're wiring it manually to GPIO 25.

### Parts You Need

**1. ESP32 DevKit (any)**
- Standard 30-pin ESP32
- $5-6

**2. Qwiic/STEMMA Buzzer Breakout**
- SparkFun Qwiic Buzzer (~$7)
- Or Adafruit STEMMA-compatible buzzer
- Comes with Qwiic cable

**3. Female-to-Female Jumpers**
- 3× jumpers to adapt Qwiic to GPIO
- $1 for pack of 40

### Assembly Steps

**Step 1: Identify Qwiic Buzzer Pins**

Qwiic buzzers usually have:
- **SIG** = Signal
- **VCC** = 3.3V or 5V
- **GND** = Ground
- **I2C pins** = (ignore, we're using SIG)

**Step 2: Wire with Jumpers**

```
ESP32 Pin   →  Female Jumper  →  Buzzer Pin
━━━━━━━───────────────────────────━━━━━━━━
GPIO 25     →  (any color)    →  SIG
GND         →  Black          →  GND  
3V3         →  Red            →  VCC (optional)
```

1. Take 3 female-to-female jumpers
2. Plug one end into ESP32 GPIO 25 pin
3. Plug other end into Buzzer SIG pin
4. Repeat for GND and VCC (if needed)

**Why Qwiic?**
- Compact, professional modules
- Same ecosystem as SparkFun/Adafruit
- Easier to expand with other sensors later

---

## 🧰 Recommended Products (Links)

### Breadboard Build Components

**ESP32 DevKit:**
- Search: "ESP32 DevKit V1 WROOM-32"
- Amazon: ~$6 with Prime shipping
- AliExpress: ~$3-4 (2-3 week shipping)

**Passive Buzzer Module:**
- Search: "KY-006 passive buzzer module"
- Amazon: ~$8 for 5-pack
- AliExpress: ~$1 each

**Breadboard & Jumpers:**
- Search: "400 point breadboard jumper wire kit"
- Amazon: ~$10 for complete kit
- Includes breadboard + 140 jumpers

### Grove Build Components

**Grove Base Shield:**
- Search: "ESP32 Grove base shield"
- Seeed Studio: Official source
- Amazon: Usually in stock

**Grove Buzzer:**
- Search: "Grove buzzer passive"
- Seeed: Part #101020082
- Amazon: Also available

### Complete Kits

**All-in-One ESP32 Breadboard Kit:**
- Search: "ESP32 breadboard kit"
- Includes: ESP32 + breadboard + jumpers + LEDs + buzzers
- Amazon: $15-20
- Perfect for beginners - everything included!

---

## 🐛 Troubleshooting

### No Startup Sound

**Possible causes:**

1. **Active vs Passive Buzzer**
   - **Problem:** You have an "active" buzzer (won't play tones)
   - **Solution:** Order a "passive" buzzer module
   - **How to tell:** Active buzzers have a black chip on back

2. **Wrong GPIO Pin**
   - **Problem:** Buzzer connected to wrong pin
   - **Solution:** Verify GPIO 25 connection (count from end)
   - **Check:** Use multimeter or LED to test pin

3. **Power Issue**
   - **Problem:** USB port not providing enough current
   - **Solution:** Try powered USB hub or different port
   - **Check:** Does onboard ESP32 LED light up?

4. **Buzzer Polarity**
   - **Problem:** Some passive buzzers are polarized
   - **Solution:** Swap the Signal and GND wires
   - **Test:** Try both orientations

5. **Firmware Not Using GPIO 25**
   - **Problem:** Old firmware version
   - **Solution:** Re-download latest main.cpp
   - **Verify:** Check line 12 has `#define BUZZER_PIN 25`

### Buzzer Sounds Distorted

**Causes & Fixes:**

- **Too much current:** Add 100Ω resistor between GPIO 25 and buzzer Signal
- **Wrong voltage:** Some buzzers need 5V, try connecting VCC to VIN (5V) instead of 3V3
- **EMI interference:** Move buzzer away from ESP32 antenna area

### Connection Keeps Falling Out

**Breadboard connections:**
- Insert pins fully - should feel firm
- If loose, try different breadboard rows
- Clean pin legs with isopropyl alcohol

**Grove connections:**
- Push connector in firmly until it clicks
- Cable might be backwards - try flipping it
- Check for bent pins in connector

### Need to Disable Buzzer Temporarily?

Edit `main.cpp` line 13:
```cpp
#define USE_BUZZER 0  // Change 1 to 0
```

Re-upload firmware. LED still works for visual feedback.

---

## ⚡ Quick Start Checklist

- [ ] ESP32 DevKit purchased ($5-6)
- [ ] Passive buzzer module ordered ($1-2)
- [ ] Breadboard & jumpers acquired ($2)
- [ ] USB cable ready (Micro USB)
- [ ] PlatformIO installed (VS Code extension)
- [ ] Firmware downloaded (`flock-you-esp32/firmware/`)
- [ ] ESP32 inserted into breadboard (straddles center)
- [ ] Buzzer GPIO 25 → Signal wire connected
- [ ] Buzzer GND → Ground wire connected
- [ ] USB plugged in (ESP32 LED lights up)
- [ ] Firmware flashed (`pio run -t upload`)
- [ ] Startup tune plays (6 notes)
- [ ] Serial monitor shows scanning
- [ ] Ready to detect!

---

## 🎯 Next Step: 3D Printed Case

Once your solderless build is working, check out `CASE_DESIGN.md` for a 3D printable enclosure that holds everything together professionally!

The case design accommodates:
- Standard ESP32 DevKit (30-pin)
- Breadboard-mounted buzzer
- USB cable strain relief
- Mounting holes for car dashboard
- LED light pipe for onboard LED

---

## 💡 Pro Tips

### Tip 1: Color-Code Your Wires
- **Red** = Power (3V3 or VCC)
- **Black** = Ground (GND)
- **Yellow** = Signal (GPIO 25)

Makes troubleshooting easier!

### Tip 2: Test Buzzer First
Before assembling, test buzzer independently:
```cpp
void setup() {
  pinMode(25, OUTPUT);
  tone(25, 1000);  // 1kHz tone
  delay(1000);
  noTone(25);
}
```

Should hear 1 second beep.

### Tip 3: Secure for Vehicle Use
If using in a car:
- Use velcro strips to mount breadboard
- Zip-tie USB cable for strain relief
- Consider Grove system for vibration resistance

### Tip 4: Battery Power
For portable use without USB:
- Use 18650 battery holder with 5V boost
- Connect to ESP32 VIN pin (5V input)
- Typical runtime: 6-8 hours

---

## 📷 Visual Assembly Guide

See the `/hardware/assembly_photos/` folder for:
- High-res photos of each assembly step
- Close-ups of correct pin connections
- Common mistake photos (what NOT to do)
- Finished build examples

---

## ✅ Success!

You now have a **fully functional Flock-You detector** without touching a soldering iron!

**Hardware cost:** $9-11  
**Assembly time:** 5-10 minutes  
**Skill required:** Plug things in  
**Detection accuracy:** Same as $85 OUI-SPY board  

**Next steps:**
1. Flash firmware and test
2. Drive near known Flock camera locations
3. Build 3D printed case (optional)
4. Add GPS module for wardriving (advanced)

Happy detecting! 🎉
