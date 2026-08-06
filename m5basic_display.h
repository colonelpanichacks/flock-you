// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// m5basic_display.h — M5Stack Basic Core v2.7 display + audio helpers
//
// Hardware (Basic v2.7 schematic):
//   ILI9342C 320×240 IPS — managed by M5Unified (LovyanGFX)
//   Speaker 1W on G25    — M5.Speaker.tone()
//   Button A: G39  Button B: G38  Button C: G37
//   IP5306 power mgmt @ I2C 0x75
//
// Display layout (320×240 landscape):
//   Row 0     — header bar  (channel, mode, det-count)
//   Row 18    — main content area
//   Row 210   — separator
//   Row 216   — button label bar [A] [B] [C]
//
// Button actions (flock-you-esp32):
//   A — force SPIFFS session save
//   B — cycle display brightness (40 → 160 → 255 → 40)
//   C — force immediate channel hop
#pragma once
#if defined(USE_M5BASIC)

#include <M5Unified.h>
#include <cstring>
#include <cstdio>

// ── RGB565 palette ────────────────────────────────────────────────────────────
static constexpr uint16_t MB_BLACK    = 0x0000;
static constexpr uint16_t MB_WHITE    = 0xFFFF;
static constexpr uint16_t MB_RED      = 0xF800;
static constexpr uint16_t MB_GREEN    = 0x07E0;
static constexpr uint16_t MB_BLUE     = 0x001F;
static constexpr uint16_t MB_YELLOW   = 0xFFE0;
static constexpr uint16_t MB_CYAN     = 0x07FF;
static constexpr uint16_t MB_ORANGE   = 0xFD20;
static constexpr uint16_t MB_DARK_RED = 0x8000;
static constexpr uint16_t MB_DARK_AMB = 0x8280;   // dark amber
static constexpr uint16_t MB_DARK_GRN = 0x0320;   // dark green header
static constexpr uint16_t MB_GREY     = 0x8410;
static constexpr uint16_t MB_LT_GREY  = 0xC618;
static constexpr uint16_t MB_DK_GREY  = 0x2104;

// ── Layout constants ──────────────────────────────────────────────────────────
static constexpr int MB_W       = 320;
static constexpr int MB_H       = 240;
static constexpr int MB_HDR_H   = 18;   // header bar height
static constexpr int MB_BTN_Y   = 214;  // button label start y
static constexpr int MB_BTN_H   = 26;   // button bar height

// ── State ─────────────────────────────────────────────────────────────────────
static uint8_t mb_brightness    = 160;
static bool    mb_needsRedraw   = true;
static int     mb_lastDetCount  = -1;
static uint8_t mb_lastCh        = 255;
static bool    mb_inAlert       = false;

// Cached last-detection data (for scanning screen summary)
static char    mb_lastMac[18]   = {0};
static char    mb_lastMethod[16]= {0};
static char    mb_lastSsid[34]  = {0};
static uint8_t mb_lastConf      = 0;
static int8_t  mb_lastRssi      = 0;
static uint8_t mb_lastChan      = 0;

// ── Internal helpers ──────────────────────────────────────────────────────────

// Format elapsed milliseconds as "[H:]MM:SS"
static void mb_fmtMs(unsigned long ms, char* buf, size_t len) {
    unsigned long s = ms / 1000;
    unsigned long m = s / 60;  s %= 60;
    unsigned long h = m / 60;  m %= 60;
    if (h > 0) snprintf(buf, len, "%lu:%02lu:%02lu", h, m, s);
    else        snprintf(buf, len, "%lu:%02lu", m, s);
}

// Filled progress bar at (x,y) w×h, percent 0–100
static void mb_bar(int x, int y, int w, int h, uint8_t pct,
                   uint16_t fillCol, uint16_t emptyCol) {
    int f = (int)((long)w * pct / 100);
    if (f > 0) M5.Display.fillRect(x,     y, f,     h, fillCol);
    if (f < w) M5.Display.fillRect(x + f, y, w - f, h, emptyCol);
}

// Horizontal divider
static void mb_hline(int y, uint16_t col = MB_GREY) {
    M5.Display.drawFastHLine(0, y, MB_W, col);
}

