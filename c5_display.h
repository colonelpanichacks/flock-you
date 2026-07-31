// c5_display.h — LILYGO T-Dongle C5 display + RGB LED helpers
// ST7735S 80x160 TFT via Adafruit_ST7735  (SPI1)
// WS2812B RGB LED on GPIO11 via Adafruit_NeoPixel
//
// Pin reference (LILYGO T-Dongle C5 schematic):
//   TFT SCLK=5  MOSI=6  CS=4  DC=2  RST=3  BL=1
//   RGB LED=11  BOOT BTN=9
#pragma once
#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_NeoPixel.h>

// ── Hardware pins ─────────────────────────────────────────────────────────────
#define C5_TFT_SCLK  5
#define C5_TFT_MOSI  6
#define C5_TFT_CS    4
#define C5_TFT_DC    2
#define C5_TFT_RST   3
#define C5_TFT_BL    1
#define C5_RGB_PIN   11   // WS2812B single LED
#define C5_BTN_PIN   9

// ── Objects (defined in header — one translation unit only) ───────────────────
static Adafruit_ST7735   c5Tft(C5_TFT_CS, C5_TFT_DC, C5_TFT_MOSI, C5_TFT_SCLK, C5_TFT_RST);
static Adafruit_NeoPixel c5Led(1, C5_RGB_PIN, NEO_GRB + NEO_KHZ800);

// ── RGB LED helpers ───────────────────────────────────────────────────────────
static inline void c5LedSet(uint8_t r, uint8_t g, uint8_t b) {
    c5Led.setPixelColor(0, c5Led.Color(r, g, b));
    c5Led.show();
}
static inline void c5LedIdle()    { c5LedSet(  0,  60,   0); }   // dim green
static inline void c5LedCaution() { c5LedSet(180,  60,   0); }   // amber
static inline void c5LedAlert()   { c5LedSet(220,   0,   0); }   // red
static inline void c5LedOff()     { c5LedSet(  0,   0,   0); }

static inline void c5LedStartup() {
    for (int i = 0; i < 3; i++) {
        c5LedSet(0, 0, 80); delay(120);
        c5LedSet(0, 0,  0); delay(80);
    }
    c5LedIdle();
}

// ── Display init ─────────────────────────────────────────────────────────────
static void c5DisplayInit() {
    pinMode(C5_TFT_BL, OUTPUT);
    digitalWrite(C5_TFT_BL, HIGH);                 // backlight on

    c5Tft.initR(INITR_MINI160x80_PLUGIN);           // 80x160 mini TFT
    c5Tft.setRotation(1);                           // USB-C connector on right
    c5Tft.fillScreen(ST77XX_BLACK);

    // Splash screen
    c5Tft.setTextSize(1);
    c5Tft.setTextColor(ST77XX_CYAN);
    c5Tft.setCursor(4, 4);
    c5Tft.print("T-Dongle C5  ready");
    c5Tft.setTextColor(0x07FF);                     // light cyan
    c5Tft.setCursor(4, 18);
    c5Tft.print("Scanning...");

    c5Led.begin();
    c5Led.setBrightness(90);
    c5LedStartup();
}

// ── Scanning status (called from heartbeat / channel hop) ─────────────────────
static void c5DisplayScanning(uint8_t ch, int detCount) {
    c5Tft.fillScreen(ST77XX_BLACK);
    c5Tft.setTextSize(1);
    c5Tft.setTextColor(ST77XX_CYAN);
    c5Tft.setCursor(4, 4);   c5Tft.print("T-Dongle C5");
    c5Tft.setTextColor(ST77XX_GREEN);
    c5Tft.setCursor(4, 18);  c5Tft.print("Scanning...");
    c5Tft.setTextColor(ST77XX_WHITE);
    c5Tft.setCursor(4, 30);
    c5Tft.printf("Ch: %-3u  Det: %d", (unsigned)ch, detCount);
    c5LedIdle();
}

