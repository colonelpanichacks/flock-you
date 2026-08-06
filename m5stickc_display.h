// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// m5stickc_display.h — M5StickC Plus SE display helpers (flock-you-esp32)
//
// Hardware (StickC Plus SE schematic):
//   ST7789v2 135×240 TFT — M5Unified manages via AXP192 backlight
//   Passive buzzer G2     — Arduino tone()/noTone()
//   Button A: G37  Button B: G39
//
// Display layout: rotation=3 → landscape 240w × 135h
//   Row 0–15   — header bar (channel, mode, det-count)
//   Row 16–117 — main content
//   Row 118–134 — button label bar [A] [B]
//
// Button actions (flock-you-esp32):
//   A — force SPIFFS session save
//   B — force channel hop / clear alert (returns slot 3, same as Basic C)
#pragma once
#if defined(USE_M5STICKC_PLUS_SE)

#include <M5Unified.h>
#include <cstring>
#include <cstdio>
#include <cmath>


// ── RGB565 palette ────────────────────────────────────────────────────────────
static constexpr uint16_t MSC_BLACK    = 0x0000;
static constexpr uint16_t MSC_WHITE    = 0xFFFF;
static constexpr uint16_t MSC_RED      = 0xF800;
static constexpr uint16_t MSC_GREEN    = 0x07E0;
static constexpr uint16_t MSC_BLUE     = 0x001F;
static constexpr uint16_t MSC_YELLOW   = 0xFFE0;
static constexpr uint16_t MSC_CYAN     = 0x07FF;
static constexpr uint16_t MSC_ORANGE   = 0xFD20;
static constexpr uint16_t MSC_DARK_RED = 0x8000;
static constexpr uint16_t MSC_DARK_AMB = 0x8280;
static constexpr uint16_t MSC_DARK_GRN = 0x0320;
static constexpr uint16_t MSC_GREY     = 0x8410;
static constexpr uint16_t MSC_LT_GREY  = 0xC618;
static constexpr uint16_t MSC_DK_GREY  = 0x2104;

// ── Layout (240×135 landscape) ────────────────────────────────────────────────
static constexpr int MSC_W     = 240;
static constexpr int MSC_H     = 135;
static constexpr int MSC_HDR_H = 16;
static constexpr int MSC_BTN_Y = 118;
static constexpr int MSC_BTN_H = 17;

// ── State ─────────────────────────────────────────────────────────────────────
static uint8_t msc_brightness   = 128;
static bool    msc_needsRedraw  = true;
static int     msc_lastDetCount = -1;
static uint8_t msc_lastCh       = 255;

// Cached last-detection data
static char    msc_lastMac[18]   = {0};
static char    msc_lastMethod[16]= {0};
static uint8_t msc_lastConf      = 0;
static int8_t  msc_lastRssi      = 0;
static char    msc_lastSsid[34]  = {0};

// ── Internal helpers ──────────────────────────────────────────────────────────

static void msc_fmtMs(unsigned long ms, char* buf, size_t len) {
    unsigned long s=ms/1000, m=s/60; s%=60; unsigned long h=m/60; m%=60;
    if (h>0) snprintf(buf,len,"%lu:%02lu:%02lu",h,m,s);
    else      snprintf(buf,len,"%lu:%02lu",m,s);
}
static void msc_hline(int y, uint16_t col = MSC_GREY) {
    M5.Display.drawFastHLine(0, y, MSC_W, col);
}
static void msc_header(const char* left, const char* right,
                        uint16_t bg, uint16_t fg) {
    M5.Display.fillRect(0, 0, MSC_W, MSC_HDR_H, bg);
    M5.Display.setTextSize(1); M5.Display.setTextColor(fg, bg);
    M5.Display.setCursor(3, 4); M5.Display.print(left);
    if (right && right[0]) {
        int rw = (int)strlen(right) * 6;
        M5.Display.setCursor(MSC_W - rw - 3, 4);
        M5.Display.print(right);
    }
}
static void msc_btnBar(const char* a, const char* b) {
    M5.Display.fillRect(0, MSC_BTN_Y, MSC_W, MSC_BTN_H, MSC_DK_GREY);
    msc_hline(MSC_BTN_Y, MSC_GREY);
    M5.Display.setTextSize(1); M5.Display.setTextColor(MSC_LT_GREY, MSC_DK_GREY);
    char buf[14];
    snprintf(buf, sizeof(buf), "[A]%-8s", a ? a : "---");
    M5.Display.setCursor(3, MSC_BTN_Y + 5); M5.Display.print(buf);
    snprintf(buf, sizeof(buf), "[B]%-8s", b ? b : "---");
    M5.Display.setCursor(122, MSC_BTN_Y + 5); M5.Display.print(buf);
}
static void msc_bar(int x, int y, int w, int h, uint8_t pct,
                    uint16_t fill, uint16_t empty) {
    int f = (int)((long)w * pct / 100);
    if (f > 0) M5.Display.fillRect(x,     y, f,     h, fill);
    if (f < w) M5.Display.fillRect(x + f, y, w - f, h, empty);
}

