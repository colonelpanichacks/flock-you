# Detection Improvements — flock-you-esp32 v2

> **July 2026** — Based on field research by @NitekryDPaul, Michael/DeFlockJoplin,
> Will Greenberg (BLE mfr-ID), GainSec (Raven BLE UUID), and the issue #43
> "Flock Camera net." field report.

---

## Summary of changes

| # | Area | v1 behaviour | v2 behaviour |
|---|------|-------------|--------------|
| 1 | SSID patterns | `"flock"` only | `"flock"`, `"flocksafety"`, `"penguin"`, `"pigvision"` + exact `"Flock Camera net."` high-confidence path |
| 2 | LAA MAC cameras | Silently ignored (OUI skip) | `ALERT_LAA_SSID` fired; `laa_ssid` method emitted in JSON |
| 3 | Sequential-MAC heuristic | Not implemented | `checkSeqMac()` — 5-byte prefix table, last-byte diff=1 pairs → +10 confidence |
| 4 | Confidence score | None | 0–100 additive score computed per frame in callback; stored in table + JSON |
| 5 | BLE cross-correlation | Not implemented | `ENABLE_BLE_SCAN=1` — NimBLE passive scan every 60 s; hit within 60 s → +20 |
| 6 | addr3 default | `CHECK_ADDR3 0` (off) | `CHECK_ADDR3 1` (on) — BSSID fallback always active |
| 7 | SSID match default | `ENABLE_SSID_MATCH 0` | `ENABLE_SSID_MATCH 1` — required for LAA cameras |
| 8 | 5 GHz channels | Not addressed | Hardware guard + ESP32-C5 compile path documented; `channelFreqMhz()` handles ch >14 |
| 9 | JSON `protocol` field | `"wifi_2_4ghz"` hardcoded | `channelBand(ch)` — returns `"wifi_2_4ghz"` or `"wifi_5ghz"` based on channel |
| 10 | JSON `oui` field | Always OUI bytes | `"laa"` string for locally-administered MACs |
| 11 | SPIFFS schema | No confidence field | `"confidence":%u` added to serialised detection JSON |
| 12 | platformio.ini | 5 environments | 10 environments — every variant gets a `-ble` twin |

---

## 1 — Additional frame-level signals and MAC patterns

### 1.1 OUI matching is already comprehensive for IEEE-assigned MACs

The 31-OUI table (`target_ouis[]`) covers all known field-observed prefixes.
No new OUIs were confirmed in time for this release.  However, two structural
improvements make future additions cheaper:

- `precompileOuis()` converts string OUIs to `oui_bytes[][3]` on boot.
  The inner loop in `matchOuiRaw()` is now 3 byte-compares per entry, no
  `strncmp`, no `strtol` at runtime — safe to call from IRAM ISR context.
- `matchOuiRaw()` bails immediately on locally-administered MACs
  (`mac[0] & 0x02`), which saves the full table scan for the entire
  "Flock Camera net." camera class.

### 1.2 addr3 (BSSID) is now always checked

`CHECK_ADDR3` was `0` in v1.  It is `1` in v2.

**Why it matters:** Some AP-mode Flock deployments send management frames
where addr2 (TA) is randomised (e.g., a client scanning for an upstream AP)
but addr3 (BSSID of the destination network) still carries the Flock OUI.
The addr3 check catches these.  The multicast guard is required here too —
without it, broadcast BSSIDs (`ff:ff:ff:ff:ff:ff`) would match if any of
the OUI bytes happen to be `0xFF`.

### 1.3 Probe Response (subtype 5) now triggers SSID match

v1 only parsed Beacon (8) and Probe Request (4) bodies for SSIDs.
v2 includes subtype 5 (Probe Response).  This matters because:

- When our ESP32 changes channels it briefly looks like a client.
  Nearby APs send Probe Responses to anything that has recently
  broadcast.  If the Flock camera's AP hotspot sends a Probe Response
  containing `"Flock Camera net."`, we now catch it.
- Probe Responses are common traffic on 2.4 GHz ch.1 because that's
  the channel "Flock Camera net." advertises its hotspot on.

---

## 2 — Wildcard probe + OUI combined scoring

