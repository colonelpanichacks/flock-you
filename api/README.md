# Flock-You Web Dashboard

Flask + Socket.IO dashboard for the `promiscious` branch of the Flock-You firmware. Provides:

- Live ingestion of WiFi-promiscuous-mode detections streaming over USB-CDC
- A host command protocol over the same USB port (`CMD:STATUS` / `CMD:DUMP_PREV` / `CMD:DUMP_LIVE` / `CMD:CLEAR_PREV` / `CMD:CLEAR_LIVE` / `CMD:VERSION`) exposed as REST endpoints and dashboard buttons
- GPS wardriving via a USB NMEA puck or the browser Geolocation API, with temporal matching between detection time and GPS timeline
- Cumulative detection persistence (`data/cumulative_detections.pkl`) across server restarts
- Export to CSV / KML / JSON, import from any of those, and an OUI lookup tool against the local IEEE registry

The dashboard is the cross-device counterpart to the device-side firmware: the device passively sniffs and persists, and this dashboard is what you connect to over USB to read the device, watch live detections come in, and pull anything the device already caught while it was off-host.

---

## Quick start

```bash
pip install -r requirements.txt
python flockyou.py
```

Open `http://localhost:5000`.

Port and bind address are env-configurable:

```bash
FLOCKYOU_PORT=5101 FLOCKYOU_HOST=127.0.0.1 python flockyou.py
```

Append `?demo=1` to the dashboard URL (`http://localhost:5000/?demo=1`) to load a front-end-only mock with the device shown as connected and a handful of sample detections covering every visual state — live, replay/FLASH (purple), replay/RAM (cyan), with and without GPS — so the polished layout and the command toolbar are previewable without flashing real hardware. The command buttons still hit the real `/api/flock/*` endpoints in demo mode, so clicking them produces the actual "device not connected" error toast.

1. Plug your Flock-You device in over USB.
2. Pick its port from the **Sniffer** dropdown and click **Connect**.
3. Five command buttons appear next to the connect controls — that's the host command protocol.
4. (Optional) Plug a USB NMEA GPS puck in, pick its port from the **GPS** dropdown, click **Connect**.
5. Detections start appearing live.

---

## The Sniffer command bar

Once the device is connected the dashboard shows a row of five buttons:

