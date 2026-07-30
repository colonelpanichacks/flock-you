#!/usr/bin/env bash
# Flash Atom VoiceS3R (or Atom Echo S3R) with Flock-You firmware
# ─────────────────────────────────────────────────────────────────
# Works fully automatically — no manual button-hold required.
# Uses a two-stage esptool trick:
#   1. esptool v5+ (brew) triggers USB reset + uploads stub → hard reset
#   2. esptool.py v4 (PlatformIO) catches the ROM bootloader window → flashes
#
# Usage: ./flash_voices3r.sh [--no-build] [--testing]
#   --no-build   Skip PlatformIO compile step (use existing .pio/build artifacts)
#   --testing    Compile with TESTING_MODE=1 (beep on ANY WiFi frame, not just Flock OUI)

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ── Parse flags ──────────────────────────────────────────────────────────────
SKIP_BUILD=0
TESTING_MODE_FLAG=""
for arg in "$@"; do
    case "$arg" in
        --no-build)  SKIP_BUILD=1 ;;
        --testing)   TESTING_MODE_FLAG="-e m5atom-voices3r-testing" ;;
    esac
done

ENV="m5atom-voices3r"
BUILD_DIR=".pio/build/$ENV"

PYESPTOOL="/opt/homebrew/Cellar/platformio/6.1.19_2/libexec/bin/python"
ESPTOOL_PY="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# ── Verify esptool paths ─────────────────────────────────────────────────────
if [[ ! -f "$PYESPTOOL" ]]; then
    # Try to find PlatformIO python
    PYESPTOOL=$(find /opt/homebrew/Cellar/platformio -name "python" -path "*/libexec/bin/python" 2>/dev/null | head -1)
    if [[ -z "$PYESPTOOL" ]]; then
        echo "❌ Could not find PlatformIO Python. Is PlatformIO installed via brew?"
        exit 1
    fi
fi

if [[ ! -f "$ESPTOOL_PY" ]]; then
    echo "❌ Could not find PlatformIO esptool.py at: $ESPTOOL_PY"
    echo "   Run 'pio run -e m5atom-voices3r' once to install the toolchain."
    exit 1
fi

if [[ ! -f "$BOOT0" ]]; then
    echo "❌ Could not find boot_app0.bin at: $BOOT0"
    echo "   Run 'pio run -e m5atom-voices3r' once to install the toolchain."
    exit 1
fi

# ── Step 1: find port ────────────────────────────────────────────────────────
echo "🔍 Looking for Atom VoiceS3R (USB-CDC / usbmodem)..."

PORTS=$(ls /dev/cu.usbmodem* 2>/dev/null || true)
if [[ -z "$PORTS" ]]; then
    echo ""
    echo "❌ No Atom VoiceS3R found!"
    echo "   → Unplug and re-plug the USB-C cable, then run again."
    echo "   → Expected port pattern: /dev/cu.usbmodem*"
    exit 1
fi

PORT_COUNT=$(echo "$PORTS" | wc -l | tr -d ' ')
if [[ "$PORT_COUNT" -gt 1 ]]; then
    echo "⚠️  Multiple USB-CDC devices found:"
    echo "$PORTS"
    echo ""
    echo "Select which port to flash:"
    select PORT in $PORTS; do
        [[ -n "$PORT" ]] && break
    done
else
    PORT="$PORTS"
fi

echo "✅ Found device at: $PORT"
echo ""

# ── Step 2: free the port ────────────────────────────────────────────────────
PORT_HOLDER=$(lsof -t "$PORT" 2>/dev/null || true)
if [[ -n "$PORT_HOLDER" ]]; then
    echo "🔓 Releasing port held by PID $PORT_HOLDER..."
    kill "$PORT_HOLDER" 2>/dev/null || true
    sleep 0.5
fi

# Kill any stray esptool processes
pkill -f "esptool" 2>/dev/null || true
pkill -f "tool-esptoolpy" 2>/dev/null || true
sleep 0.3

# ── Step 3: build ────────────────────────────────────────────────────────────
if [[ $SKIP_BUILD -eq 0 ]]; then
    echo "🔨 Compiling firmware (env: $ENV)..."
    pio run --environment "$ENV" $TESTING_MODE_FLAG
    echo ""
else
    echo "⏩ Skipping build (--no-build)"
fi

# Verify build artifacts
for f in "$BUILD_DIR/bootloader.bin" "$BUILD_DIR/partitions.bin" "$BUILD_DIR/firmware.bin"; do
    if [[ ! -f "$f" ]]; then
        echo "❌ Missing build artifact: $f"
        echo "   Run without --no-build first."
        exit 1
    fi
