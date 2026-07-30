# Flock-You PCB Assembly Guide
## Ordering from JLCPCB or PCBWay

This guide explains how to order the Flock-You PCB with assembly service from JLCPCB or PCBWay.

---

## Prerequisites

You need these files:
1. **Gerber files** - PCB fabrication data (from KiCad export)
2. **BOM_JLCPCB.csv** - Bill of Materials
3. **CPL file** (Component Placement List) - Pick-and-place data
4. **SCHEMATIC.md** - Reference schematic

**Note:** If you don't have KiCad PCB files yet, you can use the schematic in `SCHEMATIC.md` to create them, or contact a PCB designer to convert the schematic to KiCad format (~$50-100 service).

---

## Option 1: JLCPCB Assembly

JLCPCB offers the lowest cost assembly for prototypes and small batches.

### Step 1: Create Gerber Files

If using KiCad:
1. Open the PCB file
2. File → Plot
3. Select all layers: F.Cu, B.Cu, F.Mask, B.Mask, F.Silk, B.Silk, Edge.Cuts
4. Generate Drill file
5. Zip all files into `gerber.zip`

### Step 2: Create CPL (Pick and Place) File

In KiCad:
1. File → Fabrication Outputs → Component Placement (.pos)
2. Format: CSV
3. Units: Millimeters
4. Include only SMD components
5. Save as `CPL.csv`

Format should be:
```
Designator,Mid X,Mid Y,Rotation,Layer
U1,25.4,17.5,0,Top
U2,12.7,8.3,90,Bottom
...
```

### Step 3: Order on JLCPCB Website

1. **Go to:** https://cart.jlcpcb.com/quote

2. **Upload Gerber:**
   - Click "Add gerber file"
   - Upload `gerber.zip`
   - Wait for preview

3. **PCB Specifications:**
   - **Base Material:** FR-4
   - **Layers:** 2
   - **Dimensions:** 50mm × 35mm (auto-detected)
   - **PCB Qty:** 5 (minimum)
   - **PCB Thickness:** 1.6mm
   - **PCB Color:** Green (cheapest) or Black
   - **Surface Finish:** HASL (cheapest) or ENIG (better)
   - **Copper Weight:** 1 oz
   - **Gold Fingers:** No
   - **Castellated Holes:** No
   - **Remove Order Number:** Yes (optional, +$1.50)

4. **Enable SMT Assembly:**
   - Toggle "SMT Assembly" to ON
   - **Assembly Side:** Top Side (or both if components on bottom)
   - **SMT QTY:** 5 (matches PCB qty)
   - **Tooling holes:** Added by Customer
   - **Confirm Parts Placement:** Yes

5. **Upload BOM and CPL:**
   - Click "Add BOM File" → Upload `BOM_JLCPCB.csv`
   - Click "Add CPL File" → Upload `CPL.csv`
   - Click "Next"

6. **Component Matching:**
   - JLCPCB will auto-match LCSC part numbers
   - Verify all parts are in stock (green checkmark)
   - If any parts are out of stock, find alternatives in LCSC catalog
   - Total component cost will be shown

7. **Review and Checkout:**
   - Review component placement preview (rotate if needed)
   - Check total cost (PCB + Assembly + Components + Shipping)
   - Typical cost for 5 boards: **$50-80 USD**

8. **Production Time:**
   - PCB fabrication: 24 hours
   - Component procurement: 1-2 days
   - Assembly: 1-2 days
   - Shipping (DHL): 3-5 days
   - **Total: ~7-10 days**

---

## Option 2: PCBWay Assembly

PCBWay has slightly higher prices but excellent quality and easier quoting process.

### Step 1: Get Instant Quote

1. **Go to:** https://www.pcbway.com/orderonline.aspx

2. **Upload Gerber:**
   - Click "Add Gerber File"
   - Upload `gerber.zip`

3. **PCB Specifications:**
   - Same as JLCPCB above
   - PCBWay auto-detects most settings

4. **Assembly Service:**
   - Check "Assembly" box
   - Upload BOM and CPL files
   - PCBWay engineer will review and quote

5. **Quote Review:**
   - PCBWay sends manual quote within 24 hours
   - They verify all parts and suggest alternatives if needed
   - Typical cost for 5 boards: **$80-120 USD**

6. **Production Time:**
   - Similar to JLCPCB: ~7-14 days total

---

## Cost Breakdown (Estimated)

