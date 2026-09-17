"""
flockyou_ble.py — BLE Sniffer companion for the flock-you dashboard.

Mirrors the WiFi-side serial-attached device plumbing (connect / read stream /
CMD: dump) for the oui-spy-unified-blue Mode 1 (BLE Detector) firmware.

Wire protocol on the device end (see src/raw/detector.cpp):
    CMD:STATUS      -> single-line JSON status
    CMD:VERSION     -> single-line version string
    CMD:DUMP_PREV   -> BEGIN_DUMP prev bytes=N count=N
                       <JSON line per detection, replay_source="flash">
                       ...
                       END_DUMP prev count=N
    CMD:DUMP_LIVE   -> BEGIN_DUMP live bytes=0 count=N
                       <JSON line per detection, replay_source="ram">
                       ...
                       END_DUMP live count=N
    CMD:CLEAR_PREV  -> OK
    CMD:CLEAR_LIVE  -> OK

Live detection JSON has protocol="ble" and detection_method="ble_<method>"
so the existing add_detection_from_serial() pipeline picks them up alongside
WiFi hits (they carry timestamp_source="device_replay" for dumped entries).
"""

from flask import Blueprint, jsonify, request
import json
import queue
import re
import threading
import time
from datetime import datetime

import serial
import serial.tools.list_ports


bp = Blueprint("flockyou_ble", __name__)


# ---------------------------------------------------------------------------
# BLE / Bluetooth detection signatures
# ---------------------------------------------------------------------------
#
# Authoritative target set, extracted from an actual Flock Safety camera
# firmware dump. This is the ONLY detection set this version uses. The match
# itself runs on the ESP32 BLE detector; this copy is the dashboard's
# reference for labelling, imports, and corroboration.

# Complete Local Name (AD type 0x09) patterns seen from Penguin battery packs:
#   "Penguin-NNNNNNNNNN" — "Penguin-" + 10-digit serial
#   "NNNNNNNNNN"         — bare 10-digit serial
#   "FS Ext Battery"     — extended-battery accessory
BLE_COMPLETE_NAME_PATTERNS = (
    re.compile(r"^Penguin-\d{10}$"),
    re.compile(r"^\d{10}$"),
    "FS Ext Battery",
)

# Manufacturer-specific data (AD type 0xFF) company IDs. 0x09C8 (2504) is
# XUNTONG, the Penguin pack's BLE chipset vendor; the payload embeds serials
# like TN72023022000771.
BLE_MFG_COMPANY_IDS = {
    0x09C8: "XUNTONG (Penguin battery pack, serial in payload)",
}

# Flock accessory GATT service exposed by the battery packs, with the two
# characteristics that matter (key exchange and control).
FLOCK_ACCESSORY_GATT_SERVICE_UUID = "e8ccbb38-9532-46a8-9fe5-1814df172e6f"
FLOCK_ACCESSORY_GATT_CHARACTERISTICS = (
    "628913a6-8701-40ff-a3ce-8f453ff0818d",  # key characteristic
    "bb18d1d2-fe71-439f-9529-d4b472d139b5",  # control characteristic
)

# Raven camera BLE GATT services live in 16-bit space 0x3100-0x3500 and are
# unauthenticated — 0x3101/0x3102 leak GPS latitude/longitude.
RAVEN_GATT_SERVICE_RANGE = (0x3100, 0x3500)

# Corroborating classic-Bluetooth signals (weaker on their own, strong next
# to any BLE hit above):
#   device names "msm8953_32" (Snapdragon 625 platform default) and "Android"
#   (net.bt.name=Android, no vendor override in the firmware)
CLASSIC_BT_DEVICE_NAMES = (
    "msm8953_32",
    "Android",
)
# SDP Device-ID record from bt_did.conf: Qualcomm vendor / product pair.
CLASSIC_BT_SDP_DEVICE_ID = {
    "vendor_id": 0x001D,   # Qualcomm
    "product_id": 0x1200,
}


