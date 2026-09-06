#pragma once

#include <stdint.h>

#ifdef BOARD_M5STACK_CARDPUTER
#define USE_CARDPUTER_DISPLAY
#endif

#ifdef USE_CARDPUTER_DISPLAY

void cardputerDisplayInit();
void cardputerDisplayShowIdle(uint8_t ch, int detCount);
void cardputerDisplayShowAlert(const char* method, const char* mac, int8_t rssi,
                               uint8_t ch, unsigned long alertMs);
void cardputerDisplayTick(unsigned long now, uint8_t ch, int detCount);
bool cardputerDisplayInAlert(unsigned long now);

#else

static inline void cardputerDisplayInit() {}
static inline void cardputerDisplayShowIdle(uint8_t, int) {}
static inline void cardputerDisplayShowAlert(const char*, const char*, int8_t, uint8_t,
                                             unsigned long) {}
static inline void cardputerDisplayTick(unsigned long, uint8_t, int) {}
static inline bool cardputerDisplayInAlert(unsigned long) { return false; }

#endif