### JLCPCB (5 boards)
| Item | Cost |
|------|------|
| PCB Fabrication (5 pcs) | $2 |
| Assembly Service | $8 |
| Components (per board × 5) | $30-40 |
| Shipping (DHL Express) | $15-20 |
| **Total** | **$55-70** |

### PCBWay (5 boards)
| Item | Cost |
|------|------|
| PCB Fabrication (5 pcs) | $5 |
| Assembly Service | $25 |
| Components (per board × 5) | $35-45 |
| Shipping (DHL Express) | $20-25 |
| **Total** | **$85-100** |

### Per-Board Cost (After Initial Order)
- **5 boards:** $11-14 per board
- **10 boards:** $8-10 per board
- **50 boards:** $6-7 per board (significant volume discount)
- **100 boards:** $5-6 per board

---

## Alternative: Hand Assembly (If Needed)

If some components are out of stock or you want to reduce cost, you can order partially assembled boards and hand-solder the missing parts.

**Easy to hand-solder:**
- Through-hole test points
- USB-C connector (with hot air)
- Tactile switches (large pads)

**Difficult to hand-solder:**
- CH340C (SOIC-16, 0.65mm pitch - doable with flux and patience)
- ESP32 module (41 pins, requires hot air station)

**Not recommended for hand-soldering:**
- 0603 ceramic capacitors (very small, but technically possible)

---

## Component Sourcing Alternatives

If JLCPCB/PCBWay is too expensive, you can:

1. **Order bare PCBs only** ($2-5 for 5 boards)
2. **Buy components from:**
   - AliExpress (cheapest, slow shipping)
   - LCSC.com directly
   - Mouser/Digikey (fastest, more expensive)
3. **Assemble yourself** with a hot air station and soldering iron

**DIY Assembly Cost:** ~$10-15 per board in components

---

## Testing After Assembly

Once you receive assembled boards:

1. **Visual Inspection:**
   - Check all components are placed correctly
   - No solder bridges
   - No missing parts

2. **Power Test:**
   - Plug in USB-C cable
   - Measure 3.3V at TP1 test point
   - LED should NOT light (GPIO2 default low)

3. **Connectivity Test:**
   - Should appear as USB serial device
   - Check device manager (Windows) or `ls /dev/tty*` (Mac/Linux)

4. **Flash Firmware:**
   ```bash
   cd flock-you-esp32
   source venv/bin/activate
   pio run -t upload
   ```

5. **Functional Test:**
   - LED should flash on detection
   - Buzzer should chirp on detection
   - Serial output should show "flockyou detector started"

---

## Troubleshooting

### Board doesn't power on
- Check USB cable is good
- Measure 5V at AMS1117 input
- Check for solder bridges

### Computer doesn't detect USB device
- CH340C driver issue (install driver)
- Bad USB-C cable (try another)
- D+/D- traces swapped (rare with JLCPCB)

### Can't flash firmware
- Press and hold Boot button, then press Reset
- Check TX/RX aren't swapped
- Auto-program circuit issue (use manual boot mode)

### Piezo doesn't work
- Check GPIO25 with multimeter during beep
- Piezo might be installed backwards (test both orientations)
- Cold solder joint

### LED doesn't work
- Check polarity (cathode to ground)
- Check R3 resistor is present
- GPIO2 may be in wrong state (modify code for testing)

---

## Support

For PCB design files or assistance:
- KiCad files can be created from the schematic for $50-100
- Many PCB designers on Fiverr/Upwork can help
- JLCPCB has engineering support for assembly issues

For firmware issues:
- See main README.md for flashing instructions
- Check serial monitor output for debugging

---

## Next Steps

1. **Convert schematic to KiCad** (if not done)
2. **Order 5 boards** from JLCPCB (~$60)
3. **Wait 7-10 days** for delivery
4. **Flash firmware** and test
5. **If successful, order larger batch** (10-50 units for volume pricing)

---

## Reselling Considerations

If planning to sell assembled devices:

1. **Minimum viable batch:** 10 boards (~$80 total)
2. **Suggested retail price:** $49-69 per unit
3. **Profit margin:** $30-50 per unit (60-80%)
4. **Break-even:** 2-3 units sold covers batch cost

Compare to:
- **Flock OUI-SPY:** $149 (commercial product)
- **DIY breadboard:** $10 (technical skills required)
- **Your assembled device:** $49-69 (plug-and-play, custom hardware)

This fills a market gap between DIY and expensive commercial options.

---

## License

PCB design is open-source hardware. You may manufacture and sell assembled boards.
Consider contributing improvements back to the project.
