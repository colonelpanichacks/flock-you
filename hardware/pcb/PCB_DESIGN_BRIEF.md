# PCB Design Brief - Flock-You ESP32 Device

## Project Overview

**Design a 2-layer PCB** for a WiFi surveillance detection device based on ESP32. This brief contains everything needed to create the PCB layout and export production-ready Gerber files for JLCPCB assembly.

---

## Required Deliverables

Please provide the following files:

1. **KiCad Project Files** (.kicad_pro, .kicad_pcb, .kicad_sch)
2. **Gerber Files** (.gbr) - All layers zipped together
3. **Drill Files** (.drl) 
4. **Pick-and-Place File** (CPL format, CSV with coordinates)
5. **Bill of Materials** (verify against provided BOM_JLCPCB.csv)
6. **Assembly Drawings** (PDF showing component locations, top and bottom)

---

## PCB Specifications

### Board Parameters
- **Size:** 50mm × 35mm (rectangular)
- **Layers:** 2 (top + bottom)
- **Thickness:** 1.6mm
- **Material:** FR-4
- **Copper Weight:** 1 oz (35µm)
- **Surface Finish:** HASL lead-free (or ENIG if preferred)
- **Soldermask Color:** Green (cheapest) or Black
- **Silkscreen Color:** White
- **Min Trace Width:** 0.15mm (JLCPCB standard capability)
- **Min Trace Spacing:** 0.15mm
- **Min Drill Size:** 0.3mm
- **Castellated Holes:** No
- **Gold Fingers:** No

