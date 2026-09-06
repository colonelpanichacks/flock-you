#include "display_cardputer.h"

#ifdef USE_CARDPUTER_DISPLAY

#include <M5Cardputer.h>

// M5Stack Cardputer: 1.14" ST7789 240x135 IPS, driven by M5Unified/M5GFX.
// M5Cardputer.begin() brings up the display, keyboard and I2S speaker
// together — main.cpp's fyToneOn()/fyToneOff() rely on that having run
// before the first beep.

static uint8_t idleCh = 1;
static int idleDetCount = 0;
static bool inAlert = false;
static uint8_t lastDrawnCh = 0xFF;
static int lastDrawnHits = -1;

// Text size 1 (6px/char) keeps even the longest method string —
// "wifi_wildcard_probe_ie_sig", 27 chars — under the 240px screen width.
static void drawMethodLine(const char *method, int y)
{
  auto &d = M5Cardputer.Display;
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(2);
  d.setCursor(4, y);
  d.print(method);
}

void cardputerDisplayInit()
{
  auto cfg = M5.config();
  M5Cardputer.begin(cfg);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  cardputerDisplayShowIdle(1, 0);
}

void cardputerDisplayShowIdle(uint8_t ch, int detCount)
{
  idleCh = ch;
  idleDetCount = detCount;
  inAlert = false;

  auto &d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.setTextSize(3);
  d.setCursor(6, 8);
  d.print("SCANNING");

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(2);
  d.setCursor(4, 72);
  d.printf("Ch: %u", (unsigned)ch);
  d.setCursor(4, 100);
  d.printf("Hits: %d", detCount);

  lastDrawnCh = ch;
  lastDrawnHits = detCount;
}

void cardputerDisplayShowAlert(const char *method, const char *mac, int8_t rssi,
                               uint8_t ch, unsigned long alertMs)
{
  (void)alertMs; // ignored — Cardputer latches on the alert until a key is pressed
  idleCh = ch;
  inAlert = true;

  auto &d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextColor(TFT_RED, TFT_BLACK);
  d.setTextSize(3);
  d.setCursor(6, 4);
  d.print("DETECT");

  drawMethodLine(method ? method : "?", 40);

  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextSize(2);
  d.setCursor(4, 60);
  d.print(mac ? mac : "");
  d.setCursor(4, 80);
  d.printf("RSSI %d  CH %u", (int)rssi, (unsigned)ch);
}

bool cardputerDisplayInAlert(unsigned long now)
{
  (void)now;
  return inAlert;
}

// Cardputer has a keyboard, so a detection screen doesn't need to time out
// on its own — it stays up (even through further redraws from new hits)
// until you dismiss it with a key press, then returns to idle.
void cardputerDisplayTick(unsigned long now, uint8_t ch, int detCount)
{
  (void)now;
  M5Cardputer.update(); // polls hardware, keyboard included

  idleCh = ch;
  idleDetCount = detCount;
  if (inAlert)
  {
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
    {
      cardputerDisplayShowIdle(ch, detCount);
    }
    return;
  }
  if (ch != lastDrawnCh || detCount != lastDrawnHits)
  {
    cardputerDisplayShowIdle(ch, detCount);
  }
}

#endif
