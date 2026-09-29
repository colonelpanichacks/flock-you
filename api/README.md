# Flock You Web Dashboard

A Flask + Socket.IO dashboard for real-time monitoring and analysis of Flock Safety device
detections with GPS integration. It reads detection JSON off the ESP32's USB CDC link
(`../main.cpp`), tags each hit with GPS, persists it, and serves a live single-page UI.

## Features

### Real-Time Detection Monitoring
- **Live Updates**: Real-time detection display via WebSocket
- **Detection Filtering**: Filter by detection method / confidence tier (IE fingerprint, wildcard probe, OUI transmitter, OUI receiver, OUI BSSID, SSID keyword)
- **Confidence Tiers**: Each detection is tagged with the method that caught it and a tier 0–4; the badge shows the strongest evidence, and an "also" row lists every other path that caught the same device with hit counts
- **Per-Tier Audio**: A master **Audio** switch in the header, and a gear beside it that opens the **Detection Audio** popup with one switch per tier (plus **All On** / **All Off**). Muting affects the device's buzzer only — every path still logs and exports. The mask persists on-device in NVS
- **Statistics Dashboard**: Four cards — **Session Detections**, **WiFi Detections**, **IE Confirmed**, **GPS Tagged**
- **Detection Map**: Leaflet map with OpenStreetMap / Satellite / Topographic layers, a session / cumulative / both data filter, and a legend where marker fill is the confidence tier and the border is session vs cumulative
- **Serial Terminal**: Raw device output streamed to the browser over the `serial_terminal` socket room, filterable to All Data / Sniffer Only / GPS Only
- **Auto-Reconnect**: A background monitor re-opens the device and GPS serial ports if they drop
- **OUI Lookup**: The **OUI Database Search** section resolves manufacturers from a local IEEE `oui.txt`, with **View All** and **Refresh DB**

The page is four collapsible sections: **Detections**, **Detection Map**, **Serial Terminal**,
and **OUI Database Search**.

### GPS Integration
- **GPS Dongle Support**: Connect USB GPS dongles for location tracking
- **NMEA Parsing**: Automatic parsing of GPS coordinates
- **gpsd Support**: Read from a running `gpsd` daemon over TCP
- **Location Tagging**: Each detection is matched to the closest GPS reading in time
- **Satellite Information**: Display GPS fix quality and satellite count

### Persistence
Both detection lists live in `data/` as pickles. `cumulative_detections.pkl` is the all-time
history; `session_detections.pkl` is the current session, saved on every change and reloaded on
startup, so restarting the server does not empty the Detections list. The session only resets
when you click **Clear All**. UI preferences live in `data/settings.json`.

### Data Import / Export
- **CSV / KML Export**: Download detection data, or a Google Earth overlay
- **JSON / CSV / KML Import**: Load previously exported files back into the session
- **Device Session Import**: Pull the ESP32's own SPIFFS detection table over serial
- **GPS Coordinates**: Exports include latitude, longitude, and altitude where available
- **Timestamped Files**: Automatic filename generation with timestamps, written to `exports/`

## Installation

### Prerequisites
- Python 3.8 or higher
- USB GPS dongle (optional, for location tracking)

### Setup
1. **Install dependencies**:
   ```bash
   pip install -r requirements.txt
   ```

2. **Run the application** (from this `api/` directory — `oui.txt`, `data/` and `exports/` are
   resolved relative to the working directory):
   ```bash
   python flockyou.py
   ```

3. **Access the dashboard**:
   Open your browser and navigate to `http://localhost:5000`

### Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `FLOCKYOU_PORT` | `5000` | Listen port. macOS AirPlay Receiver squats on 5000 — use e.g. `5001` |
| `FLOCKYOU_HOST` | `0.0.0.0` | Bind address |
| `SECRET_KEY` | `flockyou_dev_key_2024` | Flask session secret; override outside local use |

## Usage

