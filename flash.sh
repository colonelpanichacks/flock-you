#!/usr/bin/env bash
# ────────────────────────────────────────────────────────────────────────────
# flash.sh — Unified Flock-You flasher
#
# Supported devices (you are always prompted to identify each one):
#   /dev/cu.usbserial-*  → Atom Lite / Atom Echo / Atom Voice (FTDI)
#   /dev/cu.usbmodem*    → Atom VoiceS3R (ESP32-S3 native USB CDC)
#
# Usage:
#   ./flash.sh          Flash connected device, then loop for the next one
#   ./flash.sh --once   Flash one device and exit
# ────────────────────────────────────────────────────────────────────────────

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOOP=true
[[ "${1}" == "--once" ]] && LOOP=false

BOOT_PORT=""   # set by flash_device; read by show_boot_output after each flash

# ── PlatformIO esptool paths (for ESP32-S3 native USB flashing) ──────────────
PYESPTOOL="/opt/homebrew/Cellar/platformio/6.1.19_2/libexec/bin/python"
# If the versioned path doesn't exist, find it dynamically
if [[ ! -f "$PYESPTOOL" ]]; then
    PYESPTOOL=$(find /opt/homebrew/Cellar/platformio -name "python" -path "*/libexec/bin/python" 2>/dev/null | head -1)
fi
ESPTOOL_PY="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
BOOT0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# ── Venv auto-load ────────────────────────────────────────────────────────────
activate_venv() {
    local p="$1"
    [[ -f "$p/bin/activate" ]] || return 1
    # shellcheck disable=SC1091
    source "$p/bin/activate"
}

if [[ -z "$VIRTUAL_ENV" ]]; then
    for candidate in \
        "$SCRIPT_DIR/.venv"      \
        "$SCRIPT_DIR/venv"       \
        "$SCRIPT_DIR/env"        \
        "$SCRIPT_DIR/api/.venv"  \
        "$SCRIPT_DIR/api/venv"   \
        "$HOME/.platformio/penv"
    do
        activate_venv "$candidate" && break
    done
fi
# ─────────────────────────────────────────────────────────────────────────────

cd "$SCRIPT_DIR"

FLASHED_MACS=()   # indexed array — works on macOS default bash 3.2

# ── Post-flash boot monitor ───────────────────────────────────────────────────
# Streams serial output from the freshly-flashed device until:
#   • We see a "[flockyou] scanning" heartbeat  (firmware confirmed running), OR
#   • TIMEOUT_S seconds elapse with no recognizable firmware output
# Uses exec 3<>$port (O_RDWR) so DTR stays asserted — avoids the DTR-pulse
# reset that happens each time /dev/cu.* is opened freshly with cat or < $port.
show_boot_output() {
    local port="$1"
    local timeout_s="${2:-15}"

    # For usbmodem ports that might still be reappearing after the flash hard-reset,
    # wait up to 5 s for the node to exist before giving up.
    local deadline=$(( $(date +%s) + 5 ))
    while [[ ! -c "$port" ]] && (( $(date +%s) < deadline )); do
        # If port path changed (usbmodem can renumber), find the new one
        if [[ "$port" == *usbmodem* ]]; then
            local newp
            newp=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
            [[ -n "$newp" ]] && port="$newp" && break
        fi
        sleep 0.2
    done

    if [[ ! -c "$port" ]]; then
        echo "   ⚠️  Boot monitor: port $port not available — skipping"
        return
    fi

    echo ""
    echo "📡 Boot output from $(basename "$port") (waiting for firmware, Ctrl-C skips):"
    echo "────────────────────────────────────────────────────────────────"

    # O_RDWR open — keeps DTR steady, prevents ESP32 auto-reset on open
    exec 3<>"$port" 2>/dev/null || {
        echo "   ⚠️  Could not open $port for boot monitor"
        return
    }
    # -hupcl  : don't drop DTR when last fd closes (no unintentional resets)
    # clocal  : ignore DCD/modem-control lines
    # raw     : pass all bytes through; bash read splits on \n
    stty -f "$port" 115200 raw cs8 -cstopb -parenb clocal -hupcl 2>/dev/null

    local start_ts
    start_ts=$(date +%s)
    local seen_flockyou=0

    while (( $(date +%s) - start_ts < timeout_s )); do
        # read -t 2: block up to 2 s waiting for a newline from the device
        if IFS= read -r -t 2 line <&3 2>/dev/null; then
            line="${line%$'\r'}"     # strip ESP32 Serial.println() trailing \r
            [[ -n "$line" ]] && printf "   \033[2m%s\033[0m\n" "$line"
            # Track that we've heard from this firmware
            [[ "$line" == *"[flockyou]"* ]] && seen_flockyou=1
            # Stop once the scanning heartbeat arrives — confirms firmware is running
            if [[ $seen_flockyou -eq 1 && "$line" == *"scanning"* ]]; then
                echo "────────────────────────────────────────────────────────────────"
                echo "✅ Firmware confirmed running — device is scanning."
                exec 3>&- 2>/dev/null
                return
            fi
        fi
    done

    exec 3>&- 2>/dev/null
    echo "────────────────────────────────────────────────────────────────"
    if [[ $seen_flockyou -eq 1 ]]; then
        echo "✅ Firmware started (heartbeat not yet received — still booting)."
    else
        echo "⚠️  No firmware output seen in ${timeout_s}s — check baud rate or reboot manually."
    fi
}

