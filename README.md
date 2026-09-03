# Flock-You: Promiscuous WiFi Edition

<img src="flock.png" alt="Flock You" width="300px">

**Passive 2.4 GHz promiscuous-mode detector for Flock Cam surveillance infrastructure. Runs standalone or feeds the Flask dashboard over USB for live GPS-tagged wardriving.**


> **For research & educational use only.** You assume all liability for any use or misuse of these devices — don't do anything illegal or dumb. This project is not affiliated with, endorsed by, or associated with any camera-network operator; all trademarks belong to their respective owners.

This is the `dev` branch. It carries the DeFlockJoplin Information Element research and wildcard-probe signature, and runs @NitekryDPaul's broad OUI paths alongside them under a confidence-tier system — see "Running both methods together" below.

> **Region:** Flock Cam hardware is deployed primarily in the United States (and to a lesser extent Canada). If you're outside North America the OUI list and probe-request signatures here won't match anything — the tool is still useful for research, but it's not going to find infrastructure that isn't there.

---

## Credit
Full credit to @NitekryDPaul for the initial research this work is built on. His OUI list is what makes a strong method rock solid, and none of this would exist without his original submission.

Additional credit goes to the researchers that found that information element fields are unique to devices: Lucia Pintor & Luigi Atzori. 

Pintor, Lucia & Atzori, Luigi. (2022). Analysis of Wi-Fi Probe Requests Towards Information Element Fingerprinting. 3857-3862. 10.1109/GLOBECOM48099.2022.10001618. 

---

## What this branch does

Turns a Seeed XIAO ESP32-S3 into a passive WiFi receiver that watches 2.4 GHz management and data frames for Flock Cam MAC OUIs. No AP, no transmit — the radio stays dedicated to sniffing while the device hops channels 11 / 6 / 1 (descending) at 250 ms dwell.

Every detection is:

- beeped (piezo on GPIO3) and flashed (onboard LED on GPIO21)
- written to on-device SPIFFS in an CRC-validated persistence, previous session preserved
- emitted as one JSON line over USB CDC in the schema `api/flockyou.py` expects, so the Flask dashboard auto-ingests it with GPS temporal matching

The device works standalone (no USB host needed) and plugged in (live dashboard) without any mode switch.

---

## DeFlock Joplin Research Continues - Behavior and IE Fingerprint

Earlier descriptions of Flock behavior differ from what DeFlockJoplin observed, and this firmware is based on those observations. No claim is made that other reports are wrong, or that this behavior has always existed.

**Past Flock Camera Behavior**
In the past, Flock cameras were detectable by the AP they were broadcasting for management.  Some time around December 2025, this AP was deactivated.  The community began using other detection methods like BLE.  Those stopped working some time in the spring.

**Current Camera Behavior**
These observations are not meant to cast doubt on anyone else's.  These cameras are not managed well and are updated OTA.  

