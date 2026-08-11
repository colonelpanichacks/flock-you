# Flock You Web Dashboard

A Flask-based web dashboard for real-time monitoring and analysis of Flock Safety device detections with GPS integration.

## Features

### Real-Time Detection Monitoring
- **Live Updates**: Real-time detection display via WebSocket
- **Detection Filtering**: Filter by detection method (WiFi, BLE, MAC, Device Name)
- **Statistics Dashboard**: Overview of detection counts and types
- **Detailed View**: Complete device information for each detection

### GPS Integration
- **GPS Dongle Support**: Connect USB GPS dongles for location tracking
- **NMEA Parsing**: Automatic parsing of GPS coordinates
- **Location Tagging**: Each detection can include GPS coordinates
- **Satellite Information**: Display GPS fix quality and satellite count

### Data Export
- **CSV Export**: Download detection data in CSV format
- **KML Export**: Generate Google Earth compatible KML files
- **GPS Coordinates**: Include latitude, longitude, and altitude
- **Timestamped Files**: Automatic filename generation with timestamps

## Installation

### Prerequisites
- Python 3.8 or higher
- USB GPS dongle (optional, for location tracking)

### Setup
1. **Install dependencies**:
   ```bash
   pip install -r requirements.txt
   ```

2. **Run the application**:
   ```bash
   python app.py
   ```

3. **Access the dashboard**:
   Open your browser and navigate to `http://localhost:5000`

## Usage

### Basic Operation
1. **Start the web server** using the command above
2. **Connect your Flock You device** and ensure it's sending JSON data
3. **View detections** in real-time on the dashboard
4. **Filter detections** using the dropdown menu
5. **Export data** using the export buttons

### GPS Setup
The GPS header row has a **source** dropdown with two options:

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

### Data Export
- **CSV Export**: Downloads a CSV file with all detection data
- **KML Export**: Downloads a KML file for viewing in Google Earth
- **GPS Data**: Both formats include GPS coordinates when available

## API Endpoints

### Detection Management
- `GET /api/detections` - Get all detections (with optional filtering)
- `POST /api/detections` - Add new detection from Flock You device
- `POST /api/clear` - Clear all detections

### GPS Management
- `GET /api/gps/ports` - Get available serial ports
- `POST /api/gps/connect` - Connect a GPS source. Body:
  - `{"source": "serial", "port": "/dev/tty.usbserial-XXX"}` — opens an NMEA reader at 9600 baud
  - `{"source": "gpsd", "host": "localhost", "port": 2947}` — connects to a running gpsd daemon
  - Legacy body `{"port": "..."}` still works and is treated as serial
- `POST /api/gps/disconnect` - Disconnect whichever GPS source is active

### Data Export
- `GET /api/export/csv` - Export detections as CSV
- `GET /api/export/kml` - Export detections as KML

## Integration with Flock You Device

The web dashboard is designed to receive JSON detection data from the Flock You ESP32 device. The device should send POST requests to `/api/detections` with JSON data in the following format:

```json
{
  "timestamp": 12345,
  "detection_time": "12.345s",
  "protocol": "wifi",
  "detection_method": "probe_request",
  "ssid": "Flock_Camera_001",
  "mac_address": "aa:bb:cc:dd:ee:ff",
  "rssi": -65,
  "signal_strength": "MEDIUM",
  "channel": 6
}
```

## GPS Dongle Compatibility

The dashboard supports standard NMEA GPS dongles that output GPGGA sentences. Compatible devices include:
- USB GPS receivers
- Bluetooth GPS modules (when connected via USB adapter)
- Serial GPS modules

## File Structure
```
webapp/
├── app.py              # Main Flask application
├── requirements.txt    # Python dependencies
├── templates/
│   └── index.html     # Web dashboard template
├── exports/           # Generated export files
└── README.md         # This file
```

## Troubleshooting

### GPS Connection Issues
- Ensure GPS dongle is properly connected
- Check that the correct serial port is selected
- Verify GPS dongle is powered and has satellite fix
- Check system permissions for serial port access

### No Detections Displayed
- Verify Flock You device is running and connected
- Check network connectivity between device and server
- Ensure device is sending data to correct endpoint
- Check browser console for JavaScript errors

### Export Issues
- Ensure `exports/` directory exists and is writable
- Check available disk space
- Verify file permissions

## Security Notes

- The dashboard runs on `0.0.0.0:5000` by default (accessible from any network)
- Consider using a reverse proxy (nginx) for production deployment
- Implement authentication if needed for multi-user environments
- The Flask secret key should be changed in production

## Development

### Adding New Features
- Modify `app.py` for backend functionality
- Update `templates/index.html` for frontend changes
- Add new API endpoints as needed
- Update requirements.txt for new dependencies

### Testing
- Test GPS functionality with actual GPS dongle
- Verify export functionality with sample data
- Test real-time updates with multiple browser windows
- Validate JSON data format compatibility