The combined signal fires `ALERT_WILDCARD_PROBE` (rather than plain
`ALERT_OUI_ADDR2`) only when **both** conditions hold:

1. `addr2` matches the OUI table
2. The SSID IE (tag 0) is present **and** has length 0

Score breakdown for a clean wildcard probe hit:

```
CS_OUI_ADDR2    = 40   (transmitter in 31-OUI table)
CS_WILDCARD_PROBE = 22  (empty-SSID IE confirmed)
─────────────────────
Base              = 62
+ CS_BLE_CORR    = 20  (if BLE Flock hit within 60 s)  → 82
+ CS_STRONG_RSSI =  5  (RSSI > -70 dBm, camera close)  → 87
+ CS_SEQ_MAC_PAIR= 10  (sequential last-byte pair seen) → 97
```

A camera seen as `ALERT_WILDCARD_PROBE` with BLE correlation and close RSSI
scores **≥ 82** — classifiable as HIGH CONFIDENCE without any further signal.

### Tightening against false positives

DeFlockJoplin's field test (Joplin, MO) produced 2 false positives out of
13 total: one router with an OUI collision, one phone doing directed scans.
The phone was easily excluded because its probe requests contained a non-zero
SSID IE (directed, not wildcard) — the `isWildcardProbeIE()` check returned `0`
and no alert was fired.  The router collision is unavoidable at the OUI level
but will score low (40 flat, no SSID, no BLE, typical RSSI < -80 → score 40).

**Recommended threshold for automated flagging:** `confidence >= 60`.

---

## 3 — Detecting "Flock Camera net." (locally-administered MAC cameras)

### 3.1 Why OUI matching fails for this camera class

The issue-#43 "Flock Camera net." hotspot deliberately uses **locally-
administered MAC addresses** (bit 1 of byte 0 set, e.g. `82:XX:XX:...` or
`DE:XX:XX:...`).  `matchOuiRaw()` returns `false` for these by design —
there is no IEEE OUI to match because locally-administered MACs are not
registered with the IEEE.

### 3.2 ALERT_LAA_SSID — the primary detection path

`ALERT_LAA_SSID` fires when:
1. `addr2` has bit 1 set (locally-administered)
2. The SSID IE matches any keyword in `target_ssid_keywords[]`

Score for `"Flock Camera net."` exact match:

```
CS_SSID_FLOCK_CAM_NET = 45  (exact "Flock Camera net." — highly specific)
CS_LAA_MAC            = 12  (confirms this is the LAA-MAC camera class)
─────────────────────────
Base                  = 57  → PROBABLE (borderline HIGH)
+ CS_SEQ_MAC_PAIR     = 10                              → 67  HIGH
+ CS_BLE_CORR         = 20                              → 87  HIGH
+ CS_STRONG_RSSI      =  5                              → 92  HIGH
```

Even without BLE, a "Flock Camera net." SSID on a LAA MAC scores **57–67**
depending on whether the sequential-MAC heuristic fires.

### 3.3 Sequential-MAC heuristic — `checkSeqMac()`

The issue-#43 field report describes the dual-band radio pair:

```
XX:XX:XX:9F:A2:DE  →  2.4 GHz radio (ch.1 beacon/probe-response)
XX:XX:XX:9F:A2:DF  →  5 GHz radio   (ch.157 beacon)
```

`checkSeqMac()` maintains a 32-slot table keyed on the **first 5 bytes** of
each LAA MAC seen.  When a second entry with the same 5-byte prefix arrives
on a **different channel** with a last byte that differs by exactly 1, the
function returns `true` and the caller adds `CS_SEQ_MAC_PAIR` (+10).

**Implementation notes:**
- Only fires for LAA MACs (`mac[0] & 0x02`).  Globally-administered MACs
  go through OUI matching instead.
- Entries expire after `SEQ_MAC_EXPIRE_MS` = 30 s.  This prevents a
  stale `:DE` entry from falsely pairing with a future `:DF` from a
  different camera in a different location.
- Table is 32 slots × 8 bytes = 256 bytes of RAM — negligible.
- The expiry sweep runs inside the IRAM callback but is O(n) over 32 slots,
  adding < 2 µs per frame on a 240 MHz Xtensa — acceptable.