### Basic Operation
1. **Start the web server** using the command above
2. **Connect your Flock You device**: pick its serial port next to **Sniffer:** in the header and click **Connect**. A second device running the BLE companion firmware connects next to **BLE Sniffer:**, which also exposes **Pull Prev** / **Pull Live** / **Status** / **Clear Prev** / **Clear Live**
3. **View detections** in real-time on the dashboard
4. **Filter detections** with the method dropdown (entries are labelled with their tier, e.g. `Wildcard probe (T3)`) or the free-text search box
5. **Export data** from the **Export ▼** menu — Session CSV/KML or Cumulative CSV/KML
6. **Import data** from the **Import ▼** menu — Import JSON/CSV/KML (from ESP32), or **Pull current session from device** / **Pull previous session from device**

### GPS Setup
The **GPS:** header row has a source dropdown with two options — there is no browser-geolocation
option in this dashboard:

**Serial NMEA** (USB GPS dongle)
1. **Connect GPS dongle** to your computer via USB
2. **Select GPS port** from the dropdown in the header
3. **Click "Connect"** to establish GPS connection
4. **Monitor GPS status** via the status indicator
5. **Detections will automatically include GPS data** when available

**gpsd** (shared GPS via the gpsd daemon)
1. **Start gpsd** on the host (`gpsd -N /dev/ttyUSB0`, or any working `gpsd` setup)
2. **Pick "gpsd"** from the source dropdown
3. **Enter host and port** (default `localhost:2947`)
4. **Click "Connect"** — the dashboard speaks gpsd's line-delimited JSON protocol directly, no extra Python client required. Handy when several tools already share the same GPS via gpsd.

Detections are matched to the GPS timeline temporally: the closest reading within 30 s wins
(`match_quality: "temporal"`), and a match inside 5 s also adopts the GPS timestamp. With no
temporal match the current fix is attached instead (`match_quality: "current"`).

## API Endpoints

### Detections
- `GET /api/detections` — all detections. Query params: `type=session|cumulative` (default `session`), `filter=<detection_method>|all`
- `POST /api/detections` — add a detection from an external feed (see schema below)
- `POST /api/test/detection` — inject a sample detection; body optional, missing fields are filled in
- `POST /api/detection/alias` — `{"id": <int>, "alias": "..."}`, names a device in the session list
- `POST /api/clear` — clear the **session** list and reset the session start time (cumulative history is kept)
- `GET /api/stats` — session and cumulative totals, WiFi counts, tier-4 (`ie_confirmed`) counts, GPS-tagged counts

### Device (WiFi sniffer)
- `GET /api/flock/ports` — available serial ports
- `POST /api/flock/connect` — `{"port": "/dev/tty.usbmodemXXX"}`, opens at 115200 and starts the reader thread
- `POST /api/flock/disconnect` — close the device port
- `GET /api/flock/config` — last reported per-tier beep state, plus `methods` / `tiers` maps; also asks the device to re-report
- `POST /api/flock/beep` — `{"tier": 0-4, "enabled": bool}` or `{"mask": 0-31}`
- `POST /api/flock/dump_session` — `{"source": "live"|"prev"}`; asks the device to stream its stored detection table, imported as it arrives

### BLE companion device
Served by the `flockyou_ble` blueprint (`flockyou_ble.py`) for a *second* serial device running
oui-spy-unified-blue Mode 1. It speaks a line-oriented `CMD:` protocol, not the WiFi device's
JSON commands, but its detections land in the same list.

- `GET /api/flock_ble/ports` — available serial ports
- `POST /api/flock_ble/connect` — `{"port": "..."}`
- `POST /api/flock_ble/disconnect`
- `GET /api/flock_ble/status` — local connection state plus the device's own `CMD:STATUS` blob
- `GET /api/flock_ble/version` — device firmware version
- `POST /api/flock_ble/dump_prev` / `POST /api/flock_ble/dump_live` — replay the device's flash or RAM table
- `POST /api/flock_ble/clear_prev` / `POST /api/flock_ble/clear_live` — wipe the device's stored table

