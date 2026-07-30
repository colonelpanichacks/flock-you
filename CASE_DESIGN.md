# 3D Printable Case Design - Flock-You ESP32

## 📐 Case Specifications

This case is designed to hold a complete Flock-You detector with breadboard assembly.

### Fits These Components
- ESP32 DevKit 30-pin (48mm × 28mm × 12mm)
- 170-point mini breadboard (45mm × 35mm × 9mm)
- KY-006 passive buzzer module (19mm × 15mm)
- USB Micro cable (with strain relief)
- Jumper wires

### Dimensions
- **External:** 70mm (L) × 50mm (W) × 25mm (H)
- **Wall thickness:** 2mm
- **Weight:** ~25g (PLA)
- **Print time:** 2-3 hours (0.2mm layer height)

### Features
✅ Snap-fit lid (no screws needed)  
✅ USB cable strain relief  
✅ LED light pipe for GPIO 2  
✅ Ventilation slots  
✅ Breadboard mounting posts  
✅ Car dashboard mounting holes  
✅ Optional velcro mounting surface  
✅ Compact,

 pocketable design  

---

## 🖨️ Printing Instructions

### Recommended Settings

| Setting | Value | Notes |
|---------|-------|-------|
| **Material** | PLA, PETG, or ABS | PLA easiest, PETG for heat resistance |
| **Layer Height** | 0.2mm | 0.15mm for smoother finish |
| **Infill** | 20% | Gyroid or Grid pattern |
| **Wall Line Count** | 3 | Strong walls |
| **Top/Bottom Layers** | 4 | Solid top/bottom |
| **Print Speed** | 50mm/s | Slower for better quality |
| **Support** | No | Designed to print without |
| **Bed Adhesion** | Brim | Helps with warping |
| **Rafts** | No | Not needed |

### Color Recommendations
- **Base:** Black or gray (looks professional)
- **Lid:** Clear/translucent (see LED through light pipe)
- **Accent parts:** Red or yellow (for button/indicator)

### Print Time & Cost
- **Base:** 1.5-2 hours, ~15g filament ($0.30-0.50)
- **Lid:** 30-45 minutes, ~8g filament ($0.15-0.25)
- **Total:** 2-3 hours, ~25g filament ($0.50-0.75)

---

## 📦 Files Included

All STL files are in `/firmware/hardware/stl/` folder:

1. **flock-you-case-base.stl** - Main enclosure bottom
2. **flock-you-case-lid.stl** - Snap-on top cover
3. **flock-you-led-pipe.stl** - Light guide for onboard LED
4. **flock-you-mount-bracket.stl** - Optional car mount (separate)

### Optional Parts
5. **spacer-3mm.stl** - Raises lid if components too tall
6. **usb-adapter-90deg.stl** - Right-angle USB adapter holder

---

## 🔧 Assembly Instructions

### What You Need
- Printed case base & lid
- LED light pipe (printed clear)  
- Assembled ESP32 + breadboard + buzzer
- (Optional) 2× M3×8mm screws for mounting bracket

### Step-by-Step

**Step 1: Install LED Light Pipe**
1. Locate LED light pipe hole in case lid (small circular hole)
2. Insert printed light pipe from inside
3. Push gently until it clicks flush
4. The pipe should protrude slightly inside to touch GPIO 2 LED

**Step 2: Mount Breadboard**
1. Identify 4 mounting posts inside case base
2. Peel adhesive backing off breadboard (if present)
3. Align breadboard over mounting posts
4. Press down firmly to seat breadboard
5. (Optional) Add small piece of double-sided tape for extra grip

**Step 3: Route USB Cable**
1. Thread USB cable through strain relief channel
2. Plug into ESP32
3. Cable should exit at back of case cleanly
4. Gentle pressure on strain relief locks cable in place


**Step 4: Close the Lid**
1. Align lid tabs with case base slots (4 corners)
2. Gently push down lid starting from one end
3. Work around perimeter, pressing each tab
4. Lid should "snap" into place securely
5. To open: Insert fingernail in slot at front, flex and lift

**Step 5: Test Fit**
1. Plug in USB to power on
2. LED should be visible through light pipe
3. Buzzer sound should be clear (not muffled)
4. All buttons accessible (if added)

### Common Issues & Fixes

**Lid won't snap:**
- Tabs too thick - sand them down slightly
- Posts interfering - trim with flush cutters
- Print elephants foot - scale lid X/Y by 101%

