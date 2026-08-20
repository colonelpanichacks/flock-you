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
import threading
import time
from datetime import datetime

import serial
import serial.tools.list_ports


bp = Blueprint("flockyou_ble", __name__)


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