def matches_ble_complete_name(name):
    """True when a BLE complete local name matches a firmware-derived pattern."""
    if not name:
        return False
    name = str(name)
    for pattern in BLE_COMPLETE_NAME_PATTERNS:
        if isinstance(pattern, str):
            if name == pattern:
                return True
        elif pattern.match(name):
            return True
    return False


def matches_ble_mfg_company(company_id):
    """True when a manufacturer-data company ID is a known Flock vendor."""
    return company_id in BLE_MFG_COMPANY_IDS


def matches_raven_gatt_service(uuid16):
    """True when a 16-bit GATT service UUID falls in the Raven camera range."""
    try:
        value = int(uuid16)
    except (TypeError, ValueError):
        return False
    low, high = RAVEN_GATT_SERVICE_RANGE
    return low <= value <= high


# ---------------------------------------------------------------------------
# Signature matching — turns the reference constants above into per-detection
# tags (`matched_signatures` / `firmware_sig`) before records are handed to
# the shared ingest sink.
# ---------------------------------------------------------------------------

# Parallel to BLE_COMPLETE_NAME_PATTERNS: the tag each pattern earns.
_BLE_NAME_TAGS = (
    "ble_name:penguin_serial",
    "ble_name:bare_serial",
    "ble_name:fs_ext_battery",
)


def _parse_int_flexible(value):
    """Accept an int, a decimal string ("2504"), or hex ("0x09c8" / "09c8")."""
    if value is None:
        return None
    if isinstance(value, int):
        return value
    text = str(value).strip()
    for base in (0, 16):
        try:
            return int(text, base)
        except (ValueError, TypeError):
            continue
    return None


def _ble_name_signatures(name):
    """Tags for a BLE/classic complete local name, or empty list."""
    if not name:
        return []
    name = str(name)
    sigs = []
    for pattern, tag in zip(BLE_COMPLETE_NAME_PATTERNS, _BLE_NAME_TAGS):
        hit = (name == pattern) if isinstance(pattern, str) else bool(pattern.match(name))
        if hit:
            sigs.append(tag)
            break  # first matching pattern wins; the patterns are exclusive
    if name in CLASSIC_BT_DEVICE_NAMES:
        sigs.append(f"classic_bt_name:{name.lower()}")
    return sigs


def _gatt_service_signatures(uuids):
    """Tags for advertised GATT service UUIDs (Flock accessory / Raven range)."""
    sigs = []
    for raw in uuids:
        text = str(raw).strip().lower()
        if text == FLOCK_ACCESSORY_GATT_SERVICE_UUID:
            sigs.append("gatt:flock_accessory")
            continue
        value = _parse_int_flexible(text)
        if value is None:
            # Standard Bluetooth base-UUID form: 0000XXXX-0000-1000-8000-...
            m = re.match(r"^([0-9a-f]{4})[0-9a-f]{4}-0000-1000-8000-00805f9b34fb$", text)
            if m:
                value = int(m.group(1), 16)
        if value is not None and matches_raven_gatt_service(value):
            sigs.append(f"gatt:raven_service:0x{value:04x}")
    return sigs


def ble_signature_matches(data):
    """Firmware-derived BLE/Bluetooth signature tags for one observation.

    Looks at every field spelling the BLE detector firmware has used:
    names under device_name/name/complete_name, company IDs under
    company_id/mfg_company_id/manufacturer_company_id, service UUIDs under
    service_uuids/service_uuid/gatt_services, and the classic-BT SDP
    Device-ID vendor/product pair.
    """
    if not isinstance(data, dict):
        return []
    sigs = []

    sigs += _ble_name_signatures(
        data.get("device_name") or data.get("name") or data.get("complete_name"))

    for key in ("company_id", "mfg_company_id", "manufacturer_company_id"):
        company = _parse_int_flexible(data.get(key))
        if company is not None:
            if matches_ble_mfg_company(company):
                sigs.append(f"ble_mfg_company:0x{company:04x}")
            break

    uuids = []
    for key in ("service_uuids", "gatt_services", "service_uuid"):
        value = data.get(key)
        if isinstance(value, (list, tuple)):
            uuids.extend(value)
        elif value:
            uuids.append(value)
    sigs += _gatt_service_signatures(uuids)

    vendor = _parse_int_flexible(data.get("sdp_vendor_id", data.get("vendor_id")))
    product = _parse_int_flexible(data.get("sdp_product_id", data.get("product_id")))
    if (vendor == CLASSIC_BT_SDP_DEVICE_ID["vendor_id"]
            and product == CLASSIC_BT_SDP_DEVICE_ID["product_id"]):
        sigs.append("classic_bt_sdp_did:qualcomm_001d_1200")

    # Dedupe while preserving order (a UUID list can repeat a service).
    seen = set()
    unique = []
    for sig in sigs:
        if sig not in seen:
            seen.add(sig)
            unique.append(sig)
    return unique


