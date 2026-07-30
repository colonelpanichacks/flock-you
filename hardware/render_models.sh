#!/bin/bash
# ============================================
# OpenSCAD Model Rendering Script
# ============================================
# Generates PNG renders of all case components
# Requires: OpenSCAD CLI installed

set -e  # Exit on error

# Configuration
SCAD_FILE="openscad/flock-you-case.scad"
OUTPUT_DIR="renders"
RESOLUTION="1920,1080"
DPI="300"

# Create output directory
mkdir -p "$OUTPUT_DIR"

echo "=== Flock-You Case Rendering Script ==="
echo "Checking for OpenSCAD..."

# Check if OpenSCAD is installed
if ! command -v openscad &> /dev/null; then
    echo "ERROR: OpenSCAD not found!"
    echo ""
    echo "Please install OpenSCAD:"
    echo "  macOS:   brew install openscad"
    echo "  Linux:   sudo apt-get install openscad"
    echo "  Windows: Download from https://openscad.org/downloads.html"
    exit 1
fi

echo "OpenSCAD found: $(openscad --version 2>&1 | head -n1)"
echo ""

# Function to render a part
render_part() {
    local part_name=$1
    local camera=$2
    local output_file="$OUTPUT_DIR/$part_name.png"
    
    echo "Rendering $part_name..."
    
    openscad \
        -o "$output_file" \
        --imgsize="$RESOLUTION" \
        --render \
        --colorscheme="Tomorrow Night" \
        --camera="$camera" \
        --projection=ortho \
        --autocenter \
        --viewall \
        "$SCAD_FILE"
    
    if [ -f "$output_file" ]; then
        echo "  ✓ Saved to $output_file"
    else
        echo "  ✗ Failed to render $part_name"
    fi
}

# Modify SCAD file temporarily to render each part
TEMP_SCAD=$(mktemp)

# === RENDER BASE ===
echo ""
echo "1/4: Rendering case base..."
cat > "$TEMP_SCAD" << 'EOF'
// Auto-generated for rendering
case_length = 70;
case_width = 50;
case_height = 25;
wall = 2;
corner_r = 3;
breadboard_l = 45;
breadboard_w = 35;

module case_base() {
    difference() {
        minkowski() {
            cube([case_length - 2*corner_r, case_width - 2*corner_r, case_height/2]);
            cylinder(r=corner_r, h=case_height/2, $fn=32);
        }
        translate([wall, wall, wall])
            cube([case_length - 2*wall, case_width - 2*wall, case_height]);
        translate([case_length - wall - 5, (case_width - 12)/2, wall + 2])
            cube([10, 12, 8]);
        translate([case_length - 15, (case_width - 10)/2, wall])
            cube([15, 10, 3]);
        for (i = [0:5]) {
            translate([10 + i * 4, 5, -1])
                cube([1.5, case_width - 10, wall + 2]);
        }
        for (x = [5, case_length - 5])
            for (y = [5, case_width - 5])
                translate([x, y, -1])
                    cylinder(d=3.2, h=wall + 2, $fn=16);
    }
    post_h = 3;
    post_d = 3;
    posts_offset_x = (case_length - breadboard_l) / 2;
    posts_offset_y = (case_width - breadboard_w) / 2;
    for (x = [posts_offset_x, posts_offset_x + breadboard_l - post_d])
        for (y = [posts_offset_y, posts_offset_y + breadboard_w - post_d])
            translate([x, y, wall])
                cylinder(d=post_d, h=post_h, $fn=16);
    tab_w = 4;
    tab_h = 2;
    tab_d = 1.5;
    for (x = [10, case_length - 10 - tab_w])
        for (y = [0, case_width - tab_d]) {
            translate([x, y, case_height - tab_h])
                cube([tab_w, tab_d, tab_h]);
        }
}

case_base();
EOF

openscad -o "$OUTPUT_DIR/case-base-iso.png" \
    --imgsize="$RESOLUTION" \
    --render \
    --colorscheme="Tomorrow Night" \
    --camera="0,0,0,55,0,25,200" \
    --projection=ortho \
    --autocenter \
    --viewall \
    "$TEMP_SCAD"

echo "  ✓ ISO view saved"

openscad -o "$OUTPUT_DIR/case-base-top.png" \
    --imgsize="$RESOLUTION" \
    --render \
    --colorscheme="Tomorrow Night" \
    --camera="0,0,0,0,0,0,180" \
    --projection=ortho \
    --autocenter \
    --viewall \
    "$TEMP_SCAD"

echo "  ✓ Top view saved"

# === RENDER LID ===
echo ""
echo "2/4: Rendering case lid..."
cat > "$TEMP_SCAD" << 'EOF'
// Auto-generated for rendering
case_length = 70;
case_width = 50;
wall = 2;
corner_r = 3;
tol = 0.3;