// Header bar (full-width, MB_HDR_H tall)
static void mb_header(const char* left, const char* right,
                      uint16_t bg = MB_DARK_GRN, uint16_t fg = MB_WHITE) {
    M5.Display.fillRect(0, 0, MB_W, MB_HDR_H, bg);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(4, 5);
    M5.Display.print(left);
    if (right && right[0]) {
        int rw = (int)strlen(right) * 6;
        M5.Display.setCursor(MB_W - rw - 4, 5);
        M5.Display.print(right);
    }
}

// Button label bar
static void mb_btnBar(const char* a, const char* b, const char* c) {
    M5.Display.fillRect(0, MB_BTN_Y, MB_W, MB_BTN_H, MB_DK_GREY);
    mb_hline(MB_BTN_Y, MB_GREY);
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MB_LT_GREY, MB_DK_GREY);
    char buf[16];
    snprintf(buf, sizeof(buf), "[A]%-7s", a ? a : "---");
    M5.Display.setCursor(4,   MB_BTN_Y + 9); M5.Display.print(buf);
    snprintf(buf, sizeof(buf), "[B]%-7s", b ? b : "---");
    M5.Display.setCursor(108, MB_BTN_Y + 9); M5.Display.print(buf);
    snprintf(buf, sizeof(buf), "[C]%-7s", c ? c : "---");
    M5.Display.setCursor(212, MB_BTN_Y + 9); M5.Display.print(buf);
}

// ── Public API ────────────────────────────────────────────────────────────────

// Called once in setup() — initialises M5Unified, screen, and speaker
static void m5basicInit() {
    auto cfg = M5.config();
    cfg.internal_imu = false;
    cfg.internal_rtc = false;
    M5.begin(cfg);

    M5.Speaker.setVolume(200);

    // Core2 For AWS: short startup vibration to confirm hardware is working
#if defined(USE_M5CORE2_AWS)
    M5.Power.setVibration(180); delay(150); M5.Power.setVibration(0);
#endif

    M5.Display.setBrightness(mb_brightness);
    M5.Display.fillScreen(MB_BLACK);

    // Splash
    M5.Display.setTextSize(3);
    M5.Display.setTextColor(MB_CYAN, MB_BLACK);
    M5.Display.setCursor(20, 50);
    M5.Display.print("FLOCK-YOU");
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(0x07BF, MB_BLACK); // light blue
    M5.Display.setCursor(20, 90);
    M5.Display.print("v2  M5Stack Basic");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MB_GREY, MB_BLACK);
    M5.Display.setCursor(20, 120);
    M5.Display.print("Passive Flock Safety ALPR detector");
    M5.Display.setTextColor(MB_GREEN, MB_BLACK);
    M5.Display.setCursor(20, 140);
    M5.Display.print("Initialising...");

    mb_needsRedraw = true;
    mb_inAlert     = false;
}