### Manufacturing Target
- **Manufacturer:** JLCPCB or PCBWay
- **Assembly:** SMT assembly service (top and/or bottom)
- **Parts Library:** All components must be from JLCPCB Basic or Extended parts library
- **Design Rules:** Use JLCPCB design rules (https://jlcpcb.com/capabilities/pcb-capabilities)

---

## Circuit Design

### Complete Schematic

See **SCHEMATIC.md** in this folder for the full circuit design with pin connections, component values, and design notes.

### Key Circuit Blocks

1. **Power Supply**
   - USB-C input (J1) → SS34 diode (D2) → AMS1117-3.3 (U3) → 3.3V rail
   - Input filtering: 10µF tantalum (C1)
   - Output filtering: 10µF tantalum (C2)

2. **USB-to-UART Bridge**
   - CH340C (U2) with 12MHz crystal (Y1) and 22pF load caps (C8, C9)
   - Auto-programming circuit using DTR/RTS with 100nF coupling caps

3. **Microcontroller**
   - ESP32-WROOM-32E module (U1)
   - Decoupling: 4× 100nF ceramics (C3-C6) + 1× 10µF (C7)
   - Pull-ups on EN (R1=10kΩ) and GPIO0 (R2=10kΩ)

4. **User Interface**
   - Piezo buzzer (BZ1) on GPIO25
   - Blue LED (D1) on GPIO2 via 330Ω resistor (R3)
   - Boot button (SW1) on GPIO0
   - Reset button (SW2) on EN

5. **USB-C Configuration**
   - CC1 and CC2 pins via 5.1kΩ to GND (R4, R5) for USB-C power sink

---

## Component List

See **BOM_JLCPCB.csv** for complete list with LCSC part numbers.

### Major Components (with footprints)

| Ref | Value | Package | LCSC # | Notes |
|-----|-------|---------|--------|-------|
| U1 | ESP32-WROOM-32E | SMD Module | C701341 | 18mm × 25.5mm, 38-pin |
| U2 | CH340C | SOIC-16 | C84681 | 0.65mm pitch |
| U3 | AMS1117-3.3 | SOT-223 | C6186 | Tab is output |
| J1 | USB-C-16P | Mid-Mount | C165948 | 16-pin receptacle |
| BZ1 | Piezo Buzzer 12mm | SMD-12mm | C94599 | Passive type |
| D1 | Blue LED | 0805 | C2296 | Forward voltage ~3V |
| D2 | SS34 Schottky | SMA | C8678 | 40V, 3A |
| Y1 | 12MHz Crystal | HC-49S SMD | C9002 | For CH340C |
| SW1, SW2 | Tactile Switch | 6×6mm SMD | C318884 | 4-pin |
| C1, C2 | 10µF Tantalum | 0805 | C7171 | 16V rated |
| C3-C6 | 100nF Ceramic | 0805 | C49678 | 50V rated |
| C7 | 10µF Ceramic | 0805 | C15850 | 25V rated |
| C8, C9 | 22pF Ceramic | 0603 | C1653 | Crystal load caps |
| R1, R2 | 10kΩ | 0805 | C17414 | Pull-ups |
| R3 | 330Ω | 0805 | C17630 | LED current limit |
| R4, R5 | 5.1kΩ | 0805 | C27834 | USB-C CC config |

---

## Layout Requirements

### Component Placement

**Top Side (Primary):**
```
┌─────────────────────────────────────┐
│  [USB-C J1]                         │
│                                     │
│  [SW1] [SW2]      ┌──────────────┐ │
│  Boot  Reset      │              │ │
│                   │ ESP32-WROOM  │ │
│  [D1 LED]         │    (U1)      │ │
│                   │              │ │
│                   └──────────────┘ │
│                                     │
│                        [BZ1 Piezo] │
│                          (12mm)    │
│                                     │
└─────────────────────────────────────┘
```

**Component Placement Guidelines:**
1. **USB-C connector (J1):** Centered on top edge
2. **ESP32 module (U1):** Center of board, antenna end towards right edge
3. **Boot/Reset buttons (SW1, SW2):** Near top-left, easy thumb access
4. **LED (D1):** Top-right corner, visible when looking at board
5. **Piezo buzzer (BZ1):** Bottom-right, away from USB connector
6. **CH340C (U2):** Bottom side, under ESP32 area (fits in shadow)
7. **AMS1117 (U3):** Bottom side, near USB connector (short power traces)
8. **Crystal (Y1):** Bottom side, close to CH340C XI/XO pins

**Keep-Out Zones:**
- **ESP32 antenna area:** 15mm clearance from right edge (no copper, no components)
- **Piezo buzzer:** No ground plane directly under buzzer (affects acoustics)
- **Crystal:** Guard with ground traces if needed for EMI

### Layer Stackup

```
Layer 1 (Top):    Signal + Components + Ground Pour
Layer 2 (Bottom): Ground Plane + Signal + Components
```

### Copper Pours
- **Top Layer:** Ground pour (solid fill, connect to Layer 2 via vias)
- **Bottom Layer:** Ground plane (solid fill, primary ground reference)
- **Via Stitching:** Every 5mm around board perimeter
- **Thermal Relief:** Use for through-holes (4 spokes, 0.3mm)

### Trace Widths
- **5V USB Power:** 0.5mm minimum
- **3.3V Rail:** 0.4mm minimum  
- **Signal Traces:** 0.2mm typical
- **Ground:** Plane + pour (maximize copper)

### Critical Routing

1. **USB Differential Pair (D+/D-):**
   - **Impedance:** 90Ω differential
   - **Length matching:** ±0.1mm
   - **Trace width:** 0.35mm with 0.2mm gap (assuming 1.6mm FR-4, adjust for stackup)
   - **Keep away from:** Crystal, switching regulators, high-speed signals
   - **Route:** J1 → U2 (pins 5, 6), direct path, no vias if possible

2. **Crystal Traces (CH340C XI/XO):**
   - **Keep short:** <10mm total length
   - **Symmetric:** Equal length to both crystal pads
   - **Guard:** Ground plane underneath, optional guard traces
   - **No vias:** Route on single layer

3. **UART Signals (CH340C ↔ ESP32):**
   - TXD, RXD between U2 and U1
   - Can be longer, but avoid running parallel to noisy signals
   - 0.2mm traces OK

4. **Auto-Programming Caps:**
   - DTR → 100nF → GPIO0
   - RTS → 100nF → EN
   - Keep caps close to ESP32 pins

5. **Power Decoupling:**
   - Place 100nF caps very close to IC power pins (within 2-3mm)
   - Multiple small vias (0.3mm) from caps to ground plane
   - Bulk caps (10µF) can be slightly farther

### Ground Plane Strategy
- Solid ground plane on Layer 2 (bottom)
- Ground pour on Layer 1 (top) with vias every 5mm
- Connect all ground pins to plane with short traces or direct vias
- Thermal relief on component pads to ease soldering

### Silkscreen
- **Designators:** U1, U2, U3, BZ1, D1, D2, J1, SW1, SW2, Y1, R1-R5, C1-C9
- **Button Labels:** "BOOT" near SW1, "RESET" near SW2
- **LED Label:** "STATUS" or "PWR" near D1
- **Polarity Marks:** Mark LED cathode, diode cathode, capacitor polarity
- **Version:** Add "Flock-You v1.0" silkscreen on bottom
- **Pin 1 Marks:** Dot or triangle for IC orientation
- **USB-C Orientation:** Optional icon showing USB-C is reversible

### Test Points (optional)
Add 1mm diameter test pads (no soldermask) for:
- TP1: 3.3V
- TP2: GND  
- TP3: GPIO17 (Serial TX mirror)
- TP4: EN
- TP5: GPIO0

---

## Electrical Constraints

### Design Rules (JLCPCB Standard)
- **Min trace width:** 0.15mm (but use 0.2mm for manufacturability)
- **Min trace spacing:** 0.15mm (but use 0.2mm for safety)
- **Min drill size:** 0.3mm
- **Min annular ring:** 0.15mm
- **Min soldermask bridge:** 0.1mm
- **Min silkscreen width:** 0.15mm (but use 0.2mm for readability)

### Power Budget
- **Input:** 5V USB @ 500mA max (USB 2.0 spec)
- **ESP32:** 240mA peak (WiFi TX), 80mA typical
- **CH340C:** 15mA
- **Piezo:** 20mA peak
- **LED:** 1mA
- **Total:** ~300mA peak, well within USB and AMS1117 limits

### Voltage Drops
- **USB 5V → SS34 diode:** ~0.3V drop = 4.7V
- **AMS1117 input range:** 4.75V-15V (4.7V is OK)
- **AMS1117 output:** 3.3V ±3%
- **AMS1117 dropout:** 1.0V typ, so 4.7V - 1.0V = 3.7V available (safe)

---

## Special Considerations

### ESP32 RF Design
- **Antenna:** ESP32-WROOM-32E has integrated PCB antenna
- **Keep-out zone:** 15mm × 6mm at antenna end (see ESP32 datasheet)
- **No copper:** under antenna area or within keep-out zone
- **Board edge:** Antenna side should hang over board edge slightly OR
- **Alternative:** Ensure 15mm clear space to any metal/components

### USB-C Design
- **CC1, CC2 resistors (R4, R5):** Both 5.1kΩ to GND signals USB-C power sink
- **VBUS pins:** Connect all 4 (A4, A9, B4, B9) together
- **GND pins:** Connect all 4 (A1, A12, B1, B12) together
- **Unused pins:** D+/D- for USB 2.0 only, leave SS pins unconnected

### EMI/EMC Considerations
- Route switching traces (3.3V reg output) away from RF antenna
- Decoupling caps as close as possible to ICs
- Ground plane reduces noise
- Optional: Add ferrite bead on 3.3V line between regulator and ESP32

### Thermal Management
- **AMS1117 heatsinking:** SOT-223 tab is OUTPUT (not ground)
  - Connect tab to 3.3V net
  - Pour 3.3V copper under/around U3 for heat spreading
  - Vias from 3.3V pour to bottom plane helps dissipate heat
  - At 300mA load: (4.7V - 3.3V) × 0.3A = 0.42W dissipation (acceptable)

---

## Design Validation Checklist

Before finalizing, verify:

- [ ] All components from JLCPCB Basic/Extended parts library
- [ ] USB differential pair impedance controlled at 90Ω
- [ ] Crystal traces symmetric and <10mm
- [ ] All power pins have nearby decoupling caps
- [ ] Ground plane solid on Layer 2, poured on Layer 1
- [ ] ESP32 antenna keep-out zone clear of copper/components
- [ ] USB-C CC resistors (5.1kΩ) both present
- [ ] Auto-program caps (100nF) on DTR and RTS lines
- [ ] All polarized components marked on silkscreen
- [ ] No acute angles in traces (use 45° or curved)
- [ ] DRC (Design Rule Check) passes with no errors
- [ ] ERC (Electrical Rule Check) passes with no errors
- [ ] Component footprints verified against datasheets
- [ ] Pin 1 orientation marks on all ICs
- [ ] Test points accessible (if included)
- [ ] Board outline exactly 50mm × 35mm
- [ ] Mount holes (if any) at least 2.5mm from board edge

---

## Gerber Export Settings (KiCad)

When exporting Gerbers for JLCPCB:

### Layers to Include:
- F.Cu (top copper)
- B.Cu (bottom copper)
- F.Mask (top soldermask)
- B.Mask (bottom soldermask)
- F.Silk (top silkscreen)
- B.Silk (bottom silkscreen)
- Edge.Cuts (board outline)
- F.Paste (top solder paste, for assembly)
- B.Paste (bottom solder paste, if needed)

### Drill Files:
- PTH (Plated Through Holes)
- NPTH (Non-Plated Through Holes) if any
- Format: Excellon

### Pick-and-Place (CPL):
- Format: CSV
- Columns: Designator, Mid X, Mid Y, Rotation, Layer
- Units: Millimeters
- Origin: Bottom-left corner of board
- Rotation: 0° = as shown in schematic

### Zip Files:
- Gerbers + Drill in one ZIP: `FY-ESP32-Gerber-v1.0.zip`
- Assembly files separate: BOM + CPL

---

## Reference Documents

In this folder you'll find:
1. **SCHEMATIC.md** - Complete circuit design with all connections
2. **BOM_JLCPCB.csv** - Bill of materials with LCSC part numbers
3. **ASSEMBLY_GUIDE.md** - How to upload files to JLCPCB

External references:
- ESP32-WROOM-32E Datasheet: https://www.espressif.com/sites/default/files/documentation/esp32-wroom-32e_datasheet_en.pdf
- CH340C Datasheet: https://cdn.sparkfun.com/datasheets/Dev/Arduino/Other/CH340DS1.PDF
- JLCPCB Capabilities: https://jlcpcb.com/capabilities/pcb-capabilities
- KiCad Footprint Libraries: https://kicad.github.io/footprints/

---

## Timeline & Budget

**Estimated Work:**
- Schematic entry: 2 hours
- PCB layout: 4-6 hours
- Review & DRC: 1 hour
- Gerber export & documentation: 1 hour
- **Total: 8-10 hours**

**Fair Rate:** $50-100 total (or $10-15/hour)

---

## Questions?

If anything is unclear, please ask! Key things to confirm:
- Are all components available in JLCPCB library?
- Is the 50mm × 35mm size acceptable?
- Should we add mounting holes? (Not required but possible)
- Any preference on USB-C connector style? (Mid-mount specified)
- Do you need a 3D render/preview before finalizing?

---

## Acceptance Criteria

I will approve the design when:
1. ✅ KiCad project files provided
2. ✅ Gerber + Drill files export correctly
3. ✅ CPL file matches component positions
4. ✅ BOM verified against JLCPCB stock
5. ✅ Assembly drawings show clear component placement
6. ✅ DRC passes with zero errors
7. ✅ Test import on JLCPCB website (I can check)

Thank you for taking on this project! 🚀