done

# ── Step 4: flash with automatic USB reset ───────────────────────────────────
echo "⬆️  Flashing to $PORT..."
echo "   (Using automatic USB reset — no button hold required)"
echo ""

flash_once() {
    local port="$1"

    # Run stages serially — no background processes — to avoid port contention.
    #
    # Stage A: esptool v5+ (brew) connects, uploads stub, hard resets → exits
    #          (releases the port cleanly before Stage B opens it)
    echo "   Stage A: triggering USB reset..."
    esptool --chip esp32s3 \
        --port "$port" \
        --before usb-reset \
        --after hard-reset \
        --connect-attempts 5 \
        read-mac 2>/dev/null || true
    # v5.3 has now exited and released the port. Device is rebooting.

    # Wait for port to reappear (same or new modem number)
    local FLASH_PORT=""
    local i
    for i in $(seq 1 40); do
        local P
        P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
        if [[ -n "$P" ]]; then
            FLASH_PORT="$P"
            break
        fi
        sleep 0.05
    done
    FLASH_PORT="${FLASH_PORT:-$port}"

    # Stage B: catch the ROM bootloader window → flash
    echo "   Stage B: flashing via $FLASH_PORT..."
    "$PYESPTOOL" "$ESPTOOL_PY" \
        --chip esp32s3 \
        --port "$FLASH_PORT" \
        --baud 115200 \
        --before no_reset \
        --after hard_reset \
        --connect-attempts 5 \
        write_flash \
        0x0     "$BUILD_DIR/bootloader.bin" \
        0x8000  "$BUILD_DIR/partitions.bin" \
        0xe000  "$BOOT0" \
        0x10000 "$BUILD_DIR/firmware.bin"

    return $?
}

SUCCESS=0
for attempt in 1 2 3; do
    echo "── Attempt $attempt/3 ──────────────────────────────────"
    PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [[ -z "$PORT" ]]; then
        echo "❌ Device disconnected. Please re-plug and press Enter."
        read -r
        PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    fi

    if flash_once "$PORT"; then
        SUCCESS=1
        break
    fi

    echo "⚠️  Attempt $attempt failed. Waiting 3s before retry..."
    pkill -f "esptool" 2>/dev/null || true
    sleep 3
done

if [[ $SUCCESS -eq 0 ]]; then
    echo ""
    echo "❌ Flash failed after 3 attempts."
    echo ""
    echo "Manual recovery options:"
    echo "  1. Unplug the device, re-plug, and run this script again."
    echo "  2. If still failing, unplug, hold the front button (G41), re-plug,"
    echo "     release button after green LED lights, then run:"
    echo "     ./flash_voices3r.sh --no-build"
    exit 1
fi

echo ""
echo "✅ Flash complete!"

# ── Step 5: auto-restart ─────────────────────────────────────────────────────
echo "🔄 Restarting device..."

# The flash already issued --after hard_reset. Wait for port to reappear.
RESTART_PORT=""
for i in $(seq 1 50); do
    P=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    if [[ -n "$P" ]]; then
        RESTART_PORT="$P"
        break
    fi
    sleep 0.1
done

# If port didn't come back on its own, explicitly trigger a restart
if [[ -z "$RESTART_PORT" ]]; then
    echo "   Port not back yet — triggering explicit restart..."
    esptool --chip esp32s3 \
        --port "$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || echo '/dev/cu.usbmodem11301')" \
        --before usb-reset \
        --after hard-reset \
        --connect-attempts 3 \
        read-mac 2>/dev/null || true
    sleep 2
    RESTART_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
fi

if [[ -n "$RESTART_PORT" ]]; then
    echo "✅ Device running at $RESTART_PORT"
else
    echo "⚠️  Port not detected — device may still be booting. Unplug and re-plug if needed."
fi

echo ""
echo "   Firmware: Flock-You ($ENV)"
echo "   The speaker will chirp when a Flock device is detected via WiFi."
echo ""

# ── Step 6: optional monitor ─────────────────────────────────────────────────
PORT="${RESTART_PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
if [[ -n "$PORT" ]]; then
    read -p "📡 Start serial monitor? (y/n) " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo "Starting monitor on $PORT (Ctrl+C to exit)..."
        echo "────────────────────────────────────────────────"
        pio device monitor --port "$PORT" --baud 115200 --environment "$ENV"
    fi
fi

echo "✅ Done!"