def tag_detection_signatures(data):
    """Merge firmware-signature tags onto a BLE detection dict in place.

    Sets matched_signatures (ordered, unioned with tags the device itself
    may already have attached) and firmware_sig. The shared ingest sink in
    flockyou.py preserves these when it adds its WiFi-side tags.
    """
    matched = ble_signature_matches(data)
    for sig in data.get("matched_signatures") or []:
        if sig not in matched:
            matched.append(sig)
    data["matched_signatures"] = matched
    data["firmware_sig"] = bool(matched)
    return data


# ---------------------------------------------------------------------------
# Connection state
# ---------------------------------------------------------------------------

# We keep BLE state fully separate from the WiFi (flock) state so the user can
# have both a WiFi Mode-3 device and a BLE Mode-1 device plugged in at once.
_ble_state_lock = threading.Lock()
_ble_serial = None                # serial.Serial instance while connected
_ble_port = None                  # currently connected port path
_ble_connected = False
_ble_reader_thread = None

# When a CMD: dump is in flight, the reader thread routes lines through this
# queue instead of parsing them as detections. Guarded by _dump_lock so only
# one dispatch runs at a time (the underlying serial port is one-shot).
_dump_lock = threading.Lock()
_dump_queue: "queue.Queue[str]" = queue.Queue()
_dump_active = threading.Event()  # set while a CMD: dispatch owns the reader

# Optional injection point: the parent Flask app hands us its
# add_detection_from_serial + socket-emit shim at init time so BLE hits
# land in the same detections list as WiFi hits without a circular import.
_ingest_detection = None          # callable(data_dict) or None
_socket_emit = None               # callable(event, data, room=None) or None


def init_bridge(ingest_fn=None, emit_fn=None):
    """
    Called once by flockyou.py at startup. Wires the shared detection sink
    (usually add_detection_from_serial) so BLE detections show up in the
    dashboard's main detection list without duplicating the pipeline.
    """
    global _ingest_detection, _socket_emit
    _ingest_detection = ingest_fn
    _socket_emit = emit_fn


# ---------------------------------------------------------------------------
# Background reader thread
# ---------------------------------------------------------------------------

def _reader_loop():
    """
    Reads one line at a time from the BLE device serial port. Routes lines:
      - while a CMD dump owns the port (_dump_active set), every line goes
        into _dump_queue for the request handler to drain until END_DUMP
      - otherwise, JSON lines that look like BLE detections are ingested via
        the shared sink; anything else is dropped to stderr.
    """
    global _ble_connected
    while True:
        with _ble_state_lock:
            ser = _ble_serial
            connected = _ble_connected
        if not connected or ser is None:
            return
        try:
            raw = ser.readline()
        except Exception as exc:
            print(f"[flock_ble] read error: {exc}")
            with _ble_state_lock:
                _ble_connected = False
            if _socket_emit:
                _socket_emit("ble_disconnected", {})
            return
        if not raw:
            continue
        try:
            line = raw.decode("utf-8", errors="ignore").rstrip("\r\n")
        except Exception:
            continue
        if not line:
            continue

        # While a dump is in flight, hand every line to the CMD dispatcher.
        # It is responsible for stopping at END_DUMP and releasing the flag.
        if _dump_active.is_set():
            _dump_queue.put(line)
            continue

        # Not in dump mode — try to parse as a live detection.
        if line.startswith("{"):
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                print(f"[flock_ble] non-JSON: {line}")
                continue
            if data.get("protocol") == "ble" and _ingest_detection:
                # Mark timestamps as device-relayed live so the dashboard can
                # style them differently from GPS-anchored WiFi hits.
                data.setdefault("timestamp_source", "device_live")
                tag_detection_signatures(data)
                try:
                    _ingest_detection(data)
                except Exception as exc:
                    print(f"[flock_ble] ingest error: {exc}")
        else:
            # Free-form banner text from the device — surface for debugging.
            print(f"[flock_ble] (text) {line}")


