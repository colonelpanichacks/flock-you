// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 SimeonOnSecurity <https://github.com/simeononsecurity>
//
// beacon_test.cpp — "Flock beacon tester" / detection-test firmware.
//
// WHAT THIS IS:
// A standalone firmware (separate PlatformIO environment, NOT the real
// detector) for the M5Atom Lite that continuously broadcasts a rotating
// series of fake WiFi/BLE signals that are byte-for-byte crafted to match
// every single detection path in main.cpp's wifiSniffer()/BLE-scan logic
// (see fy_detect.h / fy_confidence.h for the real matching rules — this
// file is intentionally written against those exact same rules so it never
// drifts out of sync with what the real detector considers a hit).
//
// WHY: so you can leave a real flock-you-esp32 detector running near this
// board and get an immediate, repeatable, on-demand confirmation that the
// detector (firmware + hardware + antenna) is actually alerting correctly —
// without needing to physically find a real Flock camera to test against.
//
// HOW TO USE:
//   1. Flash this to a SEPARATE M5Atom Lite:
//        pio run -e m5atom-lite-beacon -t upload
//   2. Power it on near your real detector (running the normal firmware).
//   3. It cycles through 12 scenarios (one per detection path) automatically
//      every SCENARIO_INTERVAL_MS, in a shuffled order that guarantees full
//      coverage every pass. Press the button (GPIO39) to force-fire the
//      next scenario immediately instead of waiting.
//   4. Watch the real detector's serial log / LED / chirp for each hit.
//      Serial output here logs exactly which scenario just fired.
//
// SCENARIOS (1:1 with main.cpp's AlertType enum + BLE paths):
//   0  ALERT_OUI_ADDR2        — probe response, addr2 = random high-tier OUI
//   1  ALERT_WILDCARD_PROBE   — wildcard probe req, addr2 = random high-tier OUI
//   2  ALERT_OUI_ADDR1        — probe response, addr1(dest) = random high-tier OUI
//   3  ALERT_OUI_ADDR3        — probe response, addr3(BSSID) = random high-tier OUI
//   4  ALERT_SSID             — probe response, GA addr2, SSID contains "flock"
//   5  ALERT_LAA_SSID         — beacon, LAA addr2, SSID == "Flock Camera net."
//   6  ALERT_OUI_MFR          — probe response, addr2 = random contract-mfr OUI
//   7  ALERT_SOUNDTHINKING    — probe response, addr2 = SoundThinking OUI
//   8  seq-MAC-pair bonus     — two wildcard probes, addr2 = 82:6b:f2:xx:yy:{DE,DF}
//                               (this is the one high-tier OUI that is ALSO LAA —
//                               required so checkSeqMac()'s LAA-only gate passes)
//   9  BLE mfr-ID             — adv mfr data = 0x09C8 (XUNTONG / Flock BLE ID)
//   10 BLE Raven UUID         — adv service UUID = random pick from fy_raven_uuids[]
//   11 BLE device name        — adv name = random pick from fy_ble_names[]
//
// WiFi scenarios sweep channels {1,6,11} (several bursts each) so they
// reliably overlap the real detector's 250 ms-dwell hop cycle regardless of
// phase — the same channel-overlap fix applied to the real firmware
// (see DETECTION_IMPROVEMENTS.md).

#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <NimBLEDevice.h>

// ---- Board config: M5Atom Lite (NeoPixel LED GPIO27, button GPIO39) ------
#define USE_M5ATOM_LITE 1
#define LED_PIN     27
#define NUM_LEDS    1
#define BUTTON_PIN  39
#include "led_neopixel.h"   // ledSet(bool) / ledMatrixBootSequence()

// Pattern data shared with the real detector — keeps this tool automatically
// in sync with fy_detect.h if the OUI/BLE tables are ever updated.
#include "fy_detect.h"
#include "beacon_frames.h"

// ============================================================
// CONFIG
// ============================================================

#define SCENARIO_INTERVAL_MS   4000UL   // time between automatic scenario fires
#define SWEEP_CHANNELS_COUNT   3
static const uint8_t sweepChannels[SWEEP_CHANNELS_COUNT] = {1, 6, 11};
#define BURST_FRAMES_PER_CH    4        // frames sent per channel per pass
#define SWEEP_PASSES           2        // repeat the {1,6,11} sweep this many times
#define BURST_GAP_MS           8        // gap between individual frame sends

#define NUM_SCENARIOS 12

static const uint8_t BROADCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t GA_DUMMY_MAC[6]  = {0x00,0x11,0x22,0xAA,0xBB,0xCC}; // GA, no OUI table match
static const uint8_t GA_DUMMY_MAC2[6] = {0x00,0x11,0x22,0xAA,0xBB,0xCD}; // 2nd distinct filler

