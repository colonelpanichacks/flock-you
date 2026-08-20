// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2024 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// ui_task.h — flock-you-esp32: decoupled UI/display FreeRTOS task
//
// WHY THIS FILE EXISTS:
// Previously screenTick()/printHeartbeat() (continuous "scanning" screen)
// and drainAlertQueue() (event-triggered "detection alert" screen) called
// c5DisplayScanning()/c5DisplayDetection(), m5basicScanning()/
// m5basicDetection(), and m5stickcScanning()/m5stickcDetection() directly
// from loop() — the same task that drives WiFi channel hopping and BLE
// scanning. Any slow SPI/I2C redraw (or a blocking M5.Speaker.tone()/delay()
// chirp) stalled scanning for however long the draw/tone took.
//
// This header moves ALL display drawing, M5Unified button polling
// (M5.update()/M5.BtnX), Core2 vibration ticking, and M5.Speaker playback
// onto its own FreeRTOS task, pinned to the same core as loop() (Core 1) so
// the WiFi/BLE radio tasks on Core 0 are undisturbed. loop()/drainAlertQueue()
// now just publish cheap snapshots under a critical section; this task polls
// its own copies at ~20 Hz and does all the slow work outside any lock.
//
// Unlike eye-spy (one continuously-republished score screen), flock has TWO
// display entry points per board — a continuous "scanning" screen and a
// discrete, event-triggered "detection alert" screen — so this header
// tracks both: a continuously-overwritten UiScanSnapshot, and a
// generation-counted UiAlert mailbox that's only "fresh" once per publish.
//
// Public interface expected by main.cpp:
//   uiPublishScan(channel, modeName, detCount, spiffsOk)  — call every
//     loop() iteration in place of the old screenTick() board branches.
//   uiPublishAlert(method, mac, confidence, rssi, channel, ssid, detCount,
//                  lastSeenMs, dispType) — call from drainAlertQueue() in
//     place of the old c5DisplayDetection()/m5basicDetection()/
//     m5stickcDetection() calls.
//   uiForceC5Redraw()      — request an out-of-cycle C5 scanning redraw
//                             (used by the HAS_SIMPLE_BUTTON handler).
//   uiTakeButtonAction()   — call once per loop() iteration; returns
//                             0=none 1=save 3=hop (mirrors the old
//                             m5basicButtonTick()/m5stickcButtonTick()
//                             return codes for those two actions).
//   uiRequestAudio(which)  — called internally by newDetectChirp()/
//                             heartbeatBeep() (main.cpp) instead of touching
//                             M5.Speaker directly, only when USE_M5BASIC.
//   startUiTask()          — call once at the end of setup().
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ── Continuous "scanning status" snapshot ─────────────────────────────────
// Overwritten every loop() iteration by uiPublishScan(); the UI task reads
// its own copy each poll and redraws (each board's *Scanning() function is
// already internally self-throttled via a staleness check, so polling it
// at ~20 Hz here is cheap and matches the pre-split call frequency).
struct UiScanSnapshot {
    uint8_t channel;
    char    modeName[12];
    int     detCount;
    bool    spiffsOk;
};
static UiScanSnapshot g_uiScan    = { 1, {0}, 0, false };
static portMUX_TYPE   g_uiScanMux = portMUX_INITIALIZER_UNLOCKED;

static void uiPublishScan(uint8_t channel, const char* modeName, int detCount, bool spiffsOk) {
    portENTER_CRITICAL(&g_uiScanMux);
    g_uiScan.channel = channel;
    if (modeName) {
        strncpy(g_uiScan.modeName, modeName, sizeof(g_uiScan.modeName) - 1);
        g_uiScan.modeName[sizeof(g_uiScan.modeName) - 1] = '\0';
    }
    g_uiScan.detCount = detCount;
    g_uiScan.spiffsOk = spiffsOk;
    portEXIT_CRITICAL(&g_uiScanMux);
}

// One-shot out-of-cycle redraw request for the C5 board only (its scanning
// screen is otherwise only refreshed on the 30 s HEARTBEAT_MS gate inside
// this task — see uiTaskFn below). Used by the HAS_SIMPLE_BUTTON handler in
// loop() so a manual channel-hop press still gives instant visual feedback,
// same as the old direct c5DisplayScanning() call did.
static volatile bool g_uiForceC5Redraw = false;
static void uiForceC5Redraw() { g_uiForceC5Redraw = true; }

