#!/bin/bash

# Colors
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}
  ______ _            _      _____       _
 |  ____| |          | |    |  __ \     (_)
 | |__  | | ___   ___| | __ | |  | |_ __ ___   _____
 |  __| | |/ _ \ / __| |/ / | |  | | '__| \ \ / / _ \
 | |    | | (_) | (__|   <  | |__| | |  | |\ V /  __/
 |_|    |_|\___/ \___|_|\_\ |_____/|_|  |_| \_/ \___|

      Surveillance Device Scanner - Installer
${NC}"

# Check for root
if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}[!] Please run as root (sudo ./install.sh)${NC}"
  exit
fi

# Detect Package Manager
PKG_MANAGER=""
if command -v apk &> /dev/null; then
    PKG_MANAGER="apk"
    echo -e "${GREEN}[+] Detected Alpine Linux / PostmarketOS (apk)${NC}"
elif command -v apt-get &> /dev/null; then
    PKG_MANAGER="apt"
    echo -e "${GREEN}[+] Detected Debian / Ubuntu (apt)${NC}"
else
    echo -e "${RED}[!] Unsupported package manager. This script supports apt (Debian) and apk (Alpine).${NC}"
    exit 1
fi

# Interactive Menu
echo "Select Installation Type:"
echo "1) Dedicated Device (Raspberry Pi / Headless)"
echo "   - Installs as System Service (Auto-start on boot)"
echo "   - Optimized for always-on operation"
echo "2) Personal Computer (Laptop / Desktop)"
echo "   - Installs dependencies and tools"
echo "   - Creates 'flock-drive' launch command"
echo "   - No auto-start (Manual run)"
read -p "Enter choice [1/2]: " INSTALL_TYPE

if [[ "$INSTALL_TYPE" != "1" && "$INSTALL_TYPE" != "2" ]]; then
    echo -e "${RED}Invalid selection. Exiting.${NC}"
    exit 1
fi

echo -e "${GREEN}[+] Installing System Dependencies...${NC}"

if [ "$PKG_MANAGER" == "apk" ]; then
    # Alpine / PostmarketOS
    apk update

    # Core tools
    apk add python3 py3-pip git build-base python3-dev

    # Libraries for Python builds
    apk add openblas-dev linux-headers glib-dev sdl2-dev pkgconf
    apk add freetype-dev libpng-dev libjpeg-turbo-dev portmidi-dev sdl2_image-dev sdl2_mixer-dev sdl2_ttf-dev

    # WiFi and GPS tools
    # 'wireless-tools' and 'gpsd' are standard. 'aircrack-ng' is in community.
    apk add wireless-tools gpsd gpsd-clients aircrack-ng || echo -e "${YELLOW}[!] Warning: Failed to install some wireless tools. Ensure community repo is enabled.${NC}"

elif [ "$PKG_MANAGER" == "apt" ]; then
    # Debian / Ubuntu
    apt-get update
    # Core dependencies
    apt-get install -y python3-pip python3-venv libglib2.0-dev git libsdl2-dev
    # GPS and WiFi tools
    apt-get install -y gpsd gpsd-clients aircrack-ng wireless-tools
fi

# Install Directory
INSTALL_DIR="/opt/flock_drive"
echo -e "${GREEN}[+] Installing to $INSTALL_DIR...${NC}"

mkdir -p $INSTALL_DIR
# Copy application files
cp -r flock_drive $INSTALL_DIR/

# Check for datasets
if [ -d "datasets" ]; then
    echo -e "${GREEN}[+] Installing datasets...${NC}"
    cp -r datasets $INSTALL_DIR/
else
    echo -e "${YELLOW}[!] Warning: 'datasets' directory not found. Scanner will rely on built-in signatures only.${NC}"
fi

cp setup.py $INSTALL_DIR/ 2>/dev/null || true

# Setup Virtual Environment
echo -e "${GREEN}[+] Setting up Python Virtual Environment...${NC}"
cd $INSTALL_DIR

# Remove old venv if exists to ensure clean state
if [ -d "venv" ]; then
    echo -e "${YELLOW}[!] Found existing venv, verifying...${NC}"
