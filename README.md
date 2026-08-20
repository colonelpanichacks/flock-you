# Flock-You: Promiscuous WiFi Edition

<img src="flock.png" alt="Flock You" width="300px">

**Passive 2.4 GHz promiscuous-mode detector for Flock Cam surveillance infrastructure. Runs standalone or feeds the Flask dashboard over USB for live GPS-tagged wardriving.**


> **For research & educational use only.** You assume all liability for any use or misuse of these devices — don't do anything illegal or dumb. This project is not affiliated with, endorsed by, or associated with any camera-network operator; all trademarks belong to their respective owners.

This is the `main` branch — stable line. Development happens on `promiscious-dev`, which adds the DeFlockJoplin Information Element research and wildcard-probe signature on top of this baseline. See "Further research" below.

> **Region:** Flock Cam hardware is deployed primarily in the United States (and to a lesser extent Canada). If you're outside North America the OUI list and probe-request signatures here won't match anything — the tool is still useful for research, but it's not going to find infrastructure that isn't there.

---

## Credit
Full credit to @NitekryDPaul for the initial research that sparked my research.  His OUI list helps make a strong method rock solid. None of this would be here without his original submission.  

Additional credit goes to the researchers that found that information element fields are unique to devices: Lucia Pintor & Luigi Atzori. 

Pintor, Lucia & Atzori, Luigi. (2022). Analysis of Wi-Fi Probe Requests Towards Information Element Fingerprinting. 3857-3862. 10.1109/GLOBECOM48099.2022.10001618. 

---

## What this branch does

Turns a Seeed XIAO ESP32-S3 into a passive WiFi receiver that watches 2.4 GHz management and data frames for Flock Cam MAC OUIs. No AP, no transmit — the radio stays dedicated to sniffing while the device hops channels 11 / 6 / 1 (descending) at 350 ms dwell.

Every detection is:

- beeped (piezo on GPIO3) and flashed (onboard LED on GPIO21)
- written to on-device SPIFFS in an CRC-validated persistence, previous session preserved
- emitted as one JSON line over USB CDC in the schema `api/flockyou.py` expects, so the Flask dashboard auto-ingests it with GPS temporal matching

The device works standalone (no USB host needed) and plugged in (live dashboard) without any mode switch.

---

## DeFlock Joplin Research Continues - Behavior and IE Fingerprint

Earlier descriptions of Flock behavior are different than what I have observed.  This firmware is based on my observations. I do not make claims that others are incorrect or that this behavior has always existed.

**Past Flock Camera Behavior**
In the past, Flock cameras were detectable by the AP they were broadcasting for management.  Some time around December 2025, this AP was deactivated.  The community began using other detection methods like BLE.  Those stopped working some time in the spring.

**Current Camera Behavior**
These observations are not meant to cast doubt on anyone else's.  These cameras are not managed well and are updated OTA.  

