# Flock-You: Promiscuous WiFi Edition (`promiscious` branch)

<img src="flock.png" alt="Flock You" width="300px">

**Passive 2.4 GHz promiscuous-mode detector for Flock Safety surveillance infrastructure. Runs standalone or feeds the Flask dashboard over USB for live GPS-tagged wardriving.**

> **Region:** Flock Safety hardware is deployed primarily in the United States (and to a lesser extent Canada). If you're outside North America the OUI list and probe-request signatures here won't match anything — the tool is still useful for research, but it's not going to find infrastructure that isn't there.

---

## Credit

All WiFi promiscuous detection research — the **39-OUI Flock Safety target list**, the **promiscuous-mode strategy**, and the **addr1-receiver detection technique** — is the work of **OrdoOuroborous / @NitekryDPaul** (GitHub [@nitekry](https://github.com/nitekry)). The firmware here is a mod of his original work with added SPIFFS persistence and Flask-dashboard integration. Upstream OUI source: [nitekry/nite-oui-collection](https://github.com/nitekry/nite-oui-collection). Full research writeup: [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md).

Additional research credit to **DeFlockJoplin** for the **wildcard-probe-request signature** and OUI `82:6b:f2`. Field-tested to 11/12 cameras caught with only 2 false positives in Joplin. Source: [DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you).

---

## What this branch does

Turns a Seeed XIAO ESP32-S3 into a passive WiFi receiver that watches 2.4 GHz management and data frames for Flock Safety MAC OUIs. No AP, no transmit — the radio stays dedicated to sniffing while the device hops channels 1 / 6 / 11 at 350 ms dwell.

Every detection is:

- beeped (piezo on GPIO3) and flashed (onboard LED on GPIO21)
- written to on-device SPIFFS in an atomic CRC-envelope format, surviving power loss
- emitted as one JSON line over USB CDC in the schema `api/flockyou.py` expects, so the Flask dashboard auto-ingests it with GPS temporal matching

The device works standalone (no USB host needed) and plugged in (live dashboard) without any mode switch.

---

## Why promiscuous mode, and why `addr1`

Most WiFi sniffers only check the transmitter address (`addr2`). Flock infrastructure spends most of its duty cycle **asleep** — it wakes briefly in bursts, uploads, then sleeps again. During the silence it may never transmit a single frame in your capture window.

But it may still appear on the air as the **destination** (`addr1`) of probe responses or data frames from nearby APs.

Checking `addr1` in addition to `addr2` picks those silent stations up. It requires two guards to avoid false positives:

- `addr1` is broadcast (`ff:ff:ff:ff:ff:ff`) in beacons and broadcasts — **multicast filter**
- Modern devices use randomised (locally-administered) MACs that can't be fingerprinted by OUI — **randomised-MAC filter** on byte 0 bit 1

Both are applied before the OUI match. This whole approach, including the 39-OUI baseline list (the 40th is DeFlockJoplin's), is **@NitekryDPaul's research**.

---

## Further research — the wildcard-probe signature (DeFlockJoplin)

DeFlockJoplin used the OUI + addr1/addr2/addr3 work above as a starting point and characterised what Flock cameras actually do on the air. His finding:

> The cameras are hopping channels and sending out a wildcard WiFi probe request on every channel. This specific type of request combined with OUI matching has created what seems to be a fairly unique signature.

His drive-test in Joplin caught **11 of 12 cameras** with only **2 false positives**. The 12th camera was doing the same wildcard-probe behaviour but with an OUI (`82:6b:f2`) that wasn't in @NitekryDPaul's original set — it's now in our list, credited to him.

The tightened signature that's active on this branch:

1. Frame is 802.11 Management, type=0 subtype=4 (**Probe Request**)
2. SSID Information Element (tag 0) is present with **length 0** (wildcard)
3. `addr2` (transmitter) matches the known-OUI list

When all three hit, we emit `detection_method: wifi_wildcard_probe` — the high-precision class. Non-probe frames from the same OUIs still emit `wifi_oui_addr2`, and the `addr1` receiver-side sleeper-catch still runs independently.

His proof-of-concept firmware (different enough we're not just pulling it in wholesale, but the core idea carried over cleanly): [DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you). The wildcard-probe analysis is his; we ported the detection into this firmware and kept our SPIFFS persistence, Flask JSON emission, and audio/LED feedback on top.

---

## Detection pipeline

```
  [2.4GHz air]
       │
       ▼
  wifiSniffer()                 ← IRAM promiscuous callback (WiFi task)
       │                          fast match only, no Serial / no malloc
       ▼
  alertQueue[32]                ← lock-free ring buffer (ISR-safe mux)
       │
       ▼
  drainAlertQueue()             ← loop() context, per-iteration drain
       │
       ├─► fyAddDetection()           ← always, every hit
       │        │
       │        ▼
       │   fyDet[200]                 ← unique-by-MAC on-device table
       │        │
       │        ▼
       │   autosaveTick()             ← every 60s when dirty
       │        │
       │        ▼
       │   fySaveSession()            ← atomic CRC-envelope write to SPIFFS
       │
       ├─► shouldSuppressDuplicate()  ← 5s per-MAC serial-emit rate limit
       │
       └─► emitDetectionJSON()        ← USB CDC line for Flask
            buzzerBeep() + ledFlash()
```

The split between callback and loop is deliberate: the WiFi task has hard real-time constraints and cannot call `Serial.print` or `malloc` safely. The callback writes only to the lock-free ring buffer; `loop()` does all the heavy work.

---

## OUI target list (@NitekryDPaul research)

All lowercase, colon-separated. 40 Flock Safety infrastructure prefixes —
29 from @NitekryDPaul's original set, 10 from his April 2026 additions, plus
1 from DeFlockJoplin. `f8:a2:d6` from the original set has been
demoted as a Sony Media Player false positive, and `94:2a:6f` / `f4:e2:c6`
from the April 2026 additions have been demoted as Ubiquiti false positives
per @NitekryDPaul's June 2026 update (see
[`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md)).

```
70:c9:4e   3c:91:80   d8:f3:bc   80:30:49   b8:35:32
14:5a:fc   74:4c:a1   08:3a:88   9c:2f:9d   c0:35:32
94:08:53   e4:aa:ea   f4:6a:dd   24:b2:b9   00:f4:8d
d0:39:57   e8:d0:fc   e0:4f:43   b8:1e:a4   70:08:94
58:8e:81   ec:1b:bd   3c:71:bf   58:00:e3   90:35:ea
5c:93:a2   64:6e:69   48:27:ea   a4:cf:12
04:0d:84   f0:82:c0   1c:34:f1   38:5b:44   94:34:69   ← Apr 2026 adds
b4:e3:f9   b4:1e:52   14:b5:cd
d4:11:d6   e0:0a:f6
82:6b:f2   ← contributed by DeFlockJoplin
```

Pre-compiled into a byte table in `setup()` so the matcher stays entirely in IRAM with no flash-resident lookups during callback execution.

Full dataset and methodology: [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md).

---

## SPIFFS wire format

On-flash layout, atomic and crash-safe:

```
Line 1: {"v":1,"count":N,"bytes":B,"crc":"0xXXXXXXXX"}
Line 2: [{"mac":"...","method":"...","rssi":...,...},...]
```

Save procedure:

1. Compute CRC32 + byte count over the serialised payload
2. Write envelope header + payload to `/session.tmp`
3. Re-read and re-validate `/session.tmp` (CRC check)
4. Remove `/session.json`
5. Atomic rename `/session.tmp` → `/session.json` (copy+delete fallback)

Boot recovery:

1. If `/session.json` validates, promote it to `/prev_session.json`
2. Otherwise try `/session.tmp` (interrupted save)
3. Delete both working files, start with an empty live table
4. `/prev_session.json` stays around — host pulls it via `CMD:DUMP_PREV` (see "Host command protocol" below)

CRC32 uses the standard `0xEDB88320` polynomial so the same file can be verified on a host with any off-the-shelf CRC tool.

---

## Flask dashboard integration

The firmware emits one JSON line per detection in the same schema the BLE detector uses, so `api/flockyou.py` picks it up with zero changes:

```json
{"event":"detection","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":""}
```

`detection_method` values:

- `wifi_wildcard_probe` — **Probe Request + wildcard SSID from a known OUI** (the DeFlockJoplin high-precision signature). When this fires, the `addr2` broad alert is suppressed for the same frame to avoid double-counting.
- `wifi_oui_addr2` — transmitter-side OUI match on any non-probe frame
- `wifi_oui_addr1` — **receiver-side OUI match** (the @NitekryDPaul technique)
- `wifi_oui_addr3` — BSSID OUI match (mgmt frames only; disabled by default)
- `wifi_ssid` — SSID keyword match (disabled by default)

### Host command protocol

The firmware also accepts line-delimited ASCII commands on the same
USB-CDC port so Flask (or any host) can pull stored detections, query
device status, or wipe state without re-flashing. All commands are
terminated with `\n`; every reply is a single JSON object on its own
line, matching the existing `{"event":...}` schema.

| Command | Reply event | Notes |
|---|---|---|
| `CMD:STATUS`     | `status`           | Live counters: `fy_det`, `oui_count`, `spiffs`, `prev_session`, `uptime_ms`, `free_heap`, `channel`, `rssi_min` |
| `CMD:VERSION`    | `version`          | Firmware identifier + compile-time constants (`oui_count`, `max_detections`, `autosave_ms`) |
| `CMD:DUMP_LIVE`  | N × `detection` then `replay_complete` | Streams the current in-RAM detection table; each line has `"replay":true,"replay_source":"live"` |
| `CMD:DUMP_PREV`  | N × `detection` then `replay_complete` | Same shape but reads `/prev_session.json` from SPIFFS — i.e. what the device caught before the last reboot |
| `CMD:CLEAR_LIVE` | `clear`            | Empties `fyDet[]`; the next autosave overwrites the persisted session |
| `CMD:CLEAR_PREV` | `clear`            | Deletes `/prev_session.json` and any leftover `/session.tmp` |

A replayed detection line:

```json
{"event":"detection","replay":true,"replay_source":"prev","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":"","detection_count":17,"device_first_ms":12345678,"device_last_ms":18900000}
```

`device_first_ms` / `device_last_ms` are the device's monotonic millis at
the time of recording — useful for ordering, but not wall-clock. Flask
treats replayed entries as historical (`timestamp_source: device_replay`),
skips GPS temporal matching, and does not overwrite a fresher live entry
for the same MAC.

Flask exposes the protocol as REST endpoints:

| Endpoint | Method | Sends | Returns when |
|---|---|---|---|
| `/api/flock/status`     | GET  | `CMD:STATUS`     | `status` event arrives |
| `/api/flock/version`    | GET  | `CMD:VERSION`    | `version` event arrives |
| `/api/flock/dump_prev`  | POST | `CMD:DUMP_PREV`  | `replay_complete` arrives (or 30 s timeout) |
| `/api/flock/dump_live`  | POST | `CMD:DUMP_LIVE`  | `replay_complete` arrives (or 30 s timeout) |
| `/api/flock/clear_prev` | POST | `CMD:CLEAR_PREV` | `clear` event arrives |
| `/api/flock/clear_live` | POST | `CMD:CLEAR_LIVE` | `clear` event arrives |

The typical "I just plugged the device back in after wardriving" workflow:

```bash
curl -X POST http://localhost:5000/api/flock/dump_prev
curl -X POST http://localhost:5000/api/flock/clear_prev
```

The first call pulls everything the device caught since you last had it
connected and adds it to the cumulative dataset; the second wipes the
file from SPIFFS so the next run starts clean.

### GPS wardriving

GPS is handled Flask-side, since the ESP32 radio is dedicated to sniffing and there's no on-device AP. Three options, all selectable from the dashboard's GPS **source** dropdown:

- **Serial NMEA** — a USB NMEA puck plugged into the host running Flask; Flask reads NMEA at 9600 baud and timestamps a GPS timeline
- **gpsd** — connect to a running `gpsd` daemon over TCP (default `localhost:2947`, host and port are configurable in the dashboard). Uses gpsd's line-delimited JSON protocol directly; no extra Python client library needed. Handy when several tools already share the same GPS via gpsd.
- **Browser Geolocation** — the Flask dashboard open in a phone browser; the browser Geolocation API posts updates to Flask

Flask does a temporal match between detection timestamp and GPS timeline, then exports JSON / CSV / KML for Google Earth.

### Running Flask

```bash
cd api
pip install -r requirements.txt
python flockyou.py
```

Open `http://localhost:5000`, pick your serial port from the **Sniffer** dropdown, click **Connect**. Detections start showing up live.

### Dashboard command bar

Once the Sniffer is connected, five buttons appear next to the connect controls:

| Button | Firmware command | What it does |
|---|---|---|
| **Pull Prev**  | `CMD:DUMP_PREV`  | Replays `/prev_session.json` (last boot's persisted detections) into the dashboard; entries get a purple **FLASH** badge |
| **Pull Live**  | `CMD:DUMP_LIVE`  | Replays the device's in-RAM detection table; entries get a blue **RAM** badge |
| **Status**     | `CMD:STATUS`     | Toasts a compact `det=N ouis=N prev=yes ch=N heap=KKB up=Ns` line |
| **Clear Prev** | `CMD:CLEAR_PREV` | Deletes `/prev_session.json` on the device (confirmation prompt) |
| **Clear Live** | `CMD:CLEAR_LIVE` | Wipes the device's in-RAM table (confirmation prompt) |

Replay detections are visually distinct — purple/blue badges next to the detection-method label, a subtle left-border tint on the card, and `timestamp_source: device_replay`. Replays don't get GPS temporal matching (the device's stored entries only have monotonic millis, not wall-clock) and never overwrite a fresher live entry for the same MAC. Every command response surfaces as a coloured top-right toast.

The dashboard is fully documented at [`api/README.md`](api/README.md) — endpoints, socket events, JSON wire formats, GPS setup, persistence layout, troubleshooting.

---

## Hardware

**Board:** Seeed Studio XIAO ESP32-S3

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 21 | Onboard user LED (active low) |
| GPIO 43 | Serial1 TX mirror (115200 baud) |

Boot sound: first 6 notes of Super Mario Bros. World 1-2 (underground).

---

## Build and flash

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                     # build
pio run -t upload           # flash
pio device monitor          # serial output
```

`platformio.ini` and `partitions.csv` are at the root (1.9 MB SPIFFS partition, 6 MB app). No extra libraries needed beyond the Arduino-ESP32 core that ships with the espressif32 platform.

---

## Config cheatsheet (top of `main.cpp`)

| Define | Default | Notes |
|---|---|---|
| `CHANNEL_MODE` | `CHANNEL_MODE_CUSTOM` | `CUSTOM` (1/6/11), `FULL_HOP` (1-11), or `SINGLE` |
| `CHANNEL_DWELL_MS` | 350 | Time on each channel before hop |
| `RSSI_MIN` | -95 | Drop frames weaker than this |
| `ALERT_COOLDOWN_MS` | 5000 | Per-MAC serial-emit rate limit |
| `CHECK_ADDR1` | 1 | The @NitekryDPaul receiver-side technique |
| `CHECK_ADDR3` | 0 | BSSID fallback (mgmt frames only) |
| `ENABLE_SSID_MATCH` | 0 | Substring match against `target_ssid_keywords[]` |
| `PROCESS_MGMT_FRAMES` | 1 | Beacons, probe req/resp, etc. |
| `PROCESS_DATA_FRAMES` | 1 | Data frames (where addr1 catch shines) |
| `MAX_DETECTIONS` | 200 | On-device table cap |
| `AUTOSAVE_INTERVAL_MS` | 60000 | SPIFFS save cadence |
| `LED_PIN` | 21 | Onboard user LED |
| `BUZZER_PIN` | 3 | Piezo |

---

## Standalone vs connected

**Without USB:** device boots, plays the SMB 1-2 intro, starts scanning, stores every unique detection to SPIFFS, flashes the onboard LED on each hit. Plug in later — the prior session is sitting in `/prev_session.json`.

**With USB + Flask running:** same thing, plus every detection streams live to the dashboard as a JSON line. Flask adds GPS (if configured) and deduplicates across MAC, building the wardriving map as you move.

Both modes work simultaneously — the SPIFFS write path doesn't care if a host is listening.

---

## BLE companion firmware

The BLE-only sibling of this firmware lives on the [`main` branch](https://github.com/colonelpanichacks/flock-you/tree/main). It detects Flock and Raven gear via BLE advertisements (OUI prefix, device name, manufacturer ID `0x09C8`, Raven service UUIDs), runs its own WiFi AP with a phone-facing dashboard at `192.168.4.1`, and emits the same Flask JSON schema. Flash both on separate boards for overlapping BLE + WiFi coverage feeding one Flask dashboard.

---

## Acknowledgments

- **OrdoOuroborous (@NitekryDPaul, GitHub [@nitekry](https://github.com/nitekry))** — **WiFi promiscuous detection research**: the 39-OUI Flock Safety target list and the addr1-receiver detection technique that are the baseline of this firmware. The code here is a mod of his original work. Upstream OUI tracking: [nite-oui-collection](https://github.com/nitekry/nite-oui-collection).
- **DeFlockJoplin** ([DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you), [deflockjoplin.today](https://deflockjoplin.today)) — **wildcard-probe-request signature** + OUI `82:6b:f2`. Drive-tested in Joplin to 11/12 cameras caught with only 2 false positives.
- **Will Greenberg** ([@wgreenberg](https://github.com/wgreenberg)) — BLE manufacturer company ID detection (`0x09C8` XUNTONG) sourced from his [flock-you](https://github.com/wgreenberg/flock-you) fork (used by the BLE companion on `main`)
- **[DeFlock](https://deflock.me)** ([FoggedLens/deflock](https://github.com/FoggedLens/deflock)) — crowdsourced ALPR location data and detection methodologies. Datasets included in `datasets/`
- **[GainSec](https://github.com/GainSec)** — Raven BLE service UUID dataset (`raven_configurations.json`) used by the BLE companion

---

## OUI-SPY Firmware Ecosystem

Flock-You is part of the OUI-SPY firmware family:

| Firmware | Description | Board |
|----------|-------------|-------|
| **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** | Multi-mode BLE + WiFi detector | ESP32-S3 / ESP32-C5 |
| **[OUI-SPY Detector](https://github.com/colonelpanichacks/ouispy-detector)** | Targeted BLE scanner with OUI filtering | ESP32-S3 |
| **[OUI-SPY Foxhunter](https://github.com/colonelpanichacks/ouispy-foxhunter)** | RSSI-based proximity tracker | ESP32-S3 |
| **[Flock You](https://github.com/colonelpanichacks/flock-you)** | Flock Safety / Raven surveillance detection (this project) | ESP32-S3 |
| **[Sky-Spy](https://github.com/colonelpanichacks/Sky-Spy)** | Drone Remote ID detection | ESP32-S3 / ESP32-C5 |
| **[Remote-ID-Spoofer](https://github.com/colonelpanichacks/Remote-ID-Spoofer)** | WiFi Remote ID spoofer & simulator with swarm mode | ESP32-S3 |
| **[OUI-SPY UniPwn](https://github.com/colonelpanichacks/Oui-Spy-UniPwn)** | Unitree robot exploitation system | ESP32-S3 |

---

## Author

**colonelpanichacks**

**Oui-Spy devices available at [colonelpanic.tech](https://colonelpanic.tech)**

---

## License

MIT — see [`LICENSE`](LICENSE). Free to fork, modify, and redistribute; upstream research credits above should be preserved.

---

## Disclaimer

Passive reception of publicly-broadcast 802.11 frames for security research, privacy auditing, and education. The device does not transmit and does not authenticate to any network. Detecting the presence of surveillance hardware in public spaces is legal in most jurisdictions; always comply with local laws regarding wireless reception.
