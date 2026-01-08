# Setup Guide: Raspberry Pi (Dedicated)

This guide walks you through setting up **Flock Drive** on a Raspberry Pi as a dedicated, always-on surveillance scanner.

## Recommended Hardware
- **Raspberry Pi**: Pi 3B+, 4, or 5 recommended for best performance. (Pi Zero 2 W works but may be slower).
- **Power Supply**: Official Raspberry Pi USB-C power supply or a robust portable battery bank (3A+).
- **Storage**: 16GB+ MicroSD Card (Class 10).
- **GPS**: USB GPS Dongle (e.g., VK-172, GlobalSat BU-353-S4).
- **WiFi Adapter (Optional but Recommended)**: External USB WiFi adapter supporting **Monitor Mode** (e.g., Panda PAU09, Alfa AWUS036NHA).
  - *Note: The Pi's internal WiFi can do monitor mode on some OS versions (nexmon), but an external card is reliable and offers better range.*

## 1. Operating System Setup
1. Download **Raspberry Pi Imager** from [raspberrypi.com](https://www.raspberrypi.com/software/).
2. Insert your SD card into your computer.
3. Open Imager and select:
   - **OS**: Raspberry Pi OS (Legacy, 64-bit) Lite *or* Standard. (Lite is preferred for headless).
   - **Storage**: Your SD Card.
4. Click the **Settings (Gear Icon)**:
   - Set Hostname: `flockdrive`
   - Enable SSH: Use password authentication.
   - Set Username/Password: `pi` / `yourpassword`
   - Configure WiFi: Enter your hotspot or home WiFi credentials.
5. Click **Write**.

## 2. Installation
1. Insert SD card into Pi and power it on.
2. SSH into the Pi (or use a keyboard/monitor):
   ```bash
   ssh pi@flockdrive.local
   ```
3. Update the system:
   ```bash
   sudo apt update && sudo apt upgrade -y
   ```
4. Clone the repository:
   ```bash
   git clone https://github.com/dustinth2004/flock-you.git
   cd flock-you
   ```
5. Run the **Easy Installer**:
   ```bash
   sudo ./install.sh
   ```
6. **Select Option 1** ("Dedicated Device") when prompted.
   - This installs dependencies and sets up the `flockdrive` systemd service.

## 3. Configuration (Monitor Mode)
If using an external WiFi adapter for scanning, you need to put it in Monitor Mode.

1. Find your interface name:
   ```bash
   iwconfig
   ```
   *(Look for `wlan1` or similar)*

2. Test Monitor Mode manually:
   ```bash
   sudo airmon-ng start wlan1
   ```
   *(This usually creates `wlan1mon`)*

3. Edit the service file to use this interface:
   ```bash
   sudo nano /etc/systemd/system/flockdrive.service
   ```
   Update the `ExecStart` line to include your arguments:
   ```ini
   ExecStart=/opt/flock_drive/venv/bin/python -m flock_drive.main --wifi-interface wlan1mon --gps-port /dev/ttyUSB0
   ```
4. Reload and restart:
   ```bash
   sudo systemctl daemon-reload
   sudo systemctl restart flockdrive
   ```

## 4. Usage
Once installed, the service runs automatically.

- **Dashboard**: Open `http://flockdrive.local:5000` (or the Pi's IP address) on your phone/laptop connected to the same network.
- **Audio**: Connect speakers to the 3.5mm jack (Pi 3/4) for audio alerts.
- **Logs**: Check the status if things aren't working:
  ```bash
  sudo systemctl status flockdrive
  sudo journalctl -u flockdrive -f
  ```

## 5. Headless Field Operation
For mobile use without a home WiFi:
1. Configure the Pi to create its own **WiFi Hotspot** (Access Point).
   - Use tools like `raspap` or `nmcli` to set up a hotspot.
2. Connect your phone to the Pi's WiFi.
3. Access the dashboard at the Gateway IP (usually `10.3.141.1` or similar).