**Buzzer sounds muffled:**
- Add ventilation slots (see OpenSCAD mod below)
- Orient buzzer facing down toward slots
- Add acoustic port (drill 8mm hole)

**USB cable loose:**
- Add small zip tie through strain relief
- Print thicker strain relief variant
- Use hot glue (remove

able) as temporary fix

---

## 🖥️ OpenSCAD Source Code

The case is parametric - easily modify dimensions!

### Download & Edit

1. Install [OpenSCAD](https://openscad.org/) (free, open-source)
2. Open `/firmware/hardware/openscad/flock-you-case.scad`
3. Modify parameters at top of file
4. Press F5 to preview, F6 to render
5. Export STL: File → Export → Export as STL

### Quick Customization Parameters

```openscad
// === USER PARAMETERS ===

// Case dimensions (mm)
case_length = 70;
case_width = 50;
case_height = 25;
wall_thickness = 2;

// Component fitment
esp32_length = 48;
esp32_width = 28;
breadboard_length = 45;
breadboard_width = 35;

// Features
led_pipe_diameter = 3;  // Light pipe for GPIO 2 LED
usb_cutout_width = 12;
usb_strain_relief = true;
ventilation_slots = true;
mounting_holes = true;

// Tolerances (increase if too tight)
snap_fit_tolerance = 0.3;
component_clearance = 1.0;
```

### Full OpenSCAD Code

Save as `flock-you-case.scad`:

```openscad
// ============================================
// Flock-You ESP32 Case - Parametric Design
// ============================================
// Licensed under Creative Commons CC-BY-SA 4.0
// Designed for ESP32 DevKit + Breadboard

// === PARAMETERS ===

// Case dimensions
case_length = 70;
case_width = 50;
case_height = 25;
wall = 2;
corner_r = 3;

// Component dimensions
esp32_l = 48;
esp32_w = 28;
esp32_h = 12;
breadboard_l = 45;
breadboard_w = 35;
breadboard_h = 9;

// Features
led_pipe_d = 3;
led_pipe_h = 8;
usb_w = 12;
usb_h = 8;
vent_slot_w = 1.5;
vent_slot_spacing = 4;

// Tolerances
tol = 0.3;
clearance = 1.0;

// === MODULES ===

module case_base() {
    difference() {
        // Main body
        minkowski() {
            cube([case_length - 2*corner_r, 
                  case_width - 2*corner_r, 
                  case_height/2]);
            cylinder(r=corner_r, h=case_height/2, $fn=32);
        }
        
        // Hollow interior
        translate([wall, wall, wall])
            cube([case_length - 2*wall, 
                  case_width - 2*wall,
                  case_height]);
        
        // USB cutout (back)
        translate([case_length - wall - 5, 
                   (case_width - usb_w)/2, 
                   wall + 2])
            cube([10, usb_w, usb_h]);
        
        // USB strain relief channel
        translate([case_length - 15, 
                   (case_width - usb_w + 2)/2, 
                   wall])
            cube([15, usb_w - 2, 3]);
        
        // Ventilation slots (bottom)
        for (i = [0:5]) {
            translate([10 + i * vent_slot_spacing, 
                       5, 
                       -1])
                cube([vent_slot_w, case_width - 10, wall + 2]);
        }
        
        // Mounting holes (corners, optional)
        for (x = [5, case_length - 5])
            for (y = [5, case_width - 5])
                translate([x, y, -1])
                    cylinder(d=3.2, h=wall + 2, $fn=16);
    }
    
    // Breadboard mounting posts
    post_h = 3;
    post_d = 3;
    posts_offset_x = (case_length - breadboard_l) / 2;
    posts_offset_y = (case_width - breadboard_w) / 2;
    
    for (x = [posts_offset_x, posts_offset_x + breadboard_l - post_d])
        for (y = [posts_offset_y, posts_offset_y + breadboard_w - post_d])
            translate([x, y, wall])
                cylinder(d=post_d, h=post_h, $fn=16);
    
    // Snap-fit tabs (lid retention)
    tab_w = 4;
    tab_h = 2;
    tab_d = 1.5;
    
    for (x = [10, case_length - 10 - tab_w])
        for (y = [0, case_width - tab_d]) {
            translate([x, y, case_height - tab_h])
                cube([tab_w, tab_d, tab_h]);
        }
}

module case_lid() {
    lid_thick = 2;
    lip_h = 3;
    lip_inset = wall - tol;
    
    difference() {
        union() {
            // Top surface
            minkowski() {
                cube([case_length - 2*corner_r, 
                      case_width - 2*corner_r, 
                      lid_thick/2]);
                cylinder(r=corner_r, h=lid_thick/2, $fn=32);
            }
            
            // Retaining lip
            translate([lip_inset, lip_inset, -lip_h])
                cube([case_length - 2*lip_inset, 
                      case_width - 2*lip_inset,
                      lip_h]);
        }
        
        // LED light pipe hole
        translate([case_length/2, case_width/2, -lip_h - 1])
            cylinder(d=led_pipe_d + tol, h=lid_thick + lip_h + 2, $fn=16);
        
        // Snap-fit tab slots
        tab_w = 4 + 2*tol;
        tab_h = 2 + tol;
        tab_d = 1.5 + tol;
        
        for (x = [10 - tol, case_length - 10 - tab_w + tol])
            for (y = [-1, case_width - tab_d + 1]) {
                translate([x, y, -lip_h - 1])
                    cube([tab_w, tab_d, lip_h + tab_h + 1]);
            }
        
        // Label recess (optional)
        translate([case_length/2 - 20, case_width/2 - 5, lid_thick - 0.5])
            linear_extrude(0.6)
                text("FLOCK-YOU", size=4, halign="center", font="Liberation Sans:style=Bold");
    }
}

module led_light_pipe() {
    // Clear filament light guide
    pipe_l = led_pipe_h;
    pipe_d = led_pipe_d - 0.2;  // Snug fit
    
    cylinder(d1=pipe_d, d2=pipe_d - 0.5, h=pipe_l, $fn=32);
    
    // Retention ridge
    translate([0, 0, pipe_l - 1])
        cylinder(d=pipe_d + 0.5, h=0.5, $fn=32);
}

module mounting_bracket() {
    // Car dashboard / velcro mount
    bracket_l = case_length + 10;
    bracket_w = 30;
    bracket_thick = 3;
    
    difference() {
        // Base plate
        minkowski() {
            cube([bracket_l - 2*corner_r, 
                  bracket_w - 2*corner_r, 
                  bracket_thick/2]);
           cylinder(r=corner_r, h=bracket_thick/2, $fn=32);
        }
        
        // Screw holes for case attachment
        for (x = [5, case_length + 5])
            for (y = [bracket_w/2])
                translate([x, y, -1])
                    cylinder(d=3.2, h=bracket_thick + 2, $fn=16);
        
        // Velcro adhesive area (smooth)
        translate([bracket_l/2 - 25, 5, bracket_thick - 0.5])
            cube([50, bracket_w - 10, 1]);
    }
}

// === RENDER SELECTION ===

// Uncomment the part you want to generate:

case_base();

//translate([0, case_width + 10, 0])  // Offset for printing both
//    case_lid();

//translate([case_length/2, case_width/2, 0])
//    led_light_pipe();

//translate([0, case_width + 40, 0])
//    mounting_bracket();
```

---

## 🎨 Customization Ideas

### 1. Add Text Label
```openscad
linear_extrude(0.6)
    text("DEFLOCK", size=6, font="Arial:style=Bold");
```

### 2. Bigger Ventilation
```openscad
// Replace ventilation_slots section with:
for (i = [0:8]) {
    translate([8 + i * 3, 3, -1])
        cube([2, case_width - 6, wall + 2]);
}
```

### 3. Battery Compartment
```openscad
// Add to case_base():
translate([5, 5, wall])
    cube([20, case_width - 10, 15]);  // 18650 holder space
```

### 4. External Antenna Port
```openscad
// For ESP32-WROOM-32U with U.FL connector:
translate([10, case_width - wall - 1, case_height/2])
    rotate([90, 0, 0])
        cylinder(d=6.5, h=wall + 2, $fn=16);  // SMA bulkhead mount
```

### 5. Lanyard Hole
```openscad
translate([case_length - 5, case_width/2, case_height - 5])
    rotate([0, 90, 0])
        cylinder(d=4, h=wall + 2, $fn=16);
```

---

## 📏 Dimensions Reference

### ESP32 DevKit 30-pin Footprint
```
            48mm
    ┌──────────────────┐
    │    ESP32-WROOM   │
    │                  │ 28mm
    │  USB ████████    │
    └──────────────────┘
    
Pin spacing: 2.54mm (0.1")
Board thickness: 1.6mm
Height with components: ~12mm
```

### KY-006 Buzzer Module
```
       19mm
    ┌────────┐
    │  )))   │ 15mm
    │  )))   │
    └─●─●─●──┘
     S + GND
```

### 170-Point Breadboard
```
         45mm
    ┌──────────────┐
    │ ● ● ● ● ● ●  │
    │ ● ● ● ● ● ●  │ 35mm
    │ ● ● ● ● ● ●  │
    └──────────────┘
    
Mounting holes: 41mm × 31mm spacing
```

---

## 🛠️ Post-Processing

### Smoothing & Finishing

**Method 1: Sanding**
1. Wet-sand with 220 grit → 400 grit → 800 grit
2. Wipe clean with damp cloth
3. Optional: Clear coat spray (matte or gloss)

**Method 2: Vapor Smoothing** (ABS only)
1. Suspend print in sealed container
2. Add small amount of acetone to bottom
3. Leave 30 seconds - 2 minutes
4. Remove and let cure 24 hours
5. ⚠️ Ventilate well, acetone fumes hazardous

**Method 3: Filler + Paint**
1. Apply spray filler primer
2. Sand smooth with 400 grit
3. Spray paint desired color
4. Clear coat for protection

### Light Pipe Optimization

For best LED visibility:
1. Print light pipe in **clear or natural PLA**
2. Sand inside surface lightly (diffuses light)
3. Polish outside with progressively finer sandpaper (up to 3000 grit)
4. Optional: Apply clear epoxy resin for glass-like finish

---

## 📖 Design Philosophy

### Why This Design?

**✅ Printable Without Supports**
- All overhangs < 45°
- No bridging over large gaps
- Snap-fits eliminate screws

**✅ Accessible Components**
- Easy to open for firmware updates
- USB port accessible
- LED visible
- Buzzer unobstructed

**✅ Vehicular Use**
- Mounting options for dashboard
- Strain relief prevents cable damage
- Compact size fits cupholder
- Ventilated for heat dissipation

**✅ Beginner-Friendly**
- Simple two-part design
- No complex assembly
- Tolerances forgiving (±0.3mm works)
- Remixable OpenSCAD source

---

## 🔄 Remix & Share

### License: Creative Commons CC-BY-SA 4.0

**You are free to:**
- ✅ Share — copy and redistribute
- ✅ Adapt — remix, transform, build upon
- ✅ Commercial use - sell printed cases

**Under these terms:**
- 📝 Attribution — Credit original designer
- 🔄 ShareAlike — Same license on derivatives
- 🚫 No additional restrictions

### Share Your Remix!
- Upload to Thingiverse, Printables, or Thangs
- Tag with #flockyou #esp32 #surveillance
- Link back to original design
- Post photos of your build!

---

## 🎓 Learning Resources

### New to 3D Printing?
- **Basics:** [All3DP Beginner's Guide](https://all3dp.com/)
- **Software:** [PrusaSlicer Tutorial](https://help.prusa3d.com/)
- **Troubleshooting:** [Simplify3D Print Quality Guide](https://www.simplify3d.com/support/print-quality-troubleshooting/)

### OpenSCAD Resources
- **Official Docs:** [openscad.org/documentation](https://openscad.org/documentation.html)
- **Tutorial:** [OpenSCAD for Beginners](https://en.wikibooks.org/wiki/OpenSCAD_User_Manual)
- **Gallery:** Thousands of parametric designs to learn from

### Want to Modify?
1. Start with provided parameters (easy)
2. Add simple features (holes, text)
3. Learn modules (reusable components)
4. Advanced: Create fully parametric designs

---

## ✅ Print Checklist

- [ ] STL files downloaded
- [ ] Slicer configured (settings above)
- [ ] Printer bed leveled
- [ ] Filament loaded (20g+ needed)
- [ ] Print started (base first)
- [ ] Base printed successfully (~2 hours)
- [ ] Lid printed (~45 minutes)
- [ ] Light pipe printed (clear filament, ~5 min)
- [ ] Parts cleaned (remove brim/supports if any)
- [ ] Test fit (lid snaps onto base)
- [ ] Ready to assemble!

---

## 🎉 You're Done!

You now have a **professional-looking enclosure** for your Flock-You detector!

**Case cost:** $0.50-0.75 in filament  
**Print time:** 2-3 hours  
**Assembly time:** 5 minutes  
**Result:** Clean, portable, car-ready detector  

**Optional add-ons:**
- Vinyl sticker with warning/branding
- Velcro strips for quick mounting
- Carabiner clip for lanyard attachment
- External antenna mod for extended range

Happy printing! 🖨️
