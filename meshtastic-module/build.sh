#!/bin/bash
# ============================================================================
# FLOCK-YOU MESHTASTIC BUILD SCRIPT
# ============================================================================
# Builds Meshtastic firmware with FlockDetectorModule for T-Beam Supreme
#
# Usage: ./build.sh [clean]
#   clean - removes existing firmware directory and starts fresh
# ============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="${SCRIPT_DIR}/firmware"
FIRMWARE_REPO="https://github.com/meshtastic/firmware.git"
FIRMWARE_BRANCH="master"
TARGET_ENV="tbeam-s3-core"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}"
echo "============================================"
echo " FLOCK-YOU MESHTASTIC BUILDER"
echo "============================================"
echo -e "${NC}"

# Clean if requested
if [ "$1" == "clean" ]; then
    echo -e "${YELLOW}Cleaning existing firmware directory...${NC}"
    rm -rf "${FIRMWARE_DIR}"
fi

# Clone or update firmware
if [ ! -d "${FIRMWARE_DIR}" ]; then
    echo -e "${GREEN}Cloning Meshtastic firmware...${NC}"
    git clone --depth 1 --branch "${FIRMWARE_BRANCH}" "${FIRMWARE_REPO}" "${FIRMWARE_DIR}"
    cd "${FIRMWARE_DIR}"
    git submodule update --init --recursive
else
    echo -e "${GREEN}Updating existing firmware...${NC}"
    cd "${FIRMWARE_DIR}"
    git pull origin "${FIRMWARE_BRANCH}" || true
    git submodule update --init --recursive
fi

# Copy module files
echo -e "${GREEN}Installing FlockDetectorModule...${NC}"
cp "${SCRIPT_DIR}/FlockDetectorModule.h" "${FIRMWARE_DIR}/src/modules/esp32/"
cp "${SCRIPT_DIR}/FlockDetectorModule.cpp" "${FIRMWARE_DIR}/src/modules/esp32/"

# Patch Modules.cpp to include our module
MODULES_CPP="${FIRMWARE_DIR}/src/modules/Modules.cpp"

# Check if already patched
if grep -q "FlockDetectorModule" "${MODULES_CPP}"; then
    echo -e "${YELLOW}Modules.cpp already patched, skipping...${NC}"
else
    echo -e "${GREEN}Patching Modules.cpp...${NC}"

    # Add include after other esp32 includes
    sed -i '/#include "modules\/esp32\/PaxcounterModule.h"/a\
#if defined(ARCH_ESP32) \&\& !MESHTASTIC_EXCLUDE_FLOCKDETECTOR\
#include "modules/esp32/FlockDetectorModule.h"\
#endif' "${MODULES_CPP}"

    # Add module instantiation after PaxcounterModule
    sed -i '/new PaxcounterModule();/a\
#endif\
#if defined(ARCH_ESP32) \&\& !MESHTASTIC_EXCLUDE_FLOCKDETECTOR\
    new FlockDetectorModule();' "${MODULES_CPP}"

    echo -e "${GREEN}Modules.cpp patched successfully${NC}"
fi

# Optional: Disable PaxcounterModule to avoid conflicts
# Uncomment if you want to disable it:
# echo -e "${YELLOW}Disabling PaxcounterModule...${NC}"
# sed -i 's/new PaxcounterModule();/\/\/ new PaxcounterModule(); \/\/ Disabled for FlockDetector/' "${MODULES_CPP}"

# Build
echo -e "${GREEN}Building for ${TARGET_ENV}...${NC}"
echo ""
cd "${FIRMWARE_DIR}"

# Check if platformio is available
if ! command -v pio &> /dev/null; then
    echo -e "${RED}PlatformIO not found!${NC}"
    echo "Install with: pip install platformio"
    echo "Or: curl -fsSL https://platformio.org/install.sh | bash"
    exit 1
fi

pio run -e "${TARGET_ENV}"

# Find the built firmware
FIRMWARE_BIN="${FIRMWARE_DIR}/.pio/build/${TARGET_ENV}/firmware.bin"
BOOTLOADER_BIN="${FIRMWARE_DIR}/.pio/build/${TARGET_ENV}/bootloader.bin"
PARTITIONS_BIN="${FIRMWARE_DIR}/.pio/build/${TARGET_ENV}/partitions.bin"

if [ -f "${FIRMWARE_BIN}" ]; then
    echo ""
    echo -e "${GREEN}============================================${NC}"
    echo -e "${GREEN} BUILD SUCCESSFUL!${NC}"
    echo -e "${GREEN}============================================${NC}"
    echo ""
    echo "Firmware files:"
    echo "  ${FIRMWARE_BIN}"
    echo "  ${BOOTLOADER_BIN}"
    echo "  ${PARTITIONS_BIN}"
    echo ""
    echo -e "${YELLOW}To flash via USB:${NC}"
    echo "  cd ${FIRMWARE_DIR}"
    echo "  pio run -e ${TARGET_ENV} -t upload"
    echo ""
    echo -e "${YELLOW}Or flash manually with esptool:${NC}"
    echo "  esptool.py --chip esp32s3 --port /dev/ttyACM0 \\"
    echo "    write_flash 0x0 ${BOOTLOADER_BIN} \\"
    echo "    0x8000 ${PARTITIONS_BIN} \\"
    echo "    0x10000 ${FIRMWARE_BIN}"
    echo ""

    # Copy firmware to output directory
    OUTPUT_DIR="${SCRIPT_DIR}/output"
    mkdir -p "${OUTPUT_DIR}"
    cp "${FIRMWARE_BIN}" "${OUTPUT_DIR}/flockyou-meshtastic-tbeam-supreme.bin"
    cp "${BOOTLOADER_BIN}" "${OUTPUT_DIR}/bootloader.bin" 2>/dev/null || true
    cp "${PARTITIONS_BIN}" "${OUTPUT_DIR}/partitions.bin" 2>/dev/null || true

    echo -e "${GREEN}Firmware copied to: ${OUTPUT_DIR}/${NC}"
else
    echo -e "${RED}Build failed! Firmware not found.${NC}"
    exit 1
fi