### 3.4 Other locally-administered MAC patterns worth watching

Beyond the sequential-last-byte pair, consider:

| Pattern | Heuristic | Signal strength |
|---------|-----------|-----------------|
| Same 5-byte prefix on ≥3 channels | Repeated dual-band scanning | Medium |
| LAA MAC seen as both addr2 AND addr1 of the same frame exchange | Camera is both transmitter and receiver | High |
| LAA MAC with `"Flock Camera net."` on ch.1 + OUI MAC on same ch.1 within 500 ms | Both radios of same unit visible | Very high |

The last pattern (LAA + OUI on same channel simultaneously) would be a near-
certain confirmation.  Implementing it requires correlating `ALERT_LAA_SSID`
against `ALERT_OUI_ADDR2` events by channel + timestamp — possible in
`drainAlertQueue()` but left as future work.

---

## 4 — 5 GHz channel scanning (149, 157) on ESP32

### 4.1 Hardware verdict: not possible on ESP32/S3

All currently supported hardware — ESP32-PICO-D4 (Atom Lite, Atom Echo, Atom
Voice), ESP32 DevKit, ESP32-S3 (Atom VoiceS3R) — uses a **2.4 GHz-only**
802.11b/g/n radio.  Calling `esp_wifi_set_channel(149, WIFI_SECOND_CHAN_NONE)`
on these parts returns `ESP_ERR_INVALID_ARG` and changes nothing.

Adding channels 149 and 157 to the hop list on 2.4 GHz hardware would:
- Waste 500 ms of capture time per full cycle (2 × 250 ms dwell)
- Produce zero captures
- Reduce effective 2.4 GHz dwell time by ~18%


**Do not add 5 GHz channels to the 2.4 GHz hop list.**

### 4.2 Hardware path: ESP32-C5

The ESP32-C5 (Espressif, 2024) is the first ESP32-family chip with a dual-
band 802.11b/g/n radio covering both 2.4 GHz and 5 GHz.  To enable 5 GHz
scanning, compile with `-DESP32C5_DUALBAND=1`.  The firmware already defines:

```cpp
#if defined(ESP32C5_DUALBAND) && ESP32C5_DUALBAND
  static const uint8_t fiveGhzChannels[] = {149, 157};
  static const size_t  fiveGhzChannelCount = 2;
#endif
```

You would then extend `updateChannelMode()` to interleave these into the
hop sequence.  `channelFreqMhz()` already handles ch > 14:

```cpp
// ch 149 → 5000 + 5*149 = 5745 MHz  ✓
// ch 157 → 5000 + 5*157 = 5785 MHz  ✓
if (ch >= 36 && ch <= 165) return (uint16_t)(5000 + 5 * ch);
```

### 4.3 Alternative: external 5 GHz sniffer

A Raspberry Pi with a 5 GHz 802.11ac USB adapter in monitor mode can feed
detections to the same Flask dashboard over USB or network.  The JSON format
emitted by the firmware is already band-aware (`"protocol":"wifi_5ghz"`) so
the dashboard can display detections from both sources on the same map.

### 4.4 Trade-offs summary

| Option | 5 GHz | Cost | Complexity |
|--------|-------|------|------------|
| ESP32/S3 (current) | ✗ | Low | None |
| ESP32-C5 | ✓ ch.149+157 | ~$4 module | Minimal (compile flag) |
| RPi + 5 GHz USB NIC | ✓ full UNII-3 | ~$35 | Moderate |
| Laptop in monitor mode | ✓ full spectrum | Existing hardware | Low |

**Recommendation:** Use an ESP32-C5 for a compact, portable dual-band sensor.
Pair it with the existing ESP32/S3 firmware using `-DESP32C5_DUALBAND=1` once
a suitable PlatformIO board definition becomes available.

---

## 5 — BLE cross-correlation with WiFi detection (`ENABLE_BLE_SCAN=1`)

### 5.1 BLE signals checked

