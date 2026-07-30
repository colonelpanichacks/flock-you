# Hardware Directory

This directory contains the 3D printable case design and rendering tools.

## 📁 Contents

```
hardware/
├── openscad/
│   └── flock-you-case.scad       ← Parametric CAD source
├── renders/                       ← Generated product images (create with script)
├── render_models.sh               ← Automated rendering script
├── RENDERING_GUIDE.md             ← Complete rendering documentation
└── README.md                      ← This file
```

---

## 🚀 Quick Start

### To Generate Product Images:

**1. Install OpenSCAD:**
```bash
# macOS
brew install openscad

# Linux
sudo apt-get install openscad
```

**2. Run Rendering Script:**
```bash
cd hardware
./render_models.sh
```

This creates professional renders in `renders/` folder:
- `case-base-iso.png` - Base isometric view
- `case-base-top.png` - Base top view  
- `case-lid-iso.png` - Lid isometric view
- `led-pipe.png` - Light pipe detail
- `assembly-exploded.png` - Full assembly exploded view

**3. Use in Documentation:**
Add the generated images to your README, product pages, and build guides.

---

## 📐 Case Specifications

### Dimensions
- **External:** 70mm × 50mm × 25mm
- **Weight:** ~25g (PLA)
- **Print time:** 2-3 hours

### Features
- ✅ Snap-fit lid (no screws)
- ✅ LED light pipe for GPIO 2
- ✅ USB strain relief
- ✅ Ventilation slots
- ✅ Breadboard mounting posts
- ✅ Car dashboard mounting holes

### Fits
- ESP32 DevKit 30-pin
- 170-point mini breadboard
- KY-006 passive buzzer module
- USB cable with strain relief

---

## 🔧 Customizing the Design

### Edit in OpenSCAD GUI

1. Open `openscad/flock-you-case.scad` in OpenSCAD
2. Modify parameters at top of file:
   ```openscad
   case_length = 70;  // Change dimensions
   case_width = 50;
   case_height = 25;
   ```
3. Press **F5** to preview changes
4. Press **F6** to render final model
5. Export STL: File → Export → Export as STL

### Common Customizations

**Make it bigger/smaller:**
```openscad
case_length = 80;  // 10mm longer
case_width = 55;   // 5mm wider
```

**Add ventilation:**
```openscad
vent_slot_w = 2.0;        // Wider slots
vent_slot_spacing = 3;     // More slots
```

**Different LED position:**
```openscad
// In case_lid() module, change light pipe position
translate([30, 25, -lip_h - 1])  // Move to corner
    cylinder(d=led_pipe_d + tol, h=lid_thick + lip_h + 2);
```

---

## 📦 Printing Instructions

### Recommended Settings
- **Material:** PLA, PETG, or ABS
- **Layer Height:** 0.2mm
- **Infill:** 20%
- **Supports:** None needed
- **Print Time:** 2-3 hours total
- **Cost:** $0.50-0.75 in filament

### Print Order
1. Print **base first** (~2 hours)
2. Test fit with components
3. Print **lid** (~45 min)
4. Print **light pipe** in clear filament (~5 min)

---

## 🎨 Render Examples

Once you run `./render_models.sh`, you'll get:

### Case Base (Isometric View)
Shows internal posts, ventilation slots, USB cutout, and snap-fit tabs.

### Case Lid (Isometric View)  
Shows retaining lip, LED light pipe hole, and snap-fit slots.

### LED Light Pipe
Detail view of the custom light guide for GPIO 2 LED.

### Exploded Assembly
Full assembly view showing how parts fit together.

---

## 📸 For Publishers

If you're publishing this project, generate renders by:

1. Installing OpenSCAD (free, open-source)
2. Running `./render_models.sh`
3. Adding images to:
   - Main README (hero shot)
   - Build guides (step-by-step)
   - Product pages (multiple angles)
   - Social media (eye-catching renders)

**Tip:** Use "Tomorrow Night" color scheme for professional dark backgrounds.

---

## 🎓 Learning Resources

- **Full guide:** See `RENDERING_GUIDE.md` for complete documentation
- **OpenSCAD manual:** [openscad.org/documentation](https://openscad.org/documentation.html)
- **Video tutorials:** Search "OpenSCAD rendering" on YouTube

---

## ✅ Checklist for Publication

- [ ] OpenSCAD installed
- [ ] Renders generated (`./render_models.sh`)
- [ ] STL files exported (if distributing printable files)
- [ ] Images added to main README
- [ ] Case design credited in documentation
- [ ] Print settings verified with test print

---

## 🤝 Contributing

Designed a better case? Created a variant?

1. Fork the repository
2. Modify `flock-you-case.scad`
3. Generate renders of your design
4. Submit pull request with photos

**License:** Creative Commons CC-BY-SA 4.0  
Share and remix freely, credit original designers.

---

## 📝 Notes

**Render script not working?**
- Check that OpenSCAD is installed: `openscad --version`
- Verify script is executable: `chmod +x render_models.sh`
- See `RENDERING_GUIDE.md` for troubleshooting

**Need different angles?**
- Edit camera parameters in `render_models.sh`
- Or use OpenSCAD GUI: View → Cameras → Choose angle
- See rendering guide for camera parameter details

**Want to sell printed cases?**
That's explicitly allowed under CC-BY-SA 4.0!
- Print and sell commercially
- Must credit original design
- Share derivative designs under same license

---

## 🎉 Ready to Build!

You now have:
- ✅ Parametric CAD source (customizable)
- ✅ Rendering tools (professional images)
- ✅ Complete documentation
- ✅ Print-ready design

**Next step:** Install OpenSCAD, run `./render_models.sh`, and generate your product images!
