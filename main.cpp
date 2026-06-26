#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include <ctype.h>
#include <string.h>
#include <SPIFFS.h>
#include "display_dongle.h"

// ============================================================
// CONFIG  (board defaults; override via platformio build_flags)
// ============================================================

#ifdef BOARD_LILYGO_T_DONGLE_S3
// LilyGO T-Dongle S3: ST7735 display + APA102 RGB (no buzzer).
#define USE_BUZZER         0
#define USE_LED            1
#define USE_APA102_LED     1
#define APA102_DATA_PIN    40
#define APA102_CLK_PIN     39
#define APA102_FLASH_R     255
#define APA102_FLASH_G     0
#define APA102_FLASH_B     0
#define MIRROR_SERIAL      0   // GPIO43 is UART TX on this board
#else
// Seeed XIAO ESP32-S3
#define BUZZER_PIN         3
#define USE_BUZZER         1
#define LED_PIN            21
#define USE_LED            1
#define LED_ACTIVE_HIGH    0
#define MIRROR_SERIAL      1
#define MIRROR_TX_PIN      43
#endif

#define LED_FLASH_MS       120
#define MIRROR_BAUD        115200

#define CHANNEL_MODE_FULL_HOP   0
#define CHANNEL_MODE_CUSTOM     1
#define CHANNEL_MODE_SINGLE     2

#define CHANNEL_MODE CHANNEL_MODE_CUSTOM
#define CHANNEL_DWELL_MS 250  // Changed to 2 x 125ms to aid in faster detection.  125ms is the observed hop time of the cameras (credit to nsm_barri for the observation).
#define SINGLE_CHANNEL 1
// Channel order reversed to aid in faster detection.  Credit to nsm_barri for the observation on the ascending hop order of the cameras.
static const uint8_t customChannels[]  = {11, 6, 1};
static const size_t  customChannelCount = sizeof(customChannels) / sizeof(customChannels[0]);

static const uint8_t fullHopChannels[] = {11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
static const size_t  fullHopChannelCount = sizeof(fullHopChannels) / sizeof(fullHopChannels[0]);

#define HEARTBEAT_MS    30000
#define RSSI_MIN        -95
#define ALERT_COOLDOWN_MS 5000

// Audio cadence: two fast ascending beeps on a NEW MAC, then while any
// target is still in range (seen within HB_DEVICE_ACTIVE_MS), two monotone
// heartbeat beeps every HB_BEEP_INTERVAL_MS.
#define HB_DEVICE_ACTIVE_MS    3000
#define HB_BEEP_INTERVAL_MS    10000
// A MAC we haven't heard from in REDISCOVER_MS counts as a fresh discovery
// next time it shows up — fires the ascending chirp again. Shorter than a
// Flock's burst-sleep gap would mean false chirps; longer means you'd miss
// a drive-away/return. 30 s is a good middle ground.
#define REDISCOVER_MS          30000
#define NEW_CHIRP_LO_HZ        2000
#define NEW_CHIRP_HI_HZ        2800
#define NEW_CHIRP_NOTE_MS      55
#define NEW_CHIRP_GAP_MS       25
#define HB_BEEP_HZ             1500
#define HB_BEEP_NOTE_MS        70
#define HB_BEEP_GAP_MS         70

#define ENABLE_SSID_MATCH 0
#define CHECK_ADDR1 0   // disabled — see wifiSniffer() comment block
#define CHECK_ADDR3 0   // disabled — see wifiSniffer() comment block
static const char* target_ssid_keywords[] = { "flock" };
static const size_t SSID_KEYWORD_COUNT = sizeof(target_ssid_keywords) / sizeof(target_ssid_keywords[0]);

#define STOP_ON_SSID_HIT 0
#define STOP_ON_OUI_HIT  0
#define PROCESS_MGMT_FRAMES 1
#define PROCESS_DATA_FRAMES 1

// Persistence
#define MAX_DETECTIONS       200
#define FY_SESSION_FILE      "/session.json"
#define FY_SESSION_TMP       "/session.tmp"
#define FY_PREV_FILE         "/prev_session.json"
#define AUTOSAVE_INTERVAL_MS 60000

// ============================================================
// TARGET OUI LIST  (all lowercase, colons only)
// ============================================================

static const char* target_ouis[] = {
  "70:c9:4e", "3c:91:80", "d8:f3:bc", "80:30:49", "b8:35:32",
  "14:5a:fc", "74:4c:a1", "08:3a:88", "9c:2f:9d", "c0:35:32",
  "94:08:53", "e4:aa:ea", "f4:6a:dd", "f8:a2:d6", "24:b2:b9",
  "00:f4:8d", "d0:39:57", "e8:d0:fc", "e0:4f:43", "b8:1e:a4",
  "70:08:94", "58:8e:81", "ec:1b:bd", "3c:71:bf", "58:00:e3",
  "90:35:ea", "5c:93:a2", "64:6e:69", "48:27:ea", "a4:cf:12"


};
static const size_t OUI_COUNT = sizeof(target_ouis) / sizeof(target_ouis[0]);

// Pre-compiled byte table — populated once in setup(), never touched again.
// Keeps matchOuiRaw entirely in IRAM with no flash-resident function calls.
static uint8_t oui_bytes[OUI_COUNT][3];

// ============================================================
// ALERT QUEUE  (callback → loop, avoids Serial in WiFi task)
// ============================================================

#define ALERT_QUEUE_SIZE 32

typedef enum : uint8_t {
  ALERT_OUI_ADDR2       = 0,
  ALERT_OUI_ADDR1       = 1,
  ALERT_OUI_ADDR3       = 2,
  ALERT_SSID            = 3,
  // Wildcard probe + OUI + primary IE signature (wifi_wildcard_probe_ie_sig).
  // wifi_wildcard_probe (OUI + wildcard only) was removed — superseded by this
  // path: same wildcard/OUI gates plus IE-field verification.
  ALERT_WILDCARD_PROBE_IE_SIG = 4,
} AlertType;

typedef struct {
  AlertType type;
  uint8_t   mac[6];
  int8_t    rssi;
  uint8_t   channel;
  char      ssid[33];     // populated for SSID hits
  char      frameKind[12];
} AlertEntry;

static volatile AlertEntry alertQueue[ALERT_QUEUE_SIZE];
static volatile size_t alertHead = 0;  // written by callback
static volatile size_t alertTail = 0;  // read by loop()
static portMUX_TYPE    queueMux  = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR enqueueAlert(AlertType type, const uint8_t* mac, int8_t rssi,
                                    uint8_t ch, const char* ssid, const char* kind) {
  portENTER_CRITICAL_ISR(&queueMux);
  size_t next = (alertHead + 1) % ALERT_QUEUE_SIZE;
  if (next == alertTail) {                         // drop if full — loop() is behind
    portEXIT_CRITICAL_ISR(&queueMux);
    return;
  }

  AlertEntry* e = (AlertEntry*)&alertQueue[alertHead];
  e->type    = type;
  e->rssi    = rssi;
  e->channel = ch;
  memcpy((void*)e->mac, mac, 6);

  if (ssid)  { strncpy((char*)e->ssid,      ssid, 32); ((char*)e->ssid)[32] = '\0'; }
  else        { ((char*)e->ssid)[0] = '\0'; }

  if (kind)  { strncpy((char*)e->frameKind, kind, 11); ((char*)e->frameKind)[11] = '\0'; }
  else        { ((char*)e->frameKind)[0] = '\0'; }

  alertHead = next;
  portEXIT_CRITICAL_ISR(&queueMux);
}

// ============================================================
// DETECTION TABLE  (on-device storage, persisted to SPIFFS)
// ============================================================
//
// Single-threaded: only touched from loop() — drainAlertQueue() adds, and
// fySaveSession() reads. No mutex needed. The WiFi-task callback never
// touches this table; it only writes to the lock-free alert ring buffer.

typedef struct {
  char     mac[18];
  char     method[24];     // alertTypeToMethod strings (incl. wildcard_probe_ie_sig)
  int8_t   rssi;
  uint8_t  channel;
  uint32_t firstSeen;      // millis() at first hit
  uint32_t lastSeen;       // millis() at latest hit
  uint16_t count;
  char     ssid[33];       // "" unless an SSID hit populated it
} FYDetection;

static FYDetection fyDet[MAX_DETECTIONS];
static int           fyDetCount       = 0;
static bool          fySpiffsReady    = false;
static bool          fyDirty          = false;
static unsigned long fyLastSaveAt     = 0;
static int           fyLastSaveCount  = 0;

// ============================================================
// STATE
// ============================================================

static uint8_t  currentChannel = 1;
static size_t   customChannelIndex = 0;
static size_t   fullHopIndex = 0;
static unsigned long lastHop = 0;
static unsigned long lastHeartbeat = 0;
static volatile bool sniffingStopped = false;

// Dedupe table (small circular, avoids single-slot eviction bug).
// This is the *serial-rate-limit* dedup — it suppresses beep + emit within
// ALERT_COOLDOWN_MS of a prior hit on the same MAC. The detection table
// (above) still counts every hit regardless of this suppression.
#define DEDUPE_SLOTS 8
static struct {
  char mac[18];
  unsigned long ts;
} dedupeTable[DEDUPE_SLOTS];
static size_t dedupeIdx = 0;

// LED one-shot pulse timer
static volatile unsigned long ledOffAt = 0;

#if USE_LED && defined(USE_APA102_LED)
static void apa102WriteByte(uint8_t b) {
  for (int bit = 7; bit >= 0; bit--) {
    digitalWrite(APA102_DATA_PIN, (b >> bit) & 1);
    digitalWrite(APA102_CLK_PIN, HIGH);
    digitalWrite(APA102_CLK_PIN, LOW);
  }
}

static void apa102SetColor(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < 4; i++) apa102WriteByte(0x00);
  apa102WriteByte(0xFF);  // global brightness
  apa102WriteByte(b);
  apa102WriteByte(g);
  apa102WriteByte(r);
  for (int i = 0; i < 4; i++) apa102WriteByte(0xFF);
}