Flock cameras transmit wildcard probe requests on wi-fi channels in ascending order.  These probes are emitted at around .125 second intervals (credit to nsm_barii for orginal observations of both of these).  These probes are essentially a WiFi client asking any AP to respond with the SSID of the AP.  Hidden SSIDs generally require you send the exact SSID in the probe to generate a response.  See [here](https://goodwi.fi/posts/2023/12/hunt-for-hidden-probe/).  Flock cameras are also known to use cellular LTE modems to upload to the Flock cloud.

On that basis the original branch disabled every other detection method. While they are associated with Flock cameras, they are essentially all echoes of the camera itself, which is only gated by OUI match.  The other methods can fire when any OUI matched device is sending wildcard probes because nearby APs can respond and generate a false positive match on addr1 or addr3 methods.

> **Update (`dev` branch):** those methods are back on, but demoted rather than trusted. The reasoning above is why they sit at tiers 1–2 with their own quieter tones — they are echoes, and they do misfire on addr1/addr3. The difference now is that a weak hit can't overwrite a fingerprint-confirmed one, so they cost nothing in label quality while still covering stations that never transmit during a dwell window.

The new method relies on IE fingerprinting research done by others in the past such [here](https://www.researchgate.net/publication/367065691_Analysis_of_Wi-Fi_Probe_Requests_Towards_Information_Element_Fingerprinting).  According to the linked paper, IE field detection can already be a high confidence identifier, combining it with the OUI list collected by @NitekryDPaul yields an extremely certain signature — no false positive was observed across hundreds of miles of drive-testing. Other detection methods have trended towards more active methods, but this is entirely passive. 

Descending channel hopping is enabled and the dwell time reduced to 250 ms (2x the observed hop time) to intercept a Flock signal faster.

Hypothesis on probe behavior:
When the wifi management AP was disabled around December, the devices moved from AP mode to STA mode.  The probes likely began at this time. The devices now appear to enumerating networks, perhaps as a default behavior and not something Flock has explicitly set.

The tightened signature that's active on this branch:

1. Frame is 802.11 Management, type=0 subtype=4 (**Probe Request**)
2. SSID Information Element (tag 0) is present with **length 0** (wildcard)
3. `addr2` (transmitter) matches the known-OUI list
4. IE fields match signature collected by DeFlockJoplin

A hit emits `detection_method: wifi_wildcard_probe_ie_sig`.

### Running both methods together — confidence tiers

Both detection methods run at once. The IE fingerprint is the precise one; the broad OUI matches are noisier but catch cameras the fingerprint misses. Rather than picking one, every path stays live and carries a confidence tier.

| Tier | `detection_method` | Gate | Sound |
|---|---|---|---|
| 4 | `wifi_wildcard_probe_ie_sig` | OUI + wildcard SSID + IE signature | two-note chirp, 2000→2800 Hz |
| 3 | `wifi_wildcard_probe` | OUI + wildcard SSID, IE unverified | two-note chirp, 1400→1800 Hz |
| 2 | `wifi_oui_addr2` | Flock OUI in addr2, any frame | single blip, 1200 Hz |
| 1 | `wifi_oui_addr1` / `wifi_oui_addr3` | Flock OUI in addr1 / BSSID | single blip, 800 Hz |
| 0 | `wifi_ssid` | SSID keyword (off by default) | single blip, 600 Hz |

Every tier sounds different, so the detection method is obvious by ear while driving.

The tier does two jobs beyond the sound. It decides which label sticks when several paths hit the same MAC — always the highest, and it never drops back down once a camera has been fingerprinted. And it can jump the dedupe queue: without that, a tier-1 hit fires on any frame and would beat the tier-4 path to the 5-second cooldown, hiding the confirmation for the same camera.

The dashboard badges each device with its best tier and lists the other paths that caught it underneath, with hit counts. Devices that only ever trip the broad paths and never fingerprint are the ones worth looking at.

Audio per tier can be muted live from the dashboard's **Audio** panel; detection and logging are unaffected by the switches, and the setting persists in NVS across power cycles.


---

## Detection pipeline

```
  [2.4GHz air]
       │
       ▼
  wifiSniffer()                 ← IRAM promiscuous callback (WiFi task)
       │                          fast match only, no Serial / no malloc
       │                          all tiers enqueue here
       ▼
  alertQueue[32]                ← lock-free ring buffer (ISR-safe mux)
       │
       ▼
  drainAlertQueue()             ← loop() context, per-iteration drain
       │
       ├─► fyAddDetection(tier)       ← always, every hit
       │        │                       keeps the best tier per MAC
       │        ▼
       │   fyDet[200]                 ← unique-by-MAC on-device table
       │        │
       │        ▼
       │   autosaveTick()             ← every 60s when dirty
       │        │
       │        ▼
       │   fySaveSession()            ← CRC-envelope write to SPIFFS
       │
       ├─► shouldSuppressDuplicate(mac, tier)
       │        ← 5s per-MAC rate limit; a higher tier preempts it
       │
       └─► emitDetectionJSON()        ← USB CDC line for Flask
            tierChirp(tier) + ledFlash()

  [USB CDC in]
       │
       ▼
  pollHostCommands()            ← loop(); dashboard sets the per-tier
                                  beep mask, persisted to NVS
```

The split between callback and loop is deliberate: the WiFi task has hard real-time constraints and cannot call `Serial.print` or `malloc` safely. The callback writes only to the lock-free ring buffer; `loop()` does all the heavy work.

---

## OUI target list (@NitekryDPaul research)

All lowercase, colon-separated. 32 prefixes — 31 active from @NitekryDPaul's [nite-oui-collection](https://github.com/nitekry/nite-oui-collection) as of his 2026-07-16 revision, plus 1 from DeFlockJoplin:

```
70:c9:4e   3c:91:80   d8:f3:bc   80:30:49   b8:35:32
14:5a:fc   74:4c:a1   08:3a:88   9c:2f:9d   c0:35:32
94:08:53   e4:aa:ea   f4:6a:dd   e0:0a:f6   24:b2:b9
00:f4:8d   d0:39:57   e8:d0:fc   e0:4f:43   b8:1e:a4
70:08:94   58:8e:81   ec:1b:bd   3c:71:bf   58:00:e3
90:35:ea   5c:93:a2   64:6e:69   48:27:ea   a4:cf:12
14:b5:cd
82:6b:f2   ← contributed by DeFlockJoplin
```

Changes in the 2026-07-16 sync: **removed** `f8:a2:d6` (@NitekryDPaul demoted it — hits a Sony Media Player, not a Flock device); **added** `e0:0a:f6` and `14:b5:cd`.

> Do not add a "skip locally-administered MAC" filter to the match path. `82:6b:f2` has bit 1 of the first octet set, so that rule would silently drop DeFlockJoplin's camera.

Pre-compiled into a byte table in `setup()` so the matcher stays entirely in IRAM with no flash-resident lookups during callback execution.

Full dataset and methodology: [`datasets/NitekryDPaul_wifi_ouis.md`](datasets/NitekryDPaul_wifi_ouis.md).

---

## SPIFFS wire format

On-flash layout:

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
4. `/prev_session.json` stays around for inspection

CRC32 uses the standard `0xEDB88320` polynomial so the same file can be verified on a host with any off-the-shelf CRC tool.

---

## Flask dashboard integration

The firmware emits one JSON line per detection, which `api/flockyou.py` ingests directly:

```json
{"event":"detection","detection_method":"wifi_wildcard_probe_ie_sig","detection_tier":4,"protocol":"wifi_2_4ghz","mac_address":"82:6b:f2:14:07:3a","oui":"82:6b:f2","device_name":"","rssi":-52,"channel":6,"frequency":2437,"ssid":""}
```

The device also emits a config line on boot and after every audio command, which the dashboard uses to render the per-tier switches:

```json
{"event":"config","beep_mask":31,"oui_count":32,"tiers":[{"tier":0,"method":"wifi_ssid","beep":1}, ...]}
```

`detection_method` values:

Detections carry `detection_method` and `detection_tier`; the dashboard also keeps a `methods_seen` map of every path that has caught that MAC, with per-method hit counts. All of it lands in the CSV and KML exports.

- `wifi_wildcard_probe_ie_sig` — **tier 4.** Probe Request + wildcard SSID + primary Flock IE signature from a known OUI (PACK method 2 PoC; no rolling-window gates)
- `wifi_wildcard_probe` — **tier 3.** Same wildcard/OUI gates, IE fields did not match. Either a camera running firmware that is not yet fingerprinted, or an unrelated device sharing the OUI
- `wifi_oui_addr2` — **tier 2.** Transmitter-side OUI match on any other frame
- `wifi_oui_addr1` — **tier 1.** Receiver-side OUI match (the @NitekryDPaul technique)
- `wifi_oui_addr3` — **tier 1.** BSSID OUI on mgmt frames; broad OUI-only filter, false-positive prone
- `wifi_ssid` — **tier 0.** SSID keyword match (off by default)

### Dashboard control

- `GET /api/flock/config` — current per-tier beep state, and asks the device to re-report
- `POST /api/flock/beep` — `{"tier": 0-4, "enabled": bool}` to mute one tier, or `{"mask": 0-31}` to set all five at once

- `POST /api/flock/dump_session` — `{"source": "live"}` (default) or `{"source": "prev"}`; asks the device to stream its detection table so a standalone run can be pulled into the dashboard after the fact

Commands go to the device as one JSON line over the same USB CDC link the detections come back on. The device answers with an `{"event":"config"}` line, which Flask relays to the browser over the `device_config` socket event — so the switches follow the device's actual state, not the click.

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

Open `http://localhost:5000`, pick your serial port from the UI, detections start showing up live.

---

## Hardware

### Seeed XIAO ESP32-S3 (default: `xiao_esp32s3`)

| Pin | Function |
|-----|----------|
| GPIO 3 | Piezo buzzer |
| GPIO 21 | Onboard user LED (active low) |
| GPIO 43 | Serial1 TX mirror (115200 baud) |

### LilyGO T-Dongle S3 (`BOARD_LILYGO_T_DONGLE_S3`) — no build env yet, see Build and flash

| Pin | Function |
|-----|----------|
| GPIO 1–5, 38 | ST7735 display (RST, DC, MOSI, CS, SCLK, backlight) |
| GPIO 39 / 40 | APA102 RGB LED (clock / data) — red flash on detection |
| USB CDC | Serial JSON for Flask dashboard |

**Display:** idle screen shows `SCANNING`, current WiFi channel, and unique hit count. On each emitted detection, shows `DETECT`, method, MAC, RSSI, and channel for **5 s**, then returns to idle. Backlight is driven on init (GPIO 38 active-low).

No buzzer on this env.

Boot sound (XIAO only): first 6 notes of Super Mario Bros. World 1-2 (underground).

### Detection audio

Each tier has its own tone (see the table above), so the detection method is clear without looking at the screen. Individual tiers can be muted from the dashboard's **Audio** panel; muting affects the buzzer only, and detections still log and export. The mask lives in NVS under `flockyou/beepmask` and survives a power cycle.

The heartbeat uses the tone of the strongest tier currently in range, so muting a tier silences its heartbeat too.

---

## Build and flash

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -e xiao_esp32s3              # build
pio run -e xiao_esp32s3 -t upload    # flash
pio device monitor
```

`platformio.ini` and `partitions.csv` are at the root: 6 MB app, 1.94 MB SPIFFS, and a 20 KB `nvs` partition that holds the per-tier beep mask. XIAO needs no extra libraries.

> **T-Dongle S3:** `platformio.ini` on this branch defines only `xiao_esp32s3`. The `display_dongle.cpp` source is present but excluded by `build_src_filter`, and there is no `lilygo_t_dongle_s3` env, so the T-Dongle cannot be built here yet. Tracked in [#50](https://github.com/colonelpanichacks/flock-you/issues/50) with a proposed env in [#48](https://github.com/colonelpanichacks/flock-you/pull/48).

---

## Config cheatsheet (top of `main.cpp`)

| Define | Default | Notes |
|---|---|---|
| `CHANNEL_MODE` | `CHANNEL_MODE_CUSTOM` | `CUSTOM` (11/6/1 desc), `FULL_HOP` (11-1 desc), or `SINGLE` |
| `CHANNEL_DWELL_MS` | 250 | Time on each channel before hop (2x the observed 125 ms camera hop) |
| `RSSI_MIN` | -95 | Drop frames weaker than this |
| `ALERT_COOLDOWN_MS` | 5000 | Per-MAC serial-emit rate limit; a higher tier preempts it |
| `CHECK_ADDR1` | 1 | Receiver-side OUI — @NitekryDPaul's addr1 technique (tier 1) |
| `CHECK_ADDR3` | 1 | BSSID OUI fallback (tier 1) |
| `BEEP_MASK_DEFAULT` | `0x1F` | Bit N = tier N audible. All five on; overridden by NVS once the dashboard sets it |
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

**Pulling an offline run into the dashboard:** plug the device in afterwards, connect it in the dashboard, and use **Import → Pull current session from device** (the table accumulated since this boot) or **Pull previous session from device** (`/prev_session.json`, the run before the last power cycle). The device answers `{"cmd":"dump_session","source":"live|prev"}` with a `session_begin` line, one `session_det` line per device in the same shape as the SPIFFS records, and a `session_end` line; Flask imports each record as it arrives and reports progress over the `session_dump` socket event. Records carry the on-device hit count but not wall-clock time (the device has no RTC), so imported detections are timestamped at import and get no GPS match.

---

## Scope: WiFi only

This firmware and dashboard are 2.4 GHz WiFi only. BLE detection stopped working in spring 2026, so the BLE paths — stat counters, map markers and import defaults — are gone rather than left reporting zeroes. Everything is `protocol: wifi_2_4ghz`.

---

## Acknowledgments

- **OrdoOuroboros (@NitekryDPaul**, [nitekry/nite-oui-collection](https://github.com/nitekry/nite-oui-collection)**)** — **WiFi promiscuous detection research**: the Flock Cam OUI target list (31 active prefixes as of his 2026-07-16 revision) and the addr1-receiver detection technique that are the baseline of this firmware. The code here is a mod of his original work.
- **DeFlockJoplin** ([DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you), [deflockjoplin.today](https://deflockjoplin.today)) — **wildcard-probe-request signature**, the **IE fingerprint** path, and the `82:6b:f2` OUI. Drive-tested in Joplin to 11/12 cameras caught with only 2 false positives.
- **[DeFlock](https://deflock.me)** ([FoggedLens/deflock](https://github.com/FoggedLens/deflock)) — crowdsourced ALPR location data and detection methodologies. Datasets included in `datasets/`

---

## OUI-SPY Firmware Ecosystem

Flock-You is part of the OUI-SPY firmware family:

| Firmware | Description | Board |
|----------|-------------|-------|
| **[OUI-SPY Unified](https://github.com/colonelpanichacks/oui-spy-unified-blue)** | Multi-mode BLE + WiFi detector | ESP32-S3 / ESP32-C5 |
| **[OUI-SPY Detector](https://github.com/colonelpanichacks/ouispy-detector)** | Targeted BLE scanner with OUI filtering | ESP32-S3 |
| **[OUI-SPY Foxhunter](https://github.com/colonelpanichacks/ouispy-foxhunter)** | RSSI-based proximity tracker | ESP32-S3 |
| **[Flock You](https://github.com/colonelpanichacks/flock-you)** | Flock Cam surveillance detection (this project) | ESP32-S3 |
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
