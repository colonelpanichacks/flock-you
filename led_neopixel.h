// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// led_neopixel.h — WS2812/SK6812 NeoPixel status LED for M5Atom Lite / Voice / Echo
//
// WHY THIS FILE EXISTS (bug-isolation refactor):
// Previously all board LED implementations (NeoPixel matrix AND plain GPIO)
// were interleaved directly inside main.cpp behind #ifdef branches in a
// single ledSet() function. That made it easy for a bug affecting one
// board's confidence/detection logic to *look* like an LED bug on a
// completely different board, and required reading all boards' LED code to
// understand any single board's behavior. Each board's LED handling now
// lives in its own dedicated file (this one, and led_gpio.h for the
// generic/default plain-GPIO board) so a change or bug here is physically
// incapable of affecting any other board's status LED.
//
// Public interface expected by main.cpp:
//   ledSet(bool on)             — set LED to alert-red or idle-dim-green
//   ledMatrixBootSequence()     — one-time rainbow sequence played in setup()
//
// ledFlash()/ledTick() — the generic timing state machine that decides
// *when* to call ledSet() — are identical across every LED-equipped board
// and remain in main.cpp.

#pragma once

#if defined(USE_M5ATOM_LITE) || defined(USE_M5ATOM_VOICE) || defined(USE_M5ATOM_ECHO)

#include <Adafruit_NeoPixel.h>

#ifndef LED_PIN
  #define LED_PIN 27   // M5Atom Lite/Voice/Echo onboard SK6812
#endif
#ifndef NUM_LEDS
  #define NUM_LEDS 1
#endif

static Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

static inline void ledSet(bool on) {
  if (on) { strip.setPixelColor(0, strip.Color(180, 0, 0)); }  // alert red
  else     { strip.setPixelColor(0, strip.Color(0, 60, 0)); }   // idle dim green
  strip.show();
}

// One-time colourful boot sequence, called once from setup(). Ends on the
// idle dim-green state so ledTick()/ledFlash() have a known starting point.
static inline void ledMatrixBootSequence() {
  strip.begin();
  strip.setBrightness(80);
  strip.show();
  for (int i = 0; i < 256; i++) {
    uint8_t r, g, b;
    if (i < 85)       { r = 255 - i*3; g = i*3;          b = 0; }
    else if (i < 170) { r = 0;          g = 255-(i-85)*3; b = (i-85)*3; }
    else              { r = (i-170)*3;  g = 0;            b = 255-(i-170)*3; }
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
    delay(4);
  }
  strip.setPixelColor(0, strip.Color(0, 60, 0));
  strip.show();
}

#endif // USE_M5ATOM_LITE || USE_M5ATOM_VOICE || USE_M5ATOM_ECHO
