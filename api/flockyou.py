from flask import Flask, render_template, request, jsonify, send_file
import json
import csv
import os
from datetime import datetime
import time
from flask_socketio import SocketIO, emit, join_room, leave_room
import threading
import serial
import serial.tools.list_ports
import socket
import queue
import uuid
import pickle
from pathlib import Path

app = Flask(__name__)
app.config['SECRET_KEY'] = os.environ.get('SECRET_KEY', 'flockyou_dev_key_2024')
# Pick up template edits on refresh. Without this, Jinja caches index.html on
# first render (debug is off) and dashboard changes only appear after a restart.
app.config['TEMPLATES_AUTO_RELOAD'] = True
app.jinja_env.auto_reload = True
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='threading', logger=True, engineio_logger=True)

# BLE-side companion (Mode 1 oui-spy-unified-blue Detector). Handles a second
# serial port and answers /api/flock_ble/* endpoints. Blueprint is registered
# after this app object exists; the ingest sink is wired at __main__ time so
# BLE detections land in the same detections list as WiFi hits.
from flockyou_ble import bp as flock_ble_bp, init_bridge as flock_ble_init_bridge
app.register_blueprint(flock_ble_bp)

# Global variables
detections = []
cumulative_detections = []
session_start_time = datetime.now()
gps_data = None
gps_history = []  # Buffer of recent GPS readings for temporal matching
MAX_GPS_HISTORY = 100  # Keep last 100 GPS readings
GPS_MATCH_THRESHOLD = 30  # Max seconds between detection and GPS reading
serial_connection = None
gps_enabled = False
flock_device_connected = False
flock_device_port = None
# Progress of an in-flight dump_session transfer from the device.
session_dump = {'active': False, 'source': None, 'expected': 0, 'received': 0}
flock_serial_connection = None
oui_database = {}
serial_data_buffer = []
reconnect_attempts = {'flock': 0, 'gps': 0}
max_reconnect_attempts = 5
reconnect_delay = 3  # seconds
connection_lock = threading.Lock()
serial_queue = queue.Queue()
next_detection_id = 1  # Unique ID counter

# Last per-tier beep configuration reported by the device, refreshed from the
# {"event":"config"} line it emits on boot and after every set_beep command.
# None until the device has spoken, so the UI can tell "muted" apart from
# "haven't heard from the device yet".
device_config = None
device_write_lock = threading.Lock()
settings = {
    'gps_source': 'serial',        # 'serial' | 'gpsd'
    'gps_port': '',                # serial device path
    'gpsd_host': 'localhost',      # gpsd hostname or IP
    'gpsd_port': 2947,             # gpsd TCP port
    'flock_port': '',
    'filter': 'all',
}

# gpsd (source='gpsd') state — parallel to the serial GPS globals. Both
# feed into the same `gps_data` / `gps_history` structure so the rest of
# the pipeline is source-agnostic.
gpsd_socket = None
gps_source = None              # 'serial' | 'gpsd' | None

# Data storage paths
DATA_DIR = Path('data')
CUMULATIVE_DATA_FILE = DATA_DIR / 'cumulative_detections.pkl'
SESSION_DATA_FILE = DATA_DIR / 'session_detections.pkl'
SETTINGS_FILE = DATA_DIR / 'settings.json'

# Ensure data directory exists
DATA_DIR.mkdir(exist_ok=True)

# Persistent storage functions
def load_cumulative_detections():
    """Load cumulative detections from disk"""
    global cumulative_detections
    try:
        if CUMULATIVE_DATA_FILE.exists():
            with open(CUMULATIVE_DATA_FILE, 'rb') as f:
                cumulative_detections = pickle.load(f)
            print(f"Loaded {len(cumulative_detections)} cumulative detections")
        else:
            cumulative_detections = []
    except Exception as e:
        print(f"Error loading cumulative detections: {e}")
        cumulative_detections = []

def save_cumulative_detections():
    """Save cumulative detections to disk"""
    try:
        with open(CUMULATIVE_DATA_FILE, 'wb') as f:
            pickle.dump(cumulative_detections, f)
        print(f"Saved {len(cumulative_detections)} cumulative detections")
    except Exception as e:
        print(f"Error saving cumulative detections: {e}")

def load_session_detections():
    """Restore the current session list so a dashboard restart does not wipe
    it. The session only resets when the user clicks Clear."""
    global detections, next_detection_id, session_start_time
    try:
        if SESSION_DATA_FILE.exists():
            with open(SESSION_DATA_FILE, 'rb') as f:
                state = pickle.load(f)
            detections = state.get('detections', [])
            next_detection_id = max([d.get('id', 0) for d in detections] + [0]) + 1
            start = state.get('session_start_time')
            if start:
                session_start_time = datetime.fromisoformat(start)
            print(f"Loaded {len(detections)} session detections "
                  f"(session started {session_start_time.isoformat(timespec='seconds')})")
    except Exception as e:
        print(f"Error loading session detections: {e}")
        detections = []

def save_session_detections():
    """Persist the current session list; written on every change."""
    try:
        with open(SESSION_DATA_FILE, 'wb') as f:
            pickle.dump({'detections': detections,
                         'session_start_time': session_start_time.isoformat()}, f)
    except Exception as e:
        print(f"Error saving session detections: {e}")

def load_settings():
    """Load settings from disk"""
    global settings
    try:
        if SETTINGS_FILE.exists():
            with open(SETTINGS_FILE, 'r') as f:
                settings.update(json.load(f))
            print(f"Loaded settings: {settings}")
    except Exception as e:
        print(f"Error loading settings: {e}")

def save_settings():
    """Save settings to disk"""
    try:
        with open(SETTINGS_FILE, 'w') as f:
            json.dump(settings, f, indent=2)
        print(f"Saved settings: {settings}")
    except Exception as e:
        print(f"Error saving settings: {e}")

