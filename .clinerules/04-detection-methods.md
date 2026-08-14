# Detection Methods Reference — flock-you-esp32

This is a living reference of every detection path the firmware
implements, so agents don't have to re-derive the full picture from
scattered code each session. **Keep this file in sync whenever a
detection path is added, removed, or its scoring changes** (see the
self-updating meta-rule in `05-keep-rules-current.md`).

All detections funnel through `enqueueAlert()` → `drainAlertQueue()` →
LED flash / chirp (if `confidence >= CHIRP_MIN_CONFIDENCE`, currently 30)
/ JSON emission (`emitDetectionJSON()`) / SPIFFS session persistence.

## WiFi detections (`wifiSniffer()` in `main.cpp`, patterns in `fy_detect.h`)

| AlertType             | Trigger                                                                 | Method string      | Notes |
|-----------------------|--------------------------------------------------------------------------|--------------------|-------|
| `ALERT_OUI_ADDR2`     | 802.11 `addr2` (transmitter) matches a high-confidence Flock OUI          | `oui_addr2`        | Primary/strongest WiFi signal |
| `ALERT_OUI_ADDR1`     | `addr1` (receiver/dest) matches a high-confidence OUI, not multicast      | `oui_addr1`        | Catches cameras appearing as probe-response destinations |
| `ALERT_OUI_ADDR3`     | `addr3` (BSSID) matches, mgmt frames only, not multicast                 | `oui_addr3`        | Fallback for randomized `addr2` |
| `ALERT_WILDCARD_PROBE`| Probe Request with high/mfr-tier OUI **and** a zero-length (wildcard) SSID IE | `wildcard_probe` | Flock cameras scan with empty-SSID probes |
| `ALERT_SSID`          | Beacon/Probe-Resp/Probe-Req SSID contains a keyword (`flock`, `flocksafety`, `penguin`, `pigvision`), globally-administered MAC | `ssid` | |
| `ALERT_LAA_SSID`      | Same SSID match, but transmitter MAC is **locally-administered** (bit 1 of first octet set) | `laa_ssid` | Issue-#43 "Flock Camera net." camera class — LAA MACs never match any OUI table, so SSID is the only handle. Gets a sequential-MAC pair bonus if a `:DE`/`:DF` adjacent-channel pair is seen (`checkSeqMac()`). |
| `ALERT_OUI_MFR`       | `addr2` matches a contract-manufacturer OUI (Liteon/USI) shared with non-Flock devices | `oui_mfr` | Lower confidence (`CS_OUI_MFR=20` < `CHIRP_MIN_CONFIDENCE=30`) — logged silently, no chirp/LED alone |
| `ALERT_SOUNDTHINKING` | `addr2` matches the SoundThinking/ShotSpotter acoustic-sensor OUI          | `soundthinking`    | Often co-deployed with Flock cameras; `CS_SOUNDTHINKING=35` does chirp |

Sequential-MAC bonus: two wildcard-probe hits from the same OUI prefix
with suffix bytes `:DE` then `:DF` on adjacent channels within a short
window get a confidence bonus (`applySeqMacBonus()`, tracked in
`seqMacTable[]`, size `SEQ_MAC_TABLE_SIZE`).

## BLE detections (`fyProcessBLEAdvertisedDevice()` in `main.cpp`, only
when `ENABLE_BLE_SCAN=1`)

| AlertType             | Trigger                                                          | Method string     | Confidence |
|-----------------------|--------------------------------------------------------------------|-------------------|------------|
| `ALERT_BLE_MFR_ID`    | Manufacturer-specific data with company ID `0x09C8` (XUNTONG/Flock) | `ble_mfr_id`      | `CS_BLE_MFR_ID_STANDALONE=45` (+5 if RSSI > -70) |
| `ALERT_BLE_RAVEN_UUID`| Advertised service UUID matches one of `fy_raven_uuids[]` (128-bit Raven/Flock UUIDs) | `ble_raven_uuid` | `CS_BLE_UUID_STANDALONE=45` |
| `ALERT_BLE_NAME`      | Device name substring-matches `ble_flock_names[]` (`flock`, `penguin`, `pigvision`, `fs ext battery`, `raven`) | `ble_name` | `CS_BLE_NAME_STANDALONE=35` |

These are **standalone** alerts — a BLE-only match produces a real alert
immediately (no corroborating WiFi frame required). This was a
significant historical bug: BLE matches used to only set a timestamp for
a later WiFi-hit confidence *bonus* (`CS_BLE_CORR`, `BLE_CORR_WINDOW_MS`)
and never called `enqueueAlert()` on their own — meaning BLE-only Flock
signals were completely invisible (no LED, chirp, log line, or JSON).
Fixed; see `git log` for `fyProcessBLEAdvertisedDevice()`.

`BLE_COEX_MODE=1` (used by every `-ble` PlatformIO environment) runs
WiFi promiscuous mode and a continuous NimBLE scan simultaneously via the
ESP-IDF software coexistence scheduler, rather than time-multiplexing
(pausing WiFi to run BLE scans). See `bleCoexStart()`/`bleScanTick()`.

## Confidence scoring

All weights/thresholds live in `fy_confidence.h`
(`computeConfidence()`), with `CHIRP_MIN_CONFIDENCE` (currently 30) as the
audible/visual-alert threshold — detections below it are logged but
silent, letting low-confidence signals (e.g. `ALERT_OUI_MFR`) be recorded
without generating alert fatigue.

## Test tooling that exercises these paths

- `ble_selftest.h` (`BLE_SELF_TEST=1`, `m5atom-lite-ble-selftest` env):
  single board self-advertises the 3 BLE scenarios and picks them back up
  via its own always-on coex scan.
- `beacon_test.cpp` (`m5atom-lite-beacon` env, separate standalone
  firmware): broadcasts all 12 scenarios (9 WiFi + 3 BLE, 1:1 with the
  table above) on a rotating schedule, for testing against a SECOND board
  running the real detector — the preferred test method since it doesn't
  depend on same-radio self-reception quirks.
