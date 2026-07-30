#!/usr/bin/env bash
# monitor_all.sh — Continuous multi-device serial monitor for flock-you ESP32 devices
#
# - Auto-discovers /dev/cu.usbserial-* (Atom Lite / Echo / Voice FTDI)
#                  and /dev/cu.usbmodem* (VoiceS3R native USB CDC)
# - Color-coded, labeled output per device
# - Auto-reconnects when a device is unplugged / replugged / reflashed
# - Backs off silently when esptool / pio is flashing the port
# - Does NOT hold ports exclusively — flashing tools take over any time
#
# Usage: ./monitor_all.sh [baud]   (default baud = 115200)

BAUD="${1:-115200}"
TMPDIR_ROOT="/tmp/fymon.$$"
mkdir -p "$TMPDIR_ROOT"

# ── ANSI colors ──────────────────────────────────────────────────────────────
COLORS=("\033[1;32m" "\033[1;33m" "\033[1;36m" "\033[1;35m" "\033[1;34m" "\033[1;31m")
RESET="\033[0m"
DIM="\033[2m"
BOLD="\033[1m"

# ── Cleanup on Ctrl-C ────────────────────────────────────────────────────────
cleanup() {
  echo -e "\n${DIM}[monitor_all] shutting down...${RESET}"
  for pidfile in "$TMPDIR_ROOT"/*.pid; do
    [ -f "$pidfile" ] || continue
    pid=$(cat "$pidfile" 2>/dev/null)
    [ -n "$pid" ] && kill -- "-$pid" 2>/dev/null || kill "$pid" 2>/dev/null
  done
  rm -rf "$TMPDIR_ROOT"
  exit 0
}
trap cleanup INT TERM

# ── Stable color per port (persists across reconnects) ───────────────────────
color_index_file="$TMPDIR_ROOT/color_idx"
echo "0" > "$color_index_file"

get_color_for_port() {
  local port="$1"
  local cfile="$TMPDIR_ROOT/color_$(basename "$port")"
  if [ ! -f "$cfile" ]; then
    local idx
    idx=$(cat "$color_index_file")
    echo "${COLORS[$((idx % ${#COLORS[@]}))]}" > "$cfile"
    echo $((idx + 1)) > "$color_index_file"
  fi
  cat "$cfile"
}

# ── Flash-tool detection ─────────────────────────────────────────────────────
port_is_being_flashed() {
  lsof "$1" 2>/dev/null | grep -qE "esptool|platformio|pio|upload"
}

# ── Per-port monitor loop ────────────────────────────────────────────────────
#
# Key macOS serial fix: use "exec N<>$port" (O_RDWR) to open the port ONCE
# and keep the fd alive across stty.  Re-opening /dev/cu.* with cat or < each
# time pulses DTR → ESP32 auto-reset → ESP32 bootloader → EOF on that fd.
# With exec the fd stays open (DTR held asserted), stty configures it in-place,
# and "while read <&N" blocks correctly via VMIN=1.
#
monitor_port() {
  local port="$1"
  local label="[$(basename "$port")]"
  local color
  color=$(get_color_for_port "$port")

  while true; do

    # ── Wait for the device node to exist ────────────────────────────────
    if [ ! -c "$port" ]; then
      sleep 1
      continue
    fi

    # ── Back off while a flash tool holds the port ───────────────────────
    if port_is_being_flashed "$port"; then
      echo -e "${DIM}${color}${label}${RESET}${DIM} ⚡ flash tool active — standing by...${RESET}"
      sleep 3
      continue
    fi

    # ── Open port O_RDWR — single persistent fd, no DTR re-pulse ────────
    exec 3<>"$port" 2>/dev/null
    if [ $? -ne 0 ]; then
      sleep 1
      continue
    fi

    # ── Configure terminal on the now-open fd ────────────────────────────
    # -hupcl : don't drop DTR on last close (prevents ESP32 reset on our close)
    # clocal : ignore modem-control lines (don't treat DCD loss as hangup)
    # raw    : disable all line processing (pass bytes through as-is)
    stty -f "$port" "$BAUD" raw cs8 -cstopb -parenb clocal -hupcl 2>/dev/null

    echo -e "${color}${BOLD}${label}${RESET}${color} ● connected at ${BAUD} baud${RESET}"

    # ── Stream lines from the persistent fd ──────────────────────────────
    # In raw mode with VMIN=1, bash's "read <&3" issues one character-at-a-time
    # read(2) calls, blocking until each char arrives, then lines on \n.
    # We strip the trailing \r ESP32 Serial.println() appends.
    while IFS= read -r line <&3; do
      line="${line%$'\r'}"        # strip trailing CR from \r\n line endings
      printf "${color}%s${RESET} %s\n" "$label" "$line"
    done

    # ── fd closed (port disappeared or flash tool took over) ─────────────
    exec 3>&- 2>/dev/null

    if [ -c "$port" ] && port_is_being_flashed "$port"; then
      echo -e "${DIM}${color}${label}${RESET}${DIM} waiting for flash to complete...${RESET}"
      while port_is_being_flashed "$port"; do sleep 1; done
      echo -e "${color}${label}${RESET}${DIM} flash done — reconnecting...${RESET}"
      sleep 2
    else
      echo -e "${DIM}${color}${label}${RESET}${DIM} disconnected — waiting for reconnect...${RESET}"
      sleep 2
    fi

  done
}

# ── PID tracking ─────────────────────────────────────────────────────────────
is_alive() {
  local pidfile="$1"
  [ -f "$pidfile" ] || return 1
  local pid
  pid=$(cat "$pidfile" 2>/dev/null)
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

start_monitor() {
  local port="$1"
  local pidfile="$TMPDIR_ROOT/$(basename "$port").pid"
  monitor_port "$port" &
  echo $! > "$pidfile"
}

# ── Header ───────────────────────────────────────────────────────────────────
echo -e "${BOLD}flock-you multi-device serial monitor${RESET}  (baud=${BAUD}, Ctrl-C to quit)"
echo -e "${DIM}Watching: /dev/cu.usbserial-*  /dev/cu.usbmodem*${RESET}"
echo -e "${DIM}Output: [flockyou] log lines + {\"event\":\"detection\",...} JSON per alert${RESET}"
echo ""

# ── Main scan loop (new ports picked up every 2 s) ───────────────────────────
while true; do
  ports=()
  for p in /dev/cu.usbserial-* /dev/cu.usbmodem*; do
    [ -c "$p" ] && ports+=("$p")
  done

  for port in "${ports[@]}"; do
    pidfile="$TMPDIR_ROOT/$(basename "$port").pid"
    if ! is_alive "$pidfile"; then
      start_monitor "$port"
    fi
  done

  # Reap pid files for ports that are gone for good
  for pidfile in "$TMPDIR_ROOT"/*.pid; do
    [ -f "$pidfile" ] || continue
    portname="/dev/${pidfile##*/}"
    portname="${portname%.pid}"
    if [ ! -c "$portname" ]; then
      pid=$(cat "$pidfile" 2>/dev/null)
      [ -n "$pid" ] && kill "$pid" 2>/dev/null
      rm -f "$pidfile"
    fi
  done

  sleep 2
done
