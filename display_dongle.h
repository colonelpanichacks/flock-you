#pragma once

#include <stdint.h>

#ifdef USE_T_DONGLE_DISPLAY

void dongleDisplayInit();
void dongleDisplayShowIdle(uint8_t ch, int detCount);
void dongleDisplayShowAlert(const char* method, const char* mac, int8_t rssi,
                            uint8_t ch, unsigned long alertMs);
void dongleDisplayTick(unsigned long now, uint8_t ch, int detCount);
bool dongleDisplayInAlert(unsigned long now);

#else

static inline void dongleDisplayInit() {}
static inline void dongleDisplayShowIdle(uint8_t, int) {}
static inline void dongleDisplayShowAlert(const char*, const char*, int8_t, uint8_t,
                                          unsigned long) {}
static inline void dongleDisplayTick(unsigned long, uint8_t, int) {}
static inline bool dongleDisplayInAlert(unsigned long) { return false; }

#endif