static void apa102Init() {
  pinMode(APA102_DATA_PIN, OUTPUT);
  pinMode(APA102_CLK_PIN, OUTPUT);
  digitalWrite(APA102_CLK_PIN, LOW);
  digitalWrite(APA102_DATA_PIN, LOW);
  apa102SetColor(0, 0, 0);
}
#endif

// Heartbeat audio state: last time any target was seen, last time the
// heartbeat beep-pair was played. When nothing has been seen for
// HB_DEVICE_ACTIVE_MS the heartbeat stops until the next new detection.
static unsigned long fyLastTargetSeen  = 0;
static unsigned long fyLastHeartbeatAt = 0;

// ============================================================
// 802.11 HEADER
// ============================================================

typedef struct __attribute__((packed)) {
  uint16_t frame_ctrl;
  uint16_t duration;
  uint8_t  addr1[6];
  uint8_t  addr2[6];
  uint8_t  addr3[6];
  uint16_t seq_ctrl;
} wifi_ieee80211_mac_hdr_t;

// ============================================================
// HELPERS
// ============================================================

// Dual-output: prints to both Serial (USB) and Serial1 (GPIO43)
static char _dualBuf[384];

static void dualPrintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dualPrintf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(_dualBuf, sizeof(_dualBuf), fmt, args);
  va_end(args);
  if (n > 0) {
    Serial.write(_dualBuf, n);
#if MIRROR_SERIAL
    Serial1.write(_dualBuf, n);
#endif
  }
}

static void dualPrintln(const char* str) {
  Serial.println(str);
#if MIRROR_SERIAL
  Serial1.println(str);
#endif
}

static inline void ledSet(bool on) {
#if USE_LED
#if defined(USE_APA102_LED)
  if (on) apa102SetColor(APA102_FLASH_R, APA102_FLASH_G, APA102_FLASH_B);
  else apa102SetColor(0, 0, 0);
#else
#if LED_ACTIVE_HIGH
  digitalWrite(LED_PIN, on ? HIGH : LOW);
#else
  digitalWrite(LED_PIN, on ? LOW  : HIGH);
#endif
#endif
#endif
}

static void ledFlash(unsigned ms) {
#if USE_LED
  ledSet(true);
  ledOffAt = millis() + ms;
  if (ledOffAt == 0) ledOffAt = 1;  // avoid the "off" sentinel
#endif
}

static void ledTick() {
#if USE_LED
  if (ledOffAt && (long)(millis() - ledOffAt) >= 0) {
    ledSet(false);
    ledOffAt = 0;
  }
#endif
}

static void buzzerBeep(unsigned int ms) {
#if USE_BUZZER
  digitalWrite(BUZZER_PIN, HIGH); delay(ms); digitalWrite(BUZZER_PIN, LOW);
#endif
}