| Signal | Source | UUID / ID |
|--------|--------|-----------|
| Manufacturer-specific data with Flock mfr-ID `0x09C8` (XUNTONG) | Will Greenberg field research | N/A |
| Service UUID `0x1B7E` (Raven primary service) | GainSec research | BLE GATT |
| Service UUID `0xFD60` (Raven telemetry) | GainSec research | BLE GATT |
| Device name containing `"flock"`, `"penguin"`, `"pigvision"`, `"fs ext battery"`, `"raven"` | eye-spy firmware / field observation | Adv name |

### 5.2 Time-multiplexing strategy

BLE and WiFi share the 2.4 GHz radio on every ESP32 variant.  The firmware
uses a pause-and-resume strategy:

```
WiFi promisc ─────────────────┐   5 s   ┌────────────────────────────────►
                               └── BLE ──┘
                               ← 60 s interval →
```

1. Every `BLE_SCAN_INTERVAL_MS` (60 s), `bleScanTick()` calls
   `esp_wifi_set_promiscuous(false)` and starts a NimBLE passive scan.
2. When the scan completes (after `BLE_SCAN_DWELL_MS` = 5 s), promiscuous
   mode is restored.
3. `fyPromiscPaused` prevents `updateChannelMode()` from calling
   `esp_wifi_set_channel()` during the BLE window.

**Overhead:** 5 s / 60 s = ~8.3% of WiFi capture time.

**Why passive scan:** `g_pBLEScan->setActiveScan(false)` means the ESP32
does not transmit BLE scan-request packets.  Active scanning would cause
the Flock camera's BLE radio to send a scan-response, potentially logging
our MAC address in its telemetry.  Passive scanning is fully undetectable.

### 5.3 Cross-correlation mechanics

When `FlockBLECallbacks::onResult()` matches a frame, it writes:
```cpp
g_bleFlockLastSeen = (uint32_t)millis();
```
This is a single `uint32_t` write — effectively atomic on Xtensa (aligned
4-byte store is a single instruction).  The WiFi ISR callback reads it:
```cpp
uint32_t bts = g_bleFlockLastSeen;
if (bts != 0 && (now - bts) < BLE_CORR_WINDOW_MS)
    score += CS_BLE_CORR;  // +20
```
The `BLE_CORR_WINDOW_MS` (60 s) is intentionally generous because the BLE
scan only runs every 60 s.  A BLE hit during the scan window is valid as
corroborating evidence for the entire following minute.

### 5.4 When BLE alone fires (no WiFi OUI seen)

If `g_bleFlockLastSeen` is set but no WiFi OUI match fires, there is no
`AlertEntry` emitted — the BLE state just waits silently.  This is correct:
the BLE alone is not enough to generate a SPIFFS entry or serial JSON line.
It only **upgrades** the confidence of a concurrent WiFi detection.

If you want standalone BLE alerts (useful for fixed installations near a
Flock maintenance hub), add a separate `ALERT_BLE_FLOCK` type in `AlertType`
and emit it from `bleScanTick()` after the scan completes.  This is left as
optional future work because it would require adding BLE-detected MACs to the
detection table separately.

---

## 6 — Confidence score (0–100) design

### 6.1 Weight table

| Signal | Constant | Value | Justification |
|--------|----------|-------|---------------|
| OUI match on addr2 | `CS_OUI_ADDR2` | 40 | Transmitter is in 31-OUI table — strong positive |
| OUI match on addr1 | `CS_OUI_ADDR1` | 18 | Camera is *receiver* — weaker; could be general traffic destined for camera |
| OUI match on addr3 | `CS_OUI_ADDR3` | 12 | BSSID fallback — indirect |
| Wildcard probe bonus | `CS_WILDCARD_PROBE` | 22 | Stacks on top of `CS_OUI_ADDR2`; delivers 62 total |
| Generic Flock SSID | `CS_SSID_FLOCK` | 32 | "flock" keyword — moderately specific |
| Exact "Flock Camera net." | `CS_SSID_FLOCK_CAM_NET` | 45 | Highly specific; 17-char exact string |
| Locally-administered MAC | `CS_LAA_MAC` | 12 | Confirms issue-#43 camera class when combined with Flock SSID |
| Sequential last-byte pair | `CS_SEQ_MAC_PAIR` | 10 | :DE/:DF confirms intentional dual-band assignment |
| BLE cross-correlation | `CS_BLE_CORR` | 20 | Independent radio confirmation |
| Strong RSSI (> -70 dBm) | `CS_STRONG_RSSI` | 5 | Physical proximity |