// Compact RSSI helpers for 240×135 display
static const char* msc_rssiLabel(int8_t r){
    if(r>-55)return"STRONG"; if(r>-65)return"GOOD"; if(r>-75)return"FAIR"; if(r>-85)return"WEAK"; return"POOR";
}
static uint16_t msc_rssiColor(int8_t r){
    if(r>-55)return MSC_GREEN; if(r>-65)return 0x37E0; if(r>-75)return MSC_YELLOW; if(r>-85)return MSC_ORANGE; return MSC_RED;
}
static int msc_rssiBars(int8_t r){
    if(r>-55)return 5; if(r>-65)return 4; if(r>-75)return 3; if(r>-85)return 2; return 1;
}
// Compact 5-bar WiFi signal + label (height 15px, width ~125px)
static void msc_drawSig(int x, int y, int8_t rssi) {
    int nb=msc_rssiBars(rssi); uint16_t col=msc_rssiColor(rssi);
    for(int i=0;i<5;i++){int bh=(i+1)*3;int bx=x+i*6;int by=y+(15-bh);M5.Display.fillRect(bx,by,5,bh,(i<nb)?col:MSC_DK_GREY);}
    M5.Display.setTextSize(1); M5.Display.setTextColor(col,MSC_BLACK);
    M5.Display.setCursor(x+34,y+4); M5.Display.print(msc_rssiLabel(rssi));
    M5.Display.setTextColor(MSC_LT_GREY,MSC_BLACK);
    M5.Display.setCursor(x+34+6*6,y+4); M5.Display.printf(" %ddBm",(int)rssi);
}
// RSSI trend (last 4 readings)
#define MSC_HIST 4
static int8_t msc_rH[MSC_HIST]={0}; static uint8_t msc_rI=0; static bool msc_rF=false;
static void msc_rPush(int8_t r){msc_rH[msc_rI]=r;msc_rI=(msc_rI+1)%MSC_HIST;if(msc_rI==0)msc_rF=true;}
static int  msc_rTrend(){
    int c=msc_rF?MSC_HIST:(int)msc_rI; if(c<2)return 0;
    int d=(int)msc_rH[(msc_rI+MSC_HIST-1)%MSC_HIST]-(int)msc_rH[(msc_rI+MSC_HIST-c)%MSC_HIST];
    return(d>=4)?1:(d<=-4)?-1:0;
}

// ── Distance estimate ("triangulation" proxy) ─────────────────────────────────
// Single receiver ≠ true triangulation, but a free-space path-loss estimate
// gives a practical range readout alongside the approach/recede trend arrow.
//   distance_m = 10 ^ ((TxPower - RSSI) / (10 * n))
static float msc_estimateDistanceM(int8_t rssi) {
    const float txPowerAt1m = -40.0f;
    const float pathLossExp = 2.0f;
    float ratio = (txPowerAt1m - (float)rssi) / (10.0f * pathLossExp);
    return powf(10.0f, ratio);
}
static void msc_drawRange(int x, int y, int8_t rssi) {
    float d = msc_estimateDistanceM(rssi);
    char buf[20];
    if (d >= 1000.0f) snprintf(buf, sizeof(buf), "~%.1fkm", d / 1000.0f);
    else if (d >= 10.0f) snprintf(buf, sizeof(buf), "~%.0fm", d);
    else               snprintf(buf, sizeof(buf), "~%.1fm", d);
    M5.Display.setTextColor(MSC_LT_GREY, MSC_BLACK);
    M5.Display.setCursor(x, y);
    M5.Display.print(buf);
}


// ── Public API ────────────────────────────────────────────────────────────────

// ── Red LED (G10, active LOW) ──────────────────────────────────────────────
// The StickC Plus SE has a red status LED on GPIO10.  Low = on.
static bool msc_ledInit = false;
static void msc_setLED(bool on) {
    if (!msc_ledInit) { pinMode(10, OUTPUT); msc_ledInit = true; }
    digitalWrite(10, on ? LOW : HIGH);  // active LOW
}

