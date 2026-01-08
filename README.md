# Flock You: Surveillance Device Scanner

<div align="center">
  <img src="flock.png" alt="Flock You Logo" width="300px">
  <h3>Turn the tables on surveillance.</h3>
</div>

**Flock You** (aka **Flock Drive**) is an advanced scanner that detects **Flock Safety** cameras, **Raven** gunshot detectors, and other surveillance devices using WiFi and Bluetooth signatures.

## Quick Start: Choose Your Weapon

| **Platform** | **Best For** | **Difficulty** | **Guide** |
| :--- | :--- | :--- | :--- |
| **Raspberry Pi** | **Dedicated Device**. Always-on scanning in your car (Headless). | Medium | [**Setup Guide (Pi)**](HELP_SETUP_PI.md) |
| **Laptop / PC** | **Occasional Scanning**. Use your Linux laptop for wardriving. | Easy | [**Setup Guide (PC)**](HELP_SETUP_PC.md) |
| **ESP32 (Hardware)** | **Pocket Carry**. Ultra-portable standalone device (Oui-Spy). | Hard (Soldering) | [**Setup Guide (ESP32)**](HELP_SETUP_ESP32.md) |

---

## Features

- **Multi-Method Detection**:
  - **WiFi**: Sniffs Probe Requests & Beacons (Monitor Mode).
  - **BLE**: Scans for specific Service UUIDs (Raven/ShotSpotter) and device names.
- **Audio Feedback**: Tactical audio alerts (GPIO Buzzer or System Audio).
- **Cyberpunk Dashboard**: Real-time web interface for visualizing threats.
- **GPS Logging**: Maps detections to KML/CSV files.
- **Stealth**: Passive scanning options.

## Installation (Software)

For Raspberry Pi or Linux PC users, we have a unified **Easy Installer**:

1. **Clone the repo**:
   ```bash
   git clone https://github.com/dustinth2004/flock-you.git
   cd flock-you
   ```

2. **Run the installer**:
   ```bash
   sudo ./install.sh
   ```

3. **Follow the Wizard**:
   - Select **Dedicated Device** for Raspberry Pi (Service Mode).
   - Select **Personal Computer** for Laptop (Manual Mode).

## Documentation

- [**Pi Setup Guide**](HELP_SETUP_PI.md) - Headless config, systemd service.
- [**PC Setup Guide**](HELP_SETUP_PC.md) - Monitor mode, manual running.
- [**ESP32 Setup Guide**](HELP_SETUP_ESP32.md) - Firmware flashing, wiring.
- [**API / Dashboard**](api/README.md) - Companion dashboard details.

## Legal Disclaimer

**For Educational and Research Purposes Only.**
This tool is designed to help researchers and privacy advocates understand the prevalence of surveillance technology.
- **Do not** use this tool to interfere with lawful operations.
- **Respect** local laws regarding wireless scanning and monitor mode.
- **Authors are not responsible** for misuse.

**Oui-Spy devices available at [colonelpanic.tech](https://colonelpanic.tech)**