// Two fast ascending beeps — played on the FIRST sighting of a MAC.
static void newDetectChirp() {
#if USE_BUZZER
  tone(BUZZER_PIN, NEW_CHIRP_LO_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
  delay(NEW_CHIRP_GAP_MS);
  tone(BUZZER_PIN, NEW_CHIRP_HI_HZ); delay(NEW_CHIRP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}

// Two monotone beeps — periodic heartbeat while at least one target is still
// in range (last seen within HB_DEVICE_ACTIVE_MS).
static void heartbeatBeep() {
#if USE_BUZZER
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
  delay(HB_BEEP_GAP_MS);
  tone(BUZZER_PIN, HB_BEEP_HZ); delay(HB_BEEP_NOTE_MS); noTone(BUZZER_PIN);
#endif
}
static void startupBeep() {
#if USE_BUZZER
  // First 6 notes of SMB World 1-2 (underground). Koji Kondo's descending
  // pattern: C4, C5, A3, A4, B♭3, B♭4). (alternating-octave pairs).
  static const uint16_t notes[6] = { 262, 523, 220, 440, 233, 466 };

  for (int i = 0; i < 6; i++) {
    tone(BUZZER_PIN, notes[i]);
    delay((i == 5) ? 160 : 95);
    noTone(BUZZER_PIN);
    if (i < 5) delay(22);
  }
#endif
}

static void macToStr(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
static void ouiFromMac(const uint8_t* mac, char* buf, size_t len) {
  snprintf(buf, len, "%02x:%02x:%02x", mac[0], mac[1], mac[2]);
}

static void precompileOuis() {
  for (size_t i = 0; i < OUI_COUNT; i++) {
    const char* o  = target_ouis[i];
    oui_bytes[i][0] = (uint8_t)strtol(o,     nullptr, 16);
    oui_bytes[i][1] = (uint8_t)strtol(o + 3, nullptr, 16);
    oui_bytes[i][2] = (uint8_t)strtol(o + 6, nullptr, 16);
  }
}

// Bit 0 of byte 0 set = multicast/broadcast — never a real device transmitter or receiver
// we care about. Guards addr1 checks against 01:xx, 33:33:xx, ff:ff:ff:ff:ff:ff etc.
static inline bool IRAM_ATTR isMulticast(const uint8_t* mac) {
  return mac[0] & 0x01;
}

static bool IRAM_ATTR matchOuiRaw(const uint8_t* mac) {
  // Locally-administered (randomised) MACs have bit 1 of byte 0 set.
  // Fixed infrastructure devices never use them — skip immediately.
  if (mac[0] & 0x02) return false;

  for (size_t i = 0; i < OUI_COUNT; i++) {
    if (mac[0] == oui_bytes[i][0] &&
        mac[1] == oui_bytes[i][1] &&
        mac[2] == oui_bytes[i][2]) return true;
  }
  return false;
}

static char* strcasestr_local(const char* haystack, const char* needle) {
  if (!*needle) return (char*)haystack;
  for (; *haystack; ++haystack) {
    const char* h = haystack; const char* n = needle;
    while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { ++h; ++n; }
    if (!*n) return (char*)haystack;
  }
  return nullptr;
}
static bool matchSsidKeyword(const char* ssid) {
  for (size_t i = 0; i < SSID_KEYWORD_COUNT; i++)
    if (strcasestr_local(ssid, target_ssid_keywords[i])) return true;
  return false;
}

static const char* channelModeName() {
  switch (CHANNEL_MODE) {
    case CHANNEL_MODE_FULL_HOP: return "FULL_HOP";
    case CHANNEL_MODE_CUSTOM:   return "CUSTOM";
    case CHANNEL_MODE_SINGLE:   return "SINGLE";
    default:                    return "UNKNOWN";
  }
}

static inline uint16_t channelFreqMhz(uint8_t ch) {
  return (ch >= 1 && ch <= 14) ? (uint16_t)(2407 + 5 * ch) : 0;
}

static bool shouldSuppressDuplicate(const char* macStr) {
  unsigned long now = millis();
  for (size_t i = 0; i < DEDUPE_SLOTS; i++) {
    if (strcmp(dedupeTable[i].mac, macStr) == 0) {
      if ((now - dedupeTable[i].ts) < ALERT_COOLDOWN_MS) return true;
      dedupeTable[i].ts = now;
      return false;
    }
  }
  // Not found — insert into next slot
  strlcpy(dedupeTable[dedupeIdx].mac, macStr, 18);
  dedupeTable[dedupeIdx].ts = now;
  dedupeIdx = (dedupeIdx + 1) % DEDUPE_SLOTS;
  return false;
}

static void stopSniffing(const char* reason) {
  if (sniffingStopped) return;
  sniffingStopped = true;
  esp_wifi_set_promiscuous(false);
  dualPrintf("[flockyou] sniffing stopped: %s\n", reason);
}

static void applyInitialChannel() {
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  currentChannel = SINGLE_CHANNEL;
#elif CHANNEL_MODE == CHANNEL_MODE_CUSTOM
  currentChannel = customChannels[0];
#else
  currentChannel = fullHopChannels[0];
#endif
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();  // start dwell timer precisely when channel is first set
}

static void updateChannelMode() {
  if (sniffingStopped) return;
#if CHANNEL_MODE == CHANNEL_MODE_SINGLE
  if (currentChannel != SINGLE_CHANNEL) {
    currentChannel = SINGLE_CHANNEL;
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  }
  return;
#else
  if (millis() - lastHop < CHANNEL_DWELL_MS) return;
  #if CHANNEL_MODE == CHANNEL_MODE_CUSTOM
    customChannelIndex = (customChannelIndex + 1) % customChannelCount;
    currentChannel = customChannels[customChannelIndex];
  #else
    fullHopIndex = (fullHopIndex + 1) % fullHopChannelCount;
    currentChannel = fullHopChannels[fullHopIndex];
  #endif
  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  lastHop = millis();
#endif
}

static void printHeartbeat() {
  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    dualPrintf("[flockyou] scanning (ch=%u mode=%s det=%d)\n",
                  currentChannel, channelModeName(), fyDetCount);
    lastHeartbeat = millis();
    if (!dongleDisplayInAlert(millis())) {
      dongleDisplayShowIdle(currentChannel, fyDetCount);
    }
  }
}

// ============================================================
// DETECTION TABLE OPS
// ============================================================

static const char* alertTypeToMethod(AlertType t) {
  switch (t) {
    case ALERT_OUI_ADDR2:      return "oui_addr2";
    case ALERT_OUI_ADDR1:      return "oui_addr1";
    case ALERT_OUI_ADDR3:      return "oui_addr3";
    case ALERT_SSID:                   return "ssid";
    case ALERT_WILDCARD_PROBE_IE_SIG:  return "wildcard_probe_ie_sig";
    default:                           return "unknown";
  }
}

// Returns index of entry (new or updated), or -1 if table is full.
// Returns index, and sets *outChirpWorthy = true when the caller should fire
// the ascending new-discovery chirp. Chirp-worthy means either (a) MAC is
// brand new to this session, or (b) MAC is known but hasn't been seen in
// REDISCOVER_MS — i.e. it left RF range and came back.
static int fyAddDetection(const char* mac, const char* method,
                          int8_t rssi, uint8_t ch, const char* ssid,
                          bool* outChirpWorthy) {
  uint32_t now = millis();
  for (int i = 0; i < fyDetCount; i++) {
    if (strcasecmp(fyDet[i].mac, mac) == 0) {
      bool rediscover = (now - fyDet[i].lastSeen) > REDISCOVER_MS;
      if (fyDet[i].count < 0xFFFF) fyDet[i].count++;
      fyDet[i].lastSeen = now;
      fyDet[i].rssi     = rssi;
      fyDet[i].channel  = ch;
      if (ssid && ssid[0] && !fyDet[i].ssid[0]) {
        strlcpy(fyDet[i].ssid, ssid, sizeof(fyDet[i].ssid));
      }
      fyDirty = true;
      if (outChirpWorthy) *outChirpWorthy = rediscover;
      return i;
    }
  }
  if (fyDetCount >= MAX_DETECTIONS) {
    if (outChirpWorthy) *outChirpWorthy = false;
    return -1;
  }
  FYDetection& d = fyDet[fyDetCount];
  strlcpy(d.mac,    mac,                       sizeof(d.mac));
  strlcpy(d.method, method ? method : "",      sizeof(d.method));
  d.rssi      = rssi;
  d.channel   = ch;
  d.firstSeen = now;
  d.lastSeen  = now;
  d.count     = 1;
  if (ssid && ssid[0]) strlcpy(d.ssid, ssid, sizeof(d.ssid));
  else                 d.ssid[0] = '\0';
  fyDetCount++;
  fyDirty = true;
  if (outChirpWorthy) *outChirpWorthy = true;
  return fyDetCount - 1;
}

// ============================================================
// JSON ESCAPE  — only needed for SSIDs (user-controlled bytes)
// ============================================================

static size_t jsonEscape(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  if (cap == 0) return 0;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '"' || c == '\\') {
      if (o + 2 >= cap) break;
      dst[o++] = '\\'; dst[o++] = c;
    } else if ((unsigned char)c < 0x20) {
      if (o + 6 >= cap) break;
      int n = snprintf(dst + o, cap - o, "\\u%04x", (unsigned)(unsigned char)c);
      if (n <= 0 || (size_t)n >= cap - o) break;
      o += (size_t)n;
    } else {
      if (o + 1 >= cap) break;
      dst[o++] = c;
    }
  }
  dst[o] = '\0';
  return o;
}

