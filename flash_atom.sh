#!/usr/bin/env bash
# Auto-detect and flash M5Atom Lite with Flock-You firmware

set -e  # Exit on error

# ── Virtual Environment Auto-Load ────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

activate_venv() {
    local venv_path="$1"
    if [ -f "$venv_path/bin/activate" ]; then
        echo "🐍 Activating venv: $venv_path"
        # shellcheck disable=SC1091
        source "$venv_path/bin/activate"
        return 0
    fi
    return 1
}

if [ -z "$VIRTUAL_ENV" ]; then
    # Search order: project-local venvs → PlatformIO default venv
    VENV_FOUND=false
    for candidate in \
        "$SCRIPT_DIR/.venv" \
        "$SCRIPT_DIR/venv" \
        "$SCRIPT_DIR/env" \
        "$SCRIPT_DIR/api/.venv" \
        "$SCRIPT_DIR/api/venv" \
        "$HOME/.platformio/penv"
    do
        if activate_venv "$candidate"; then
            VENV_FOUND=true
            break
        fi
    done

    if [ "$VENV_FOUND" = false ]; then
        echo "⚠️  No venv found — using system Python/pio (if available)"
    fi
else
    echo "🐍 Using active venv: $VIRTUAL_ENV"
fi
# ─────────────────────────────────────────────────────────────────────────────

echo "🔍 Looking for M5Atom Lite..."

# Find USB serial port
PORTS=$(ls /dev/tty.usbserial-* 2>/dev/null || true)

if [ -z "$PORTS" ]; then
    echo "❌ ERROR: No M5Atom Lite found!"
    echo "   Please connect the device via USB-C"
    exit 1
fi

# Count how many ports found
PORT_COUNT=$(echo "$PORTS" | wc -l | tr -d ' ')

if [ "$PORT_COUNT" -gt 1 ]; then
    echo "⚠️  Multiple devices found:"
    echo "$PORTS"
    echo ""
    echo "Please specify which one to flash:"
    select PORT in $PORTS; do
        if [ -n "$PORT" ]; then
            break
        fi
    done
else
    PORT="$PORTS"
fi

echo "✅ Found M5Atom Lite at: $PORT"
echo ""

# Change to project directory
cd "$(dirname "$0")"

echo "🔨 Compiling firmware..."
pio run --environment m5atom-lite

echo ""
echo "⬆️  Uploading firmware to $PORT..."
pio run --target upload --upload-port "$PORT" --environment m5atom-lite

echo ""
echo "✅ Flash complete!"
echo ""

# Ask if user wants to monitor
read -p "📡 Start serial monitor? (y/n) " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Starting monitor (Ctrl+C to exit)..."
    echo "────────────────────────────────────"
    pio device monitor --port "$PORT" --baud 115200
fi

echo ""
echo "✅ Done!"