# Returns 0 if MAC was already flashed, 1 otherwise
mac_already_flashed() {
    local m="$1"
    local e
    for e in "${FLASHED_MACS[@]+"${FLASHED_MACS[@]}"}"; do
        [[ "$e" == "$m" ]] && return 0
    done
    return 1
}

flash_device() {
    local port="$1"
    local env="$2"
    local rc=0

    echo ""
    echo "⬆️  Flashing [$env] → $port"
    echo "────────────────────────────────────"

    if [[ "$env" == "m5atom-voices3r" ]]; then
        # Release any process holding the CDC port
        local holder
        holder=$(lsof -t "$port" 2>/dev/null || true)
        if [[ -n "$holder" ]]; then
            echo "🔓 Releasing port held by PID $holder..."
            kill "$holder" 2>/dev/null || true
            sleep 1
        fi

        # Build (cached if source unchanged)
        pio run --environment m5atom-voices3r || rc=$?
        if [[ $rc -ne 0 ]]; then
            echo ""
            echo "❌ Build FAILED (exit code $rc)"
            return $rc
        fi

        # Two-stage flash for ESP32-S3 native USB (no button-hold needed).
        # Run stages serially to avoid port contention:
        #   Stage A: esptool v5+ (brew) connects, uploads stub → hard reset → exits
        #   Stage B: esptool.py v4 (PlatformIO) catches the ROM window → flashes
        pkill -f "tool-esptoolpy" 2>/dev/null || true
        sleep 0.3

        echo "   Stage A: triggering USB reset..."
        esptool --chip esp32s3 \
            --port "$port" \
            --before usb-reset \
            --after hard-reset \
            --connect-attempts 5 \
            read-mac 2>/dev/null || true
        # v5.3 has now exited and released the port. Device is rebooting.

        # Wait for port to reappear after hard reset
        local FLASH_PORT=""
        local _i
        for _i in $(seq 1 40); do
            local _p
            _p=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
            if [[ -n "$_p" ]]; then FLASH_PORT="$_p"; break; fi
            sleep 0.05
        done
        FLASH_PORT="${FLASH_PORT:-$port}"

        echo "   Stage B: flashing via $FLASH_PORT..."
        "$PYESPTOOL" "$ESPTOOL_PY" \
            --chip esp32s3 \
            --port "$FLASH_PORT" \
            --baud 115200 \
            --before no_reset \
            --after hard_reset \
            --connect-attempts 5 \
            write_flash \
            0x0     .pio/build/m5atom-voices3r/bootloader.bin \
            0x8000  .pio/build/m5atom-voices3r/partitions.bin \
            0xe000  "$BOOT0" \
            0x10000 .pio/build/m5atom-voices3r/firmware.bin || rc=$?
    else
        # Atom Lite / Atom Echo / Atom Voice — pio handles everything
        pio run -e "$env" -t upload --upload-port "$port" || rc=$?
    fi

    echo ""
    if [[ $rc -ne 0 ]]; then
        echo "❌ Flash FAILED (exit code $rc)"
        return $rc
    fi
    echo "✅ Flash complete!"

    # ── Auto-restart: wait for device to come back up ─────────────────────────
    if [[ "$env" == "m5atom-voices3r" ]]; then
        echo "🔄 Waiting for device to restart..."
        local RESTART_PORT=""
        local _j
        for _j in $(seq 1 50); do
            local _rp
            _rp=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
            if [[ -n "$_rp" ]]; then
                RESTART_PORT="$_rp"
                break
            fi
            sleep 0.1
        done

        # If port still not back, trigger an explicit restart
        if [[ -z "$RESTART_PORT" ]]; then
            echo "   Triggering explicit restart..."
            esptool --chip esp32s3 \
                --port "$port" \
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
            echo "⚠️  Device port not detected — may still be booting."
        fi
        # Expose final port to caller so show_boot_output knows where to connect
        BOOT_PORT="${RESTART_PORT:-$port}"
    else
        # FTDI devices stay on the same port after upload reset
        BOOT_PORT="$port"
    fi
}