// ============================================================
// STATE
// ============================================================

static NimBLEAdvertising* g_pAdv = nullptr;
static uint8_t   scenarioOrder[NUM_SCENARIOS];
static uint8_t   scenarioPos = NUM_SCENARIOS; // force reshuffle on first loop
static unsigned long lastFireAt = 0;
static unsigned long ledOffAt = 0;

static bool          btnLastState    = true;
static unsigned long btnLastChangeMs = 0;
#define BTN_DEBOUNCE_MS 50

// ============================================================
// SMALL HELPERS
// ============================================================

static void ledPulse(unsigned ms) {
  ledSet(true);
  ledOffAt = millis() + ms;
}

static void ledTick() {
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(false);
    ledOffAt = 0;
  }
}

static bool buttonPressed() {
  bool cur = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  if (cur != btnLastState && (now - btnLastChangeMs) > BTN_DEBOUNCE_MS) {
    btnLastChangeMs = now;
    btnLastState     = cur;
    if (!cur) return true;
  }
  return false;
}

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Parses an "xx:xx:xx" OUI string into 3 bytes.
static void parseOui(const char* oui, uint8_t* out3) {
  out3[0] = (uint8_t)strtol(oui,     nullptr, 16);
  out3[1] = (uint8_t)strtol(oui + 3, nullptr, 16);
  out3[2] = (uint8_t)strtol(oui + 6, nullptr, 16);
}

// Builds a 6-byte MAC = <3-byte OUI prefix> + 3 random suffix bytes.
static void randomMacWithOui(const char* oui, uint8_t* mac6) {
  parseOui(oui, mac6);
  mac6[3] = (uint8_t)random(0, 256);
  mac6[4] = (uint8_t)random(0, 256);
  mac6[5] = (uint8_t)random(0, 256);
}

// Picks a random entry from a string OUI array whose first byte is
// globally-administered (bit1 clear) — needed for addr1/addr3 scenarios,
// since matchOuiRaw() (used for those checks) rejects LAA-bit MACs outright
// (fy_confidence.h: matchOuiRaw() has `if (mac[0]&0x02) return false;`).
// fy_oui_high[] has exactly one LAA entry (82:6b:f2); this retries past it.
static const char* pickRandomOuiGA(const char* const* arr, size_t count) {
  for (int tries = 0; tries < 20; tries++) {
    const char* pick = arr[random(0, (long)count)];
    uint8_t b0 = (uint8_t)strtol(pick, nullptr, 16);
    if (!(b0 & 0x02)) return pick;
  }
  return arr[0]; // fallback, should not normally happen
}

// Shuffles 0..NUM_SCENARIOS-1 (Fisher-Yates) into scenarioOrder[].
static void reshuffleScenarios() {
  for (uint8_t i = 0; i < NUM_SCENARIOS; i++) scenarioOrder[i] = i;
  for (int i = NUM_SCENARIOS - 1; i > 0; i--) {
    int j = random(0, i + 1);
    uint8_t t = scenarioOrder[i];
    scenarioOrder[i] = scenarioOrder[j];
    scenarioOrder[j] = t;
  }
  scenarioPos = 0;
}

// ============================================================
// WIFI TRANSMISSION  (channel-sweep for reliable overlap with the real
// detector's 250 ms-dwell hop cycle across {11,6,1})
// ============================================================

static void txSweep(uint8_t* frame, size_t len) {
  for (int pass = 0; pass < SWEEP_PASSES; pass++) {
    for (int c = 0; c < SWEEP_CHANNELS_COUNT; c++) {
      esp_wifi_set_channel(sweepChannels[c], WIFI_SECOND_CHAN_NONE);
      for (int b = 0; b < BURST_FRAMES_PER_CH; b++) {
        esp_wifi_80211_tx(WIFI_IF_STA, frame, (int)len, false);
        delay(BURST_GAP_MS);
      }
    }
  }
}

// ============================================================
// BLE ADVERTISING  (fresh NimBLEAdvertisementData object each call — avoids
// stale-field accumulation across scenario rotations, since each set*()
// call on that object appends directly to its internal payload buffer)
// ============================================================

static void bleAdvertiseAndHold(NimBLEAdvertisementData& data, uint32_t holdMs) {
  g_pAdv->stop();
  g_pAdv->setAdvertisementData(data);
  g_pAdv->start();
  delay(holdMs);
  g_pAdv->stop();
}

// ============================================================
// SCENARIO IMPLEMENTATIONS
// ============================================================