else
    python3 -m venv venv
fi

source venv/bin/activate

# Install Python Requirements
echo -e "${GREEN}[+] Installing Python Dependencies...${NC}"
pip install --upgrade pip

# On Alpine, numpy compilation can be slow.
if [ "$PKG_MANAGER" == "apk" ]; then
    echo -e "${YELLOW}[!] Note: Compiling numpy on ARM devices may take 10-20 minutes. Please be patient.${NC}"
fi

pip install -r flock_drive/requirements.txt

# --- Branching Logic ---

if [ "$INSTALL_TYPE" == "1" ]; then
    # === Dedicated Device Setup ===

    if [ "$PKG_MANAGER" == "apt" ]; then
        # Systemd (Debian/Ubuntu/Raspbian)
        echo -e "${GREEN}[+] Configuring Systemd Service (Dedicated Mode)...${NC}"
        cat > /etc/systemd/system/flockdrive.service <<EOL
[Unit]
Description=Flock Drive Surveillance Scanner
After=network.target bluetooth.target sound.target

[Service]
Type=simple
User=root
WorkingDirectory=$INSTALL_DIR
ExecStart=$INSTALL_DIR/venv/bin/python -m flock_drive.main
Restart=always
RestartSec=5
Environment=SDL_AUDIODRIVER=alsa

[Install]
WantedBy=multi-user.target
EOL
        systemctl daemon-reload
        systemctl enable flockdrive
        echo -e "${GREEN}[+] Service enabled. Starting now...${NC}"
        systemctl start flockdrive

        echo -e "${BLUE}=== Installation Complete (Dedicated Mode) ===${NC}"
        echo "Service Status: sudo systemctl status flockdrive"
        echo "View Logs:      sudo journalctl -u flockdrive -f"

    elif [ "$PKG_MANAGER" == "apk" ]; then
        # OpenRC (Alpine/PostmarketOS)
        echo -e "${GREEN}[+] Configuring OpenRC Service (Dedicated Mode)...${NC}"

        cat > /etc/init.d/flockdrive <<EOL
#!/sbin/openrc-run

name="flockdrive"
description="Flock Drive Surveillance Scanner"
directory="$INSTALL_DIR"
command="$INSTALL_DIR/venv/bin/python"
command_args="-m flock_drive.main"
command_background=true
pidfile="/run/flockdrive.pid"
user="root"

depend() {
    need net
    after firewall
}
EOL
        chmod +x /etc/init.d/flockdrive
        rc-update add flockdrive default
        echo -e "${GREEN}[+] Service added to default runlevel. Starting now...${NC}"
        rc-service flockdrive start

        echo -e "${BLUE}=== Installation Complete (Dedicated Mode) ===${NC}"
        echo "Service Status: sudo rc-service flockdrive status"
        echo "View Logs:      Check /var/log/messages or enable internal logging"
    fi

    echo "Dashboard:      http://<DEVICE_IP>:5000"

else
    # === PC Setup ===
    echo -e "${GREEN}[+] Configuring User Command (PC Mode)...${NC}"

    # Create a launcher script in /usr/local/bin
    cat > /usr/local/bin/flock-drive <<EOL
#!/bin/bash
if [ "\$EUID" -ne 0 ]; then
  echo "Error: flock-drive requires root privileges for hardware access."
  echo "Try: sudo flock-drive"
  exit 1
fi

cd $INSTALL_DIR
source venv/bin/activate
echo "Starting Flock Drive..."
python -m flock_drive.main "\$@"
EOL

    chmod +x /usr/local/bin/flock-drive

    echo -e "${BLUE}=== Installation Complete (PC Mode) ===${NC}"
    echo "Start Scanner:  sudo flock-drive"
    echo "Dashboard:      http://localhost:5000"
    echo "Options:        sudo flock-drive --help"
fi

echo -e "${YELLOW}
NOTE: For WiFi scanning, ensure your adapter is in Monitor Mode.
Example:
  sudo airmon-ng start wlan1
  sudo flock-drive --wifi-interface wlan1mon
${NC}"
