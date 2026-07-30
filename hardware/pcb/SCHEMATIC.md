# Flock-You ESP32 PCB Schematic

## Board Overview
Compact surveillance detection device with USB-C connectivity, designed for JLCPCB/PCBWay assembly service.

**Board Dimensions:** 50mm × 35mm (fits standard enclosure)
**Layer Count:** 2-layer PCB
**Power:** USB-C 5V input, regulated to 3.3V

---

## Component List

### Main Components
1. **ESP32-WROOM-32E** (U1) - Main microcontroller module
2. **CH340C** (U2) - USB-to-UART bridge IC (SOIC-16)
3. **AMS1117-3.3** (U3) - 3.3V LDO voltage regulator (SOT-223)
4. **USB-C Connector** (J1) - 16-pin mid-mount type
5. **Piezo Buzzer** (BZ1) - SMD passive piezo (12mm diameter)
6. **LED** (D1) - 0805 Blue LED
7. **Schottky Diode** (D2) - SS34 (SMA package) for USB protection

### Passive Components
- **C1, C2**: 10µF tantalum capacitors (input/output for AMS1117)
- **C3, C4, C5, C6**: 100nF ceramic capacitors (0805) - decoupling
- **C7**: 10µF ceramic capacitor (0805) - ESP32 power
- **C8, C9**: 22pF ceramic capacitors (0603) - CH340C crystal
- **R1**: 10kΩ resistor (0805) - EN pull-up
- **R2**: 10kΩ resistor (0805) - GPIO0 pull-up
- **R3**: 330Ω resistor (0805) - LED current limiting
- **R4, R5**: 5.1kΩ resistors (0805) - USB-C CC pins
- **Y1**: 12MHz crystal (HC-49S SMD) - for CH340C

### User Interface
- **SW1**: Tactile switch (6×6mm SMD) - Boot button
- **SW2**: Tactile switch (6×6mm SMD) - Reset button

---

## Circuit Description

### Power Supply Section
```
USB-C (J1) → D2 (SS34) → AMS1117-3.3 (U3) → 3.3V Rail
                ↓
              C1 (10µF)
                                      ↓
                                    C2 (10µF)
```

**USB-C Configuration:**
- VBUS (pins A4, A9, B4, B9) connected together → input power
- GND (pins A1, A12, B1, B12) connected to ground plane
- CC1 (pin A5) and CC2 (pin B5) each through 5.1kΩ to GND (USB-C power sink config)
- D+, D- connected to CH340C

**Voltage Regulation:**
- Input: 5V from USB (after SS34 diode, ~4.7V)
- Output: 3.3V @ 1A max
- C1 = 10µF (input stabilization)
- C2 = 10µF (output stabilization)

### USB-to-UART Bridge (CH340C)
```
USB-C D+/D- → CH340C (U2) → TX/RX → ESP32 (U1)
        ↓                            ↓
     Y1 (12MHz)                  Auto-program
     C8, C9 (22pF)               circuit (DTR/RTS)
```