// ── Detection alert (flock-you-esp32 variant) ─────────────────────────────────
// detType  = short label for detection method ("OUI", "SSID", "BLE", etc.)
// mac      = "xx:xx:xx:xx:xx:xx"
// confidence = 0-100
// rssi       = e.g. -72
// ch         = channel number
static void c5DisplayDetection(const char* detType, const char* mac,
                                uint8_t confidence, int8_t rssi, uint8_t ch) {
    // Background colour by confidence level
    uint16_t bg = ST77XX_BLACK;
    if (confidence >= 60)      bg = 0x8000;   // dark red
    else if (confidence >= 30) bg = 0x8400;   // dark orange

    c5Tft.fillScreen(bg);
    c5Tft.setTextSize(1);

    // Header
    c5Tft.setTextColor(ST77XX_CYAN);
    c5Tft.setCursor(4, 4);
    c5Tft.print("! FLOCK DETECT !");

    // Detection type — larger text
    c5Tft.setTextSize(2);
    c5Tft.setTextColor(ST77XX_YELLOW);
    c5Tft.setCursor(4, 18);
    c5Tft.print(detType);

    // MAC (last 3 bytes) + RSSI
    c5Tft.setTextSize(1);
    c5Tft.setTextColor(ST77XX_WHITE);
    c5Tft.setCursor(4, 38);
    // show last 8 chars of mac (xx:xx:xx)
    size_t mlen = strlen(mac);
    const char* shortMac = (mlen >= 8) ? mac + mlen - 8 : mac;
    c5Tft.printf("MAC: ...%s", shortMac);

    c5Tft.setCursor(4, 50);
    c5Tft.printf("RSSI:%d  Ch:%-2u", (int)rssi, (unsigned)ch);

    // Confidence bar
    c5Tft.setCursor(4, 62);
    c5Tft.setTextColor(confidence >= 60 ? ST77XX_RED : ST77XX_YELLOW);
    c5Tft.printf("Conf: %u%%", (unsigned)confidence);

    // LED
    if (confidence >= 60)      c5LedAlert();
    else if (confidence >= 30) c5LedCaution();
    else                        c5LedIdle();
}

// ── Eye-spy variant: score-based display ─────────────────────────────────────
// score     = current aggregate score
// lastDet   = short label of most-recent detection type (may be nullptr)
// phase     = "BLE" | "WIFI" | "PROMISC"
static void c5DisplayScore(int score, const char* lastDet, const char* phase) {
    uint16_t bg = ST77XX_BLACK;
    if (score >= 6)      bg = 0x8000;   // dark red
    else if (score >= 3) bg = 0x8400;   // dark amber

    c5Tft.fillScreen(bg);
    c5Tft.setTextSize(1);

    // Header
    c5Tft.setTextColor(ST77XX_CYAN);
    c5Tft.setCursor(4, 4);
    c5Tft.print("EYE SPY");

    // Score — big
    c5Tft.setTextSize(3);
    uint16_t scoreCol = (score >= 6) ? ST77XX_RED :
                        (score >= 3) ? ST77XX_YELLOW : ST77XX_GREEN;
    c5Tft.setTextColor(scoreCol);
    c5Tft.setCursor(4, 20);
    c5Tft.printf("%d", score);

    // Status label
    c5Tft.setTextSize(1);
    c5Tft.setTextColor(ST77XX_WHITE);
    c5Tft.setCursor(30, 28);
    const char* lbl = (score >= 6) ? "ALERT" : (score >= 3) ? "CAUTION" : "CLEAR";
    c5Tft.print(lbl);

    // Last detection
    if (lastDet && lastDet[0]) {
        c5Tft.setTextColor(ST77XX_YELLOW);
        c5Tft.setCursor(4, 50);
        c5Tft.print(lastDet);
    }

    // Phase
    c5Tft.setTextColor(0x8410);   // grey
    c5Tft.setCursor(4, 62);
    c5Tft.printf("Phase: %s", phase ? phase : "?");

    // LED
    if (score >= 6)      c5LedAlert();
    else if (score >= 3) c5LedCaution();
    else                  c5LedIdle();
}

#endif // USE_C5_DISPLAY