// ============================================================
// CRC32  (zlib / SPIFFS-tool compatible polynomial 0xEDB88320)
// ============================================================

static uint32_t fyCRC32Update(uint32_t crc, const uint8_t* data, size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
  }
  return ~crc;
}

// ============================================================
// SPIFFS SESSION PERSISTENCE  — bulletproof envelope format
// ============================================================
//
// Wire format on disk:
//   Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}\n
//   Line 2+: [{"mac":...},...]     (exactly B bytes, CRC32 == X)
//
// Atomic write procedure:
//   1. Compute payload size + CRC (pass 1)
//   2. Write envelope + payload to /session.tmp (pass 2)
//   3. Re-validate /session.tmp from disk
//   4. Remove /session.json, rename tmp → main (with copy+delete fallback)
//
// Boot-time recovery:
//   - Try /session.json. If missing or CRC-invalid, try /session.tmp.
//   - Copy whichever validates to /prev_session.json, then delete both.

static size_t fySerializeDet(const FYDetection& d, char* dst, size_t cap) {
  char ssidEsc[sizeof(d.ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), d.ssid);
  int n = snprintf(dst, cap,
      "{\"mac\":\"%s\",\"method\":\"%s\",\"rssi\":%d,\"channel\":%u,"
      "\"first\":%lu,\"last\":%lu,\"count\":%u,\"ssid\":\"%s\"}",
      d.mac, d.method, d.rssi, (unsigned)d.channel,
      (unsigned long)d.firstSeen, (unsigned long)d.lastSeen, (unsigned)d.count,
      ssidEsc);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

static uint32_t fyComputePayloadCRC(size_t& outBytes) {
  char line[384];
  uint32_t crc = 0;
  outBytes = 0;
  crc = fyCRC32Update(crc, (const uint8_t*)"[", 1); outBytes += 1;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { crc = fyCRC32Update(crc, (const uint8_t*)",", 1); outBytes += 1; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    crc = fyCRC32Update(crc, (const uint8_t*)line, n);
    outBytes += n;
  }
  crc = fyCRC32Update(crc, (const uint8_t*)"]", 1); outBytes += 1;
  return crc;
}

// Minimal envelope parser: pulls bytes + crc fields by substring search.
// Robust to field reordering; rejects anything without both required keys.
static bool fyParseEnvelope(const char* hdr, size_t& outBytes, uint32_t& outCrc) {
  const char* b = strstr(hdr, "\"bytes\":");
  const char* c = strstr(hdr, "\"crc\":\"0x");
  if (!b || !c) return false;
  b += 8;
  long long bv = 0;
  if (sscanf(b, "%lld", &bv) != 1 || bv < 0) return false;
  c += 9;
  unsigned cv = 0;
  if (sscanf(c, "%x", &cv) != 1) return false;
  outBytes = (size_t)bv;
  outCrc   = (uint32_t)cv;
  return true;
}

static bool fyValidateSessionFile(const char* path) {
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;

  String hdr = f.readStringUntil('\n');
  if (hdr.length() < 10 || hdr[0] != '{') { f.close(); return false; }

  size_t   expectedBytes = 0;
  uint32_t expectedCRC   = 0;
  if (!fyParseEnvelope(hdr.c_str(), expectedBytes, expectedCRC)) {
    f.close(); return false;
  }

  size_t bodyOffset = hdr.length() + 1;
  size_t fileSize   = f.size();
  if (fileSize < bodyOffset + expectedBytes) { f.close(); return false; }
  if ((fileSize - bodyOffset) != expectedBytes) { f.close(); return false; }

  uint8_t buf[256];
  uint32_t crc = 0;
  size_t remaining = expectedBytes;
  while (remaining > 0) {
    int n = f.read(buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
    if (n <= 0) break;
    crc = fyCRC32Update(crc, buf, (size_t)n);
    remaining -= (size_t)n;
  }
  f.close();
  return (remaining == 0 && crc == expectedCRC);
}

static bool fySpiffsCopy(const char* src, const char* dst) {
  File s = SPIFFS.open(src, "r");
  if (!s) return false;
  File d = SPIFFS.open(dst, "w");
  if (!d) { s.close(); return false; }
  uint8_t buf[256];
  int n;
  bool ok = true;
  while ((n = s.read(buf, sizeof(buf))) > 0) {
    if (d.write(buf, (size_t)n) != (size_t)n) { ok = false; break; }
  }
  s.close();
  d.close();
  return ok;
}

static bool fyAtomicPromote(const char* src, const char* dst) {
  if (SPIFFS.rename(src, dst)) return true;
  if (!fySpiffsCopy(src, dst)) return false;
  SPIFFS.remove(src);
  return true;
}

static void fySaveSession() {
  if (!fySpiffsReady) return;
  if (!fyDirty && fyDetCount == fyLastSaveCount) return;

  size_t   payloadBytes = 0;
  uint32_t crc          = fyComputePayloadCRC(payloadBytes);
  int      savedCount   = fyDetCount;

  File f = SPIFFS.open(FY_SESSION_TMP, "w");
  if (!f) {
    dualPrintf("[flockyou] save failed: cannot open %s\n", FY_SESSION_TMP);
    return;
  }
  f.printf("{\"v\":1,\"count\":%d,\"bytes\":%u,\"crc\":\"0x%08lX\"}\n",
           savedCount, (unsigned)payloadBytes, (unsigned long)crc);

  char line[384];
  size_t wrote = 0;
  f.write((uint8_t*)"[", 1); wrote++;
  for (int i = 0; i < fyDetCount; i++) {
    if (i > 0) { f.write((uint8_t*)",", 1); wrote++; }
    size_t n = fySerializeDet(fyDet[i], line, sizeof(line));
    if (n == 0) continue;
    f.write((uint8_t*)line, n);
    wrote += n;
  }
  f.write((uint8_t*)"]", 1); wrote++;
  f.close();

  if (wrote != payloadBytes) {
    dualPrintf("[flockyou] save WARNING: wrote %u expected %u — aborting\n",
               (unsigned)wrote, (unsigned)payloadBytes);
    return;
  }

  if (!fyValidateSessionFile(FY_SESSION_TMP)) {
    dualPrintf("[flockyou] save verify FAILED — old session preserved\n");
    return;
  }

  SPIFFS.remove(FY_SESSION_FILE);
  if (!fyAtomicPromote(FY_SESSION_TMP, FY_SESSION_FILE)) {
    dualPrintf("[flockyou] promote FAILED — data in %s for recovery\n", FY_SESSION_TMP);
    return;
  }

  fyLastSaveAt    = millis();
  fyLastSaveCount = savedCount;
  fyDirty         = false;
  dualPrintf("[flockyou] session saved: %d det, %u bytes, crc=0x%08lX\n",
             savedCount, (unsigned)payloadBytes, (unsigned long)crc);
}

// Promote any valid session file from last boot into /prev_session.json, then
// start this boot with a fresh empty table. Preserves history across power cycles.
static void fyPromotePrevSession() {
  if (!fySpiffsReady) return;

  const char* source = nullptr;
  if      (fyValidateSessionFile(FY_SESSION_FILE)) source = FY_SESSION_FILE;
  else if (fyValidateSessionFile(FY_SESSION_TMP))  source = FY_SESSION_TMP;

  if (!source) {
    if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
    if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);
    dualPrintln("[flockyou] no valid prior session to promote");
    return;
  }

  if (!fySpiffsCopy(source, FY_PREV_FILE)) {
    dualPrintf("[flockyou] failed to promote %s → %s\n", source, FY_PREV_FILE);
    return;
  }
  if (SPIFFS.exists(FY_SESSION_FILE)) SPIFFS.remove(FY_SESSION_FILE);
  if (SPIFFS.exists(FY_SESSION_TMP))  SPIFFS.remove(FY_SESSION_TMP);

  File v = SPIFFS.open(FY_PREV_FILE, "r");
  size_t sz = v ? v.size() : 0;
  if (v) v.close();
  dualPrintf("[flockyou] prior session promoted from %s (%u bytes)\n",
             source, (unsigned)sz);
}

// ============================================================
// FLASK-COMPATIBLE JSON EMISSION
// ============================================================
//
// The Flask app (flock-you/api/flockyou.py) reads one JSON object per line
// from the USB CDC serial port. It filters by presence of `detection_method`
// and extracts these fields:  mac_address, rssi, channel, frequency, ssid,
// device_name, gps.latitude, gps.longitude, gps.accuracy.
//
// GPS is handled Flask-side via its own USB NMEA puck or browser geolocation;
// we don't embed GPS here because there's no on-device AP / phone link.

static void emitDetectionJSON(const char* mac, const char* method,
                              int8_t rssi, uint8_t ch, const char* ssid) {
  char ssidEsc[sizeof(((FYDetection*)0)->ssid) * 6 + 1];
  jsonEscape(ssidEsc, sizeof(ssidEsc), ssid ? ssid : "");
  char oui[9];
  uint8_t mbytes[6] = {0};
  sscanf(mac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &mbytes[0], &mbytes[1], &mbytes[2], &mbytes[3], &mbytes[4], &mbytes[5]);
  ouiFromMac(mbytes, oui, sizeof(oui));

  dualPrintf(
      "{\"event\":\"detection\","
      "\"detection_method\":\"wifi_%s\","
      "\"protocol\":\"wifi_2_4ghz\","
      "\"mac_address\":\"%s\","
      "\"oui\":\"%s\","
      "\"device_name\":\"\","
      "\"rssi\":%d,"
      "\"channel\":%u,"
      "\"frequency\":%u,"
      "\"ssid\":\"%s\"}\n",
      method, mac, oui, rssi,
      (unsigned)ch, (unsigned)channelFreqMhz(ch), ssidEsc);
}

// ============================================================
// PROMISCUOUS CALLBACK  — keep it fast, no Serial, no malloc
// ============================================================

static bool IRAM_ATTR extractSsidFromMgmtBody(const uint8_t* body, int len,
                                     char* outSsid, size_t outLen) {
  if (!body || len <= 0 || !outSsid || outLen == 0) return false;
  while (len >= 2) {
    uint8_t id = body[0], elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) {
      size_t n = (elen < (outLen - 1)) ? elen : (outLen - 1);
      memcpy(outSsid, body + 2, n);
      outSsid[n] = '\0';
      return true;
    }
    body += elen + 2; len -= elen + 2;
  }
  return false;
}

