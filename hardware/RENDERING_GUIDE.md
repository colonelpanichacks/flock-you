# OpenSCAD Model Rendering Guide

## 🎨 Generating Product Images

This guide shows you how to create professional renders of the 3D case models for documentation, marketing, and publishing.

---

## ⚡ Quick Start

### Option 1: Automated Script (Recommended)

```bash
cd hardware
./render_models.sh
```

This generates 5 high-quality renders:
- `case-base-iso.png` - Base isometric view
- `case-base-top.png` - Base top-down view
- `case-lid-iso.png` - Lid isometric view
- `led-pipe.png` - Light pipe detail
- `assembly-exploded.png` - Full assembly

**Output:** `hardware/renders/` folder

### Option 2: Manual OpenSCAD GUI

1. Open `openscad/flock-you-case.scad` in OpenSCAD
2. Uncomment the part you want (lines at bottom)
3. Press **F6** to render (may take 30-60 seconds)
4. **F12** or View → Top/Bottom/etc for different angles
5. File → Export → Export as Image

---

## 🖥️ Installing OpenSCAD

### macOS
```bash
brew install openscad
```

### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install openscad
```

### Windows
Download from: [openscad.org/downloads.html](https://openscad.org/downloads.html)

### Verify Installation
```bash
openscad --version
```

Should output: `OpenSCAD version 2021.01` (or newer)

---

## 📸 Render Settings

### For Documentation (High Quality)
```bash
openscad -o output.png \
    --imgsize=1920,1080 \
    --render \
    --colorscheme="Tomorrow Night" \
    --camera="100,80,60,55,0,25,300" \
    model.scad
```

### For Web (Optimized Size)
```bash
openscad -o output.png \
    --imgsize=1200,675 \
    --render \
    --colorscheme="Cornfield" \
    model.scad
```

### For Print (Ultra High-Res)
```bash
openscad -o output.png \
    --imgsize=3840,2160 \
    --render \
    --colorscheme="Tomorrow Night" \
    model.scad
```

---

## 🎨 Color Schemes

OpenSCAD includes several built-in themes:

| Scheme | Style | Best For |
|--------|-------|----------|
| **Tomorrow Night** | Dark, modern | Product shots, documentation |
| **Cornfield** | Light, clean | Web, light backgrounds |
| **Metallic** | Gray metallic | Professional renders |
| **Sunset** | Warm tones | Marketing materials |
| **Nature** | Green/brown | Eco-friendly branding |

Test them all:
```bash
for scheme in "Tomorrow Night" "Cornfield" "Metallic" "Sunset"; do
    openscad -o "test-$scheme.png" \
        --colorscheme="$scheme" \
        --render \
        openscad/flock-you-case.scad
done
```

---

## 📐 Camera Positioning

### Understanding Camera Parameters
```bash
--camera="X,Y,Z,RotX,RotY,RotZ,Distance"
```

**Examples:**

**Isometric View** (default):
```bash
--camera="100,80,60,55,0,25,300"
```

**Top-Down:**
```bash
--camera="35,25,100,0,0,0,200"
```

**Front View:**
```bash
--camera="0,-100,25,90,0,0,200"
```

**Side View:**
```bash
--camera="100,0,25,90,0,90,200"
```

**Close-Up Detail:**
```bash
--camera="50,40,30,55,0,25,150"
```

---

## 🎬 Creating Animations

### Rotating Turntable Animation

```bash
#!/bin/bash
# Generate 36 frames (10° rotation each)
for i in {0..35}; do
    angle=$((i * 10))
    openscad -o "frame_$(printf "%03d" $i).png" \
        --imgsize=1920,1080 \
        --render \
        --colorscheme="Tomorrow Night" \
        --camera="100,80,60,55,0,$angle,300" \
        openscad/flock-you-case.scad
done

# Combine into GIF (requires ImageMagick)
convert -delay 5 -loop 0 frame_*.png animation.gif

# Or video (requires ffmpeg)
ffmpeg -framerate 30 -i frame_%03d.png -c:v libx264 animation.mp4
```

### Exploded Assembly Animation

Modify the SCAD file to use a parameter:
```openscad
// Add to top of file
explode_distance = 0;  // Animate from 0 to 30

// In assembly:
translate([0, 0, case_height + explode_distance])
    case_lid();
```

Then render with increasing values.

---

## 🖼️ Post-Processing

### Optimizing File Size

```bash
# Install optimization tools
brew install pngquant optipng  # macOS
sudo apt-get install pngquant optipng  # Linux

# Optimize PNG (90% quality, huge size reduction)
pngquant --quality=85-95 input.png
optipng -o7 output.png

# Typical savings: 2MB → 300KB
```

### Adding Backgrounds

```bash
# Transparent background (for overlays)
convert input.png -fuzz 2% -transparent white output.png

# Add gradient background
convert input.png \
    \( +clone -fill "gradient:white-lightgray" \
       -draw "color 0,0 reset" \) \
    +swap -compose Over -composite \
    output.png

# Add shadow
convert input.png \
    \( +clone -background black -shadow 80x3+5+5 \) \
    +swap -background white -layers merge +repage \
    output.png