### GPS
- `GET /api/gps/ports` — available serial ports
- `POST /api/gps/connect` — connect a GPS source. Body:
  - `{"source": "serial", "port": "/dev/tty.usbserial-XXX"}` — opens an NMEA reader at 9600 baud
  - `{"source": "gpsd", "host": "localhost", "port": 2947}` — connects to a running gpsd daemon
  - Legacy body `{"port": "..."}` still works and is treated as serial
- `POST /api/gps/disconnect` — disconnect whichever GPS source is active

### Import / Export
- `GET /api/export/csv` — export as CSV. `?type=session|cumulative`
- `GET /api/export/kml` — export as KML. `?type=session|cumulative`
- `POST /api/import/json` — multipart upload, field `file`
- `POST /api/import/csv` — multipart upload, field `file`
- `POST /api/import/kml` — multipart upload, field `file`

Exports 400 when there is nothing to export. Files are written to `exports/` (created on demand)
and returned as a download.

### Status, settings, OUI database
- `GET /api/status` — connection state for both the device and GPS (`gps_source`, `gpsd_endpoint`, `flock_port`, …)
- `GET /api/settings` / `POST /api/settings` — UI preferences, persisted to `data/settings.json`
- `POST /api/oui/search` — `{"query": "b4:1e:52"}` or a manufacturer name (max 100 name matches)
- `GET /api/oui/all` — the whole parsed OUI table
- `POST /api/oui/refresh` — re-download `oui.txt` from `standards-oui.ieee.org` and reload

### Socket.IO events

Server → client: `new_detection`, `detection_updated`, `detections_cleared`, `device_config`,
`session_dump`, `gps_update`, `gps_disconnected`, `gps_reconnected`, `flock_disconnected`,
`flock_reconnected`, `ble_disconnected`, `reconnect_failed`, `serial_connected`, `serial_data`,
`serial_error`, `heartbeat`, `heartbeat_ack`.

Client → server: `heartbeat`, `request_serial_terminal` (`{"port": "..."}` — joins the
`serial_terminal` room and replays the last 50 lines).

## Integration with Flock You Device

The dashboard receives JSON detection data from the Flock You ESP32 over USB CDC. Each line
looks like:

```json
{
  "event": "detection",
  "protocol": "wifi_2_4ghz",
  "detection_method": "wifi_wildcard_probe_ie_sig",
  "detection_tier": 4,
  "ssid": "",
  "mac_address": "82:6b:f2:14:07:3a",
  "oui": "82:6b:f2",
  "rssi": -52,
  "channel": 6,
  "frequency": 2437
}
```

In normal use the device streams these over serial and the dashboard ingests them directly —
`POST /api/detections` is for testing and external feeds.

The reader thread dispatches on the `event` field: `config` (per-tier beep state),
`session_begin` / `session_det` / `session_end` (offline session import), and anything carrying
`detection_method` as a live detection. Non-JSON lines are logged and forwarded to the serial
terminal.

### Detection methods and tiers

| Tier | `detection_method` | Meaning |
|---|---|---|
| 4 | `wifi_wildcard_probe_ie_sig` | Wildcard probe from a Flock OUI whose IE fields match the Flock signature |
| 3 | `wifi_wildcard_probe` | Wildcard probe from a Flock OUI, IE fields did not match |
| 2 | `wifi_oui_addr2` | Flock OUI as frame transmitter |
| 2 | `ble_name` / `ble_mfg` / `ble_gatt_svc` | On-device NimBLE hit: advertised name, XUNTONG manufacturer ID, or a Flock/Raven/Nordic-DFU GATT service |
| 1 | `wifi_oui_addr1` / `wifi_oui_addr3` | Flock OUI as receiver or BSSID — AP echoes |
| 0 | `wifi_ssid` | SSID keyword match (off in firmware) |

BLE hits carry `protocol: "ble"`, the advertised name in `device_name`, `channel: 255` and
`frequency: 0`. The `ble_*` methods are **not** in this module's `METHOD_TIERS` map, so their
tier comes from the device's own `detection_tier` field (`method_tier()` prefers it when
present) and `method_label()` falls through to the raw method string. The detection filter
dropdown has no BLE entry — BLE hits show only under **All Detections**.