# ---------------------------------------------------------------------------
# CMD: dispatch helpers
# ---------------------------------------------------------------------------

class DumpError(RuntimeError):
    pass


def _drain_pending():
    """Clear anything the reader may have queued between dumps."""
    try:
        while True:
            _dump_queue.get_nowait()
    except queue.Empty:
        pass


def _send_cmd_stream(cmd: str, marker: str, timeout: float = 8.0):
    """
    Send `CMD:<cmd>` and stream all payload lines back until an END_DUMP
    line carrying `marker` arrives. Returns a list of the JSON detection
    strings between BEGIN_DUMP and END_DUMP (BEGIN/END themselves are
    stripped). Raises DumpError on timeout or malformed reply.
    """
    with _ble_state_lock:
        ser = _ble_serial
        connected = _ble_connected
    if not connected or ser is None:
        raise DumpError("device not connected")

    with _dump_lock:
        _drain_pending()
        _dump_active.set()
        try:
            ser.write((f"CMD:{cmd}\n").encode("ascii"))
            ser.flush()

            deadline = time.time() + timeout
            lines: list[str] = []
            saw_begin = False
            while time.time() < deadline:
                try:
                    line = _dump_queue.get(timeout=0.5)
                except queue.Empty:
                    continue
                if not saw_begin:
                    if line.startswith(f"BEGIN_DUMP {marker}"):
                        saw_begin = True
                        continue
                    # Ignore stray text (banners etc.) before the header.
                    continue
                if line.startswith(f"END_DUMP {marker}"):
                    return lines
                # Payload line — should be one JSON object per line.
                if line:
                    lines.append(line)
            raise DumpError(f"timeout waiting for END_DUMP {marker}")
        finally:
            _dump_active.clear()
            _drain_pending()


def _send_cmd_oneline(cmd: str, timeout: float = 3.0) -> str:
    """
    Send `CMD:<cmd>` and return the single next reply line. Used for STATUS
    (returns JSON), VERSION (returns free-form text), CLEAR_* (returns "OK").
    """
    with _ble_state_lock:
        ser = _ble_serial
        connected = _ble_connected
    if not connected or ser is None:
        raise DumpError("device not connected")

    with _dump_lock:
        _drain_pending()
        _dump_active.set()
        try:
            ser.write((f"CMD:{cmd}\n").encode("ascii"))
            ser.flush()

            deadline = time.time() + timeout
            while time.time() < deadline:
                try:
                    line = _dump_queue.get(timeout=0.5)
                except queue.Empty:
                    continue
                if line:
                    return line
            raise DumpError("timeout waiting for reply")
        finally:
            _dump_active.clear()
            _drain_pending()


def _ingest_replayed(lines, source_tag: str):
    """
    Parse the JSON lines returned by a DUMP command and feed them through the
    shared detection sink. `source_tag` is either "flash" or "ram" and is
    written back as replay_source; timestamp_source becomes "device_replay"
    so the dashboard can badge them appropriately and skip GPS temporal
    matching (a replayed hit has no timely GPS anchor).
    """
    ingested = 0
    for line in lines:
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            print(f"[flock_ble] dropped non-JSON dump line: {line[:80]}")
            continue
        # Preserve what the device set, but ensure the badge fields exist.
        data.setdefault("replay_source", source_tag)
        data.setdefault("timestamp_source", "device_replay")
        data.setdefault("protocol", "ble")
        tag_detection_signatures(data)
        if _ingest_detection:
            try:
                _ingest_detection(data)
                ingested += 1
            except Exception as exc:
                print(f"[flock_ble] ingest error: {exc}")
    return ingested