# ── Main loop ────────────────────────────────────────────────────────────────
while true; do
    echo ""
    echo "🔍 Scanning for devices..."

    # macOS exposes serial devices as both tty.* and cu.* for the same physical
    # port. cu.* (call-up) is the correct one for programming — use that.
    SERIAL_PORTS=$(ls /dev/cu.usbserial-* 2>/dev/null || true)
    MODEM_PORTS=$(ls /dev/cu.usbmodem*    2>/dev/null || true)

    # Combine all found ports (serial first, then modem)
    ALL_PORTS=""
    [[ -n "$SERIAL_PORTS" ]] && ALL_PORTS="$SERIAL_PORTS"
    [[ -n "$MODEM_PORTS"  ]] && ALL_PORTS="${ALL_PORTS}${ALL_PORTS:+ }$MODEM_PORTS"

    if [[ -z "$ALL_PORTS" ]]; then
        echo "❌ No device found."
        echo ""
        echo "   Expected ports:"
        echo "     Atom Lite / Atom Echo / Atom Voice → /dev/cu.usbserial-*"
        echo "     Atom VoiceS3R (native USB)         → /dev/cu.usbmodem*"
        echo ""

        # Show what USB devices ARE visible to macOS for diagnosis
        USB_DEVS=$(system_profiler SPUSBDataType 2>/dev/null \
                   | grep -E "^\s+(Product ID|Manufacturer|Location ID|M5|Espressif|FTDI|Silicon|CP210)" \
                   | head -20 || true)
        if [[ -n "$USB_DEVS" ]]; then
            echo "   USB devices macOS can see:"
            echo "$USB_DEVS"
            echo ""
        else
            echo "   ⚠️  macOS sees no USB devices at all."
            echo "   → Try a different cable (many USB-C cables are charge-only)"
            echo "   → Try a different USB port"
            echo "   → For VoiceS3R: unplug, hold the button, then plug in"
            echo ""
        fi

        if $LOOP; then
            read -r -p "   Press Enter to retry, or Ctrl-C to quit… "
            continue
        else
            exit 1
        fi
    fi

    # Pick the first available port
    PORT=$(echo "$ALL_PORTS" | tr ' ' '\n' | head -1)

    # Determine connection type for this port
    if echo "$PORT" | grep -q "usbmodem"; then
        PORT_TYPE="usbmodem (native USB / ESP32-S3)"
    else
        PORT_TYPE="usbserial (FTDI)"
    fi

    # ── Always prompt: identify the device ────────────────────────────────────
    echo ""
    echo "🔌 Device detected at $PORT  [$PORT_TYPE]"
    echo ""
    echo "   What device is this?"
    echo "   1) Atom Lite        — LED only, no speaker          (FTDI/usbserial)"
    echo "   2) Atom Echo        — speaker only, no LED          (FTDI/usbserial)"
    echo "   3) Atom Voice       — LED + I²S speaker (NS4168)    (FTDI/usbserial)"
    echo "   4) Atom VoiceS3R    — native USB, ES8311 codec      (ESP32-S3, usbmodem)"
    echo "   5) Atom Echo S3R    — native USB, I²S speaker       (ESP32-S3, usbmodem)"
    echo ""
    echo "   ℹ️  Options 4/5 (ESP32-S3 / Atom VoiceS3R or Echo S3R):"
    echo "      Flashing is fully automatic — no button-hold required."
    echo "      If all 3 attempts fail, unplug + re-plug the device and try again."
    echo ""
    read -r -p "   Enter 1, 2, 3, 4, or 5: " VARIANT

    case "$VARIANT" in
        2)
            ENV="m5atom-echo"
            LABEL="Atom Echo (speaker only)"
            EXPECTED_PORT_TYPE="usbserial"
            ;;
        3)
            ENV="m5atom-voice"
            LABEL="Atom Voice (LED + I²S speaker)"
            EXPECTED_PORT_TYPE="usbserial"
            ;;
        4)
            ENV="m5atom-voices3r"
            LABEL="Atom VoiceS3R (native USB)"
            EXPECTED_PORT_TYPE="usbmodem"
            ;;
        5)
            ENV="m5atom-voices3r"
            LABEL="Atom Echo S3R (native USB)"
            EXPECTED_PORT_TYPE="usbmodem"
            ;;
        *)
            ENV="m5atom-lite"
            LABEL="Atom Lite (LED only)"
            EXPECTED_PORT_TYPE="usbserial"
            ;;
    esac

    # ── Port-type mismatch warning ────────────────────────────────────────────
    if [[ "$EXPECTED_PORT_TYPE" == "usbserial" ]] && echo "$PORT" | grep -q "usbmodem"; then
        echo ""
        echo "   ⚠️  WARNING: You selected an FTDI device ($LABEL)"
        echo "      but the port $PORT looks like native USB (ESP32-S3)."
        echo "      FTDI devices show up as /dev/cu.usbserial-*, not /dev/cu.usbmodem*."
        echo "      Did you mean to select option 4 (Atom VoiceS3R)?"
        echo ""
        read -r -p "   Continue anyway? (y/N): " CONFIRM
        if [[ "$CONFIRM" != "y" && "$CONFIRM" != "Y" ]]; then
            echo "   Restarting selection…"
            continue
        fi
    fi

    if [[ "$EXPECTED_PORT_TYPE" == "usbmodem" ]] && echo "$PORT" | grep -q "usbserial"; then
        echo ""
        echo "   ⚠️  WARNING: You selected Atom VoiceS3R (native USB)"
        echo "      but the port $PORT looks like an FTDI device (usbserial)."
        echo "      VoiceS3R shows up as /dev/cu.usbmodem*, not /dev/cu.usbserial-*."
        echo ""
        read -r -p "   Continue anyway? (y/N): " CONFIRM
        if [[ "$CONFIRM" != "y" && "$CONFIRM" != "Y" ]]; then
            echo "   Restarting selection…"
            continue
        fi
    fi

    # ── Duplicate MAC guard ───────────────────────────────────────────────────
    MAC=$(esptool.py --port "$PORT" --no-stub chip_id 2>/dev/null \
          | grep -i "MAC:" | awk '{print $2}' || echo "unknown")

    if [[ -n "$MAC" && "$MAC" != "unknown" ]] && mac_already_flashed "$MAC"; then
        echo "⚠️  Already flashed this device ($MAC) — unplug it and plug in the next one."
        if $LOOP; then
            read -r -p "   Press Enter when ready… "
            continue
        else
            exit 0
        fi
    fi

    echo "✅ Identified: $LABEL at $PORT  (MAC: ${MAC:-n/a})"
    if flash_device "$PORT" "$ENV"; then
        [[ -n "$MAC" && "$MAC" != "unknown" ]] && FLASHED_MACS+=("$MAC")
    else
        echo ""
        echo "   Flash failed. Check the output above for errors."
        if $LOOP; then
            read -r -p "   Press Enter to try again or Ctrl-C to quit… "
            continue
        else
            exit 1
        fi
    fi

    # ── Stream boot output until firmware heartbeat (or 15 s timeout) ────────
    show_boot_output "${BOOT_PORT:-$PORT}"

    if $LOOP; then
        echo ""
        read -r -p "🔁 Plug in the next device, then press Enter… (Ctrl-C to stop) "
    else
        break
    fi
done

echo ""
echo "🎉 All done! Flashed ${#FLASHED_MACS[@]} device(s) this session:"
for mac in "${FLASHED_MACS[@]+"${FLASHED_MACS[@]}"}"; do
    echo "   • $mac"
done
