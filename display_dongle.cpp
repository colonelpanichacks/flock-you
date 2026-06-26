#ifdef USE_T_DONGLE_DISPLAY

#include "display_dongle.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>

// LilyGO T-Dongle S3 ST7735 80x160 IPS — backlight active-low on GPIO38.
#define DONGLE_TFT_BL_PIN 38
#define DONGLE_TFT_RST_PIN 1

// 160x80 landscape — matches LilyGO examples/TFT_eSPI/TFT_eSPI.ino
#define DONGLE_TFT_ROTATION 1

static TFT_eSPI tft = TFT_eSPI();
static unsigned long alertUntilMs = 0;
static uint8_t idleCh = 1;
static int idleDetCount = 0;
static bool inAlert = false;
static uint8_t lastDrawnCh = 0xFF;
static int lastDrawnHits = -1;

static void backlightOn() {
  pinMode(DONGLE_TFT_BL_PIN, OUTPUT);
  digitalWrite(DONGLE_TFT_BL_PIN, LOW);
}

static void tftHardwareReset() {
  pinMode(DONGLE_TFT_RST_PIN, OUTPUT);
  digitalWrite(DONGLE_TFT_RST_PIN, HIGH);
  delay(10);
  digitalWrite(DONGLE_TFT_RST_PIN, LOW);
  delay(10);
  digitalWrite(DONGLE_TFT_RST_PIN, HIGH);
  delay(120);
}

static void drawMethodLines(const char* method, int y) {
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  char line[32];
  const char* p = method;
  while (*p) {
    size_t n = 0;
    const char* seg = p;
    while (p[n] && p[n] != '_' && n < 16) n++;
    if (p[n] == '_') n++;
    size_t copy = n;
    if (copy >= sizeof(line)) copy = sizeof(line) - 1;
    memcpy(line, seg, copy);
    line[copy] = '\0';
    tft.setCursor(2, y);
    tft.print(line);
    y += 10;
    p += n;
    if (y > 58) break;
  }
}

void dongleDisplayInit() {
  tftHardwareReset();
  tft.init();
  tft.setRotation(DONGLE_TFT_ROTATION);
  tft.fillScreen(TFT_BLACK);
  backlightOn();
  dongleDisplayShowIdle(1, 0);
}

void dongleDisplayShowIdle(uint8_t ch, int detCount) {
  idleCh = ch;
  idleDetCount = detCount;
  inAlert = false;
  alertUntilMs = 0;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(4, 4);
  tft.print("SCAN");
  tft.setCursor(4, 24);
  tft.print("NING");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 50);
  tft.printf("Ch: %u", (unsigned)ch);
  tft.setCursor(2, 64);
  tft.printf("Hits: %d", detCount);

  lastDrawnCh = ch;
  lastDrawnHits = detCount;
}

void dongleDisplayShowAlert(const char* method, const char* mac, int8_t rssi,
                            uint8_t ch, unsigned long alertMs) {
  idleCh = ch;
  inAlert = true;
  if (alertMs == 0) alertMs = 1;
  alertUntilMs = millis() + alertMs;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(2);
  tft.setCursor(4, 2);
  tft.print("DETECT");

  drawMethodLines(method ? method : "?", 22);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(2, 44);
  tft.print(mac ? mac : "");
  tft.setCursor(2, 56);
  tft.printf("RSSI %d", (int)rssi);
  tft.setCursor(2, 68);
  tft.printf("CH %u", (unsigned)ch);
}

bool dongleDisplayInAlert(unsigned long now) {
  return inAlert && alertUntilMs != 0 && (long)(now - alertUntilMs) < 0;
}

void dongleDisplayTick(unsigned long now, uint8_t ch, int detCount) {
  idleCh = ch;
  idleDetCount = detCount;
  if (inAlert && alertUntilMs != 0 && (long)(now - alertUntilMs) >= 0) {
    dongleDisplayShowIdle(idleCh, idleDetCount);
    return;
  }
  if (!inAlert && (ch != lastDrawnCh || detCount != lastDrawnHits)) {
    dongleDisplayShowIdle(ch, detCount);
  }
}

#endif