// ── Discrete "detection alert" mailbox ─────────────────────────────────────
// drainAlertQueue() already serializes alerts one at a time (it pops
// alertQueue under queueMux and processes each entry fully before the
// next), so a single-slot mailbox with a generation counter is sufficient —
// the UI task just needs to notice "the alert changed since I last looked".
struct UiAlert {
    char          method[16];
    char          mac[18];
    uint8_t       confidence;
    int8_t        rssi;
    uint8_t       channel;
    char          ssid[33];
    int           detCount;
    unsigned long lastSeenMs;
    char          dispType[6];   // "SSID" or "OUI" — c5DisplayDetection() only
};
static UiAlert            g_uiAlert    = { {0}, {0}, 0, 0, 0, {0}, 0, 0, {0} };
static volatile uint32_t  g_uiAlertSeq = 0;
static portMUX_TYPE       g_uiAlertMux = portMUX_INITIALIZER_UNLOCKED;

// ── Severity-priority display gate ────────────────────────────────────────
// WHY THIS EXISTS: drainAlertQueue() calls uiPublishAlert() for EVERY
// dequeued alert regardless of confidence (only the chirp/LED are gated on
// CHIRP_MIN_CONFIDENCE). Before this gate, uiTaskFn() below unconditionally
// redrew the alert screen — and each board's *Detection() unconditionally
// reset its own MB_ALERT_HOLD_MS/MSC_ALERT_HOLD_MS hold timer — on every
// freshAlert. That meant a low-confidence alert (e.g. ALERT_OUI_MFR,
// conf=20) arriving mid-hold-window would cut short and overwrite a
// high-confidence detection's (e.g. ALERT_OUI_ADDR2, conf=40) screen time.
// Symptoms reported: rapid flicker when several alerts land close together,
// and "most-recently-fired-wins" instead of "most-important-wins". This
// state tracks the confidence+MAC of whatever is CURRENTLY on screen so a
// new alert only gets to redraw if it's at least as important, the same
// target re-firing (allowed to refresh its own hold), or the hold window
// has fully elapsed. Alerts that lose this comparison are still logged/
// JSON'd/counted as before in drainAlertQueue() — only the on-screen draw
// is withheld.
static uint8_t             g_uiDisplayedConf   = 0;
static char                g_uiDisplayedMac[18] = {0};
static unsigned long       g_uiDisplayedAtMs   = 0;
// Shared across all boards so C5/M5Basic/StickC hold the screen for the
// same duration; MB_ALERT_HOLD_MS/MSC_ALERT_HOLD_MS (each board's own
// repaint-guard for its "scanning" screen) are set to match this value —
// they can't directly reference this constant since m5basic_display.h/
// m5stickc_display.h are #included by main.cpp before this file.
static const unsigned long UI_ALERT_HOLD_MS = 15000UL;

// Returns true if `a` is allowed to overwrite whatever is currently
// displayed (and, as a side effect, updates the tracked "currently
// displayed" state when it returns true — callers must actually draw `a`
// immediately after receiving true).
static bool uiAlertMaySupersede(const UiAlert& a, unsigned long now) {
    bool wins = (g_uiDisplayedConf == 0) ||
                (a.confidence >= g_uiDisplayedConf) ||
                (g_uiDisplayedMac[0] != '\0' && strcmp(a.mac, g_uiDisplayedMac) == 0) ||
                (now - g_uiDisplayedAtMs >= UI_ALERT_HOLD_MS);
    if (wins) {
        g_uiDisplayedConf = a.confidence;
        strncpy(g_uiDisplayedMac, a.mac, sizeof(g_uiDisplayedMac) - 1);
        g_uiDisplayedMac[sizeof(g_uiDisplayedMac) - 1] = '\0';
        g_uiDisplayedAtMs = now;
    }
    return wins;
}

static void uiPublishAlert(const char* method, const char* mac, uint8_t confidence,
                            int8_t rssi, uint8_t channel, const char* ssid,
                            int detCount, unsigned long lastSeenMs,
                            const char* dispType) {
    portENTER_CRITICAL(&g_uiAlertMux);
    if (method) {
        strncpy(g_uiAlert.method, method, sizeof(g_uiAlert.method) - 1);
        g_uiAlert.method[sizeof(g_uiAlert.method) - 1] = '\0';
    }
    if (mac) {
        strncpy(g_uiAlert.mac, mac, sizeof(g_uiAlert.mac) - 1);
        g_uiAlert.mac[sizeof(g_uiAlert.mac) - 1] = '\0';
    }
    g_uiAlert.confidence = confidence;
    g_uiAlert.rssi       = rssi;
    g_uiAlert.channel    = channel;
    ssid = ssid ? ssid : "";
    strncpy(g_uiAlert.ssid, ssid, sizeof(g_uiAlert.ssid) - 1);
    g_uiAlert.ssid[sizeof(g_uiAlert.ssid) - 1] = '\0';
    g_uiAlert.detCount   = detCount;
    g_uiAlert.lastSeenMs = lastSeenMs;
    if (dispType) {
        strncpy(g_uiAlert.dispType, dispType, sizeof(g_uiAlert.dispType) - 1);
        g_uiAlert.dispType[sizeof(g_uiAlert.dispType) - 1] = '\0';
    }
    g_uiAlertSeq++;
    portEXIT_CRITICAL(&g_uiAlertMux);
}