Observations:
Flock cameras transmit wildcard probe requests on wi-fi channels in ascending order.  These probes are emitted at around .125 second intervals (credit to nsm_barii for orginal observations of both of these).  These probes are essentially a WiFi client asking any AP to respond with the SSID of the AP.  Hidden SSIDs generally require you send the exact SSID in the probe to generate a response.  See [here](https://goodwi.fi/posts/2023/12/hunt-for-hidden-probe/).  Flock cameras are also known to use cellular LTE modems to upload to the Flock cloud.

Given this, I have disabled all other detection methods in this branch.  While they are associated with Flock cameras, they are essentially all echoes of the camera itself, which is only gated by OUI match.  The other methods can fire when any OUI matched device is sending wildcard probes because nearby APs can respond and generate a false positive match on addr1 or addr3 methods.

The new method relies on IE fingerprinting research done by others in the past such [here](https://www.researchgate.net/publication/367065691_Analysis_of_Wi-Fi_Probe_Requests_Towards_Information_Element_Fingerprinting).  According to the linked paper, IE field detection can already be a high confidence identifier, but we combine it with the OUI list collected by @NitekryDPaul for an extremely certain signature.  To the point that I have not experience a false postive after hundreds of miles of driving. Other detection methods have trended towards more active methods, but this is entirely passive. 

I have also enabled descending channel hopping and reduced dwell time to 250 ms (2x observed hop time) to assist with faster intercept with a Flock signal.

Hypothesis on probe behavior:
When the wifi management AP was disabled around December, the devices moved from AP mode to STA mode.  The probes likely began at this time. The devices now appear to enumerating networks, perhaps as a default behavior and not something Flock has explicitly set.

The tightened signature that's active on this branch:

1. Frame is 802.11 Management, type=0 subtype=4 (**Probe Request**)
2. SSID Information Element (tag 0) is present with **length 0** (wildcard)
3. `addr2` (transmitter) matches the known-OUI list
4. IE fields match signature collected by DeFlockJoplin

When we get a hit, we emit `detection_method: wifi_wildcard_probe_ie_sig`. Broad OUI paths (`wifi_wildcard_probe`, `wifi_oui_addr2`, `wifi_oui_addr1`, `wifi_oui_addr3`) are disabled in firmware — IE fingerprint on uplink probe requests is the active detection path.


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
       │   fySaveSession()            ← CRC-envelope write to SPIFFS
       │
       ├─► shouldSuppressDuplicate()  ← 5s per-MAC serial-emit rate limit
       │
       └─► emitDetectionJSON()        ← USB CDC line for Flask
            buzzerBeep() + ledFlash()
```

The split between callback and loop is deliberate: the WiFi task has hard real-time constraints and cannot call `Serial.print` or `malloc` safely. The callback writes only to the lock-free ring buffer; `loop()` does all the heavy work.

---

## OUI target list (@NitekryDPaul research)

All lowercase, colon-separated. 31 Flock Cam infrastructure prefixes:

```
70:c9:4e   3c:91:80   d8:f3:bc   80:30:49   b8:35:32
14:5a:fc   74:4c:a1   08:3a:88   9c:2f:9d   c0:35:32
94:08:53   e4:aa:ea   f4:6a:dd   f8:a2:d6   24:b2:b9
00:f4:8d   d0:39:57   e8:d0:fc   e0:4f:43   b8:1e:a4
70:08:94   58:8e:81   ec:1b:bd   3c:71:bf   58:00:e3
90:35:ea   5c:93:a2   64:6e:69   48:27:ea   a4:cf:12
82:6b:f2   ← contributed by DeFlockJoplin
```

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

The firmware emits one JSON line per detection in the same schema the BLE detector uses, so `api/flockyou.py` picks it up with zero changes:

```json
{"event":"detection","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":""}
```

`detection_method` values:

- `wifi_wildcard_probe_ie_sig` — **Probe Request + wildcard SSID + primary Flock IE signature** from a known OUI (PACK method 2 PoC; no rolling-window gates). Supersedes the removed `wifi_wildcard_probe` tier (same wildcard/OUI gates plus IE-field verification).
- `wifi_oui_addr2` — *(disabled in firmware)* transmitter-side OUI match on any frame
- `wifi_oui_addr1` — *(disabled in firmware)* receiver-side OUI match (the @NitekryDPaul technique)
- `wifi_oui_addr3` — *(disabled in firmware)* BSSID OUI on mgmt frames; broad OUI-only filter, false-positive prone
- `wifi_ssid` — SSID keyword match (disabled by default)

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

### LilyGO T-Dongle S3 (`lilygo_t_dongle_s3`). Added because testing hardware got misplaced.

| Pin | Function |
|-----|----------|
| GPIO 1–5, 38 | ST7735 display (RST, DC, MOSI, CS, SCLK, backlight) |
| GPIO 39 / 40 | APA102 RGB LED (clock / data) — red flash on detection |
| USB CDC | Serial JSON for Flask dashboard |

**Display:** idle screen shows `SCANNING`, current WiFi channel, and unique hit count. On each emitted detection, shows `DETECT`, method, MAC, RSSI, and channel for **5 s**, then returns to idle. Backlight is driven on init (GPIO 38 active-low).

No buzzer on this env.

Boot sound (XIAO only): first 6 notes of Super Mario Bros. World 1-2 (underground).

---

## Build and flash

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -e xiao_esp32s3              # Seeed XIAO (default)
pio run -e lilygo_t_dongle_s3        # LilyGO T-Dongle S3

pio run -e xiao_esp32s3 -t upload    # flash XIAO
pio run -e lilygo_t_dongle_s3 -t upload   # flash T-Dongle (hold BOOT if port missing)
pio device monitor
```

`platformio.ini` and `partitions.csv` are at the root (1.9 MB SPIFFS partition, 6 MB app). The T-Dongle env adds **TFT_eSPI** for the onboard display; XIAO needs no extra libraries.

---

## Config cheatsheet (top of `main.cpp`)

| Define | Default | Notes |
|---|---|---|
| `CHANNEL_MODE` | `CHANNEL_MODE_CUSTOM` | `CUSTOM` (11/6/1 desc), `FULL_HOP` (11-1 desc), or `SINGLE` |
| `CHANNEL_DWELL_MS` | 350 | Time on each channel before hop |
| `RSSI_MIN` | -95 | Drop frames weaker than this |
| `ALERT_COOLDOWN_MS` | 5000 | Per-MAC serial-emit rate limit |
| `CHECK_ADDR1` | 0 | Receiver-side OUI (disabled — see main.cpp) |
| `CHECK_ADDR3` | 0 | BSSID OUI fallback (disabled — see main.cpp) |
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

- **ØяĐöØцяöЪöяцฐ (@NitekryDPaul)** — **WiFi promiscuous detection research**: the 30-OUI Flock Safety target list and the addr1-receiver detection technique that are the baseline of this firmware. The code here is a mod of his original work.
- **DeFlockJoplin** ([DeflockJoplin/flock-you](https://github.com/DeflockJoplin/flock-you), [deflockjoplin.today](https://deflockjoplin.today)) — **wildcard-probe-request signature** + the 31st OUI (`82:6b:f2`). Drive-tested in Joplin to 11/12 cameras caught with only 2 false positives.
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