# Load OUI database
def load_oui_database():
    """Load the IEEE OUI database for manufacturer lookups"""
    global oui_database
    try:
        with open('oui.txt', 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#') and '(hex)' in line:
                    # Parse OUI line format: "28-6F-B9   (hex)                Nokia Shanghai Bell Co., Ltd."
                    parts = line.split('(hex)')
                    if len(parts) == 2:
                        mac_prefix = parts[0].strip().replace('-', '').replace(' ', '').upper()
                        manufacturer = parts[1].strip()
                        if mac_prefix and manufacturer and len(mac_prefix) == 6:
                            oui_database[mac_prefix] = manufacturer
        print(f"Loaded {len(oui_database)} OUI entries")
    except Exception as e:
        print(f"Error loading OUI database: {e}")

def lookup_manufacturer(mac_address):
    """Look up manufacturer information for a MAC address"""
    if not mac_address:
        return None
    
    # Extract first 6 characters (3 bytes) of MAC address
    mac_clean = mac_address.replace(':', '').replace('-', '').upper()
    if len(mac_clean) >= 6:
        oui = mac_clean[:6]
        return oui_database.get(oui, "Unknown Manufacturer")
    return "Unknown Manufacturer"

# GPS Dongle Configuration
GPS_BAUDRATE = 9600
GPS_TIMEOUT = 1

class GPSData:
    def __init__(self):
        self.latitude = None
        self.longitude = None
        self.altitude = None
        self.timestamp = None
        self.fix_quality = 0
        self.satellites = 0

def parse_nmea_sentence(sentence):
    """Parse NMEA GPS sentence with improved accuracy"""
    if not sentence.startswith('$'):
        return None
    
    parts = sentence.strip().split(',')
    if len(parts) < 1:
        return None
    
    sentence_type = parts[0]
    
    if sentence_type in ['$GPGGA', '$GNGGA']:  # Global Positioning System Fix Data (GPS + GLONASS)
        if len(parts) >= 15:
            try:
                time_str = parts[1]
                lat_raw = parts[2]
                lat_dir = parts[3]
                lon_raw = parts[4]
                lon_dir = parts[5]
                fix_quality = int(parts[6]) if parts[6] else 0
                satellites = int(parts[7]) if parts[7] else 0
                hdop = float(parts[8]) if parts[8] else 0  # Horizontal Dilution of Precision
                altitude = float(parts[9]) if parts[9] else 0
                
                # Skip if no fix
                if fix_quality == 0 or not lat_raw or not lon_raw:
                    return None
                
                # Convert NMEA format (DDMM.MMMM) to decimal degrees with high precision
                lat_degrees = int(lat_raw[:2])
                lat_minutes = float(lat_raw[2:])
                lat = lat_degrees + (lat_minutes / 60.0)
                
                lon_degrees = int(lon_raw[:3])
                lon_minutes = float(lon_raw[3:])
                lon = lon_degrees + (lon_minutes / 60.0)
                
                # Apply direction
                if lat_dir == 'S':
                    lat = -lat
                if lon_dir == 'W':
                    lon = -lon
                
                return {
                    'latitude': round(lat, 8),  # 8 decimal places for ~1.1mm accuracy
                    'longitude': round(lon, 8),
                    'altitude': round(altitude, 3),
                    'fix_quality': fix_quality,
                    'satellites': satellites,
                    'hdop': hdop,
                    'timestamp': time_str
                }
            except (ValueError, IndexError) as e:
                print(f"GPS parsing error: {e}")
                return None
    
    return None

def safe_socket_emit(event, data, room=None):
    """Safely emit socket events with error handling"""
    try:
        if room:
            socketio.emit(event, data, room=room)
        else:
            socketio.emit(event, data)
    except Exception as e:
        print(f"Socket emit error for {event}: {e}")

def gps_reader():
    """Background thread for reading GPS data"""
    global gps_data, serial_connection, gps_enabled
    
    while gps_enabled:
        if serial_connection and serial_connection.is_open:
            try:
                line = serial_connection.readline().decode('utf-8', errors='ignore')
                if line:
                    # Send raw GPS data to serial terminal
                    safe_socket_emit('serial_data', f"GPS: {line.strip()}", room='serial_terminal')
                    
                    parsed = parse_nmea_sentence(line)
                    if parsed:
                        gps_data = parsed
                        
                        # Add to GPS history with timestamp for temporal matching
                        if parsed.get('fix_quality') > 0:
                            gps_entry = parsed.copy()
                            gps_entry['system_timestamp'] = time.time()
                            gps_history.append(gps_entry)
                            
                            # Keep only recent GPS readings
                            if len(gps_history) > MAX_GPS_HISTORY:
                                gps_history.pop(0)
                        
                        safe_socket_emit('gps_update', parsed)
                        
                        # Also send parsed GPS data to terminal
                        if parsed.get('fix_quality') > 0:
                            gps_info = f"GPS Fix: {parsed.get('latitude', 'N/A')}, {parsed.get('longitude', 'N/A')} - {parsed.get('satellites', 0)} satellites"
                            safe_socket_emit('serial_data', gps_info, room='serial_terminal')
            except Exception as e:
                print(f"GPS read error: {e}")
                with connection_lock:
                    gps_enabled = False
                safe_socket_emit('gps_disconnected', {})
                break
        time.sleep(0.1)

def parse_gpsd_tpv(msg, sat_count=0):
    """Convert a gpsd TPV JSON object into the same dict shape as
    parse_nmea_sentence(). Returns None if the fix isn't usable."""
    if msg.get('class') != 'TPV':
        return None
    mode = msg.get('mode', 0)  # 0=unknown, 1=no-fix, 2=2D, 3=3D
    if mode < 2:
        return None
    lat = msg.get('lat')
    lon = msg.get('lon')
    if lat is None or lon is None:
        return None
    alt = msg.get('altHAE')
    if alt is None:
        alt = msg.get('alt', 0) or 0
    return {
        'latitude': round(float(lat), 8),
        'longitude': round(float(lon), 8),
        'altitude': round(float(alt), 3),
        # gpsd mode 2 (2D) ≈ NMEA quality 1, mode 3 (3D) ≈ quality 2.
        'fix_quality': mode - 1,
        'satellites': int(sat_count) if sat_count else 0,
        'hdop': float(msg.get('hdop', 0) or 0),
        'timestamp': msg.get('time', ''),
    }

def gpsd_reader():
    """Background thread that reads from a running gpsd instance and
    feeds gps_data / gps_history in the same shape as the serial NMEA
    reader. Speaks gpsd's line-delimited JSON protocol directly — no
    third-party gps client library needed."""
    global gps_data, gpsd_socket, gps_enabled

    sat_count = 0
    buf = ''

    while gps_enabled:
        sock = gpsd_socket
        if not sock:
            time.sleep(0.1)
            continue
        try:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError('gpsd closed the socket')
            buf += chunk.decode('utf-8', errors='ignore')
            while '\n' in buf:
                line, buf = buf.split('\n', 1)
                line = line.strip()
                if not line:
                    continue
                safe_socket_emit('serial_data', f"gpsd: {line}", room='serial_terminal')
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue

                cls = msg.get('class')
                if cls == 'SKY':
                    sats = msg.get('satellites') or []
                    sat_count = sum(1 for s in sats if s.get('used'))
                    continue
                if cls != 'TPV':
                    continue

                parsed = parse_gpsd_tpv(msg, sat_count=sat_count)
                if not parsed:
                    continue

                gps_data = parsed
                if parsed.get('fix_quality', 0) > 0:
                    gps_entry = parsed.copy()
                    gps_entry['system_timestamp'] = time.time()
                    gps_history.append(gps_entry)
                    if len(gps_history) > MAX_GPS_HISTORY:
                        gps_history.pop(0)

                safe_socket_emit('gps_update', parsed)
                if parsed.get('fix_quality', 0) > 0:
                    gps_info = (f"gpsd Fix: {parsed.get('latitude', 'N/A')}, "
                                f"{parsed.get('longitude', 'N/A')} - "
                                f"{parsed.get('satellites', 0)} satellites")
                    safe_socket_emit('serial_data', gps_info, room='serial_terminal')
        except socket.timeout:
            continue
        except Exception as e:
            print(f"gpsd read error: {e}")
            with connection_lock:
                gps_enabled = False
            try:
                if gpsd_socket:
                    gpsd_socket.close()
            except Exception:
                pass
            gpsd_socket = None
            safe_socket_emit('gps_disconnected', {})
            break

def flock_reader():
    """Background thread for reading Flock device data"""
    global flock_serial_connection, flock_device_connected, serial_data_buffer
    
    with app.app_context():
        while flock_device_connected:
            if flock_serial_connection and flock_serial_connection.is_open:
                try:
                    line = flock_serial_connection.readline().decode('utf-8', errors='ignore')
                    if line:
                        line = line.strip()
                        if line:
                            # Store in buffer for terminal
                            serial_data_buffer.append(line)
                            if len(serial_data_buffer) > 1000:  # Keep last 1000 lines
                                serial_data_buffer.pop(0)
                            
                            # Forward to all serial terminal clients
                            safe_socket_emit('serial_data', line, room='serial_terminal')
                            print(f"Serial data sent to terminal: {line}")
                            
                            # Try to parse as detection data
                            try:
                                data = json.loads(line)
                                event = data.get('event')
                                if event == 'session_begin':
                                    session_dump.update(active=True, source=data.get('source'),
                                                        expected=data.get('count', 0), received=0)
                                    safe_socket_emit('session_dump', {'phase': 'begin', **session_dump})
                                elif event == 'session_det':
                                    import_detection_item(data)
                                    session_dump['received'] += 1
                                    if session_dump['received'] % 10 == 0:
                                        safe_socket_emit('session_dump', {'phase': 'progress', **session_dump})
                                elif event == 'session_end':
                                    session_dump['active'] = False
                                    safe_socket_emit('session_dump', {'phase': 'end', **session_dump,
                                                                      'device_count': data.get('count')})
                                    print(f"Session dump ({session_dump['source']}): imported {session_dump['received']} detections")
                                elif event == 'session_error':
                                    session_dump['active'] = False
                                    safe_socket_emit('session_dump', {'phase': 'error', **session_dump,
                                                                      'error': data.get('error', 'unknown')})
                                elif event == 'config':
                                    # Device reporting its per-tier beep mask.
                                    globals()['device_config'] = data
                                    safe_socket_emit('device_config', data)
                                    print(f"Device config: beep_mask={data.get('beep_mask')}")
                                elif 'detection_method' in data:
                                    # Map ESP32 GPS from phone to Flask GPS format
                                    esp_gps = data.get('gps')
                                    if esp_gps:
                                        data['gps'] = {
                                            'latitude': esp_gps.get('latitude'),
                                            'longitude': esp_gps.get('longitude'),
                                            'fix_quality': 1,
                                            'match_quality': 'esp32_phone_gps',
                                            'time_diff': 0,
                                        }
                                        if esp_gps.get('accuracy') is not None:
                                            data['gps']['accuracy'] = esp_gps['accuracy']
                                    # This is a detection, add it
                                    add_detection_from_serial(data)
                                else:
                                    print(f"JSON data without detection_method: {data}")
                            except json.JSONDecodeError:
                                # Not JSON, just log it
                                print(f"Flock device (non-JSON): {line}")
                                
                except Exception as e:
                    print(f"Flock device read error: {e}")
                    with connection_lock:
                        flock_device_connected = False
                    safe_socket_emit('flock_disconnected', {})
                    # Trigger reconnection immediately
                    attempt_reconnect_flock()
                    break
            time.sleep(0.1)

def find_best_gps_match(detection_timestamp):
    """Find the GPS reading closest in time to the detection timestamp"""
    global gps_history
    
    if not gps_history:
        return None
    
    try:
        # Convert detection timestamp to epoch time
        if isinstance(detection_timestamp, str):
            # Try parsing ISO format first
            try:
                dt = datetime.fromisoformat(detection_timestamp.replace('Z', '+00:00'))
            except:
                # Try parsing the display format
                dt = datetime.strptime(detection_timestamp, '%Y-%m-%d %H:%M:%S')
            detection_time = dt.timestamp()
        else:
            detection_time = detection_timestamp
        
        best_match = None
        min_time_diff = float('inf')
        
        for gps_entry in gps_history:
            gps_time = gps_entry['system_timestamp']
            time_diff = abs(detection_time - gps_time)
            
            if time_diff < min_time_diff and time_diff <= GPS_MATCH_THRESHOLD:
                min_time_diff = time_diff
                best_match = gps_entry
        
        return best_match
    except Exception as e:
        print(f"Error finding GPS match: {e}")
        return None

def validate_gps_data(gps_data):
    """Validate GPS data integrity"""
    if not gps_data:
        return False, "No GPS data"
    
    lat = gps_data.get('latitude')
    lon = gps_data.get('longitude')
    
    if lat is None or lon is None:
        return False, "Missing coordinates"
    
    # Basic coordinate validation
    if not (-90 <= lat <= 90):
        return False, f"Invalid latitude: {lat}"
    
    if not (-180 <= lon <= 180):
        return False, f"Invalid longitude: {lon}"
    
    fix_quality = gps_data.get('fix_quality', 0)
    if fix_quality < 1:
        return False, f"Poor GPS fix quality: {fix_quality}"
    
    return True, "Valid GPS data"

# Confidence tier per detection method, mirroring the firmware's tier ladder.
# Used to decide which label wins when several paths catch the same MAC, and
# to colour-code the dashboard. Kept as a lookup rather than trusting the
# device's detection_tier field alone, so detections imported from older
# firmware (or from JSON/CSV files) still classify correctly.
METHOD_TIERS = {
    'wifi_wildcard_probe_ie_sig': 4,
    'wifi_wildcard_probe':        3,
    'wifi_oui_addr2':             2,
    'wifi_oui_addr1':             1,
    'wifi_oui_addr3':             1,
    'wifi_ssid':                  0,
}

METHOD_LABELS = {
    'wifi_wildcard_probe_ie_sig': 'IE fingerprint',
    'wifi_wildcard_probe':        'Wildcard probe',
    'wifi_oui_addr2':             'OUI transmitter',
    'wifi_oui_addr1':             'OUI receiver',
    'wifi_oui_addr3':             'OUI BSSID',
    'wifi_ssid':                  'SSID keyword',
}

# Reverse map so a KML exported with human-readable labels can still be
# imported back to canonical method keys.
LABEL_TO_METHOD = {v: k for k, v in METHOD_LABELS.items()}

# The firmware's SPIFFS session format writes bare method names (no wifi_
# prefix); normalise them on import so an on-device session file and a
# dashboard export both land on the same canonical keys.
def normalize_method(raw):
    if not raw:
        return 'unknown'
    m = str(raw).strip()
    if m in METHOD_TIERS:
        return m
    if m in LABEL_TO_METHOD:              # "IE fingerprint"
        return LABEL_TO_METHOD[m]
    if ('wifi_' + m) in METHOD_TIERS:     # "wildcard_probe_ie_sig"
        return 'wifi_' + m
    return m


def method_tier(data_or_method):
    """Resolve a detection's confidence tier, preferring the device's own value."""
    if isinstance(data_or_method, dict):
        tier = data_or_method.get('detection_tier')
        if isinstance(tier, int):
            return tier
        method = data_or_method.get('detection_method')
    else:
        method = data_or_method
    return METHOD_TIERS.get(method, 0)


def method_label(method):
    return METHOD_LABELS.get(method, method or 'unknown')


def add_detection_from_serial(data):
    """Add detection from serial data - counts detections per MAC address"""
    global detections, cumulative_detections, gps_data, next_detection_id
    
    # Add server timestamp first (system time when detection was processed)
    system_time = time.time()
    data['server_timestamp'] = datetime.fromtimestamp(system_time).isoformat()
    
    # Try to find the best GPS match for this detection's timestamp
    best_gps = find_best_gps_match(system_time)
    preferred_timestamp = None
    
    if best_gps:
        # Validate GPS data before using it
        is_valid, validation_msg = validate_gps_data(best_gps)
        if is_valid:
            time_diff = abs(system_time - best_gps['system_timestamp'])
            data['gps'] = {
                'latitude': best_gps.get('latitude'),
                'longitude': best_gps.get('longitude'),
                'altitude': best_gps.get('altitude'),
                'timestamp': best_gps.get('timestamp'),
                'satellites': best_gps.get('satellites'),
                'fix_quality': best_gps.get('fix_quality'),
                'time_diff': time_diff,
                'match_quality': 'temporal'
            }
            # Prefer GPS timestamp when available and accurate
            if time_diff < 5:  # Very close temporal match
                preferred_timestamp = best_gps.get('timestamp')
                print(f"✓ Using GPS timestamp for MAC {data.get('mac_address', 'unknown')}: {time_diff:.2f}s difference")
            else:
                print(f"✓ GPS temporal match for MAC {data.get('mac_address', 'unknown')}: {time_diff:.2f}s difference")
        else:
            print(f"⚠ Invalid GPS data for temporal match: {validation_msg}")
            best_gps = None
    
    # Fallback to current GPS if no good temporal match
    if not best_gps and gps_data and gps_data.get('fix_quality') > 0:
        is_valid, validation_msg = validate_gps_data(gps_data)
        if is_valid:
            data['gps'] = {
                'latitude': gps_data.get('latitude'),
                'longitude': gps_data.get('longitude'),
                'altitude': gps_data.get('altitude'),
                'timestamp': gps_data.get('timestamp'),
                'satellites': gps_data.get('satellites'),
                'fix_quality': gps_data.get('fix_quality'),
                'time_diff': None,  # Unknown time difference
                'match_quality': 'current'
            }
            # Use current GPS timestamp if available
            preferred_timestamp = gps_data.get('timestamp')
            print(f"○ Using current GPS timestamp for MAC {data.get('mac_address', 'unknown')} (no temporal match)")
        else:
            print(f"⚠ Current GPS data invalid: {validation_msg}")
    
    # Set timestamps - prefer GPS timestamp when available
    if preferred_timestamp:
        data['timestamp'] = preferred_timestamp
        data['detection_time'] = preferred_timestamp
        data['timestamp_source'] = 'gps'
        print(f"📍 Using GPS timestamp as primary timestamp for {data.get('mac_address', 'unknown')}")
    else:
        # Fallback to system timestamps
        system_dt = datetime.fromtimestamp(system_time)
        data['timestamp'] = system_dt.isoformat()
        data['detection_time'] = system_dt.strftime('%Y-%m-%d %H:%M:%S')
        data['timestamp_source'] = 'system'
        print(f"🕐 Using system timestamp for {data.get('mac_address', 'unknown')} (no GPS available)")
    
    # Log if no GPS could be assigned
    if not data.get('gps'):
        print(f"✗ No valid GPS data available for MAC {data.get('mac_address', 'unknown')}")
    
    # Add manufacturer information
    if 'mac_address' in data:
        data['manufacturer'] = lookup_manufacturer(data['mac_address'])
    
    # Check if we already have a detection for this MAC address
    mac_address = data.get('mac_address')
    existing_detection = None
    
    if mac_address:
        for detection in detections:
            if detection.get('mac_address') == mac_address:
                existing_detection = detection
                break
    
    if existing_detection:
        # Update existing detection with new data and increment count
        existing_detection['detection_count'] = existing_detection.get('detection_count', 1) + 1
        existing_detection['last_seen'] = datetime.now().isoformat()
        existing_detection['last_rssi'] = data.get('rssi', existing_detection.get('last_rssi'))
        existing_detection['last_channel'] = data.get('channel', existing_detection.get('last_channel'))
        existing_detection['last_frequency'] = data.get('frequency', existing_detection.get('last_frequency'))
        existing_detection['last_ssid'] = data.get('ssid', existing_detection.get('last_ssid'))
        existing_detection['last_device_name'] = data.get('device_name', existing_detection.get('last_device_name'))
        
        # Detection method is best-ever, not first-ever. A camera first caught
        # by a broad OUI echo and later confirmed by the IE fingerprint should
        # read as the fingerprint from then on — the old code only filled the
        # field when empty, so the weaker label stuck permanently.
        new_method = data.get('detection_method')
        if new_method:
            new_tier = method_tier(data)
            if new_tier >= method_tier(existing_detection):
                existing_detection['detection_method'] = new_method
                existing_detection['detection_tier'] = new_tier
            # Every path that has ever caught this device, with a hit count.
            # This is the interesting research artifact: it shows which
            # cameras only ever trip the broad paths and never fingerprint.
            seen = existing_detection.setdefault('methods_seen', {})
            seen[new_method] = seen.get(new_method, 0) + 1
            existing_detection['last_method'] = new_method


        # Update GPS if new data is available
        if data.get('gps'):
            existing_detection['gps'] = data['gps']
        
        # Update cumulative detections
        for cum_detection in cumulative_detections:
            if cum_detection.get('mac_address') == mac_address:
                cum_detection.update(existing_detection)
                break
        save_cumulative_detections()
        save_session_detections()
        
        # Emit updated detection
        safe_socket_emit('detection_updated', existing_detection)
        print(f"Updated detection: MAC {mac_address}, Count: {existing_detection['detection_count']}, Method: {existing_detection.get('detection_method')}")
    else:
        # Create new detection
        data['id'] = next_detection_id
        next_detection_id += 1
        data['alias'] = ''  # Empty alias by default
        # Imported records (file import, dump_session) carry the device's hit count.
        data['detection_count'] = int(data.get('detection_count') or 1)
        data['first_seen'] = datetime.now().isoformat()
        data['last_seen'] = datetime.now().isoformat()
        data['detection_tier'] = method_tier(data)
        data['last_method'] = data.get('detection_method')
        if data.get('detection_method'):
            data['methods_seen'] = {data['detection_method']: 1}
        
        detections.append(data)
        
        # Add to cumulative detections
        cumulative_detections.append(data.copy())
        save_cumulative_detections()
        save_session_detections()
        
        # Emit to connected clients
        safe_socket_emit('new_detection', data)
        print(f"New detection added: ID {data['id']}, Method: {data.get('detection_method')}, MAC: {mac_address}")

def connection_monitor():
    """Background thread for monitoring device connections"""
    global gps_enabled, flock_device_connected, serial_connection, reconnect_attempts
    
    with app.app_context():
        while True:
            # Check GPS connection
            if gps_enabled:
                try:
                    if not serial_connection or not serial_connection.is_open:
                        with connection_lock:
                            gps_enabled = False
                        safe_socket_emit('gps_disconnected', {})
                        print("GPS connection lost")
                        # Start reconnection attempts
                        attempt_reconnect_gps()
                    else:
                        # Test if the connection is still valid
                        serial_connection.in_waiting
                except Exception as e:
                    print(f"GPS connection test failed: {e}")
                    with connection_lock:
                        gps_enabled = False
                    safe_socket_emit('gps_disconnected', {})
                    attempt_reconnect_gps()
            
            # Check Flock You device connection
            if flock_device_connected:
                try:
                    # Test if the connection is still valid
                    if not flock_serial_connection or not flock_serial_connection.is_open:
                        with connection_lock:
                            flock_device_connected = False
                        safe_socket_emit('flock_disconnected', {})
                        print("Flock You device connection lost")
                        # Start reconnection attempts
                        attempt_reconnect_flock()
                    else:
                        # Try a simple read to test connection
                        flock_serial_connection.in_waiting
                except Exception as e:
                    print(f"Flock device connection test failed: {e}")
                    with connection_lock:
                        flock_device_connected = False
                    safe_socket_emit('flock_disconnected', {})
                    # Start reconnection attempts
                    attempt_reconnect_flock()
            
            time.sleep(2)  # Check every 2 seconds

def attempt_reconnect_flock():
    """Attempt to reconnect to Flock device"""
    global flock_device_connected, reconnect_attempts, flock_serial_connection
    
    def reconnect_thread():
        global flock_device_connected, reconnect_attempts, flock_serial_connection
        
        with app.app_context():
            while not flock_device_connected and reconnect_attempts['flock'] < max_reconnect_attempts:
                try:
                    print(f"Attempting to reconnect to Flock device (attempt {reconnect_attempts['flock'] + 1}/{max_reconnect_attempts})")
                    
                    # Try to reconnect
                    if flock_serial_connection:
                        try:
                            flock_serial_connection.close()
                        except:
                            pass
                    
                    # Wait a moment for the device to be ready
                    time.sleep(1)
                    
                    flock_serial_connection = serial.Serial(flock_device_port, 115200, timeout=1)
                    
                    # Test the connection
                    test_data = flock_serial_connection.readline()
                    
                    # If successful, update status
                    with connection_lock:
                        flock_device_connected = True
                    reconnect_attempts['flock'] = 0
                    print(f"Successfully reconnected to Flock device on {flock_device_port}")
                    safe_socket_emit('flock_reconnected', {'port': flock_device_port})
                    
                    # Restart the reading thread
                    flock_thread = threading.Thread(target=flock_reader, daemon=True)
                    flock_thread.start()
                    return
                    
                except Exception as e:
                    print(f"Reconnection attempt failed: {e}")
                    reconnect_attempts['flock'] += 1
                    time.sleep(reconnect_delay)
            
            if reconnect_attempts['flock'] >= max_reconnect_attempts:
                print("Max reconnection attempts reached for Flock device")
                safe_socket_emit('reconnect_failed', {'device': 'flock'})
                reconnect_attempts['flock'] = 0  # Reset for future attempts
    
    thread = threading.Thread(target=reconnect_thread, daemon=True)
    thread.start()

def attempt_reconnect_gps():
    """Attempt to reconnect whichever GPS source was in use. Only fires
    if the reader thread died — for a user-initiated disconnect the
    source is cleared and this exits immediately."""
    global gps_enabled, gpsd_socket, serial_connection, reconnect_attempts

    prior_source = gps_source
    # Snapshot the endpoint from whichever source was live so we can
    # retry without depending on the (now-torn-down) object.
    prior_serial_port = serial_connection.port if serial_connection else None
    prior_gpsd_endpoint = None
    if gpsd_socket:
        try:
            prior_gpsd_endpoint = gpsd_socket.getpeername()[:2]
        except Exception:
            prior_gpsd_endpoint = None

    if prior_source is None:
        return

    def reconnect_thread():
        global gps_enabled, gpsd_socket, serial_connection, gps_source, reconnect_attempts

        with app.app_context():
            while not gps_enabled and reconnect_attempts['gps'] < max_reconnect_attempts:
                try:
                    print(f"Attempting to reconnect GPS ({prior_source}) "
                          f"attempt {reconnect_attempts['gps'] + 1}/{max_reconnect_attempts}")

                    if prior_source == 'serial' and prior_serial_port:
                        serial_connection = serial.Serial(prior_serial_port, GPS_BAUDRATE,
                                                          timeout=GPS_TIMEOUT)
                        with connection_lock:
                            gps_enabled = True
                            gps_source = 'serial'
                        threading.Thread(target=gps_reader, daemon=True).start()
                        safe_socket_emit('gps_reconnected',
                                         {'source': 'serial', 'port': prior_serial_port})
                    elif prior_source == 'gpsd' and prior_gpsd_endpoint:
                        host, port = prior_gpsd_endpoint
                        sock = socket.create_connection((host, port), timeout=5)
                        sock.settimeout(2.0)
                        sock.sendall(b'?WATCH={"enable":true,"json":true}\n')
                        gpsd_socket = sock
                        with connection_lock:
                            gps_enabled = True
                            gps_source = 'gpsd'
                        threading.Thread(target=gpsd_reader, daemon=True).start()
                        safe_socket_emit('gps_reconnected',
                                         {'source': 'gpsd', 'endpoint': f'{host}:{port}'})
                    else:
                        print(f"Cannot reconnect: no endpoint recorded for {prior_source}")
                        break

                    reconnect_attempts['gps'] = 0
                    print(f"Successfully reconnected GPS ({prior_source})")
                    return

                except Exception as e:
                    print(f"GPS reconnection attempt failed: {e}")
                    reconnect_attempts['gps'] += 1
                    time.sleep(reconnect_delay)

            if reconnect_attempts['gps'] >= max_reconnect_attempts:
                print("Max reconnection attempts reached for GPS device")
                safe_socket_emit('reconnect_failed', {'device': 'gps'})
                reconnect_attempts['gps'] = 0

    thread = threading.Thread(target=reconnect_thread, daemon=True)
    thread.start()

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/detections', methods=['GET'])
def get_detections():
    """Get all detections with optional filtering"""
    filter_type = request.args.get('filter', 'all')
    data_type = request.args.get('type', 'session')
    
    # Choose data source
    if data_type == 'cumulative':
        source_data = cumulative_detections
    else:
        source_data = detections
    
    # Apply filter
    if filter_type == 'all':
        return jsonify(source_data)
    else:
        filtered = [d for d in source_data if d.get('detection_method') == filter_type]
        return jsonify(filtered)

@app.route('/api/detections', methods=['POST'])
def add_detection():
    """Add a new detection from serial data"""
    global detections, gps_data
    
    data = request.json
    
    # Add GPS data if available
    if gps_data and gps_data.get('fix_quality') > 0:
        data['gps'] = {
            'latitude': gps_data.get('latitude'),
            'longitude': gps_data.get('longitude'),
            'altitude': gps_data.get('altitude'),
            'timestamp': gps_data.get('timestamp'),
            'satellites': gps_data.get('satellites')
        }
    
    # Add manufacturer information
    if 'mac_address' in data:
        data['manufacturer'] = lookup_manufacturer(data['mac_address'])
    
    # Add server timestamp
    data['server_timestamp'] = datetime.now().isoformat()
    
    detections.append(data)
    
    # Emit to connected clients
    socketio.emit('new_detection', data)
    
    return jsonify({'status': 'success', 'id': len(detections)})

@app.route('/api/gps/connect', methods=['POST'])
def connect_gps():
    """Connect a GPS source.

    Request body:
      {"source": "serial", "port": "/dev/tty.usbserial-XXX"}
      {"source": "gpsd",   "host": "localhost", "port": 2947}

    Legacy body {"port": "..."} still works and is treated as serial.
    """
    global serial_connection, gpsd_socket, gps_enabled, gps_source

    data = request.json or {}
    source = (data.get('source') or 'serial').lower()

    if source == 'serial':
        port = data.get('port')
        if not port:
            return jsonify({'status': 'error', 'message': 'Serial port is required'}), 400
        try:
            _teardown_gps()
            serial_connection = serial.Serial(port, GPS_BAUDRATE, timeout=GPS_TIMEOUT)
            with connection_lock:
                gps_enabled = True
                gps_source = 'serial'
            threading.Thread(target=gps_reader, daemon=True).start()
            return jsonify({'status': 'success', 'source': 'serial',
                            'message': f'Connected to {port}'})
        except Exception as e:
            return jsonify({'status': 'error', 'message': str(e)}), 400

    if source == 'gpsd':
        host = data.get('host') or 'localhost'
        # Accept either "port" or "gpsd_port" — the frontend uses the
        # same field name for both sources but the settings key is
        # gpsd_port, so let both work.
        port = data.get('port') or data.get('gpsd_port') or 2947
        try:
            port = int(port)
        except (TypeError, ValueError):
            return jsonify({'status': 'error', 'message': f'Bad gpsd port: {port}'}), 400
        try:
            _teardown_gps()
            sock = socket.create_connection((host, port), timeout=5)
            sock.settimeout(2.0)
            # Ask gpsd to start streaming JSON reports.
            sock.sendall(b'?WATCH={"enable":true,"json":true}\n')
            gpsd_socket = sock
            with connection_lock:
                gps_enabled = True
                gps_source = 'gpsd'
            threading.Thread(target=gpsd_reader, daemon=True).start()
            return jsonify({'status': 'success', 'source': 'gpsd',
                            'message': f'Connected to gpsd at {host}:{port}'})
        except Exception as e:
            return jsonify({'status': 'error',
                            'message': f'gpsd connect failed ({host}:{port}): {e}'}), 400

    return jsonify({'status': 'error', 'message': f'Unknown GPS source: {source}'}), 400


def _teardown_gps():
    """Close whichever GPS source is currently active. Called on
    reconnect and disconnect so the reader threads see gps_enabled go
    False and exit cleanly."""
    global serial_connection, gpsd_socket, gps_enabled, gps_source
    with connection_lock:
        gps_enabled = False
        gps_source = None
    if serial_connection:
        try:
            serial_connection.close()
        except Exception:
            pass
        serial_connection = None
    if gpsd_socket:
        try:
            gpsd_socket.shutdown(socket.SHUT_RDWR)
        except Exception:
            pass
        try:
            gpsd_socket.close()
        except Exception:
            pass
        gpsd_socket = None


@app.route('/api/gps/disconnect', methods=['POST'])
def disconnect_gps():
    """Disconnect whichever GPS source is currently active."""
    _teardown_gps()
    return jsonify({'status': 'success', 'message': 'GPS disconnected'})

@app.route('/api/flock/connect', methods=['POST'])
def connect_flock():
    """Connect to Flock You device"""
    global flock_device_connected, flock_device_port, flock_serial_connection
    
    data = request.json
    port = data.get('port')
    
    try:
        # Create persistent connection to the port
        flock_serial_connection = serial.Serial(port, 115200, timeout=1)
        with connection_lock:
            flock_device_connected = True
        flock_device_port = port
        
        # Start reading thread
        flock_thread = threading.Thread(target=flock_reader, daemon=True)
        flock_thread.start()
        
        return jsonify({'status': 'success', 'message': f'Connected to Flock You device on {port}'})
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)}), 400