module case_lid() {
    lid_thick = 2;
    lip_h = 3;
    lip_inset = wall - tol;
    difference() {
        union() {
            minkowski() {
                cube([case_length - 2*corner_r, case_width - 2*corner_r, lid_thick/2]);
                cylinder(r=corner_r, h=lid_thick/2, $fn=32);
            }
            translate([lip_inset, lip_inset, -lip_h])
                cube([case_length - 2*lip_inset, case_width - 2*lip_inset, lip_h]);
        }
        translate([case_length/2, case_width/2, -lip_h - 1])
            cylinder(d=3 + tol, h=lid_thick + lip_h + 2, $fn=16);
        tab_w = 4 + 2*tol;
        tab_h = 2 + tol;
        tab_d = 1.5 + tol;
        for (x = [10 - tol, case_length - 10 - tab_w + tol])
            for (y = [-1, case_width - tab_d + 1]) {
                translate([x, y, -lip_h - 1])
                    cube([tab_w, tab_d, lip_h + tab_h + 1]);
            }
        translate([case_length/2 - 20, case_width/2 - 5, lid_thick - 0.5])
            linear_extrude(0.6)
                text("FLOCK-YOU", size=4, halign="center", font="Arial:style=Bold");
    }
}

case_lid();
EOF

openscad -o "$OUTPUT_DIR/case-lid-iso.png" \
    --imgsize="$RESOLUTION" \
    --render \
    --colorscheme="Tomorrow Night" \
    --camera="0,0,0,55,0,25,150" \
    --projection=ortho \
    --autocenter \
    --viewall \
    "$TEMP_SCAD"

echo "  ✓ ISO view saved"

# === RENDER LIGHT PIPE ===
echo ""
echo "3/4: Rendering LED light pipe..."
cat > "$TEMP_SCAD" << 'EOF'
// Auto-generated for rendering
module led_light_pipe() {
    pipe_l = 8;
    pipe_d = 2.8;
    cylinder(d1=pipe_d, d2=pipe_d - 0.5, h=pipe_l, $fn=32);
    translate([0, 0, pipe_l - 1])
        cylinder(d=pipe_d + 0.5, h=0.5, $fn=32);
}

led_light_pipe();
EOF

openscad -o "$OUTPUT_DIR/led-pipe.png" \
    --imgsize="800,600" \
    --render \
    --colorscheme="Tomorrow Night" \
    --camera="0,0,0,55,0,25,25" \
    --projection=ortho \
    --autocenter \
    --viewall \
    "$TEMP_SCAD"

echo "  ✓ Saved"

# === RENDER ASSEMBLY ===
echo ""
echo "4/4: Rendering complete assembly..."
cat > "$TEMP_SCAD" << 'EOF'
// Auto-generated for rendering - Complete assembly
case_length = 70;
case_width = 50;
case_height = 25;
wall = 2;
corner_r = 3;
breadboard_l = 45;
breadboard_w = 35;
tol = 0.3;

module case_base() {
    difference() {
        minkowski() {
            cube([case_length - 2*corner_r, case_width - 2*corner_r, case_height/2]);
            cylinder(r=corner_r, h=case_height/2, $fn=32);
        }
        translate([wall, wall, wall])
            cube([case_length - 2*wall, case_width - 2*wall, case_height]);
        translate([case_length - wall - 5, (case_width - 12)/2, wall + 2])
            cube([10, 12, 8]);
        for (i = [0:5]) {
            translate([10 + i * 4, 5, -1])
                cube([1.5, case_width - 10, wall + 2]);
        }
    }
    post_h = 3;
    post_d = 3;
    posts_offset_x = (case_length - breadboard_l) / 2;
    posts_offset_y = (case_width - breadboard_w) / 2;
    for (x = [posts_offset_x, posts_offset_x + breadboard_l - post_d])
        for (y = [posts_offset_y, posts_offset_y + breadboard_w - post_d])
            translate([x, y, wall])
                cylinder(d=post_d, h=post_h, $fn=16);
}

module case_lid() {
    lid_thick = 2;
    lip_h = 3;
    lip_inset = wall - tol;
    difference() {
        union() {
            minkowski() {
                cube([case_length - 2*corner_r, case_width - 2*corner_r, lid_thick/2]);
                cylinder(r=corner_r, h=lid_thick/2, $fn=32);
            }
            translate([lip_inset, lip_inset, -lip_h])
                cube([case_length - 2*lip_inset, case_width - 2*lip_inset, lip_h]);
        }
        translate([case_length/2, case_width/2, -lip_h - 1])
            cylinder(d=3 + tol, h=lid_thick + lip_h + 2, $fn=16);
    }
}

// Base
case_base();

// Lid (offset above)
translate([0, 0, case_height + 0.2])
    case_lid();
EOF

openscad -o "$OUTPUT_DIR/assembly-exploded.png" \
    --imgsize="$RESOLUTION" \
    --render \
    --colorscheme="Tomorrow Night" \
    --camera="0,0,0,55,0,25,250" \
    --projection=ortho \
    --autocenter \
    --viewall \
    "$TEMP_SCAD"

echo "  ✓ Exploded view saved"

# Cleanup
rm "$TEMP_SCAD"

echo ""
echo "=== Rendering Complete ==="
echo "Output directory: $OUTPUT_DIR/"
echo ""
echo "Generated files:"
ls -lh "$OUTPUT_DIR/"/*.png 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'

echo ""
echo "To use in documentation:"
echo "  ![Case Base](hardware/renders/case-base-iso.png)"
echo "  ![Case Lid](hardware/renders/case-lid-iso.png)"
echo "  ![Assembly](hardware/renders/assembly-exploded.png)"