// Returns:
//   1  = wildcard SSID IE found (tag 0, length 0)  → Flock-style probe
//   0  = SSID IE found, non-zero length            → directed probe, not ours
//  -1  = no SSID IE found at all                   → caller should retry with
//                                                    FCS-stripped length, then bail
static int IRAM_ATTR isWildcardProbeIE(const uint8_t* body, int len) {
  if (!body || len < 2) return -1;
  while (len >= 2) {
    uint8_t id   = body[0];
    uint8_t elen = body[1];
    if ((int)elen + 2 > len) break;
    if (id == 0) return (elen == 0) ? 1 : 0;
    body += elen + 2;
    len  -= elen + 2;
  }
  return -1;
}

// --- PACK method 2 PoC: Flock probe IE signature (primary allowlist only) ---

static const char FLOCK_PROBE_IE_SIG_PRIMARY[] =
    "2,12,127,221:506f9a16030103,45,191,221:0050f208000000";
static const char FLOCK_LITEON_IE_SIG_PREFIX[] = "221:506f9a16030103";

#define FY_IE_SSID    0
#define FY_IE_VENDOR  221
#define FY_PHANTOM_SKIP_CAP 16
#define FY_TLV_RESYNC_MAX   64

// Encode n raw bytes as lowercase hex pairs (no separator) for vendor IE tokens.
static void IRAM_ATTR fyHexNibbles(char* dst, const uint8_t* b, int n) {
  static const char hd[] = "0123456789abcdef";
  for (int i = 0; i < n; i++) {
    dst[i * 2]     = hd[b[i] >> 4];
    dst[i * 2 + 1] = hd[b[i] & 0x0f];
  }
}
// True when ies[pos] starts vendor IE 221 with OUI 50:6f:9a (LiteON / Flock stack).
// Used to spot real IE boundaries inside corrupted/overflow TLV runs.
static bool IRAM_ATTR fyLiteonVendorAt(const uint8_t* ies, int len, int pos) {
  return pos + 9 <= len && ies[pos] == FY_IE_VENDOR && ies[pos + 1] == 7
      && ies[pos + 2] == 0x50 && ies[pos + 3] == 0x6f && ies[pos + 4] == 0x9a;
}
// Scan up to 32 bytes past a bogus TLV header for a real LiteON vendor IE —
// signals a phantom overflow (driver length/FCS skew) rather than end of frame.
static bool IRAM_ATTR fyPhantomLiteonAhead(const uint8_t* ies, int len, int pos) {
  int end = pos + 2 + 32;
  if (end > len - 1) end = len - 1;
  for (int j = pos + 2; j < end; j++) {
    if (fyLiteonVendorAt(ies, len, j)) return true;
  }
  return false;
}
// True when declared IE length extends past the buffer but looks like a phantom
// tag-64/len-128 overflow with LiteON payload still present ahead in the buffer.
static bool IRAM_ATTR fyIsPhantomOverflow(const uint8_t* ies, int len,
                                          uint8_t id, int elen, int i) {
  if (i + 2 + elen <= len) return false;
  if (elen > 200) return true;
  return id == 64 && elen == 128 && fyPhantomLiteonAhead(ies, len, i);
}
// After a TLV parse failure, slide forward up to FY_TLV_RESYNC_MAX bytes to find
// the next plausible IE header (id + len that fits in the buffer).
static int IRAM_ATTR fyTlvResync(const uint8_t* ies, int len, int start) {
  int end = start + FY_TLV_RESYNC_MAX;
  if (end > len - 1) end = len - 1;
  for (int j = start; j < end; j++) {
    int elen = (int)ies[j + 1];
    if (elen <= 200 && j + 2 + elen <= len) return j;
  }
  return -1;
}
// Append a comma-separated fragment to the growing IE signature string; fails if cap exceeded.
static bool IRAM_ATTR fySigAppend(char* out, size_t cap, size_t* pos, const char* part) {
  size_t plen = strlen(part);
  if (*pos != 0) {
    if (*pos + 1 >= cap) return false;
    out[(*pos)++] = ',';
  }
  if (*pos + plen >= cap) return false;
  memcpy(out + *pos, part, plen);
  *pos += plen;
  out[*pos] = '\0';
  return true;
}
// Append a non-vendor IE as its decimal tag id (e.g. "12", "127", "45").
static bool IRAM_ATTR fySigAppendTag(char* out, size_t cap, size_t* pos, uint8_t id) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)id);
  return fySigAppend(out, cap, pos, buf);
}
// Append vendor IE as "221:" + up to 8 payload bytes hex (matches PACK sig format).
static bool IRAM_ATTR fySigAppendVendor(char* out, size_t cap, size_t* pos,
                                        const uint8_t* body, int elen) {
  char buf[24];
  int take = elen < 8 ? elen : 8;
  buf[0] = '2'; buf[1] = '2'; buf[2] = '1'; buf[3] = ':';
  fyHexNibbles(buf + 4, body, take);
  buf[4 + take * 2] = '\0';
  return fySigAppend(out, cap, pos, buf);
}