@app.route('/api/flock/disconnect', methods=['POST'])
def disconnect_flock():
    """Disconnect Flock You device"""
    global flock_device_connected, flock_device_port, flock_serial_connection
    
    with connection_lock:
        flock_device_connected = False
    flock_device_port = None
    
    if flock_serial_connection and flock_serial_connection.is_open:
        flock_serial_connection.close()
        flock_serial_connection = None
    
    return jsonify({'status': 'success', 'message': 'Flock You device disconnected'})


def send_device_command(payload):
    """Write one JSON command line to the device over USB CDC.

    Returns (ok, message). The device answers asynchronously with an
    {"event":"config"} line that flock_reader() picks up, so callers get the
    authoritative state via the device_config socket event rather than from
    this function's return value.
    """
    if not (flock_serial_connection and flock_serial_connection.is_open):
        return False, 'Flock You device not connected'
    line = (json.dumps(payload) + '\n').encode('utf-8')
    try:
        # Serialise writes: the reader thread runs concurrently and pyserial
        # gives no write atomicity guarantee across threads.
        with device_write_lock:
            flock_serial_connection.write(line)
            flock_serial_connection.flush()
        return True, 'sent'
    except Exception as e:
        return False, f'write failed: {e}'


@app.route('/api/flock/config', methods=['GET'])
def get_flock_config():
    """Last known per-tier beep config, and ask the device to re-report."""
    send_device_command({'cmd': 'get_config'})
    return jsonify({
        'status': 'success',
        'config': device_config,
        'connected': bool(flock_serial_connection and flock_serial_connection.is_open),
        'methods': METHOD_LABELS,
        'tiers': METHOD_TIERS,
    })