All scores are additive and capped at 100.

### 6.2 Confidence tiers

| Range | Tier | Action recommended |
|-------|------|--------------------|
| 0–29 | LOW | Log only; likely background noise |
| 30–59 | PROBABLE | Log + flag for manual review |
| 60–100 | HIGH | Log + alert; high confidence of Flock camera |

### 6.3 Example score paths

```
OUI addr2 only (sleeping camera, one frame):           40  PROBABLE
OUI addr2 + wildcard probe:                            62  HIGH ✓
OUI addr2 + wildcard probe + BLE:                      82  HIGH ✓
"Flock Camera net." + LAA MAC:                         57  PROBABLE
"Flock Camera net." + LAA MAC + seq pair:              67  HIGH ✓
"Flock Camera net." + LAA MAC + BLE:                   77  HIGH ✓
"Flock Camera net." + LAA MAC + BLE + seq + RSSI:      97  HIGH ✓
OUI addr1 only (camera in deep sleep, only as dst):    18  LOW
OUI addr1 + BLE:                                       38  PROBABLE
```

### 6.4 Per-detection persistence

`FYDetection.maxConfidence` stores the **highest** confidence score observed
for a given MAC across all alerts for that MAC.  The SPIFFS session JSON now
includes `"confidence":%u` alongside each detection record, and the Flask
API receives it in every `detection` JSON line.

---

## 7 — Emitted JSON format changes

### v1 JSON line
```json
{"event":"detection","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz",
 "mac_address":"70:c9:4e:xx:xx:xx","oui":"70:c9:4e","device_name":"",
 "rssi":-72,"channel":1,"frequency":2412,"ssid":""}
```

### v2 JSON line
```json
{"event":"detection","detection_method":"wifi_wildcard_probe","protocol":"wifi_2_4ghz",
 "mac_address":"70:c9:4e:xx:xx:xx","oui":"70:c9:4e","device_name":"",
 "rssi":-72,"channel":1,"frequency":2412,"ssid":"","confidence":82}
```

### v2 JSON line — LAA SSID path ("Flock Camera net.")
```json
{"event":"detection","detection_method":"wifi_laa_ssid","protocol":"wifi_2_4ghz",
 "mac_address":"de:ad:be:9f:a2:de","oui":"laa","device_name":"",
 "rssi":-65,"channel":1,"frequency":2412,"ssid":"Flock Camera net.","confidence":77}
```

**Key changes:**
- `"confidence"` field added (0–100 integer)
- `"oui"` is `"laa"` for locally-administered MACs (instead of meaningless bytes)
- `"detection_method"` gains two new values: `"wifi_laa_ssid"`, `"wifi_wildcard_probe"`
- `"protocol"` is band-aware: `"wifi_2_4ghz"` or `"wifi_5ghz"` (future)

**Flask app update required:** Filter on `detection_method` as before; the
new `"wifi_laa_ssid"` and `"wifi_wildcard_probe"` methods should be treated
the same as `"wifi_ssid"` and `"wifi_oui_addr2"` respectively.  The
`confidence` field can be used to colour-code markers on the map.

---

## 8 — Build instructions

### WiFi-only (default, identical to v1 behaviour + all improvements)
```bash
cd flock-you-esp32
pio run -e esp32dev          # DevKit
pio run -e m5atom-lite       # M5Atom Lite
pio run -e m5atom-echo       # M5Atom Echo
pio run -e m5atom-voices3r   # Atom VoiceS3R
```

### WiFi + BLE cross-correlation (new)
```bash
pio run -e esp32dev-ble
pio run -e m5atom-lite-ble
pio run -e m5atom-echo-ble
pio run -e m5atom-voices3r-ble
```

### Upload + monitor
```bash
pio run -e m5atom-lite-ble -t upload && pio device monitor -e m5atom-lite-ble
```

### Testing mode (alert on any WiFi traffic)
```bash
pio run -e esp32dev --build-flag=-DTESTING_MODE=1 -t upload
```

