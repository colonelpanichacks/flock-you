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
  depend on same-radio self-reception quirks. Each WiFi scenario is sent
  via `txSweep()`, which repeats the {1,6,11} channel sweep
  `SWEEP_PASSES` times (currently 6, ~576 ms total burst) so a single
  scenario firing is long enough to overlap a real detector's channel-hop
  dwell window — see "WiFi channel hopping & channel lock" below for why
  this matters.

## WiFi channel hopping & channel lock

The WiFi radio can only listen on one 2.4 GHz channel at a time, so
`updateChannelMode()` hops the promiscuous-mode channel on a timer
(`CHANNEL_DWELL_MS`, currently 100 ms per channel) across `{11, 6, 1}`
(Flock's observed primaries), giving a full rotation period of ~300 ms.
`CHANNEL_DWELL_MS` was previously 250 ms (750 ms/rotation) — lowered
after cross-device `beacon_test.cpp` testing showed short-lived bursts
could land entirely within a dwell window on the *wrong* channel and be
missed for that hop cycle with no second chance. Any transmitter whose
signal duration is shorter than a full rotation period risks being missed
purely due to this timing, independent of matching-logic correctness —
this is why `beacon_test.cpp`'s `SWEEP_PASSES` was also raised (see
above): the tester's burst duration must exceed the detector's rotation
period for a reliable test.

**Channel lock** (`maybeLockChannel()`, called from `drainAlertQueue()`):
once a chirp-worthy WiFi detection fires (`confidence >=
CHIRP_MIN_CONFIDENCE`), the detector stops hopping and locks
`currentChannel` to the exact channel the hit came in on
(`channelLockActive = true`), so it can keep receiving frames from a
camera we KNOW is live right now instead of spending 2/3 of its time on
channels with nothing confirmed. The lock releases automatically
(`updateChannelMode()`) after `CHANNEL_LOCK_TIMEOUT_MS` (5000 ms) of no
fresh qualifying hit on that channel, resuming normal hop. BLE detections
never trigger or interact with channel lock — `e.channel` is meaningless
for BLE alerts (`ALERT_BLE_MFR_ID`/`ALERT_BLE_RAVEN_UUID`/`ALERT_BLE_NAME`
are explicitly excluded in `maybeLockChannel()`), and `BLE_COEX_MODE`'s
NimBLE scan runs independently of `currentChannel` regardless.

Hardware-validated (two-board cross-device test, `m5atom-lite-beacon` →
`m5atom-lite-ble`): channel lock engages/releases correctly with clean
5-second-timeout cycles and no freezes/reboots observed across two
independent ~90-second runs.

## Known open issue: some alert types under-detected in cross-device testing

During the same two-board hardware validation runs referenced above, the
following alert types were **never** caught despite `beacon_test.cpp`
firing their scenarios repeatedly (0 hits out of ~17 combined fires
across both runs): `ALERT_OUI_ADDR1`, `ALERT_OUI_ADDR2`, `ALERT_OUI_ADDR3`,
`ALERT_LAA_SSID`, the `SEQ_MAC_PAIR_BONUS` on top of
`ALERT_WILDCARD_PROBE`, `ALERT_BLE_RAVEN_UUID`, and `ALERT_BLE_NAME`. In
the same runs, `ALERT_WILDCARD_PROBE`, `ALERT_SSID`, `ALERT_OUI_MFR`,
`ALERT_SOUNDTHINKING`, and `ALERT_BLE_MFR_ID` were all reliably caught
using structurally similar code paths.

Two independent full code-review passes (across separate sessions) of
`wifiSniffer()`'s addr1/addr2/addr3/SSID gating, `fyProcessBLEAdvertisedDevice()`'s
mfr-ID/UUID/name matching, `fy_confidence.h`'s OUI/sequential-MAC logic,
and `beacon_test.cpp`'s/`beacon_frames.h`'s frame construction (confirmed
byte-layout-compatible with `wifiSniffer()`'s parsing offsets) **found no
coding bug** that would explain this specific pattern. The channel-lock
feature was also specifically checked as a possible new cause and ruled
out, since `txSweep()` always sweeps all three channels regardless of
which channel the detector is currently dwelling/locked on.

This remains an **open, unresolved limitation** as of this writing. It is
flagged here rather than silently left for a future session to
re-discover from scratch. Suspected candidate causes not yet
instrumented/tested:
- BLE scenarios (`ALERT_BLE_RAVEN_UUID`, `ALERT_BLE_NAME`): possible NimBLE
  scan-window/advertisement-interval timing mismatch analogous to the
  WiFi dwell-vs-burst issue above, or an issue specific to
  `bleAdvertiseAndHold()`'s advertisement parameters not reliably
  reaching the scanner during its active NimBLE scan window.
- WiFi addr1/addr3/LAA-SSID/seq-mac scenarios: possible RSSI or antenna
  orientation effects specific to those frame subtypes/scenarios, an
  as-yet-unfound edge case in `beacon_test.cpp`'s `pickRandomOuiGA()` or
  the hardcoded LAA MAC/SSID in scenario5, or a real-world timing
  interaction not reproduced by static code review.
- Recommended next diagnostic step for a future session: add temporary
  frame/advertisement-arrival instrumentation (e.g. a promiscuous-mode
  packet counter surfaced via the periodic heartbeat log, independent of
  the matching logic) to distinguish "frame never physically arrived at
  the radio" from "frame arrived but failed to match" — this would
  conclusively separate a timing/RF root cause from a logic bug without
  further blind code review.