@app.route('/api/flock/beep', methods=['POST'])
def set_flock_beep():
    """Mute or unmute one detection tier's audio on the device.

    Body: {"tier": 0-4, "enabled": bool}
       or {"mask": 0-31} to set every tier at once.
    """
    body = request.get_json(silent=True) or {}

    if 'mask' in body:
        try:
            mask = int(body['mask'])
        except (TypeError, ValueError):
            return jsonify({'status': 'error', 'message': 'mask must be an integer'}), 400
        if not 0 <= mask <= 0x1F:
            return jsonify({'status': 'error', 'message': 'mask out of range 0-31'}), 400
        ok, msg = send_device_command({'cmd': 'set_beep_mask', 'mask': mask})
    else:
        try:
            tier = int(body.get('tier'))
        except (TypeError, ValueError):
            return jsonify({'status': 'error', 'message': 'tier must be an integer 0-4'}), 400
        if not 0 <= tier <= 4:
            return jsonify({'status': 'error', 'message': 'tier out of range 0-4'}), 400
        enabled = bool(body.get('enabled', True))
        ok, msg = send_device_command(
            {'cmd': 'set_beep', 'tier': tier, 'on': 1 if enabled else 0})

    if not ok:
        return jsonify({'status': 'error', 'message': msg}), 503
    return jsonify({'status': 'success', 'message': msg})