// Walk 802.11 IE TLVs and build comma-separated fingerprint: skip SSID (tag 0),
// encode vendor 221 payloads, otherwise record tag numbers. Handles phantom
// overflows and resync. Sets *complete when every byte was consumed.
static bool IRAM_ATTR fyBuildFlockIeSigFromIes(const uint8_t* ies, int len,
                                               char* out, size_t cap, bool* complete) {
  if (!ies || len < 2 || !out || cap < 2) return false;
  size_t pos = 0;
  out[0] = '\0';
  int i = 0;
  uint8_t phantomSkips = 0;
  while (i + 2 <= len) {
    uint8_t id = ies[i];
    int elen = (int)ies[i + 1];
    if (i + 2 + elen > len) {
      if (phantomSkips < FY_PHANTOM_SKIP_CAP
          && fyIsPhantomOverflow(ies, len, id, elen, i)) {
        phantomSkips++;
        i += 2;
        continue;
      }
      int j = fyTlvResync(ies, len, i);
      if (j > i) {
        i = j;
        continue;
      }
      return false;
    }
    i += 2;
    if (id == FY_IE_SSID) {
      if (elen == 0) {
        while (i + 2 <= len && ies[i] == 0 && ies[i + 1] == 0) i += 2;
      } else {
        i += elen;
      }
      continue;
    }
    if (id == FY_IE_VENDOR && elen >= 4) {
      if (!fySigAppendVendor(out, cap, &pos, ies + i, elen)) return false;
    } else {
      if (!fySigAppendTag(out, cap, &pos, id)) return false;
    }
    i += elen;
  }
  if (complete) *complete = (i == len);
  return pos > 0;
}
// Normalize signature to "2,12,127,<rest from LiteON anchor>" when the LiteON
// vendor prefix is present but leading tags were truncated by parse skew.
static void IRAM_ATTR fyCanonicalizeFlockIeSig(char* sig, size_t cap) {
  if (!sig || cap < 8) return;
  if (strncmp(sig, "2,12,127,", 9) == 0
      && strstr(sig, FLOCK_LITEON_IE_SIG_PREFIX) != nullptr) {
    return;
  }
  const char* anchor = strstr(sig, FLOCK_LITEON_IE_SIG_PREFIX);
  if (!anchor) return;
  char tmp[128];
  int n = snprintf(tmp, sizeof(tmp), "2,12,127,%s", anchor);
  if (n > 0 && (size_t)n < cap) memcpy(sig, tmp, (size_t)n + 1);
}
// Normalize signature to "2,12,127,<rest from LiteON anchor>" when the LiteON
// vendor prefix is present but leading tags were truncated by parse skew.
static bool IRAM_ATTR fyPickBetterSig(const char* a, bool aComplete,
                                      const char* b, bool bComplete,
                                      char* out, size_t cap) {
  if (!a[0] && !b[0]) return false;
  if (a[0] && !b[0]) {
    strncpy(out, a, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }
  if (!a[0] && b[0]) {
    strncpy(out, b, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }
  const char* pick = a;
  if (aComplete && !bComplete) pick = a;
  else if (!aComplete && bComplete) pick = b;
  else if (strlen(b) > strlen(a)) pick = b;
  strncpy(out, pick, cap - 1);
  out[cap - 1] = '\0';
  return true;
}
// Build fingerprint from full body and from body+2 (skip leading empty SSID IE pair);
// merge, canonicalize, write to out.
static bool IRAM_ATTR fyBuildFlockIeSigFromProbeBody(const uint8_t* body, int bodyLen,
                                                     char* out, size_t cap) {
  if (!body || bodyLen < 2 || !out || cap < 16) return false;
  char sigA[128] = {0};
  char sigB[128] = {0};
  bool completeA = false, completeB = false;
  bool okA = fyBuildFlockIeSigFromIes(body, bodyLen, sigA, sizeof(sigA), &completeA);
  bool okB = false;
  if (bodyLen >= 2 && body[0] == 0 && body[1] == 0) {
    okB = fyBuildFlockIeSigFromIes(body + 2, bodyLen - 2, sigB, sizeof(sigB), &completeB);
  }
  char merged[128] = {0};
  if (!fyPickBetterSig(okA ? sigA : "", completeA, okB ? sigB : "", completeB,
                       merged, sizeof(merged))) {
    return false;
  }
  fyCanonicalizeFlockIeSig(merged, sizeof(merged));
  strncpy(out, merged, cap - 1);
  out[cap - 1] = '\0';
  return out[0] != '\0';
}
// True when sig exactly matches FLOCK_PROBE_IE_SIG_PRIMARY (drive-tested allowlist entry).
static bool IRAM_ATTR fyFlockIeSigIsPrimary(const char* sig) {
  return sig && strcmp(sig, FLOCK_PROBE_IE_SIG_PRIMARY) == 0;
}

static bool IRAM_ATTR fyProbeBodyFlockIeSigPrimary(const uint8_t* body, int bodyLen) {
  char ieSig[128];
  int len = bodyLen;
  if (fyBuildFlockIeSigFromProbeBody(body, len, ieSig, sizeof(ieSig))
      && fyFlockIeSigIsPrimary(ieSig)) {
    return true;
  }
  if (len > 4 && fyBuildFlockIeSigFromProbeBody(body, len - 4, ieSig, sizeof(ieSig))
      && fyFlockIeSigIsPrimary(ieSig)) {
    return true;
  }
  return false;
}

static void IRAM_ATTR wifiSniffer(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || sniffingStopped) return;

#if PROCESS_MGMT_FRAMES && PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
#elif PROCESS_MGMT_FRAMES
  if (type != WIFI_PKT_MGMT) return;
#elif PROCESS_DATA_FRAMES
  if (type != WIFI_PKT_DATA) return;
#else
  return;  // nothing configured to process
#endif

  wifi_promiscuous_pkt_t*      pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < sizeof(wifi_ieee80211_mac_hdr_t)) return;
  wifi_ieee80211_mac_hdr_t*    hdr = (wifi_ieee80211_mac_hdr_t*)pkt->payload;
  int8_t rssi = pkt->rx_ctrl.rssi;

  if (rssi < RSSI_MIN) return;

  uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;  // actual rx channel from driver

  // --- OUI check: addr2 (transmitter/source) ---
  //
  // Probe requests (type=0 subtype=4) from a matched OUI: wildcard SSID IE
  // (tag 0, length 0) plus primary Flock IE signature → wifi_wildcard_probe_ie_sig.
  //
  // wifi_wildcard_probe (OUI + wildcard only, no IE check) was removed here.
  // Suggest superseded by the IE fingerprint path: it uses the same wildcard
  // and OUI gates and adds verification on probe IE fields.
  if (matchOuiRaw(hdr->addr2)) {
    if (type == WIFI_PKT_MGMT) {
      uint8_t fc0     = hdr->frame_ctrl & 0xFF;
      uint8_t ftype   = (fc0 >> 2) & 0x03;
      uint8_t subtype = (fc0 >> 4) & 0x0F;
      if (ftype == 0 && subtype == 4) {                        // Probe Request
        int sigLen  = (int)pkt->rx_ctrl.sig_len;
        int bodyLen = sigLen - (int)sizeof(wifi_ieee80211_mac_hdr_t);
        const uint8_t* body = pkt->payload + sizeof(wifi_ieee80211_mac_hdr_t);
        int r = (bodyLen > 0) ? isWildcardProbeIE(body, bodyLen) : -1;
        // FCS-trailer retry: only when the first parse found no SSID IE AT
        // ALL (-1). A found-but-nonzero (0) means legit directed probe; do
        // not retry — it would mis-classify.
        if (r == -1 && bodyLen > 4) r = isWildcardProbeIE(body, bodyLen - 4);
        if (r == 1 && fyProbeBodyFlockIeSigPrimary(body, bodyLen)) {
          enqueueAlert(ALERT_WILDCARD_PROBE_IE_SIG, hdr->addr2, rssi, ch,
                       nullptr, "probe_req");
        }
      }
    }
    // wifi_oui_addr2 — broad transmitter OUI on any non-fingerprint frame:
    // if (!emitted) {
    //   enqueueAlert(ALERT_OUI_ADDR2, hdr->addr2, rssi, ch, nullptr, "addr2");
    // }
  }

  // --- Disabled: wifi_oui_addr1 (receiver / addr1) ---
  //
  // Suggest leaving this disabled. Flock cameras are known to channel-hop and
  // send wildcard probe requests (addr2 = camera). Nearby APs that hear those
  // probes reply with probe responses where addr1 = camera MAC and addr2 = AP.
  // This path is the same OUI list again, but matching addr1 (destination) —
  // i.e. "is anyone sending *to* a Flock OUI?" — not the camera transmitting.
  //
  // 802.11 MAC header roles (infrastructure / mgmt):
  //   addr1 = receiver (DA)   addr2 = transmitter (SA)   addr3 = BSSID
  // On a camera probe request:  addr2=camera, addr1 often broadcast.
  // On an AP probe response:    addr1=camera, addr2=AP, addr3=AP BSSID.
  //
  // addr1 hits are therefore mostly second-hand fallout from the same probe
  // behavior (AP replies), redundant with wildcard+IE detection on the uplink
  // probe request itself.
#if CHECK_ADDR1
  // if (!isMulticast(hdr->addr1) && matchOuiRaw(hdr->addr1)) {
  //   enqueueAlert(ALERT_OUI_ADDR1, hdr->addr1, rssi, ch, nullptr, "addr1");
  // }
#endif

  // --- Disabled: wifi_oui_addr3 (BSSID / addr3) ---
  //
  // Suggest leaving this disabled. Another broad OUI filter on addr3 (BSSID)
  // on management frames — intended for randomised addr2 with real OUI in
  // addr3, but still OUI-only with no probe/IE behavioral check, so it can
  // generate false positives on unrelated mgmt traffic.
#if CHECK_ADDR3
  // if (type == WIFI_PKT_MGMT && matchOuiRaw(hdr->addr3)) {
  //   enqueueAlert(ALERT_OUI_ADDR3, hdr->addr3, rssi, ch, nullptr, "addr3");
  // }
#endif

#if ENABLE_SSID_MATCH
  if (type == WIFI_PKT_MGMT) {
    uint8_t fc0     = hdr->frame_ctrl & 0xFF;
    uint8_t subtype = (fc0 >> 4) & 0x0F;
    uint8_t ftype   = (fc0 >> 2) & 0x03;

    if (ftype == 0) {
      int sigLen = pkt->rx_ctrl.sig_len - 4;  // strip 4-byte FCS
      if (sigLen < (int)sizeof(wifi_ieee80211_mac_hdr_t)) return;

      const uint8_t* mgmtBody    = nullptr;
      int            mgmtBodyLen = 0;
      const char*    frameKind   = nullptr;

      if (subtype == 8 || subtype == 5) {
        // Beacon / Probe Response: fixed params = 12 bytes after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t) + 12;
        if (sigLen > off) {
          frameKind   = (subtype == 8) ? "beacon" : "probe_resp";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      } else if (subtype == 4) {
        // Probe Request: IEs follow directly after MAC hdr
        int off = sizeof(wifi_ieee80211_mac_hdr_t);
        if (sigLen > off) {
          frameKind   = "probe_req";
          mgmtBody    = pkt->payload + off;
          mgmtBodyLen = sigLen - off;
        }
      }

      if (mgmtBody && mgmtBodyLen > 0) {
        char ssid[33] = {0};
        if (extractSsidFromMgmtBody(mgmtBody, mgmtBodyLen, ssid, sizeof(ssid))) {
          if (matchSsidKeyword(ssid)) {
            enqueueAlert(ALERT_SSID, hdr->addr2, rssi, ch, ssid, frameKind);
          }
        }
      }
    }
  }
#endif
}

// ============================================================
// DRAIN QUEUE — called from loop(), safe to Serial.print here
// ============================================================

static void drainAlertQueue() {
  while (true) {
    portENTER_CRITICAL(&queueMux);
    if (alertTail == alertHead) { portEXIT_CRITICAL(&queueMux); break; }
    AlertEntry e;
    memcpy(&e, (const void*)&alertQueue[alertTail], sizeof(AlertEntry));
    alertTail = (alertTail + 1) % ALERT_QUEUE_SIZE;
    portEXIT_CRITICAL(&queueMux);

    char macStr[18];
    macToStr(e.mac, macStr, sizeof(macStr));
    const char* method = alertTypeToMethod(e.type);

    // Always update the on-device detection table (survives reboot via SPIFFS).
    // chirpWorthy = true for brand-new MACs AND for MACs rediscovered after
    // REDISCOVER_MS of silence (drove away and came back).
    bool chirpWorthy = false;
    int idx = fyAddDetection(macStr, method, e.rssi, e.channel,
                             (e.type == ALERT_SSID) ? e.ssid : nullptr,
                             &chirpWorthy);

    // Refresh the global "still around" timer for the heartbeat tick.
    // Done unconditionally so a device counts as active even when serial is
    // rate-limited (still audible via heartbeat, just quieter on the wire).
    fyLastTargetSeen = millis();

    // Serial-rate-limit: suppress emit/beep/flash within ALERT_COOLDOWN_MS.
    if (shouldSuppressDuplicate(macStr)) continue;

    // Human-readable line (for serial terminal / mirror).
    char oui[9];
    ouiFromMac(e.mac, oui, sizeof(oui));
    if (e.type == ALERT_SSID) {
      dualPrintf("[flockyou] DETECT-SSID type=%s mac=%s ssid=\"%s\" rssi=%d ch=%u count=%d\n",
                 e.frameKind, macStr, e.ssid, e.rssi, e.channel,
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    } else {
      dualPrintf("[flockyou] DETECT-OUI mac=%s oui=%s rssi=%d ch=%u addr=%s count=%d\n",
                 macStr, oui, e.rssi, e.channel,
                 e.frameKind[0] ? e.frameKind : "addr2",
                 (idx >= 0) ? (int)fyDet[idx].count : 0);
    }

    // Flask-compatible JSON line (parsed by api/flockyou.py over USB CDC).
    emitDetectionJSON(macStr, method, e.rssi, e.channel,
                      (e.type == ALERT_SSID) ? e.ssid : "");

    // Audio feedback:
    //   - NEW MAC  → two fast ascending beeps (clearly distinct sound)
    //   - REPEAT   → silent; the heartbeat tick covers continued presence
    // LED flashes on every emitted detection either way.
    if (chirpWorthy) {
      newDetectChirp();
      // Reset the heartbeat phase so the first follow-up beep lands
      // HB_BEEP_INTERVAL_MS after the initial chirp, not mid-window.
      fyLastHeartbeatAt = millis();
    }
    ledFlash(LED_FLASH_MS);

    char methodLine[40];
    snprintf(methodLine, sizeof(methodLine), "wifi_%s", method);
    dongleDisplayShowAlert(methodLine, macStr, e.rssi, e.channel, ALERT_COOLDOWN_MS);

#if STOP_ON_OUI_HIT
    if (e.type != ALERT_SSID) stopSniffing("OUI hit");
#endif
#if STOP_ON_SSID_HIT
    if (e.type == ALERT_SSID) stopSniffing("SSID hit");
#endif
  }
}

// ============================================================
// AUTOSAVE
// ============================================================

static void autosaveTick() {
  if (!fySpiffsReady || !fyDirty) return;
  if (millis() - fyLastSaveAt < AUTOSAVE_INTERVAL_MS) return;
  fySaveSession();
}

// Heartbeat beep while at least one target was seen in the last
// HB_DEVICE_ACTIVE_MS. Fires HB_BEEP_INTERVAL_MS apart.
static void heartbeatTick() {
  if (fyLastTargetSeen == 0) return;                           // never seen one
  unsigned long now = millis();
  if (now - fyLastTargetSeen > HB_DEVICE_ACTIVE_MS) return;    // gone silent
  if (now - fyLastHeartbeatAt < HB_BEEP_INTERVAL_MS) return;   // too soon
  heartbeatBeep();
  fyLastHeartbeatAt = now;
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  // Crucial for USB-optional operation: without this, Serial.write() will
  // block indefinitely on an ESP32-S3 USB-CDC port when no host is attached.
  Serial.setTxTimeoutMs(0);
  delay(300);

#ifdef BOARD_LILYGO_T_DONGLE_S3
  dongleDisplayInit();
#endif

#if MIRROR_SERIAL
  Serial1.begin(MIRROR_BAUD, SERIAL_8N1, -1, MIRROR_TX_PIN);  // TX-only on GPIO43
#endif

#if USE_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
#endif

#if USE_LED
#if defined(USE_APA102_LED)
  apa102Init();
#else
  pinMode(LED_PIN, OUTPUT);
  ledSet(false);
#endif
#endif

  startupBeep();
#if USE_LED
  ledFlash(200);
#endif

  precompileOuis();
  memset(dedupeTable, 0, sizeof(dedupeTable));

  // SPIFFS — format on first boot if missing. Non-fatal if it fails.
  if (SPIFFS.begin(true)) {
    fySpiffsReady = true;
    dualPrintln("[flockyou] SPIFFS ready");
    fyPromotePrevSession();
  } else {
    dualPrintln("[flockyou] SPIFFS init FAILED — running without persistence");
  }

  WiFi.mode(WIFI_MODE_NULL);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();

  applyInitialChannel();

  wifi_promiscuous_filter_t filt = {
    .filter_mask = 0
#if PROCESS_MGMT_FRAMES
        | WIFI_PROMIS_FILTER_MASK_MGMT
#endif
#if PROCESS_DATA_FRAMES
        | WIFI_PROMIS_FILTER_MASK_DATA
#endif
  };
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&wifiSniffer);
  esp_wifi_set_promiscuous(true);

  dualPrintln("[flockyou] merged WiFi detector started");
  dualPrintf("[flockyou] mode=%s dwell_ms=%u start_channel=%u rssi_min=%d spiffs=%d\n",
                channelModeName(), CHANNEL_DWELL_MS, currentChannel,
                RSSI_MIN, fySpiffsReady ? 1 : 0);

  lastHeartbeat = millis();
  fyLastSaveAt  = millis();

#ifdef BOARD_LILYGO_T_DONGLE_S3
  dongleDisplayShowIdle(currentChannel, fyDetCount);
#endif
}

void loop() {
  updateChannelMode();
  drainAlertQueue();   // Serial.printf happens here, not in callback
  autosaveTick();      // periodic SPIFFS write if dirty
  heartbeatTick();     // audible beep-pair while a target is still in range
  ledTick();           // turn off LED after LED_FLASH_MS
  dongleDisplayTick(millis(), currentChannel, fyDetCount);
  printHeartbeat();
  delay(1);
}