// ── Scanning/idle screen ──────────────────────────────────────────────────────
// Call from printHeartbeat().  Only redraws when data has changed.
static void m5basicScanning(uint8_t ch, const char* modeName, int detCount,
                              unsigned long runtimeMs, bool spiffsOk,
                              int ouiHighCnt, int ouiMfrCnt) {
    bool changed = (ch != mb_lastCh) || (detCount != mb_lastDetCount) || mb_needsRedraw;
    if (!changed) return;
    mb_lastCh = ch; mb_lastDetCount = detCount;
    mb_needsRedraw = false;
    mb_inAlert = false;

    // Header
    char hdrR[28];
    snprintf(hdrR, sizeof(hdrR), "Ch:%-2u  Det:%-3d", (unsigned)ch, detCount);
    mb_header("FLOCK-YOU  SCANNING", hdrR, MB_DARK_GRN, MB_WHITE);

    // Clear content area
    M5.Display.fillRect(0, MB_HDR_H, MB_W, MB_BTN_Y - MB_HDR_H, MB_BLACK);

    int y = MB_HDR_H + 8;

    // Status
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(MB_GREEN, MB_BLACK);
    M5.Display.setCursor(8, y);
    M5.Display.print(detCount > 0 ? "Targets found!" : "Monitoring...");
    y += 26;

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MB_WHITE, MB_BLACK);
    M5.Display.setCursor(8, y);
    M5.Display.printf("Mode: %-10s  RSSI min: -95 dBm", modeName ? modeName : "?");
    y += 13;
    M5.Display.setCursor(8, y);
    M5.Display.printf("OUIs: %d hi + %d mfr + 1 SoundThinking",
                      ouiHighCnt, ouiMfrCnt);
    y += 14;

    mb_hline(y); y += 7;

    if (detCount == 0) {
        M5.Display.setTextColor(MB_GREY, MB_BLACK);
        M5.Display.setCursor(8, y);
        M5.Display.print("No Flock cameras detected yet");
        y += 13;
    } else {
        // Last detection summary
        M5.Display.setTextSize(1);
        M5.Display.setTextColor(MB_YELLOW, MB_BLACK);
        M5.Display.setCursor(8, y);
        M5.Display.printf("%d target(s) this session", detCount);
        y += 13;

        if (mb_lastMac[0]) {
            M5.Display.setTextColor(MB_WHITE, MB_BLACK);
            M5.Display.setCursor(8, y);
            M5.Display.printf("Last MAC:  %s", mb_lastMac);
            y += 12;
            M5.Display.setCursor(8, y);
            M5.Display.printf("Method:    %-14s  Conf:%u%%",
                              mb_lastMethod, (unsigned)mb_lastConf);
            y += 12;
            M5.Display.setCursor(8, y);
            M5.Display.printf("RSSI: %d dBm   Ch: %u",
                              (int)mb_lastRssi, (unsigned)mb_lastChan);
            y += 12;
            if (mb_lastSsid[0]) {
                M5.Display.setTextColor(MB_CYAN, MB_BLACK);
                M5.Display.setCursor(8, y);
                char s[28]; strncpy(s, mb_lastSsid, 27); s[27] = '\0';
                M5.Display.printf("SSID: \"%s\"", s);
                y += 12;
            }
        }
    }

    // Runtime + SPIFFS
    int ry = MB_BTN_Y - 28;
    mb_hline(ry); ry += 6;
    char el[12];
    mb_fmtMs(runtimeMs, el, sizeof(el));
    M5.Display.setTextColor(MB_GREY, MB_BLACK);
    M5.Display.setCursor(8, ry);
    M5.Display.printf("Runtime: %-10s  SPIFFS: %s", el, spiffsOk ? "OK" : "ERR");

    mb_btnBar("SAVE", "BRIGHT", "HOP CH");
}