@app.route('/api/flock/dump_session', methods=['POST'])
def dump_flock_session():
    """Ask the device to stream its detection table over USB.

    Body: {"source": "live"} (default) for the table accumulated since boot,
       or {"source": "prev"} for /prev_session.json, the previous boot's run.
    The device answers with session_begin / session_det / session_end lines;
    flock_reader() imports each record and relays progress on the
    session_dump socket event.
    """
    body = request.get_json(silent=True) or {}
    source = body.get('source', 'live')
    if source not in ('live', 'prev'):
        return jsonify({'status': 'error', 'message': 'source must be "live" or "prev"'}), 400
    ok, msg = send_device_command({'cmd': 'dump_session', 'source': source})
    if not ok:
        return jsonify({'status': 'error', 'message': msg}), 503
    return jsonify({'status': 'success', 'message': f'Requested {source} session from device'})


@app.route('/api/status', methods=['GET'])
def get_status():
    """Get connection status of both devices"""
    gpsd_endpoint = None
    if gpsd_socket:
        try:
            host, port = gpsd_socket.getpeername()[:2]
            gpsd_endpoint = f'{host}:{port}'
        except Exception:
            gpsd_endpoint = None
    return jsonify({
        'gps_connected': gps_enabled,
        'gps_source': gps_source,
        'gps_port': serial_connection.port if serial_connection else None,
        'gpsd_endpoint': gpsd_endpoint,
        'flock_connected': flock_device_connected,
        'flock_port': flock_device_port
    })

@app.route('/api/gps/ports', methods=['GET'])
def get_gps_ports():
    """Get available serial ports for GPS"""
    ports = []
    for port in serial.tools.list_ports.comports():
        port_info = {
            'device': port.device,
            'description': port.description,
            'manufacturer': port.manufacturer if port.manufacturer else 'Unknown',
            'product': port.product if port.product else 'Unknown',
            'vid': port.vid,
            'pid': port.pid
        }
        ports.append(port_info)
    return jsonify(ports)

@app.route('/api/flock/ports', methods=['GET'])
def get_flock_ports():
    """Get available serial ports for Flock You device"""
    ports = []
    for port in serial.tools.list_ports.comports():
        port_info = {
            'device': port.device,
            'description': port.description,
            'manufacturer': port.manufacturer if port.manufacturer else 'Unknown',
            'product': port.product if port.product else 'Unknown',
            'vid': port.vid,
            'pid': port.pid
        }
        ports.append(port_info)
    return jsonify(ports)

@app.route('/api/export/csv', methods=['GET'])
def export_csv():
    """Export session detections as CSV"""
    export_type = request.args.get('type', 'session')
    
    if export_type == 'cumulative':
        data_to_export = cumulative_detections
        filename_prefix = "flockyou_cumulative"
    else:
        data_to_export = detections
        filename_prefix = f"flockyou_session_{session_start_time.strftime('%Y%m%d_%H%M%S')}"
    
    if not data_to_export:
        return jsonify({'status': 'error', 'message': 'No detections to export'}), 400
    
    filename = f"{filename_prefix}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
    filepath = os.path.join('exports', filename)
    
    os.makedirs('exports', exist_ok=True)
    
    with open(filepath, 'w', newline='', encoding='utf-8') as csvfile:
        # Grouped so the sheet reads left to right: what it is, how we caught
        # it, how strong the signal was, where, then when. GPS columns are all
        # gps_-prefixed so they sort and filter together.
        fieldnames = [
            # identity
            'mac_address', 'alias', 'manufacturer', 'ssid', 'device_name',
            # detection
            'detection_method', 'detection_label', 'detection_tier',
            'last_method', 'methods_seen', 'detection_count', 'protocol',
            # signal
            'rssi', 'last_rssi', 'signal_strength', 'channel', 'last_channel',
            # location
            'latitude', 'longitude', 'altitude',
            'gps_fix_quality', 'gps_satellites', 'gps_timestamp',
            'gps_time_diff', 'gps_match_quality',
            # time
            'first_seen', 'last_seen', 'detection_time', 'timestamp',
            'server_timestamp', 'timestamp_source',
        ]
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        
        for detection in data_to_export:
            gps_data = detection.get('gps', {})
            row = {
                # identity
                'mac_address': detection.get('mac_address'),
                'alias': detection.get('alias', ''),
                'manufacturer': detection.get('manufacturer', 'Unknown'),
                'ssid': detection.get('ssid', ''),
                'device_name': detection.get('device_name', ''),
                # detection
                'detection_method': detection.get('detection_method'),
                'detection_label': method_label(detection.get('detection_method')),
                'detection_tier': detection.get('detection_tier', method_tier(detection)),
                'last_method': detection.get('last_method', detection.get('detection_method')),
                # Flattened as "method:count; method:count" so a spreadsheet
                # keeps it in one cell instead of exploding the schema.
                'methods_seen': '; '.join(
                    f'{m}:{c}' for m, c in sorted((detection.get('methods_seen') or {}).items())
                ),
                'detection_count': detection.get('detection_count', 1),
                'protocol': detection.get('protocol'),
                # signal
                'rssi': detection.get('rssi'),
                'last_rssi': detection.get('last_rssi'),
                'signal_strength': detection.get('signal_strength'),
                'channel': detection.get('channel'),
                'last_channel': detection.get('last_channel'),
                # location
                'latitude': gps_data.get('latitude'),
                'longitude': gps_data.get('longitude'),
                'altitude': gps_data.get('altitude'),
                'gps_fix_quality': gps_data.get('fix_quality'),
                'gps_satellites': gps_data.get('satellites'),
                'gps_timestamp': gps_data.get('timestamp'),
                'gps_time_diff': gps_data.get('time_diff'),
                'gps_match_quality': gps_data.get('match_quality'),
                # time
                'first_seen': detection.get('first_seen'),
                'last_seen': detection.get('last_seen'),
                'detection_time': detection.get('detection_time'),
                'timestamp': detection.get('timestamp'),
                'server_timestamp': detection.get('server_timestamp'),
                'timestamp_source': detection.get('timestamp_source', 'unknown'),
            }
            writer.writerow(row)
    
    return send_file(filepath, as_attachment=True, download_name=filename)