---

## 9 — Future work

1. **LAA+OUI same-channel temporal correlation** — if `ALERT_LAA_SSID` and
   `ALERT_OUI_ADDR2` fire within 500 ms on the same channel, that is very
   likely both radios of the same camera unit.  Implement in `drainAlertQueue()`
   by checking channel + timestamp on the last 8 alerts.

2. **ESP32-C5 dual-band support** — add a `[env:esp32c5-dualband]` block to
   `platformio.ini` once the PlatformIO ESP32-C5 board definition stabilises,
   and extend `updateChannelMode()` to interleave ch.149 + ch.157.

3. **Standalone BLE alerts** — add `ALERT_BLE_FLOCK` for fixed installations
   where BLE alone is sufficient confirmation (e.g., a permanently-mounted
   sensor next to a known camera location).

4. **RSSI-based distance estimate** — emit an estimated distance in metres
   alongside RSSI using the free-space path-loss formula for 2.4 GHz.
   Σ(path-loss) = 40.05 + 20*log10(dist_m).  Useful for drive-test logs.

5. **Watchdog for dead OUI / no OUI cameras** — periodically emit a
   structured `{"event":"scan_complete","channel_dwell_ms":250,...}` heartbeat
   so the Flask app can distinguish "scanning, nothing found" from "device
   disconnected".

6. **Duplicate dedup across sessions** — the current dedup table (`DEDUPE_SLOTS=8`)
   is in-memory only and resets on reboot.  Persist seen MACs across reboots
   using a compact bloom filter in SPIFFS to avoid re-alerting on cameras that
   were already logged in a prior session.

7. **Port upstream's "IE fingerprint" probe-request verification as an
   ADDITIVE confidence signal (not a replacement gate)** — colonelpanichacks
   /flock-you (the upstream project this fork is based on) added a TLV-level
   802.11 Information-Element fingerprint check for Probe Request frames: it
   walks the raw IE tags and builds a signature string (IE tag numbers +
   vendor-IE tag-221 payload prefixes), then compares against a hardcoded,
   drive-tested allowlist:
   `"2,12,127,221:506f9a16030103,45,191,221:0050f208000000"`.  A probe
   request whose IE structure matches this exact signature is almost
   certainly the specific LiteOn/USI Flock-chipset firmware, not merely any
   device sharing the same OUI. Upstream uses this to fully replace its
   plain wildcard-probe check and to justify disabling `CHECK_ADDR1`/
   `CHECK_ADDR3` — flock-you-esp32 should NOT copy that replacement
   decision (our `CHECK_ADDR1`/`CHECK_ADDR3` hits already can't chirp alone
   — see `CS_OUI_ADDR1`/`CS_OUI_ADDR3` below `CHIRP_MIN_CONFIDENCE` — so the
   false-positive risk upstream is guarding against is already contained).
   Instead, port the IE-signature builder as a new `CS_IE_SIG_MATCH` bonus
   (~+15–20) added on top of the existing `ALERT_WILDCARD_PROBE` score when
   the signature matches, which should reduce OUI-collision false positives
   ("alerts where I don't see a Flock") without reducing recall. Left as
   future work because it requires porting a nontrivial TLV-parsing helper
   chain (`fyBuildFlockIeSigFromProbeBody()` and friends) with the same
   defensive "phantom overflow" / "TLV resync" handling upstream added for
   malformed/truncated ESP32 promiscuous captures.

8. **Cross-reference `jbohack/nyanBOX`'s Flock detector** (August 2026
   research) — nyanBOX is a commercial multi-tool ESP32 gadget whose Flock
   detector traces back to this same upstream codebase. Its OUI list
   (`mac_prefixes[]`) is a strict subset of `fy_oui_high[]` already (no new
   OUIs to add), and its scan design is *less* continuous than ours: it
   fully stops the WiFi radio (`esp_wifi_stop()`) for an 8 s BLE phase every
   ~16–30 s cycle, versus our 5 s/60 s (or continuous coex) time-sharing.
   Nothing worth porting from nyanBOX beyond confirming our OUI coverage is
   already ahead of it.