```

### Image Grid (Multiple Views)

```bash
# 2×2 grid of different angles
montage case-base-*.png case-lid-*.png \
    -tile 2x2 -geometry +10+10 \
    -background white \
    grid.png
```

---

## 📊 Batch Rendering All Parts

### Complete Render Script

Create `render_all_parts.sh`:

```bash
#!/bin/bash

PARTS=("case_base" "case_lid" "led_light_pipe" "mounting_bracket")
VIEWS=("iso" "top" "front" "side")

CAMERAS=(
    "100,80,60,55,0,25,300"      # Isometric
    "35,25,100,0,0,0,200"        # Top
    "0,-100,25,90,0,0,200"       # Front
    "100,0,25,90,0,90,200"       # Side
)

for part in "${PARTS[@]}"; do
    echo "Rendering $part..."
    
    # Edit SCAD to show only this part
    sed -i.bak "s|^//\($part\);|\1;|" openscad/flock-you-case.scad
    
    for i in "${!VIEWS[@]}"; do
        view="${VIEWS[$i]}"
        cam="${CAMERAS[$i]}"
        
        openscad -o "renders/${part}-${view}.png" \
            --imgsize=1920,1080 \
            --render \
            --colorscheme="Tomorrow Night" \
            --camera="$cam" \
            openscad/flock-you-case.scad
    done
    
    # Restore SCAD
    mv openscad/flock-you-case.scad.bak openscad/flock-you-case.scad
done

echo "All renders complete!"
```

---

## 🎯 Use Cases

### For GitHub README
```markdown
![Case Assembly](hardware/renders/assembly-exploded.png)

*Professional 3D printed enclosure with snap-fit lid*
```

### For Product Page
- Hero image: Isometric assembled view
- Features grid: Top, front, side views
- Detail shots: Light pipe, mounting holes
- Animation: 360° turntable GIF

### For Build Guide
- Step 1: Base with labels
- Step 2: Components placement
- Step 3: Lid installation
- Final: Complete assembly

### For Marketing
- High-res hero shot (4K)
- Lifestyle shots (in car, on desk)
- Size comparison (next to phone)
- Materials showcase (different colors)

---

## 💡 Pro Tips

### Faster Preview Rendering
```bash
# Use --preview instead of --render for quick tests
openscad -o preview.png \
    --imgsize=800,600 \
    --colorscheme="Tomorrow Night" \
    model.scad
# Renders in ~2 seconds vs 30+ seconds
```

### Consistent Lighting
Always use the same camera/lighting for product lineup:
```bash
CAMERA="100,80,60,55,0,25,300"
SCHEME="Tomorrow Night"
SIZE="1920,1080"

# Save as template and reuse
```

### Quality vs Speed Trade-off
- **Preview** (2s): Draft, testing angles
- **Normal render** (30s): Documentation, web
- **High-quality** (2min): Print, hero images
- **Ultra** (10min): Marketing, large prints

---

## 🐛 Troubleshooting

### "Command not found: openscad"
Install OpenSCAD (see installation section above)

### Render takes forever (>5 minutes)
- Reduce `$fn` values in SCAD (32 → 16)
- Use `--preview` for testing
- Check CPU usage (should be 100%)

### Black/blank output image
- Verify SCAD file has no errors (test in GUI first)
- Check camera position (too far/close?)
- Try different color scheme

### "Cannot create output file"
- Check write permissions on output directory
- Ensure path exists: `mkdir -p renders/`
- Check disk space

### Jagged edges / low quality
- Increase resolution: `--imgsize=3840,2160`
- Increase `$fn` in SCAD file
- Use `--render` not `--preview`

---

## 📚 Learning Resources

### Official Docs
- [OpenSCAD Manual](https://en.wikibooks.org/wiki/OpenSCAD_User_Manual)
- [Camera Parameters](https://en.wikibooks.org/wiki/OpenSCAD_User_Manual/The_OpenSCAD_Language#Camera)
- [Command Line](https://en.wikibooks.org/wiki/OpenSCAD_User_Manual/Using_OpenSCAD_in_a_command_line_environment)

### Video Tutorials
- "OpenSCAD Rendering Guide" - YouTube
- "Product Photography with OpenSCAD" - YouTube

### Example Galleries
- Thingiverse (search "openscad renders")
- Printables (filter by "openscad")

---

## ✅ Render Checklist

Before publishing:

- [ ] All parts rendered (base, lid, pipe, bracket)
- [ ] Multiple views (iso, top, side, front)
- [ ] Consistent lighting/camera across set
- [ ] High resolution (1920×1080 minimum)
- [ ] File size optimized (<500KB each)
- [ ] Assembly/exploded view included
- [ ] Optional: 360° animation
- [ ] Optional: Size reference added
- [ ] Images added to documentation
- [ ] Credited in README

---

## 🎉 You're Ready!

Run the rendering script or explore manual rendering.

**Quick command:**
```bash
cd hardware && ./render_models.sh
```

**Output:** Professional product images ready for documentation, marketing, and publication!

Happy rendering! 📸
