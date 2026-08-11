// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// led_gpio.h — plain digitalWrite() status LED (generic/default board)
//
// WHY THIS FILE EXISTS (bug-isolation refactor):
// See led_neopixel.h for the full rationale. This file is the plain-GPIO
// counterpart used only by the generic/default board branch in main.cpp's
// per-board CONFIG cascade (the final #else, which is the only branch that
// defines LED_ACTIVE_HIGH). It is mutually exclusive with led_neopixel.h —
// exactly one of the two is ever compiled in for a given board — so a bug
// in one can never affect the other.
//
// Public interface expected by main.cpp: ledSet(bool on)

#pragma once

#if defined(LED_ACTIVE_HIGH)

#ifndef LED_PIN
  #define LED_PIN 2
#endif

static inline void ledSet(bool on) {
#if LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW : HIGH);
#endif
}

#endif // LED_ACTIVE_HIGH