@app.route('/api/export/kml', methods=['GET'])
def export_kml():
    """Export detections as KML"""
    export_type = request.args.get('type', 'session')
    
    if export_type == 'cumulative':
        data_to_export = cumulative_detections
        filename_prefix = "flockyou_cumulative"
        document_name = "Flock You Cumulative Detections"
    else:
        data_to_export = detections
        filename_prefix = f"flockyou_session_{session_start_time.strftime('%Y%m%d_%H%M%S')}"
        document_name = f"Flock You Session Detections - {session_start_time.strftime('%Y-%m-%d %H:%M:%S')}"
    
    if not data_to_export:
        return jsonify({'status': 'error', 'message': 'No detections to export'}), 400
    
    filename = f"{filename_prefix}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.kml"
    filepath = os.path.join('exports', filename)
    
    os.makedirs('exports', exist_ok=True)
    
    kml_content = f"""<?xml version="1.0" encoding="UTF-8"?>
<kml xmlns="http://www.opengis.net/kml/2.2">
<Document>
    <name>{document_name}</name>
    <description>Surveillance device detections with GPS coordinates ({len(data_to_export)} detections)</description>
"""
    
    for i, detection in enumerate(data_to_export):
        gps = detection.get('gps', {})
        if gps.get('latitude') and gps.get('longitude'):
            # Use alias if available, otherwise use detection number
            # Fall back to the MAC, not "Detection N" — the name is the only
            # identity Google Earth shows, and a re-import needs it to match
            # an existing device instead of inventing a new one.
            placemark_name = detection.get('alias') or detection.get('mac_address') or f"Detection {i+1}"
            
            # GPS accuracy indicator
            gps_accuracy = ""
            if gps.get('time_diff') is not None:
                time_diff = gps.get('time_diff')
                if time_diff < 5:
                    gps_accuracy = f" (✓ Precise: {time_diff:.1f}s)"
                elif time_diff < 15:
                    gps_accuracy = f" (~ Good: {time_diff:.1f}s)"
                else:
                    gps_accuracy = f" (⚠ Approximate: {time_diff:.1f}s)"
            else:
                gps_accuracy = " (? Unknown accuracy)"
            
            # Build device info
            device_info = ""
            if detection.get('ssid'):
                device_info += f"<b>SSID:</b> {detection.get('ssid')}<br/>"
            if detection.get('device_name'):
                device_info += f"<b>Device Name:</b> {detection.get('device_name')}<br/>"
            
            # RSSI info
            rssi_info = detection.get('last_rssi') or detection.get('rssi', 'N/A')
            
            # Channel info
            channel_info = detection.get('last_channel') or detection.get('channel', 'N/A')
            
            kml_content += f"""
    <Placemark>
        <name>{placemark_name}</name>
        <description>
            <![CDATA[
            <b>Protocol:</b> {detection.get('protocol')}<br/>
            <b>Detection Method:</b> {method_label(detection.get('detection_method'))}<br/>
            <b>Method Key:</b> {detection.get('detection_method') or 'unknown'}<br/>
            <b>Tier:</b> {detection.get('detection_tier', method_tier(detection))}<br/>
            <b>All Methods:</b> {', '.join(f"{method_label(m)} x{c}" for m, c in sorted((detection.get('methods_seen') or {}).items())) or 'n/a'}<br/>
            {device_info}
            <b>MAC Address:</b> {detection.get('mac_address')}<br/>
            <b>Manufacturer:</b> {detection.get('manufacturer', 'Unknown')}<br/>
            <b>Alias:</b> {detection.get('alias', 'None')}<br/>
            <b>RSSI:</b> {rssi_info} dBm<br/>
            <b>Signal Strength:</b> {detection.get('signal_strength', 'N/A')}<br/>
            <b>Channel:</b> {channel_info}<br/>
            <b>Detection Count:</b> {detection.get('detection_count', 1)}<br/>
            <b>Detection Time:</b> {detection.get('detection_time', 'N/A')}<br/>
            <b>Server Timestamp:</b> {detection.get('server_timestamp', 'N/A')}<br/>
            <hr/>
            <b>GPS Coordinates:</b> {gps.get('latitude'):.6f}, {gps.get('longitude'):.6f}{gps_accuracy}<br/>
            <b>GPS Altitude:</b> {gps.get('altitude', 'N/A')} m<br/>
            <b>GPS Satellites:</b> {gps.get('satellites', 'N/A')}<br/>
            <b>GPS Fix Quality:</b> {gps.get('fix_quality', 'N/A')}<br/>
            <b>GPS Match Quality:</b> {gps.get('match_quality', 'N/A')}<br/>
            <b>GPS Timestamp:</b> {gps.get('timestamp', 'N/A')}<br/>
            <b>Timestamp Source:</b> {detection.get('timestamp_source', 'Unknown').upper()}
            ]]>
        </description>
        <Point>
            <coordinates>{gps.get('longitude')},{gps.get('latitude')},{gps.get('altitude', 0)}</coordinates>
        </Point>
    </Placemark>
"""
    
    kml_content += """
</Document>
</kml>"""
    
    with open(filepath, 'w', encoding='utf-8') as f:
        f.write(kml_content)
    
    return send_file(filepath, as_attachment=True, download_name=filename)

def import_detection_item(item):
    """Map one ESP32-format detection record onto the Flask detection schema
    and add it. Accepts both the on-device session keys (mac, method, tier,
    count, ssid) and dashboard-export keys (mac_address, detection_method...).
    Shared by the JSON file import and the live dump_session stream."""
    # Map ESP32 export fields to Flask detection format
    method = normalize_method(item.get('method', item.get('detection_method')))
    data = {
        'detection_method': method,
        # This firmware is WiFi-only; the old hardcoded bluetooth_le
        # mislabelled every imported record.
        'protocol': item.get('protocol', 'wifi_2_4ghz'),
        'mac_address': item.get('mac', item.get('mac_address', '')),
        'device_name': item.get('name', item.get('device_name', '')),
        'rssi': item.get('rssi', 0),
        'detection_count': item.get('count', item.get('detection_count', 1)),
        # Trust an explicit tier from the device, else derive it.
        'detection_tier': item.get('tier', item.get('detection_tier',
                                                    METHOD_TIERS.get(method, 0))),
    }
    if item.get('channel') is not None:
        data['channel'] = item['channel']
    if item.get('ssid'):
        data['ssid'] = item['ssid']

    # GPS fields from ESP32 wardriving export
    gps_obj = item.get('gps')
    if gps_obj and (gps_obj.get('lat') or gps_obj.get('latitude')):
        data['gps'] = {
            'latitude': gps_obj.get('lat', gps_obj.get('latitude')),
            'longitude': gps_obj.get('lon', gps_obj.get('longitude')),
            'altitude': gps_obj.get('alt', gps_obj.get('altitude', 0)),
            'fix_quality': 1,
            'match_quality': 'esp32_phone_gps',
            'time_diff': 0,
        }
        if gps_obj.get('acc') is not None:
            data['gps']['accuracy'] = gps_obj['acc']

    add_detection_from_serial(data)


