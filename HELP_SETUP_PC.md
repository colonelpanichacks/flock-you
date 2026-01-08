# Setup Guide: Laptop / PC (Linux)

This guide is for users running **Flock Drive** on a Linux laptop (e.g., Kali, Ubuntu, Debian) or a portable setup like a Steam Deck (running Linux desktop).

## Requirements
- **OS**: Linux (Kali Linux, Ubuntu 20.04+, Debian 11+).
  - *Windows Users*: Use WSL2 (advanced) or a Virtual Machine with USB Passthrough.
- **WiFi Adapter**: **CRITICAL**. You need an external USB WiFi adapter that supports **Monitor Mode** and **Packet Injection**.
  - **Recommended**: Alfa AWUS036NHA (Atheros AR9271), Panda PAU09.
  - *Internal laptop WiFi cards often do NOT support monitor mode well.*
- **GPS (Optional)**: USB GPS Dongle.

## 1. Installation
1. Clone the repository:
   ```bash
git clone https://github.com/dustinth2004/flock-you.git
   cd flock-you
   ```
2. Run the **Easy Installer**:
   ```bash
   sudo ./install.sh
   ```
3. **Select Option 2** ("Personal Computer") when prompted.
   - This installs dependencies (Python, Aircrack-ng, etc.) and creates the `flock-drive` command.
   - It does *not* set up an auto-start background service, giving you full control.

## 2. Preparing Monitor Mode
Before running the scanner, you must enable Monitor Mode on your WiFi adapter.

1. **Identify your adapter**:
   ```bash
   iwconfig
   ```
   *(Note the interface name, e.g., `wlan1`)*

2. **Kill conflicting processes** (Important!):
   ```bash
   sudo airmon-ng check kill
   ```
   *(This stops NetworkManager, so you might lose internet connection. This is normal during scanning).*

3. **Enable Monitor Mode**:
   ```bash
   sudo airmon-ng start wlan1
   ```
   *(Your interface will likely rename to `wlan1mon`)*

## 3. Running Flock Drive
Run the scanner using the command installed by the script:

```bash
sudo flock-drive --wifi-interface wlan1mon
```

### Common Arguments:
- `--gps-port /dev/ttyUSB0`: Specify your GPS dongle port.
- `--no-ble`: Disable Bluetooth scanning (if you don't have a supported BLE adapter).
- `--web-port 8080`: Change dashboard port (default is 5000).

## 4. The Dashboard
Open your browser and navigate to:
`http://localhost:5000`

- **Cyberpunk UI**: Visualizes threats and devices.
- **Audio**: Audio alerts will play through your laptop speakers (ensure system volume is up).

## 5. Cleaning Up
When finished:
1. Stop the scanner (`Ctrl+C`).
2. **Restore Networking**:
   ```bash
   sudo airmon-ng stop wlan1mon
   sudo service NetworkManager start
   ```
   *(This restores your normal WiFi internet connection).*