// ── Button-action feedback (UI task -> loop()) ─────────────────────────────
// M5.update()/M5.BtnX are only ever called from the UI task now (see
// uiTaskFn). loop() consumes whichever action (if any) got recorded since
// its last check. Codes match the old m5basicButtonTick()/
// m5stickcButtonTick() return values for the two actions loop() must act
// on: 1 = Btn A (save session), 3 = Btn C/B (force channel hop). M5Basic's
// brightness cycle (code 2) is fully handled inside the button-tick call
// itself and needs no feedback here.
static volatile uint8_t g_uiButtonAction = 0;
static portMUX_TYPE     g_uiBtnMux       = portMUX_INITIALIZER_UNLOCKED;

static uint8_t uiTakeButtonAction() {
    uint8_t a;
    portENTER_CRITICAL(&g_uiBtnMux);
    a = g_uiButtonAction;
    g_uiButtonAction = 0;
    portEXIT_CRITICAL(&g_uiBtnMux);
    return a;
}
static void uiSetButtonAction(uint8_t a) {
    portENTER_CRITICAL(&g_uiBtnMux);
    if (g_uiButtonAction == 0) g_uiButtonAction = a;
    portEXIT_CRITICAL(&g_uiBtnMux);
}

// ── Audio-request feedback (loop()/drainAlertQueue() -> UI task) ──────────
// M5Basic/Core2-For-AWS's M5.Speaker is part of the same M5Unified singleton
// as the display, which the UI task now exclusively owns. newDetectChirp()/
// heartbeatBeep() (main.cpp) hand off here instead of calling M5.Speaker
// directly when USE_M5BASIC is defined; every other board's audio path
// (plain tone()/noTone() buzzer, or M5.Speaker on a display-less Atom
// Voice/VoiceS3R build) never touches a display object and is left calling
// M5.Speaker/tone() directly from the scan/main task exactly as before.
static volatile uint8_t g_uiAudioReq = 0;   // 0=none 1=newDetectChirp 2=heartbeatBeep
static portMUX_TYPE     g_uiAudioMux = portMUX_INITIALIZER_UNLOCKED;

static void uiRequestAudio(uint8_t which) {
    portENTER_CRITICAL(&g_uiAudioMux);
    if (g_uiAudioReq == 0) g_uiAudioReq = which;
    portEXIT_CRITICAL(&g_uiAudioMux);
}
static uint8_t uiTakeAudioRequest() {
    uint8_t v;
    portENTER_CRITICAL(&g_uiAudioMux);
    v = g_uiAudioReq;
    g_uiAudioReq = 0;
    portEXIT_CRITICAL(&g_uiAudioMux);
    return v;
}

#if defined(USE_M5BASIC)
// Identical tone/delay/tone sequences to the ones newDetectChirp()/
// heartbeatBeep() used to run inline on the scan task — now played on the
// UI task instead, so the delay()s here never stall WiFi/BLE scanning.
static void uiPlayChirp() {
    M5.Speaker.tone(NEW_CHIRP_LO_HZ, NEW_CHIRP_NOTE_MS);
    delay(NEW_CHIRP_NOTE_MS + NEW_CHIRP_GAP_MS);
    M5.Speaker.tone(NEW_CHIRP_HI_HZ, NEW_CHIRP_NOTE_MS);
    delay(NEW_CHIRP_NOTE_MS);
    M5.Speaker.stop();
}
static void uiPlayHeartbeatBeep() {
    M5.Speaker.tone(HB_BEEP_HZ, HB_BEEP_NOTE_MS);
    delay(HB_BEEP_NOTE_MS + HB_BEEP_GAP_MS);
    M5.Speaker.tone(HB_BEEP_HZ, HB_BEEP_NOTE_MS);
    delay(HB_BEEP_NOTE_MS);
    M5.Speaker.stop();
}
#endif

// ── UI task ─────────────────────────────────────────────────────────────
static TaskHandle_t g_uiTaskHandle = nullptr;