static void scenario0_OuiAddr2() {
  uint8_t mac[6]; randomMacWithOui(fy_oui_high[random(0, (long)FY_OUI_HIGH_COUNT)], mac);
  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildBeaconLike(buf, BF_FC_PROBE_RESP, BROADCAST_MAC, mac,
                                  GA_DUMMY_MAC2, "TestNet-OUI2", sweepChannels[0]);
  txSweep(buf, len);
  char s[18]; macToStr(mac, s, sizeof(s));
  Serial.printf("[beacon] 0 ALERT_OUI_ADDR2       addr2=%s\n", s);
}

static void scenario1_WildcardProbe() {
  uint8_t mac[6]; randomMacWithOui(fy_oui_high[random(0, (long)FY_OUI_HIGH_COUNT)], mac);
  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildProbeRequest(buf, BROADCAST_MAC, mac, BROADCAST_MAC, nullptr);
  txSweep(buf, len);
  char s[18]; macToStr(mac, s, sizeof(s));
  Serial.printf("[beacon] 1 ALERT_WILDCARD_PROBE  addr2=%s\n", s);
}

static void scenario2_OuiAddr1() {
  uint8_t mac[6]; randomMacWithOui(pickRandomOuiGA(fy_oui_high, FY_OUI_HIGH_COUNT), mac);
  uint8_t buf[BF_MAX_FRAME];
  // addr1 (dest) = the OUI-matching MAC; addr2/addr3 = non-matching fillers.
  size_t len = bfBuildBeaconLike(buf, BF_FC_PROBE_RESP, mac, GA_DUMMY_MAC,
                                  GA_DUMMY_MAC2, "TestNet-OUI1", sweepChannels[0]);
  txSweep(buf, len);
  char s[18]; macToStr(mac, s, sizeof(s));
  Serial.printf("[beacon] 2 ALERT_OUI_ADDR1       addr1=%s\n", s);
}

static void scenario3_OuiAddr3() {
  uint8_t mac[6]; randomMacWithOui(pickRandomOuiGA(fy_oui_high, FY_OUI_HIGH_COUNT), mac);
  uint8_t buf[BF_MAX_FRAME];
  // addr3 (BSSID) = the OUI-matching MAC; addr1/addr2 = non-matching fillers.
  size_t len = bfBuildBeaconLike(buf, BF_FC_PROBE_RESP, BROADCAST_MAC, GA_DUMMY_MAC,
                                  mac, "TestNet-OUI3", sweepChannels[0]);
  txSweep(buf, len);
  char s[18]; macToStr(mac, s, sizeof(s));
  Serial.printf("[beacon] 3 ALERT_OUI_ADDR3       addr3=%s\n", s);
}

static void scenario4_Ssid() {
  // GA addr2 (bit1 clear) that does NOT match any OUI table + SSID keyword hit.
  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildBeaconLike(buf, BF_FC_PROBE_RESP, BROADCAST_MAC, GA_DUMMY_MAC,
                                  GA_DUMMY_MAC, "Flock-TEST-GA", sweepChannels[0]);
  txSweep(buf, len);
  Serial.println("[beacon] 4 ALERT_SSID            ssid=\"Flock-TEST-GA\"");
}

static void scenario5_LaaSsid() {
  // LAA addr2 (bit1 set) + exact "Flock Camera net." SSID -> ALERT_LAA_SSID.
  uint8_t mac[6] = {0x02, 0x11, 0x22, 0xAA, 0xBB, 0xCC};
  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildBeaconLike(buf, BF_FC_BEACON, BROADCAST_MAC, mac, mac,
                                  "Flock Camera net.", sweepChannels[0]);
  txSweep(buf, len);
  Serial.println("[beacon] 5 ALERT_LAA_SSID        ssid=\"Flock Camera net.\"");
}

static void scenario6_OuiMfr() {
  uint8_t mac[6]; randomMacWithOui(fy_oui_mfr[random(0, (long)FY_OUI_MFR_COUNT)], mac);
  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildBeaconLike(buf, BF_FC_PROBE_RESP, BROADCAST_MAC, mac,
                                  GA_DUMMY_MAC, "TestNet-MFR", sweepChannels[0]);
  txSweep(buf, len);
  char s[18]; macToStr(mac, s, sizeof(s));
  Serial.printf("[beacon] 6 ALERT_OUI_MFR         addr2=%s\n", s);
}

static void scenario7_SoundThinking() {
  uint8_t mac[6]; randomMacWithOui(fy_oui_soundthinking[0], mac);
  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildBeaconLike(buf, BF_FC_PROBE_RESP, BROADCAST_MAC, mac,
                                  GA_DUMMY_MAC, "TestNet-ST", sweepChannels[0]);
  txSweep(buf, len);
  char s[18]; macToStr(mac, s, sizeof(s));
  Serial.printf("[beacon] 7 ALERT_SOUNDTHINKING   addr2=%s\n", s);
}

