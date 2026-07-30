// ============================================
// Flock-You ESP32 Case - Parametric Design
// ============================================
// Licensed under Creative Commons CC-BY-SA 4.0
// Designed for ESP32 DevKit + Breadboard
// https://github.com/yourusername/flock-you-esp32

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
        // Main body with rounded corners
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
            // Top surface with rounded corners
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
        // Base plate with rounded corners
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
        
        // Velcro adhesive area (smooth recessed surface)
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