@app.route('/api/import/json', methods=['POST'])
def import_json():
    """Import detections from a JSON file (exported from ESP32 Flock-You dashboard)"""
    global detections, cumulative_detections, next_detection_id

    if 'file' not in request.files:
        return jsonify({'status': 'error', 'message': 'No file provided'}), 400

    file = request.files['file']
    if not file.filename:
        return jsonify({'status': 'error', 'message': 'No file selected'}), 400

    try:
        content = file.read().decode('utf-8')
        imported = json.loads(content)

        if not isinstance(imported, list):
            imported = [imported]

        count = 0
        for item in imported:
            import_detection_item(item)
            count += 1

        print(f"Imported {count} detections from JSON file: {file.filename}")
        return jsonify({'status': 'success', 'message': f'Imported {count} detections', 'count': count})

    except json.JSONDecodeError as e:
        return jsonify({'status': 'error', 'message': f'Invalid JSON: {str(e)}'}), 400
    except Exception as e:
        print(f"JSON import error: {e}")
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/import/csv', methods=['POST'])
def import_csv():
    """Import detections from a CSV file (exported from ESP32 Flock-You dashboard)"""
    global detections, cumulative_detections, next_detection_id

    if 'file' not in request.files:
        return jsonify({'status': 'error', 'message': 'No file provided'}), 400

    file = request.files['file']
    if not file.filename:
        return jsonify({'status': 'error', 'message': 'No file selected'}), 400

    try:
        content = file.read().decode('utf-8')
        reader = csv.DictReader(content.splitlines())

        count = 0
        for row in reader:
            method = normalize_method(row.get('method', row.get('detection_method')))
            data = {
                'detection_method': method,
                'protocol': row.get('protocol') or 'wifi_2_4ghz',
                'mac_address': row.get('mac', row.get('mac_address', '')),
                'device_name': row.get('name', row.get('device_name', '')),
                'rssi': int(row.get('rssi', 0)) if row.get('rssi') else 0,
                'detection_count': int(row.get('count', row.get('detection_count', 1))) if row.get('count', row.get('detection_count')) else 1,
            }
            tier_str = row.get('detection_tier', row.get('tier', ''))
            try:
                data['detection_tier'] = int(tier_str) if str(tier_str).strip() != '' \
                    else METHOD_TIERS.get(method, 0)
            except (ValueError, TypeError):
                data['detection_tier'] = METHOD_TIERS.get(method, 0)
            if row.get('channel'):
                data['channel'] = row['channel']
            if row.get('ssid'):
                data['ssid'] = row['ssid']

            # GPS fields from ESP32 wardriving CSV export
            lat_str = row.get('latitude', '')
            lon_str = row.get('longitude', '')
            if lat_str and lon_str:
                try:
                    data['gps'] = {
                        'latitude': float(lat_str),
                        'longitude': float(lon_str),
                        'fix_quality': 1,
                        'match_quality': 'esp32_phone_gps',
                        'time_diff': 0,
                    }
                    acc_str = row.get('gps_accuracy', '')
                    if acc_str:
                        data['gps']['accuracy'] = float(acc_str)
                except (ValueError, TypeError):
                    pass  # Skip GPS if values are invalid

            add_detection_from_serial(data)
            count += 1

        print(f"Imported {count} detections from CSV file: {file.filename}")
        return jsonify({'status': 'success', 'message': f'Imported {count} detections', 'count': count})

    except Exception as e:
        print(f"CSV import error: {e}")
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/import/kml', methods=['POST'])
def import_kml():
    """Import detections from a KML file (exported from ESP32 Flock-You dashboard)"""
    import xml.etree.ElementTree as ET

    if 'file' not in request.files:
        return jsonify({'status': 'error', 'message': 'No file provided'}), 400

    file = request.files['file']
    if not file.filename:
        return jsonify({'status': 'error', 'message': 'No file selected'}), 400

    try:
        content = file.read().decode('utf-8')
        root = ET.fromstring(content)

        # Handle KML namespace
        ns = {'kml': 'http://www.opengis.net/kml/2.2'}
        placemarks = root.findall('.//kml:Placemark', ns)
        if not placemarks:
            # Try without namespace (some KML generators omit it)
            placemarks = root.findall('.//Placemark')

        count = 0
        for pm in placemarks:
            # NB: `a or b` is wrong for ElementTree — an element with no
            # children is falsy, so leaf tags like <name> fell through to the
            # non-namespaced lookup, returned None, and every placemark
            # imported as "unknown_N". Compare against None explicitly.
            def pick(*paths):
                for path in paths:
                    el = pm.find(path, ns) if path.startswith('kml:') or '/kml:' in path \
                        else pm.find(path)
                    if el is not None:
                        return el
                return None

            name_el = pick('kml:name', 'name')
            desc_el = pick('kml:description', 'description')
            coord_el = pick('.//kml:coordinates', './/coordinates')

            # The placemark name may be a user alias, so it is not a reliable
            # MAC source. Read the MAC from the description first and only
            # fall back to <name> when it actually looks like a MAC.
            placemark_label = name_el.text.strip() if name_el is not None and name_el.text else ""

            # Parse coordinates (lon,lat,alt)
            gps_data = None
            if coord_el is not None and coord_el.text:
                parts = coord_el.text.strip().split(',')
                if len(parts) >= 2:
                    try:
                        lon = float(parts[0])
                        lat = float(parts[1])
                        alt = float(parts[2]) if len(parts) > 2 else 0
                        gps_data = {
                            'latitude': lat,
                            'longitude': lon,
                            'altitude': alt,
                            'fix_quality': 1,
                            'match_quality': 'esp32_kml_import',
                            'time_diff': 0,
                        }
                    except (ValueError, TypeError):
                        pass

            # Parse description for metadata
            desc_text = desc_el.text.strip() if desc_el is not None and desc_el.text else ""
            device_name = ""
            method = "kml_import"
            rssi = 0
            det_count = 1

            # Try to extract fields from CDATA description
            import re
            name_match = re.search(r'<b>Name:</b>\s*([^<]+)', desc_text)
            if name_match:
                device_name = name_match.group(1).strip()
            # Prefer the machine-readable key the exporter emits; fall back to
            # the human label (reverse-mapped) for older files.
            key_match = re.search(r'<b>Method Key:</b>\s*([^<\s]+)', desc_text)
            method_match = re.search(r'<b>Detection Method:</b>\s*([^<]+)', desc_text) \
                or re.search(r'<b>Method:</b>\s*([^<]+)', desc_text)
            if key_match:
                method = normalize_method(key_match.group(1))
            elif method_match:
                method = normalize_method(method_match.group(1))

            tier = METHOD_TIERS.get(method, 0)
            tier_match = re.search(r'<b>Tier:</b>\s*(\d+)', desc_text)
            if tier_match:
                tier = int(tier_match.group(1))

            rssi_match = re.search(r'<b>RSSI:</b>\s*(-?\d+)', desc_text)
            if rssi_match:
                rssi = int(rssi_match.group(1))
            # Exporter writes "Detection Count"; the old '<b>Count:</b>' pattern
            # never matched it, so every import came back with count 1.
            count_match = re.search(r'<b>Detection Count:</b>\s*(\d+)', desc_text) \
                or re.search(r'<b>Count:</b>\s*(\d+)', desc_text)
            if count_match:
                det_count = int(count_match.group(1))

            proto_match = re.search(r'<b>Protocol:</b>\s*([^<]+)', desc_text)

            MAC_RE = r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}'
            mac_match = re.search(r'<b>MAC Address:</b>\s*(' + MAC_RE + ')', desc_text)
            if mac_match:
                mac = mac_match.group(1).strip()
            elif re.fullmatch(MAC_RE, placemark_label):
                mac = placemark_label
            else:
                mac = f"unknown_{count}"

            alias_match = re.search(r'<b>Alias:</b>\s*([^<]+)', desc_text)
            alias = alias_match.group(1).strip() if alias_match else ''
            if alias.lower() in ('none', 'n/a'):
                alias = ''

            data = {
                'detection_method': method,
                'detection_tier': tier,
                'protocol': proto_match.group(1).strip() if proto_match else 'wifi_2_4ghz',
                'mac_address': mac,
                'device_name': device_name,
                'rssi': rssi,
                'detection_count': det_count,
            }
            if alias:
                data['alias'] = alias
            elif placemark_label and not re.fullmatch(MAC_RE, placemark_label) \
                    and not placemark_label.lower().startswith('detection '):
                data['alias'] = placemark_label

            if gps_data:
                data['gps'] = gps_data

            add_detection_from_serial(data)
            count += 1

        print(f"Imported {count} detections from KML file: {file.filename}")
        return jsonify({'status': 'success', 'message': f'Imported {count} detections from KML', 'count': count})

    except ET.ParseError as e:
        return jsonify({'status': 'error', 'message': f'Invalid KML: {str(e)}'}), 400
    except Exception as e:
        print(f"KML import error: {e}")
        return jsonify({'status': 'error', 'message': str(e)}), 500


@app.route('/api/clear', methods=['POST'])
def clear_detections():
    """Clear session detections"""
    global detections, next_detection_id, session_start_time
    detections.clear()
    next_detection_id = 1  # Reset ID counter
    session_start_time = datetime.now()  # Reset session start time
    save_session_detections()
    safe_socket_emit('detections_cleared', {})
    return jsonify({'status': 'success', 'message': 'Session detections cleared'})

@app.route('/api/test/detection', methods=['POST'])
def test_detection():
    """Test endpoint to add a sample detection"""
    if request.is_json:
        # Use provided detection data
        sample_detection = request.json
        # Ensure required fields are present
        if 'detection_method' not in sample_detection:
            sample_detection['detection_method'] = 'probe_request'
        if 'protocol' not in sample_detection:
            sample_detection['protocol'] = 'wifi'
        if 'mac_address' not in sample_detection:
            sample_detection['mac_address'] = 'AA:BB:CC:DD:EE:FF'
        if 'detection_time' not in sample_detection:
            sample_detection['detection_time'] = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        if 'timestamp' not in sample_detection:
            sample_detection['timestamp'] = datetime.now().isoformat()
    else:
        # Use default sample detection
        sample_detection = {
            'detection_method': 'probe_request',
            'protocol': 'wifi',
            'mac_address': 'AA:BB:CC:DD:EE:FF',
            'ssid': 'TestNetwork',
            'rssi': -45,
            'signal_strength': 'Excellent',
            'channel': 6,
            'detection_time': datetime.now().strftime('%Y-%m-%d %H:%M:%S'),
            'timestamp': datetime.now().isoformat()
        }
    
    add_detection_from_serial(sample_detection)
    return jsonify({'status': 'success', 'message': 'Test detection added'})

@app.route('/api/detection/alias', methods=['POST'])
def update_detection_alias():
    """Update detection alias"""
    global detections
    
    data = request.json
    detection_id = data.get('id')
    alias = data.get('alias', '').strip()
    
    if detection_id is None:
        return jsonify({'status': 'error', 'message': 'Detection ID required'}), 400
    
    # Find and update the detection
    for detection in detections:
        if detection.get('id') == detection_id:
            detection['alias'] = alias
            save_session_detections()
            # Emit update to all clients
            safe_socket_emit('detection_updated', detection)
            return jsonify({'status': 'success', 'message': 'Alias updated'})
    
    return jsonify({'status': 'error', 'message': 'Detection not found'}), 404

@app.route('/api/settings', methods=['GET'])
def get_settings():
    """Get current settings"""
    return jsonify(settings)

@app.route('/api/settings', methods=['POST'])
def update_settings():
    """Update settings"""
    global settings
    data = request.json
    settings.update(data)
    save_settings()
    return jsonify({'status': 'success', 'settings': settings})