| Button | Endpoint | Firmware command | What it does |
|---|---|---|---|
| **Pull Prev** | `POST /api/flock/dump_prev` | `CMD:DUMP_PREV` | Streams `/prev_session.json` (the previous boot's persisted SPIFFS session) back into the dashboard. Each entry shows up tagged with a purple **FLASH** badge. |
| **Pull Live** | `POST /api/flock/dump_live` | `CMD:DUMP_LIVE` | Streams the device's in-RAM detection table (`fyDet[]`) — what the device has seen since this boot. Entries are tagged with a blue **RAM** badge. |
| **Status** | `GET /api/flock/status` | `CMD:STATUS` | Returns live device telemetry: detection count, OUI count, SPIFFS state, free heap, uptime, current channel, RSSI min. Shown as a top-right toast. |
| **Clear Prev** | `POST /api/flock/clear_prev` | `CMD:CLEAR_PREV` | Deletes `/prev_session.json` and any leftover `/session.tmp` on the device. Confirmation prompt before sending. |
| **Clear Live** | `POST /api/flock/clear_live` | `CMD:CLEAR_LIVE` | Wipes the device's in-RAM detection table; the next autosave overwrites the persisted session. Confirmation prompt before sending. |

All five buttons disable each other during a pending request so two dumps can't interleave on the wire (the protocol serializes one `CMD:*` at a time).

### Canonical "post-wardrive" workflow

You went driving with the device unplugged. Now you're back at your laptop:

1. **Plug device in → Connect**
2. **Click "Pull Prev"** — every detection from the drive flows into the dashboard with `FLASH` badges
3. **Click "Clear Prev"** — wipes the file from SPIFFS so the next outing starts clean

Or the equivalent from a terminal:

```bash
curl -X POST http://localhost:5000/api/flock/dump_prev
curl -X POST http://localhost:5000/api/flock/clear_prev
```

---

## How replay detections are handled

Detections pulled with **Pull Prev** or **Pull Live** flow through `add_replay_detection_from_serial()` in `flockyou.py`, separate from the live ingestion path. The differences:

- **No GPS temporal matching.** The device's stored detections only have monotonic millis (`device_first_ms` / `device_last_ms`) since its last boot — not wall-clock — so we can't pair them with the GPS timeline. They land in the dashboard without GPS unless they already had it from an earlier live capture.
- **No overwrite of fresher live data.** If a MAC is already in the dashboard (because it was just seen live), the replay only bumps `detection_count` if the device's saved count is higher. Live RSSI / channel / last-seen stay.
- **Separate visual treatment.** A purple **FLASH** or blue **RAM** badge appears next to the detection-method label, the card gets a subtle left-border tint, and `timestamp_source` is set to `device_replay`.
- **Separate socket event.** The browser receives `replay_detection` instead of `new_detection`, so other clients can render them differently.

The end of a dump is signalled by a `flock_replay_complete` event on the socket (with `count`, `source`, `ok`, and any `reason` like `no_file` or `crc_mismatch` if the SPIFFS file was missing or corrupt).

---

## Toast notifications

Every command response surfaces as a coloured top-right toast that auto-dismisses (4 s default, 6 s for status):

| Colour | Meaning |
|---|---|
| Green | Success — command completed, returned data |
| Blue (info) | In-progress / informational |
| Yellow (warning) | Completed but nothing returned (e.g. "Pulled 0 detections" because no prev file existed) |
| Red (error) | Timeout, device disconnected, or firmware error event |

`flock_error` socket events from the firmware (e.g. unknown command) also raise an error toast.

---

## API endpoints

### Sniffer device (USB-CDC to the ESP32-S3)

| Method | Path | Description |
|---|---|---|
| `GET`  | `/api/flock/ports`        | List available serial ports |
| `POST` | `/api/flock/connect`      | Body: `{"port": "/dev/ttyACM0"}`. Opens serial @115200 and starts the reader thread |
| `POST` | `/api/flock/disconnect`   | Closes the serial port |
| `GET`  | `/api/flock/status`       | Sends `CMD:STATUS`, returns the `{"event":"status",...}` reply (2 s timeout) |
| `GET`  | `/api/flock/version`      | Sends `CMD:VERSION`, returns firmware identifier and compile-time constants |
| `POST` | `/api/flock/dump_prev`    | Sends `CMD:DUMP_PREV`. Returns when `replay_complete` arrives (30 s timeout). Replay detections stream in via socket during the call |
| `POST` | `/api/flock/dump_live`    | Same shape as `dump_prev` but reads the in-RAM detection table |
| `POST` | `/api/flock/clear_prev`   | Sends `CMD:CLEAR_PREV`. Returns the `{"event":"clear","target":"prev","ok":...}` reply |
| `POST` | `/api/flock/clear_live`   | Sends `CMD:CLEAR_LIVE`. Wipes in-RAM `fyDet[]` |

### GPS

| Method | Path | Description |
|---|---|---|
| `GET`  | `/api/gps/ports`      | List available serial ports |
| `POST` | `/api/gps/connect`    | Body: `{"port": "..."}`. Opens NMEA reader @9600 baud |
| `POST` | `/api/gps/disconnect` | Closes the GPS port |

### Detection management

| Method | Path | Description |
|---|---|---|
| `GET`  | `/api/detections` | Query params: `filter=<method>` and `type=session\|cumulative`. Returns the matching detection list |
| `POST` | `/api/detections` | Manually inject a detection record |
| `POST` | `/api/clear`      | Wipe the in-memory session detections (cumulative + device-side untouched) |
| `POST` | `/api/detection/alias` | Body: `{"id": N, "alias": "..."}`. Sets a human-readable label for a detection |
| `GET`  | `/api/stats`      | Per-protocol counters for session + cumulative |
| `GET`  | `/api/status`     | Connection status of Sniffer + GPS |

### Export / import

| Method | Path | Description |
|---|---|---|
| `GET`  | `/api/export/csv` | Query: `type=session\|cumulative`. Downloads a CSV |
| `GET`  | `/api/export/kml` | Same, KML for Google Earth |
| `POST` | `/api/import/json` | Multipart: `file=<json>`. Imports detections exported from another instance |
| `POST` | `/api/import/csv`  | Same, CSV |
| `POST` | `/api/import/kml`  | Same, KML |

### OUI lookup

| Method | Path | Description |
|---|---|---|
| `POST` | `/api/oui/search`  | Body: `{"query": "...."}`. Looks up a MAC prefix or manufacturer name against `oui.txt` |
| `GET`  | `/api/oui/all`     | Dumps the entire local IEEE OUI registry |
| `POST` | `/api/oui/refresh` | Re-downloads `oui.txt` from `standards-oui.ieee.org` |

---

## Socket.IO events

### From server to client

| Event | Payload | When |
|---|---|---|
| `new_detection` | detection | Live detection just arrived from the device |
| `replay_detection` | detection (with `replay:true`) | Pulled from `/prev_session.json` or live RAM |
| `detection_updated` | detection | A known MAC was seen again |
| `flock_replay_complete` | `{ok, count, source, reason?}` | End of a `DUMP_*` stream |
| `flock_status` | status object | `CMD:STATUS` reply (also broadcast to other tabs) |
| `flock_clear` | `{target, ok}` | `CMD:CLEAR_*` reply |
| `flock_error` | `{reason, cmd?}` | Firmware rejected an unknown command |
| `flock_disconnected` / `flock_reconnected` | port info | Serial reader thread state changes |
| `gps_update` / `gps_disconnected` / `gps_reconnected` | GPS data | GPS reader thread |
| `detections_cleared` | — | Someone called `/api/clear` |
| `serial_data` | raw line | Streams Serial output to the in-browser terminal |

### From client to server

| Event | When |
|---|---|
| `request_serial_terminal` | Open the live serial-output drawer |
| `heartbeat` / `heartbeat_ack` | Keep-alive ping |

---

## JSON wire format

Live detection (Flask reads one of these per line from USB-CDC):

```json
{"event":"detection","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":""}
```

Replay detection (extra fields on top of the live format):

```json
{"event":"detection","replay":true,"replay_source":"prev","detection_method":"wifi_oui_addr2","protocol":"wifi_2_4ghz","mac_address":"aa:bb:cc:dd:ee:ff","oui":"aa:bb:cc","device_name":"","rssi":-62,"channel":6,"frequency":2437,"ssid":"","detection_count":17,"device_first_ms":12345678,"device_last_ms":18900000}
```

Command reply events:

```json
{"event":"status","fy_det":42,"oui_count":42,"spiffs":1,"prev_session":1,"prev_bytes":4280,"uptime_ms":900000,"free_heap":221408,"channel":6,"channel_mode":"CUSTOM","rssi_min":-95}
{"event":"version","firmware":"flock-you-promiscious","branch":"promiscious","oui_count":42,"max_detections":200,"autosave_ms":60000}
{"event":"replay_complete","source":"prev","count":47,"ok":true}
{"event":"clear","target":"prev","ok":true}
{"event":"error","reason":"unknown_command","cmd":"CMD:GARBAGE"}
```

---

## GPS wardriving setup

GPS is handled server-side because the ESP32's radio is dedicated to sniffing and there's no on-device AP. Two options:

- **USB NMEA puck** plugged into the host running Flask — pick its port from the **GPS** dropdown
- **Phone browser** at `http://<host>:5000` — the dashboard's Geolocation API hook posts updates to Flask

Either way, Flask does a temporal match between the detection's arrival timestamp and the GPS timeline (default ±30 s window, prefers the closest reading; "precise" matches are flagged as such in CSV/KML exports).

### Android Chrome over HTTP

Geolocation is restricted to HTTPS unless you whitelist the dashboard:

1. `chrome://flags` → search **"Insecure origins treated as secure"**
2. Add `http://<host>:5000`
3. Enable, relaunch Chrome, grant location when prompted

iOS Safari does not support Geolocation over plain HTTP.

---

## Persistence

| What | Where |
|---|---|
| Cumulative detections | `data/cumulative_detections.pkl` (pickle, loaded on startup) |
| Settings | `data/settings.json` |
| OUI registry | `oui.txt` (5.9 MB, IEEE master list) |
| Exports | `exports/` (created on first export) |

The device-side persistence is separate — see [`../README.md`](../README.md#spiffs-wire-format) for the SPIFFS layout. The `Pull Prev` button is the bridge between the two.

---

## File structure

```
api/
├── flockyou.py                    # Main Flask + Socket.IO app
├── requirements.txt               # Python dependencies
├── templates/
│   └── index.html                 # Dashboard UI (single-file, ~3100 lines)
├── oui.txt                        # IEEE OUI registry (mirrored from standards-oui.ieee.org)
├── data/                          # Created on first run
│   ├── cumulative_detections.pkl
│   └── settings.json
├── exports/                       # CSV / KML output (created on first export)
└── README.md
```

---

## Troubleshooting

**"Device did not respond (timeout)" on Pull Prev / Status / etc.**
The firmware on the device is older than the `promiscious` branch and doesn't speak the `CMD:*` protocol. Reflash from [`../main.cpp`](../main.cpp).

**Pull Prev returns count=0 with reason "no_file"**
The device has never had a session persist to SPIFFS, or you just called Clear Prev. Drive around a bit, let the device autosave (every 60 s), reboot, plug it back in, try again.

**Pull Prev returns count=0 with reason "crc_mismatch"**
The persisted session file got corrupted (interrupted write, flash wear). The firmware refuses to replay it. Call Clear Prev and start fresh.

**Replay detections show up without GPS coordinates**
Expected — the device's stored detections only have monotonic millis from the device's previous boot, not wall-clock. They can't be paired with the host-side GPS timeline. If you want GPS-tagged data, keep the device plugged in during the drive.

**The Sniffer command buttons don't appear after connect**
They're inside `#flockExtraControls` and toggled by `setFlockExtraControls()`. If the connect succeeded (status indicator green, Disconnect button visible) but the row of five buttons is missing, hard-refresh (Ctrl+Shift+R) to pull the latest template.

**Two browser tabs trigger Pull at the same time**
Don't. The protocol serializes commands at the firmware level (one `CMD:*` at a time, blocking on the matching `event`), but the dashboard's command lock is per-Flask-process. Two tabs firing Pull simultaneously will queue, not interleave, so the second one waits — but the first tab's busy state won't reflect the second tab's request.

---

## Security notes

The dashboard listens on `0.0.0.0:5000` by default — anyone on your network can reach it. Bind to `127.0.0.1` or put it behind nginx/auth if that's not what you want. The Flask secret key in `flockyou.py` defaults to a dev value; set `SECRET_KEY` in the environment for anything production-shaped.