**CH340C Connections:**
- Pin 1 (GND) → Ground
- Pin 2 (TXD) → ESP32 RXD0 (GPIO3)
- Pin 3 (RXD) → ESP32 TXD0 (GPIO1)
- Pin 4 (V3) → 3.3V rail
- Pin 5 (UD+) → USB D+
- Pin 6 (UD-) → USB D-
- Pin 7 (XI) → Crystal Y1 pin 1
- Pin 8 (XO) → Crystal Y1 pin 2
- Pin 13 (DTR#) → Auto-program circuit
- Pin 14 (RTS#) → Auto-program circuit
- Pin 16 (VCC) → 3.3V rail
- C8, C9: 22pF crystal load caps to ground

### Auto-Programming Circuit (DTR/RTS Method)
```
DTR (CH340C pin 13) → 100nF → GPIO0 (ESP32)
RTS (CH340C pin 14) → 100nF → EN (ESP32)

GPIO0: Also connected to SW1 (Boot button) and R2 (10kΩ pull-up)
EN:    Also connected to SW2 (Reset button) and R1 (10kΩ pull-up)
```

This allows automatic bootloader entry when uploading firmware via USB.

### ESP32-WROOM-32E Module (U1)
```
Main connections:
- Pin 1 (GND) → Ground plane
- Pin 2 (3V3) → 3.3V rail + C7 (10µF)
- Pin 3 (EN) → Pull-up R1 + SW2 + auto-program
- Pin 25 (GPIO0) → Pull-up R2 + SW1 + auto-program
- Pin 34 (TXD0/GPIO1) → CH340C RXD
- Pin 35 (RXD0/GPIO3) → CH340C TXD
- Pin 12 (GPIO25) → Piezo buzzer + resistor
- Pin 24 (GPIO2) → LED anode (via R3)
- Pin 28 (GPIO17) → Test point (serial mirror TX)
- Pin 38 (GND) → Ground plane
- Pin 39 (GND) → Ground plane (thermal pad)
```

**Decoupling:**
- C3, C4, C5, C6: 100nF ceramics placed near ESP32 power pins
- C7: 10µF bulk capacitance on 3.3V supply

### Piezo Buzzer (BZ1)
```
GPIO25 → [optional 100Ω series R] → Piezo+ 
                                    Piezo- → GND
```

**Notes:**
- Passive piezo buzzer (12mm SMD type)
- Driven by ESP32's tone() function via GPIO25
- Series resistor optional (limits current)
- JLCPCB Part: Murata 7BB-12-9 or equivalent

### Status LED (D1)
```
GPIO2 → R3 (330Ω) → LED Anode
                    LED Cathode → GND
```

**Notes:**
- Blue 0805 LED
- Forward voltage ~3.0V
- Current: (3.3V - 3.0V) / 330Ω ≈ 1mA (very low, visible)
- Active HIGH (LED on when GPIO2 = HIGH)

---

## PCB Layout Guidelines

### Layer Stack
```
TOP:    Components, signal traces, ground pour
BOTTOM: Ground plane, signal traces, component footprints
```

### Ground Plane
- Solid ground pour on both layers
- Thermal relief for through-holes
- Via stitching around board perimeter (every 5mm)

### Power Routing
- 5V USB trace: 0.5mm width minimum
- 3.3V rail: 0.4mm width minimum
- Ground: plane + pour, multiple vias for thermal dissipation

### Component Placement

**Front (Top) Side:**
```
┌─────────────────────────────────────┐
│  [USB-C J1]                         │
│                                     │
│  [SW1] [SW2]                        │
│  Boot  Reset                        │
│                                     │
│  ┌──────────────────┐              │
│  │                  │     [D1 LED] │
│  │  ESP32-WROOM-32E │              │
│  │       (U1)       │              │
│  │                  │              │
│  └──────────────────┘              │
│                                     │
│  [U2]    [U3]          [BZ1]       │
│  CH340C  AMS1117       Piezo       │
│  (under) (under)       (12mm)      │
│                                     │
└─────────────────────────────────────┘
```

**Bottom Side:**
- CH340C (U2) placed under ESP32 module area
- AMS1117-3.3 (U3) near USB connector
- Crystal (Y1) close to CH340C
- Passive components distributed

### Critical Traces
1. **USB differential pair (D+/D-):**
   - Same length ±0.1mm
   - 90Ω differential impedance
   - Keep away from crystal/switching noise

2. **Crystal traces (CH340C):**
   - Keep short (<10mm)
   - Ground plane under crystal
   - Guard traces optional

3. **ESP32 RF area:**
   - Keep clear per ESP32 design guidelines
   - No ground pour under antenna
   - 50Ω trace to antenna (if external antenna used)

### Antenna
- Use ESP32-WROOM-32E module with PCB antenna (built-in)
- Keep board edge clear 15mm from module antenna side

---

## Manufacturing Specifications

### PCB Parameters
- **Board size:** 50mm × 35mm
- **Layers:** 2
- **Thickness:** 1.6mm
- **Copper weight:** 1oz (35µm)
- **Surface finish:** HASL lead-free or ENIG
- **Soldermask:** Green (or black for stealth)
- **Silkscreen:** White
- **Min trace/space:** 0.15mm/0.15mm (JLCPCB standard)
- **Min drill:** 0.3mm

### Assembly
- **Assembly side:** Top + Bottom (specify in order)
- **Stencil:** Required for SMD assembly
- **Parts sourcing:** JLCPCB Basic + Extended parts library

---

## Test Points

Add the following test points for debugging:

- **TP1:** 3.3V
- **TP2:** GND
- **TP3:** GPIO17 (Serial1 TX mirror output)
- **TP4:** ESP32 EN
- **TP5:** GPIO0

Test points can be simple 1mm diameter pads with silkscreen labels.

---

## Bill of Materials

See `BOM_JLCPCB.csv` for complete JLCPCB-compatible BOM with part numbers.

---

## Design Notes

1. **Auto-programming:** The DTR/RTS circuit allows automatic bootloader entry via USB without manually pressing buttons.

2. **USB-C configuration:** 5.1kΩ resistors on CC pins signal to USB-C sources that this device sinks 5V @ up to 3A (though we only draw ~200mA typical).

3. **Power budget:**
   - ESP32: 80mA typical, 240mA peak (WiFi TX)
   - CH340C: 15mA
   - Piezo: 20mA peak
   - LED: 1mA
   - **Total: ~300mA peak** - well within AMS1117-3.3 capability (800mA)

4. **Thermal:** AMS1117 dissipates ~500mW at 300mA load. SOT-223 package with ground plane thermal relief is adequate.

5. **Cost optimization:** All components available in JLCPCB Basic parts library for lowest cost assembly.

---

## Revision History

- **v1.0** (2026-05-24): Initial design for JLCPCB assembly