// Called once from setup()
static void m5stickcInit() {
    auto cfg = M5.config();
    cfg.internal_imu = false;
    cfg.internal_rtc = false;
    cfg.internal_spk = false;   // SE has passive buzzer G2, NOT NS4168 I2S — prevent GPIO2 conflict
    M5.begin(cfg);

    // Init red LED — brief blink to confirm hardware
    msc_setLED(true); delay(120); msc_setLED(false);

    M5.Display.setRotation(3);   // landscape: 240w × 135h
    M5.Display.setBrightness(msc_brightness);
    M5.Display.fillScreen(MSC_BLACK);

    // Splash
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(MSC_CYAN, MSC_BLACK);
    M5.Display.setCursor(10, 36);
    M5.Display.print("FLOCK-YOU");
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MSC_GREY, MSC_BLACK);
    M5.Display.setCursor(10, 64);
    M5.Display.print("StickC Plus SE  Init...");

    msc_needsRedraw = true;
}

// ── Scanning / idle screen ────────────────────────────────────────────────────
// Call from printHeartbeat().
static void m5stickcScanning(uint8_t ch, const char* mode, int detCount,
                               unsigned long runtimeMs, bool spiffsOk,
                               int ouiHi, int ouiMfr) {
    bool chg = (ch != msc_lastCh) || (detCount != msc_lastDetCount) || msc_needsRedraw;
    if (!chg) return;
    msc_lastCh = ch; msc_lastDetCount = detCount; msc_needsRedraw = false;
    // Red LED: on when at least one target has been detected
    msc_setLED(detCount > 0);

    char hdrR[22];
    snprintf(hdrR, sizeof(hdrR), "Ch:%-2u  Det:%-3d", (unsigned)ch, detCount);
    msc_header("FLOCK-YOU  SCANNING", hdrR, MSC_DARK_GRN, MSC_WHITE);
    M5.Display.fillRect(0, MSC_HDR_H, MSC_W, MSC_BTN_Y - MSC_HDR_H, MSC_BLACK);

    int y = MSC_HDR_H + 4;

    // Status
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(detCount > 0 ? MSC_YELLOW : MSC_GREEN, MSC_BLACK);
    M5.Display.setCursor(3, y);
    M5.Display.print(detCount > 0 ? "Targets found!" : "Monitoring...");
    y += 20;

    // Config line
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MSC_WHITE, MSC_BLACK);
    M5.Display.setCursor(3, y);
    M5.Display.printf("Mode:%-8s  OUIs:%d hi + %d mfr", mode ? mode : "?", ouiHi, ouiMfr);
    y += 12;

    // Last detection (if any)
    if (detCount > 0 && msc_lastMac[0]) {
        M5.Display.setTextColor(MSC_YELLOW, MSC_BLACK);
        M5.Display.setCursor(3, y);
        M5.Display.printf("MAC: %s", msc_lastMac); y += 11;
        M5.Display.setTextColor(MSC_WHITE, MSC_BLACK);
        M5.Display.setCursor(3, y);
        M5.Display.printf("%-14s  RSSI:%d  Conf:%u%%",
                          msc_lastMethod, (int)msc_lastRssi, (unsigned)msc_lastConf);
        y += 11;
        if (msc_lastSsid[0]) {
            M5.Display.setTextColor(MSC_CYAN, MSC_BLACK);
            M5.Display.setCursor(3, y);
            char s[26]; strncpy(s, msc_lastSsid, 25); s[25] = '\0';
            M5.Display.printf("SSID:\"%s\"", s);
            y += 11;
        }
    } else {
        M5.Display.setTextColor(MSC_GREY, MSC_BLACK);
        M5.Display.setCursor(3, y);
        M5.Display.print("No cameras detected yet");
        y += 11;
    }

    // Runtime / SPIFFS
    char el[12]; msc_fmtMs(runtimeMs, el, sizeof(el));
    M5.Display.setTextColor(MSC_GREY, MSC_BLACK);
    M5.Display.setCursor(3, MSC_BTN_Y - 13);
    M5.Display.printf("Runtime: %-8s  SPIFFS: %s", el, spiffsOk ? "OK" : "ERR");

    msc_btnBar("SAVE", "HOP CH");
}