The stored method is best-ever: a device caught first by a low tier and later confirmed by the
fingerprint is relabelled upward and never back down. `normalize_method()` folds the firmware's
bare SPIFFS names (`wildcard_probe_ie_sig`) and exported human labels (`IE fingerprint`) back to
the canonical keys, so device dumps and re-imported exports agree.

### Network dependency

`templates/index.html` pulls all of its third-party assets from CDNs — Socket.IO from cdnjs,
Leaflet from unpkg, Chart.js from jsdelivr, and Google Fonts — and the map fetches tiles from
`tile.openstreetmap.org`, `server.arcgisonline.com`, or `tile.opentopomap.org`. There is no
`static/` fallback. The Flask side is fully offline-capable, but the browser side is not: the
page calls `io()` at the top of its inline script, unguarded, so if the Socket.IO CDN is
unreachable that line throws and the whole script block aborts — `DOMContentLoaded` is never
wired up and **the dashboard renders empty**, not merely without live updates.

The device and server are unaffected: detections are still read off serial, GPS-tagged, and
written to `data/`, and `GET /api/detections` still returns them. Only the browser UI breaks.
Worth knowing before a drive out of coverage.

## GPS Dongle Compatibility

Standard NMEA GPS dongles that output GPGGA sentences, and anything reachable through `gpsd`
over TCP:
- USB GPS receivers
- Serial GPS modules
- Anything already feeding `gpsd`

## File Structure
```
api/
├── flockyou.py         # Main Flask application (routes, serial reader, GPS, exports)
├── flockyou_ble.py     # BLE companion device blueprint (/api/flock_ble/*)
├── requirements.txt    # Python dependencies
├── oui.txt             # IEEE OUI registry, parsed at startup
├── templates/
│   └── index.html      # Single-page dashboard
├── data/               # Pickled detections + settings.json (gitignored)
├── exports/            # Generated export files (gitignored)
└── README.md           # This file
```

`initialize_app()` runs at import time, not under `__main__`, so a WSGI host (gunicorn, uwsgi)
comes up with the OUI database and persisted state loaded.

## Troubleshooting

### GPS Connection Issues
- Ensure GPS dongle is properly connected
- Check that the correct serial port is selected
- Verify GPS dongle is powered and has satellite fix
- Check system permissions for serial port access

### No Detections Displayed
- Verify the Flock You device is connected in the header (`GET /api/status` shows `flock_connected`)
- Open the serial terminal to confirm the device is emitting JSON lines
- Ensure the device is on the right serial port — the ESP32 and a GPS puck both show up in the port list
- Check browser console for JavaScript errors

### Everything shows "Unknown Manufacturer"
`oui.txt` is opened relative to the working directory. Start the server from `api/`, or refresh
the database with `POST /api/oui/refresh`.

### Port 5000 in use on macOS
AirPlay Receiver binds it. Run with `FLOCKYOU_PORT=5001 python flockyou.py`, or turn AirPlay
Receiver off in System Settings.

### Export Issues
- `exports/` is created automatically; check the directory is writable
- Exports return 400 when the selected list is empty
- Check available disk space

## Security Notes

- The dashboard binds `0.0.0.0:5000` by default (accessible from any network) — set `FLOCKYOU_HOST=127.0.0.1` to keep it local
- There is no authentication; put it behind a reverse proxy if it needs to be reachable
- Set `SECRET_KEY` rather than shipping the development default

## Development

- `TEMPLATES_AUTO_RELOAD` is on, so edits to `templates/index.html` show up on a browser refresh without restarting Flask
- Adding a detection method means touching `METHOD_TIERS` and `METHOD_LABELS` here, the tier constants in `../main.cpp`, and both the CSV and KML exporters
- There is no test suite; verify GPS against a real dongle, exports against sample data, and live updates with several browser windows open