static void scenario8_SeqMacPair() {
  // 82:6b:f2 is the one fy_oui_high[] entry that is BOTH a confirmed
  // high-tier Flock OUI AND locally-administered (0x82 & 0x02 != 0) — the
  // only combination that can pass both matchFlockHighOui() (OUI-gate for
  // ALERT_WILDCARD_PROBE) and checkSeqMac()'s LAA-only requirement.
  uint8_t prefix[5] = {0x82, 0x6b, 0xf2,
                        (uint8_t)random(0, 256), (uint8_t)random(0, 256)};
  uint8_t macA[6]; memcpy(macA, prefix, 5); macA[5] = 0xDE;
  uint8_t macB[6]; memcpy(macB, prefix, 5); macB[5] = 0xDF;

  uint8_t buf[BF_MAX_FRAME];
  size_t len = bfBuildProbeRequest(buf, BROADCAST_MAC, macA, BROADCAST_MAC, nullptr);
  txSweep(buf, len);
  delay(600);   // let the real detector's channel hop advance between the pair
  len = bfBuildProbeRequest(buf, BROADCAST_MAC, macB, BROADCAST_MAC, nullptr);
  txSweep(buf, len);

  char sa[18], sb[18]; macToStr(macA, sa, sizeof(sa)); macToStr(macB, sb, sizeof(sb));
  Serial.printf("[beacon] 8 SEQ_MAC_PAIR_BONUS    %s -> %s\n", sa, sb);
}

static void scenario9_BleMfrId() {
  uint16_t id = fy_ble_mfr_ids[0];   // 0x09C8
  uint8_t mfr[3] = { (uint8_t)(id & 0xFF), (uint8_t)(id >> 8), 0x01 }; // LE + 1 filler byte
  NimBLEAdvertisementData data;
  data.setManufacturerData(std::string((char*)mfr, sizeof(mfr)));
  data.setName("FYTest");
  bleAdvertiseAndHold(data, 1200);
  Serial.printf("[beacon] 9 BLE_MFR_ID            id=0x%04X\n", id);
}

static void scenario10_BleRavenUuid() {
  const char* uuid = fy_raven_uuids[random(0, (long)FY_RAVEN_UUID_COUNT)];
  NimBLEAdvertisementData data;
  data.setCompleteServices(NimBLEUUID(uuid));
  data.setName("FYTest");
  bleAdvertiseAndHold(data, 1200);
  Serial.printf("[beacon] 10 BLE_RAVEN_UUID        uuid=%s\n", uuid);
}

static void scenario11_BleName() {
  int n = 0;
  while (fy_ble_names[n]) n++;   // count non-null entries
  const char* name = fy_ble_names[random(0, n)];
  NimBLEAdvertisementData data;
  data.setName(name);
  bleAdvertiseAndHold(data, 1200);
  Serial.printf("[beacon] 11 BLE_NAME              name=%s\n", name);
}

typedef void (*ScenarioFn)();
static const ScenarioFn scenarios[NUM_SCENARIOS] = {
  scenario0_OuiAddr2, scenario1_WildcardProbe, scenario2_OuiAddr1,
  scenario3_OuiAddr3, scenario4_Ssid,          scenario5_LaaSsid,
  scenario6_OuiMfr,   scenario7_SoundThinking, scenario8_SeqMacPair,
  scenario9_BleMfrId, scenario10_BleRavenUuid, scenario11_BleName,
};

static void fireNextScenario() {
  if (scenarioPos >= NUM_SCENARIOS) reshuffleScenarios();
  uint8_t idx = scenarioOrder[scenarioPos++];
  ledPulse(150);
  scenarios[idx]();
  lastFireAt = millis();
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[beacon] flock-you-esp32 beacon/detection tester starting...");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  btnLastState = digitalRead(BUTTON_PIN);

  ledMatrixBootSequence();
  randomSeed((uint32_t)esp_random());

  // WiFi.mode(WIFI_STA) brings up esp_wifi_init()/esp_wifi_start() for us —
  // all we need for esp_wifi_80211_tx() raw injection; no promiscuous mode,
  // no association attempt needed.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(sweepChannels[0], WIFI_SECOND_CHAN_NONE);

  NimBLEDevice::init("FYTest");
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  g_pAdv = NimBLEDevice::getAdvertising();

  reshuffleScenarios();
  lastFireAt = millis();

  Serial.printf("[beacon] ready — %d scenarios, auto-fire every %lu ms, "
                "button (GPIO%d) = force-fire now\n",
                NUM_SCENARIOS, (unsigned long)SCENARIO_INTERVAL_MS, BUTTON_PIN);
}

void loop() {
  ledTick();

  bool force = buttonPressed();
  if (force || (millis() - lastFireAt >= SCENARIO_INTERVAL_MS)) {
    fireNextScenario();
  }

  delay(5);
}