// ── Detection alert screen ────────────────────────────────────────────────────
// Call from drainAlertQueue().
static void m5stickcDetection(const char* method, const char* mac,
                                uint8_t conf, int8_t rssi, uint8_t ch,
                                const char* ssid, int detCount,
                                unsigned long lastSeenMs) {
    // Cache for scanning summary
    if (mac)    { strncpy(msc_lastMac,    mac,    17); msc_lastMac[17]    = '\0'; }
    if (method) { strncpy(msc_lastMethod, method, 15); msc_lastMethod[15] = '\0'; }
    ssid = ssid ? ssid : "";
    strncpy(msc_lastSsid, ssid, 33); msc_lastSsid[33] = '\0';
    msc_lastConf = conf; msc_lastRssi = rssi; msc_needsRedraw = true;

    uint16_t hdrBg = (conf >= 60) ? MSC_DARK_RED :
                     (conf >= 30) ? MSC_DARK_AMB : 0x0010;
    const char* hdrLbl = (conf >= 60) ? "!! ALERT !!" :
                         (conf >= 30) ? "PROBABLE"    : "LOW CONF";
    char hdrR[18];
    snprintf(hdrR, sizeof(hdrR), "C:%u%%  Ch:%-2u", (unsigned)conf, (unsigned)ch);
    msc_header(hdrLbl, hdrR, hdrBg, MSC_WHITE);
    M5.Display.fillRect(0, MSC_HDR_H, MSC_W, MSC_BTN_Y - MSC_HDR_H, MSC_BLACK);

    int y = MSC_HDR_H + 3;

    // Method — large
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(MSC_YELLOW, MSC_BLACK);
    M5.Display.setCursor(3, y);
    M5.Display.print(method ? method : "?");
    y += 20;

    M5.Display.setTextSize(1);
    M5.Display.setTextColor(MSC_WHITE, MSC_BLACK);
    M5.Display.setCursor(3, y);
    M5.Display.printf("MAC: %s", mac ? mac : "??:??:??:??:??:??");
    y += 11;
    // Compact signal bars + trend
    msc_rPush(rssi);
    msc_drawSig(3, y, rssi);
    {
        int tr=msc_rTrend();
        const char* ta=(tr>0)?"\xe2\x86\x91":(tr<0)?"\xe2\x86\x93":"\xe2\x86\x92";
        uint16_t tc=(tr>0)?MSC_RED:(tr<0)?MSC_GREEN:MSC_GREY;
        M5.Display.setTextColor(tc,MSC_BLACK);
        M5.Display.setCursor(3+126,y+4); M5.Display.printf("%s Ch:%u", ta, (unsigned)ch);
    }
    y += 17;
    // Estimated range ("triangulation" proxy)
    msc_drawRange(3, y, rssi);
    y += 11;

    if (ssid && ssid[0]) {
        M5.Display.setTextColor(MSC_CYAN, MSC_BLACK);
        M5.Display.setCursor(3, y);
        char s[26]; strncpy(s, ssid, 25); s[25] = '\0';
        M5.Display.printf("SSID: \"%s\"", s);
        y += 11;
    }

    msc_hline(y); y += 4;

    // Time since detection
    M5.Display.setTextColor(MSC_LT_GREY, MSC_BLACK);
    M5.Display.setCursor(3, y);
    if (lastSeenMs < 3000) {
        M5.Display.setTextColor(MSC_RED, MSC_BLACK);
        M5.Display.print("JUST DETECTED");
    } else {
        char el[10]; msc_fmtMs(lastSeenMs, el, sizeof(el));
        M5.Display.printf("Last seen: %s ago", el);
    }
    y += 11;

    // Confidence bar
    uint16_t barFill = (conf >= 60) ? MSC_RED :
                       (conf >= 30) ? MSC_ORANGE : MSC_BLUE;
    msc_bar(3, y, 200, 8, conf, barFill, MSC_DK_GREY);
    M5.Display.setTextColor(MSC_WHITE, MSC_BLACK);
    M5.Display.setCursor(207, y); M5.Display.printf("%u%%", (unsigned)conf);

    // Red LED: on for high-confidence detections, brief flash for low
    if (conf >= 60) {
        msc_setLED(true);   // solid red — definite camera
    } else if (conf >= 30) {
        // Fast double-blink for probable detection
        msc_setLED(true); delay(80); msc_setLED(false); delay(60);
        msc_setLED(true); delay(80); msc_setLED(false);
    } else {
        msc_setLED(false);  // low confidence — no LED
    }

    msc_btnBar("SAVE", "CLEAR");
}

// ── Button tick ───────────────────────────────────────────────────────────────
// Returns: 0=none  1=A(save)  3=B(hop/clear)  — slot 3 matches Basic Btn C handler
static int m5stickcButtonTick() {
    M5.update();
    if (M5.BtnA.wasPressed()) return 1;
    if (M5.BtnB.wasPressed()) {
        msc_needsRedraw = true;
        return 3;   // maps to same handler slot as Basic/Core2 Btn C
    }
    return 0;
}

// ── Audio helpers (passive buzzer G2) ─────────────────────────────────────────
// Buzzer is passive, driven by Arduino tone() — NOT I2S.
// Use these wrappers to keep call sites consistent across all target boards.
static inline void m5stickcBeep(uint32_t hz, uint32_t ms) {
    tone(2, hz, ms);
}
static inline void m5stickcBeepStop() {
    noTone(2);
}

#endif // USE_M5STICKC_PLUS_SE