@app.route('/api/stats', methods=['GET'])
def get_stats():
    """Get detection statistics"""
    return jsonify({
        # WiFi-only firmware — the BLE counters are gone. `protocol` is
        # 'wifi_2_4ghz', so the old exact match on 'wifi' counted zero;
        # startswith fixes that. 'ie_confirmed' replaces the BLE slot with
        # the number that matters: devices verified by the IE fingerprint.
        'session': {
            'total': len(detections),
            'wifi': len([d for d in detections if (d.get('protocol') or '').startswith('wifi')]),
            'ie_confirmed': len([d for d in detections if method_tier(d) == 4]),
            'gps': len([d for d in detections if d.get('gps')]),
            'start_time': session_start_time.isoformat()
        },
        'cumulative': {
            'total': len(cumulative_detections),
            'wifi': len([d for d in cumulative_detections if (d.get('protocol') or '').startswith('wifi')]),
            'ie_confirmed': len([d for d in cumulative_detections if method_tier(d) == 4]),
            'gps': len([d for d in cumulative_detections if d.get('gps')])
        }
    })

@app.route('/api/oui/search', methods=['POST'])
def search_oui():
    """Search OUI database"""
    global oui_database
    
    data = request.json
    query = data.get('query', '').strip()
    
    if not query:
        return jsonify({'status': 'error', 'message': 'Query required'}), 400
    
    results = []
    
    # Clean the query - strip every common MAC separator, convert to uppercase.
    # Dashes matter: IEEE's own oui.txt writes prefixes as "28-6F-B9", so a
    # copy-paste from that file used to return nothing. Dots cover Cisco's
    # "286f.b928" style.
    clean_query = (query.replace(':', '').replace(' ', '')
                        .replace('-', '').replace('.', '').upper())
    
    # Check if query looks like a MAC address (6 hex characters)
    if len(clean_query) >= 6 and all(c in '0123456789ABCDEF' for c in clean_query[:6]):
        # Search by MAC prefix
        mac_prefix = clean_query[:6]
        if mac_prefix in oui_database:
            results.append({
                'mac': mac_prefix,
                'manufacturer': oui_database[mac_prefix]
            })
    else:
        # Search by manufacturer name
        query_lower = query.lower()
        for mac, manufacturer in oui_database.items():
            if query_lower in manufacturer.lower():
                results.append({
                    'mac': mac,
                    'manufacturer': manufacturer
                })
                if len(results) >= 100:  # Increased limit
                    break
    
    print(f"Search query: '{query}' -> '{clean_query}', found {len(results)} results")
    
    return jsonify({
        'status': 'success',
        'results': results,
        'count': len(results)
    })

@app.route('/api/oui/all')
def get_all_oui():
    """Get all OUI entries"""
    global oui_database
    
    # Return all entries
    results = []
    for mac, manufacturer in oui_database.items():
        results.append({
            'mac': mac,
            'manufacturer': manufacturer
        })
    
    return jsonify({
        'status': 'success',
        'results': results,
        'count': len(results),
        'total': len(oui_database)
    })

@app.route('/api/oui/refresh', methods=['POST'])
def refresh_oui_database():
    global oui_database
    
    try:
        import urllib.request
        import urllib.error
        import tempfile
        import os
        
        url = "https://standards-oui.ieee.org/oui/oui.txt"
        print(f"Downloading OUI database from {url}...")
        
        req = urllib.request.Request(
            url,
            headers={
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36',
                'Accept': 'text/plain,text/html,application/xhtml+xml',
                'Accept-Language': 'en-US,en;q=0.9',
                'Connection': 'keep-alive'
            }
        )
        
        with tempfile.NamedTemporaryFile(delete=False, suffix='.txt') as temp_file:
            temp_path = temp_file.name
            
        with urllib.request.urlopen(req, timeout=30) as response:
            with open(temp_path, 'wb') as out_file:
                out_file.write(response.read())
                
        print(f"Downloaded file to {temp_path}, parsing...")
        new_oui_database = {}
        with open(temp_path, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith('#') and '(hex)' in line:
                    parts = line.split('(hex)')
                    if len(parts) == 2:
                        mac_prefix = parts[0].strip().replace('-', '').replace(' ', '').upper()
                        manufacturer = parts[1].strip()
                        if mac_prefix and manufacturer and len(mac_prefix) == 6:
                            new_oui_database[mac_prefix] = manufacturer
                            
        print(f"Parsed {len(new_oui_database)} entries from downloaded file")
        os.unlink(temp_path)
        
        if len(new_oui_database) < 1000:
            raise Exception(f"Downloaded database appears incomplete ({len(new_oui_database)} entries). File may be corrupted or format changed.")

        oui_database = new_oui_database
        
        with open('oui.txt', 'w', encoding='utf-8') as f:
            for mac, manufacturer in sorted(oui_database.items()):
                formatted_mac = f"{mac[0:2]}-{mac[2:4]}-{mac[4:6]}"
                f.write(f"{formatted_mac}   (hex)\t\t\t\t{manufacturer}\n")
                
        print(f"Successfully refreshed OUI database with {len(oui_database)} entries")
        
        return jsonify({
            'status': 'success',
            'message': 'Database refreshed successfully',
            'count': len(oui_database)
        })
    
    except urllib.error.HTTPError as e:
        if e.code == 418:
            return jsonify({
                'status': 'error',
                'message': 'IEEE server is refusing automated requests (HTTP 418). Please wait and try again later.'
            }), 418
        else:
            return jsonify({
                'status': 'error',
                'message': f'HTTP error {e.code}: {str(e)}'
            }), 500
        
    except Exception as e:
        print(f"Error refreshing OUI database: {str(e)}")
        import traceback
        traceback.print_exc()
        return jsonify({
            'status': 'error',
            'message': f'Failed to refresh database: {str(e)}'
        }), 500


# Socket.IO event handlers
@socketio.on('connect')
def handle_connect():
    print(f"Client connected: {request.sid}")

@socketio.on('disconnect')
def handle_disconnect():
    print(f"Client disconnected: {request.sid}")
    # Clean up any room memberships
    try:
        leave_room('serial_terminal')
    except:
        pass

@socketio.on('heartbeat')
def handle_heartbeat():
    """Handle client heartbeat to keep connection alive"""
    try:
        emit('heartbeat_ack')
    except Exception as e:
        print(f"Heartbeat response error: {e}")

def send_heartbeat():
    """Send periodic heartbeat to all clients"""
    with app.app_context():
        while True:
            try:
                safe_socket_emit('heartbeat', {})
                time.sleep(30)  # Send heartbeat every 30 seconds
            except Exception as e:
                print(f"Heartbeat error: {e}")
                time.sleep(5)

@socketio.on('request_serial_terminal')
def handle_serial_terminal_request(data):
    """Handle serial terminal connection request"""
    global serial_data_buffer
    port = data.get('port')
    
    print(f"Serial terminal request from {request.sid} for port: {port}")
    
    if not port:
        emit('serial_error', {'message': 'No port specified'})
        return
    
    if not flock_device_connected or flock_device_port != port:
        emit('serial_error', {'message': 'Device not connected. Please connect to the Sniffer device first.'})
        return
    
    try:
        # Add to serial terminal room
        join_room('serial_terminal')
        emit('serial_connected')
        
        # Send recent buffer data
        buffer_count = len(serial_data_buffer)
        print(f"Sending {min(50, buffer_count)} recent lines to terminal")
        for line in serial_data_buffer[-50:]:  # Send last 50 lines
            emit('serial_data', line)
        
        print(f"Serial terminal connected for client {request.sid}")
        
    except Exception as e:
        print(f"Serial terminal connection error: {e}")
        emit('serial_error', {'message': f'Failed to start terminal: {str(e)}'})

def initialize_app():
    """Load persisted state and start background threads.

    Called at import time, not only from __main__: the OUI database, the
    cumulative detection history and the saved settings were previously loaded
    inside the __main__ guard, so any WSGI host that imports this module
    (gunicorn, uwsgi, an embedded runner) came up with an empty OUI database —
    every detection resolved to "Unknown Manufacturer" and OUI search returned
    nothing. Guarded so repeated imports don't double-start the monitor.
    """
    global _initialized
    if globals().get('_initialized'):
        return
    globals()['_initialized'] = True

    load_oui_database()
    load_cumulative_detections()
    load_session_detections()
    load_settings()

    # Wire the BLE blueprint to the shared detection sink so BLE hits (live
    # emit + CMD dump replays) land in the same detections list as WiFi hits.
    flock_ble_init_bridge(ingest_fn=add_detection_from_serial,
                          emit_fn=safe_socket_emit)

    monitor_thread = threading.Thread(target=connection_monitor, daemon=True)
    monitor_thread.start()

    heartbeat_thread = threading.Thread(target=send_heartbeat, daemon=True)
    heartbeat_thread.start()


initialize_app()


if __name__ == '__main__':
    # macOS AirPlay Receiver squats on 5000; override with FLOCKYOU_PORT.
    port = int(os.environ.get('FLOCKYOU_PORT', '5000'))
    host = os.environ.get('FLOCKYOU_HOST', '0.0.0.0')

    print("Starting Flock You API server...")
    print(f"Server will be available at: http://localhost:{port}")
    print("Press Ctrl+C to stop the server")

    try:
        socketio.run(app, debug=False, host=host, port=port, allow_unsafe_werkzeug=True)
    except KeyboardInterrupt:
        print("\nShutting down server...")
        # Clean up connections
        if flock_serial_connection and flock_serial_connection.is_open:
            flock_serial_connection.close()
        if serial_connection and serial_connection.is_open:
            serial_connection.close()
        print("Server stopped.")
        