// ── Detection alert screen ────────────────────────────────────────────────────
// Call after each detection is processed from the alert queue.
// lastSeenMs = millis() - fyLastTargetSeen (0 = just now)
static void m5basicDetection(const char* method, const char* mac,
                               uint8_t confidence, int8_t rssi, uint8_t ch,
                               const char* ssid, int detCount,
                               unsigned long lastSeenMs) {
    // Cache for scanning summary
    if (mac)    { strncpy(mb_lastMac,    mac,    17); mb_lastMac[17]    = '\0'; }
    if (method) { strncpy(mb_lastMethod, method, 15); mb_lastMethod[15] = '\0'; }
    ssid = ssid ? ssid : "";
    strncpy(mb_lastSsid, ssid, 33); mb_lastSsid[33] = '\0';
    mb_lastConf = confidence; mb_lastRssi = rssi; mb_lastChan = ch;
    mb_inAlert = true; mb_needsRedraw = true;

    // Header
    uint16_t hdrBg = (confidence >= 60) ? MB_DARK_RED :
                     (confidence >= 30) ? MB_DARK_AMB : 0x0010;
    const char* hdrLbl = (confidence >= 60) ? "!! FLOCK ALERT !!" :
                         (confidence >= 30) ? "FLOCK PROBABLE"    : "LOW CONFIDENCE";
    char hdrR[28];
    snprintf(hdrR, sizeof(hdrR), "CONF:%u%%  CH:%-2u",
             (unsigned)confidence, (unsigned)ch);
    mb_header(hdrLbl, hdrR, hdrBg, MB_WHITE);

    M5.Display.fillRect(0, MB_HDR_H, MB_W, MB_BTN_Y - MB_HDR_H, MB_BLACK);

    int y = MB_HDR_H + 6;

    // Detection method — large text
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(MB_YELLOW, MB_BLACK);
    M5.Display.setCursor(8, y);
    M5.Display.print(method ? method : "unknown");
    y += 26;

    // MAC
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MB_WHITE, MB_BLACK);
    M5.Display.setCursor(8, y);
    M5.Display.printf("MAC:    %s", mac ? mac : "??:??:??:??:??:??");
    y += 13;

    // RSSI + channel
    M5.Display.setCursor(8, y);
    M5.Display.printf("RSSI:   %d dBm        Channel: %u",
                      (int)rssi, (unsigned)ch);
    y += 13;

    // SSID
    if (ssid && ssid[0]) {
        M5.Display.setTextColor(MB_CYAN, MB_BLACK);
        M5.Display.setCursor(8, y);
        char s[32]; strncpy(s, ssid, 31); s[31] = '\0';
        M5.Display.printf("SSID:   \"%s\"", s);
        y += 13;
    }

    mb_hline(y); y += 7;

    // Time since detection
    M5.Display.setTextColor(MB_LT_GREY, MB_BLACK);
    M5.Display.setCursor(8, y);
    if (lastSeenMs < 3000) {
        M5.Display.setTextColor(MB_RED, MB_BLACK);
        M5.Display.print("JUST DETECTED  \xe2\x86\x90 NOW");
    } else {
        char el[12]; mb_fmtMs(lastSeenMs, el, sizeof(el));
        M5.Display.printf("Last seen: %s ago", el);
    }
    y += 13;

    // Session total
    M5.Display.setTextColor(MB_LT_GREY, MB_BLACK);
    M5.Display.setCursor(8, y);
    M5.Display.printf("Session: %d detection(s) total", detCount);
    y += 16;

    // Confidence bar
    mb_hline(y); y += 7;
    uint16_t barFill = (confidence >= 60) ? MB_RED :
                       (confidence >= 30) ? MB_ORANGE : MB_BLUE;
    mb_bar(8, y, 282, 14, confidence, barFill, MB_DK_GREY);
    // Percentage text on the right
    M5.Display.setTextColor(MB_WHITE, MB_BLACK);
    M5.Display.setCursor(295, y + 3);
    M5.Display.printf("%u%%", (unsigned)confidence);
    y += 20;

    // Confidence label
    M5.Display.setTextColor(barFill, MB_BLACK);
    M5.Display.setCursor(8, y);
    if (confidence >= 60)
        M5.Display.print("HIGH CONFIDENCE -- definite Flock camera");
    else if (confidence >= 30)
        M5.Display.print("PROBABLE -- worth investigating");
    else
        M5.Display.print("LOW -- possible false positive");

    mb_btnBar("SAVE", "BRIGHT", "CLEAR");

    // Core2 For AWS: vibrate on high-confidence detection (tactile alert)
#if defined(USE_M5CORE2_AWS)
    if (confidence >= 30) {
        M5.Power.setVibration(220); delay(200); M5.Power.setVibration(0);
        if (confidence >= 60) { delay(80); M5.Power.setVibration(220); delay(200); M5.Power.setVibration(0); }
    }
#endif
}

// ── Button tick ───────────────────────────────────────────────────────────────
// Call from loop() every iteration.
// Returns: 0=none  1=A(save)  2=B(brightness)  3=C(hop/clear)
static int m5basicButtonTick() {
    M5.update();
    if (M5.BtnA.wasPressed()) return 1;
    if (M5.BtnB.wasPressed()) {
        mb_brightness = (mb_brightness < 80)  ? 160 :
                        (mb_brightness < 200) ? 255 : 40;
        M5.Display.setBrightness(mb_brightness);
        return 2;
    }
    if (M5.BtnC.wasPressed()) {
        mb_needsRedraw = true;
        mb_inAlert     = false;
        return 3;
    }
    return 0;
}

// ── Audio helpers (replace tone()/noTone() for Basic speaker) ─────────────────
static inline void m5basicBeep(uint32_t hz, uint32_t ms) {
    M5.Speaker.tone(hz, ms);
}
static inline void m5basicBeepStop() {
    M5.Speaker.stop();
}

#endif // USE_M5BASIC