# ---------------------------------------------------------------------------
# HTTP endpoints
# ---------------------------------------------------------------------------

@bp.route("/api/flock_ble/ports", methods=["GET"])
def ble_ports():
    """List available serial ports for the BLE Mode-1 device."""
    ports = []
    for port in serial.tools.list_ports.comports():
        ports.append({
            "device": port.device,
            "description": port.description,
            "manufacturer": port.manufacturer or "Unknown",
            "product": port.product or "Unknown",
            "vid": port.vid,
            "pid": port.pid,
        })
    return jsonify(ports)


@bp.route("/api/flock_ble/connect", methods=["POST"])
def ble_connect():
    """Open the serial port and start the background reader thread."""
    global _ble_serial, _ble_port, _ble_connected, _ble_reader_thread
    port = (request.json or {}).get("port")
    if not port:
        return jsonify({"status": "error", "message": "port required"}), 400
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except Exception as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400

    with _ble_state_lock:
        if _ble_serial and _ble_serial.is_open:
            _ble_serial.close()
        _ble_serial = ser
        _ble_port = port
        _ble_connected = True

    _ble_reader_thread = threading.Thread(target=_reader_loop, daemon=True)
    _ble_reader_thread.start()
    return jsonify({"status": "success", "message": f"Connected to BLE device on {port}"})


@bp.route("/api/flock_ble/disconnect", methods=["POST"])
def ble_disconnect():
    global _ble_serial, _ble_port, _ble_connected
    with _ble_state_lock:
        _ble_connected = False
        if _ble_serial and _ble_serial.is_open:
            try:
                _ble_serial.close()
            except Exception:
                pass
        _ble_serial = None
        _ble_port = None
    return jsonify({"status": "success", "message": "BLE device disconnected"})


@bp.route("/api/flock_ble/status", methods=["GET"])
def ble_status():
    """
    Report both local connection state AND the device's own STATUS blob
    (if reachable), so the dashboard's status bar can show one row.
    """
    with _ble_state_lock:
        connected = _ble_connected
        port = _ble_port
    payload = {
        "connected": connected,
        "port": port,
    }
    if connected:
        try:
            line = _send_cmd_oneline("STATUS")
            try:
                payload["device"] = json.loads(line)
            except json.JSONDecodeError:
                payload["device_raw"] = line
        except DumpError as exc:
            payload["device_error"] = str(exc)
    return jsonify(payload)


@bp.route("/api/flock_ble/version", methods=["GET"])
def ble_version():
    try:
        line = _send_cmd_oneline("VERSION")
        return jsonify({"status": "success", "version": line})
    except DumpError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400


@bp.route("/api/flock_ble/dump_prev", methods=["POST"])
def ble_dump_prev():
    try:
        lines = _send_cmd_stream("DUMP_PREV", marker="prev")
    except DumpError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400
    ingested = _ingest_replayed(lines, source_tag="flash")
    return jsonify({
        "status": "success",
        "returned": len(lines),
        "ingested": ingested,
        "source": "flash",
    })


@bp.route("/api/flock_ble/dump_live", methods=["POST"])
def ble_dump_live():
    try:
        lines = _send_cmd_stream("DUMP_LIVE", marker="live")
    except DumpError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400
    ingested = _ingest_replayed(lines, source_tag="ram")
    return jsonify({
        "status": "success",
        "returned": len(lines),
        "ingested": ingested,
        "source": "ram",
    })


@bp.route("/api/flock_ble/clear_prev", methods=["POST"])
def ble_clear_prev():
    try:
        line = _send_cmd_oneline("CLEAR_PREV")
    except DumpError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400
    return jsonify({"status": "success" if line.strip() == "OK" else "error",
                    "reply": line})


@bp.route("/api/flock_ble/clear_live", methods=["POST"])
def ble_clear_live():
    try:
        line = _send_cmd_oneline("CLEAR_LIVE")
    except DumpError as exc:
        return jsonify({"status": "error", "message": str(exc)}), 400
    return jsonify({"status": "success" if line.strip() == "OK" else "error",
                    "reply": line})