static void uiTaskFn(void* pv) {
    (void)pv;
    uint32_t      consumedAlertSeq  = 0;
    unsigned long lastC5HeartbeatMs = 0;
    const TickType_t period = pdMS_TO_TICKS(50);

    for (;;) {
        UiScanSnapshot scan;
        portENTER_CRITICAL(&g_uiScanMux);
        scan = g_uiScan;
        portEXIT_CRITICAL(&g_uiScanMux);

        UiAlert  alert;
        uint32_t seq;
        portENTER_CRITICAL(&g_uiAlertMux);
        alert = g_uiAlert;
        seq   = g_uiAlertSeq;
        portEXIT_CRITICAL(&g_uiAlertMux);
        bool freshAlert = (seq != consumedAlertSeq);
        if (freshAlert) consumedAlertSeq = seq;

        unsigned long now = millis();

        // Only a freshAlert that also wins the severity/MAC/hold-window
        // comparison actually gets drawn — see uiAlertMaySupersede() above
        // for why. uiAlertMaySupersede() has the side effect of updating
        // g_uiDisplayedConf/Mac/AtMs when it returns true, so it must only
        // be evaluated once per fresh alert (not once per board's #if
        // block below).
        bool alertWins = freshAlert && uiAlertMaySupersede(alert, now);

        // Whether the hold window from the currently-displayed alert is
        // still active right now — used below to stop C5's HEARTBEAT_MS
        // scanning-screen repaint (its only self-throttle; C5 has no
        // per-board hold timer of its own) from stomping a still-protected
        // alert screen. M5Basic/StickC don't need this check here because
        // their own *Scanning() functions already self-guard via
        // MB_ALERT_HOLD_MS/MSC_ALERT_HOLD_MS (kept in sync with
        // UI_ALERT_HOLD_MS — see m5basic_display.h/m5stickc_display.h).
        bool holdActive = (g_uiDisplayedConf != 0) &&
                           (now - g_uiDisplayedAtMs < UI_ALERT_HOLD_MS);

#if defined(USE_C5_DISPLAY) && USE_C5_DISPLAY
        if (alertWins) {
            c5DisplayDetection(alert.dispType, alert.mac, alert.confidence,
                                alert.rssi, alert.channel);
        }
        if (g_uiForceC5Redraw) {
            g_uiForceC5Redraw = false;
            lastC5HeartbeatMs = 0;   // force the gate below to fire this tick
        }
        if (!holdActive && now - lastC5HeartbeatMs >= HEARTBEAT_MS) {
            c5DisplayScanning(scan.channel, scan.detCount);
            lastC5HeartbeatMs = now;
        }
#endif

#if defined(USE_M5BASIC)
        if (alertWins) {
            m5basicDetection(alert.method, alert.mac, alert.confidence, alert.rssi,
                              alert.channel, alert.ssid, alert.detCount, alert.lastSeenMs);
        }
        m5basicScanning(scan.channel, scan.modeName, scan.detCount, now,
                        scan.spiffsOk, (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
        {
            int btn = m5basicButtonTick();
            if (btn == 1 || btn == 3) uiSetButtonAction((uint8_t)btn);
        }
#if defined(USE_M5CORE2_AWS)
        m5basicVibrationTick();
#endif
        {
            uint8_t req = uiTakeAudioRequest();
            if (req == 1)      uiPlayChirp();
            else if (req == 2) uiPlayHeartbeatBeep();
        }
#endif

#if defined(USE_M5STICKC_PLUS_SE)
        if (alertWins) {
            m5stickcDetection(alert.method, alert.mac, alert.confidence, alert.rssi,
                               alert.channel, alert.ssid, alert.detCount, alert.lastSeenMs);
        }
        m5stickcScanning(scan.channel, scan.modeName, scan.detCount, now,
                         scan.spiffsOk, (int)FY_OUI_HIGH_COUNT, (int)FY_OUI_MFR_COUNT);
        {
            int btn = m5stickcButtonTick();
            if (btn == 1 || btn == 3) uiSetButtonAction((uint8_t)btn);
        }
#endif

        vTaskDelay(period);
    }
}

// No-op on boards with no display at all (Atom Lite/Echo/Voice/VoiceS3R,
// plain esp32dev) — uiTaskFn is still compiled in (uiPublishScan()/
// uiPublishAlert() are called unconditionally from main.cpp for simplicity)
// but this guard keeps an idle polling task from ever being spawned on
// hardware that has nothing for it to draw.
static void startUiTask() {
#if (defined(USE_C5_DISPLAY) && USE_C5_DISPLAY) || defined(USE_M5BASIC) || defined(USE_M5STICKC_PLUS_SE)
    xTaskCreatePinnedToCore(uiTaskFn, "flock_ui", 4096, nullptr, 1, &g_uiTaskHandle, 1);
#endif
}
