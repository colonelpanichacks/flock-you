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
NC=$'\033[0m'

# ── Cleanup on Ctrl-C ────────────────────────────────────────────────────────
cleanup() {
  echo -e "\n${DIM}[monitor_all] shutting down...${RESET}"
  # kill all per-port background jobs
  for pidfile in "$TMPDIR_ROOT"/*.pid; do
    [ -f "$pidfile" ] || continue
    pid=$(cat "$pidfile" 2>/dev/null)
    [ -n "$pid" ] && kill "$pid" 2>/dev/null
  done
  rm -rf "$TMPDIR_ROOT"
  exit 0
}
trap cleanup INT TERM

# ── Color index assignment (stable across re-scans) ─────────────────────────
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
  local port="$1"
  lsof "$port" 2>/dev/null | grep -qE "esptool|platformio|pio|upload"
}

# ── Per-port monitor loop (runs in background subshell) ─────────────────────
monitor_port() {
  local port="$1"
  local label="[$(basename "$port")]"
  local color
  color=$(get_color_for_port "$port")
  local pidfile="$TMPDIR_ROOT/$(basename "$port").pid"

  while true; do
    # ── Wait for device to appear ────────────────────────────────────────
    if [ ! -c "$port" ]; then
      sleep 1
      continue
    fi

    # ── Back off if a flash tool owns the port ───────────────────────────
    if port_is_being_flashed "$port"; then
      echo -e "${DIM}${color}${label}${RESET}${DIM} ⚡ flash tool active — standing by...${RESET}"
      sleep 3
      continue
    fi

    # ── Configure port (non-exclusive) ──────────────────────────────────
    stty -f "$port" "$BAUD" raw cs8 -cstopb -parenb clocal 2>/dev/null || {
      sleep 1
      continue
    }

    echo -e "${color}${BOLD}${label}${RESET}${color} ● connected at ${BAUD} baud${RESET}"

    # ── Stream lines via cat (blocks correctly on a character device) ────
    # cat blocks on the serial fd and exits when the port disappears.
    # We track its PID so we can kill it if a flash tool takes over.
    cat "$port" | while IFS= read -r line; do
      printf "${color}%s${RESET} %s\n" "$label" "$line"
    done

    # ── Port closed / unplugged ──────────────────────────────────────────
    if [ -c "$port" ] && port_is_being_flashed "$port"; then
      # Flash in progress — wait for it to finish then reconnect
      echo -e "${DIM}${color}${label}${RESET}${DIM} waiting for flash to complete...${RESET}"
      while port_is_being_flashed "$port"; do sleep 1; done
      echo -e "${color}${label}${RESET}${DIM} flash done, reconnecting...${RESET}"
      sleep 2
    else
      echo -e "${DIM}${color}${label}${RESET}${DIM} disconnected — waiting for reconnect...${RESET}"
      sleep 2
    fi
  done
}

# ── Track which ports already have a monitor running ────────────────────────
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
echo ""

# ── Main scan loop ───────────────────────────────────────────────────────────
while true; do
  # Discover all candidate ports
  ports=()
  for p in /dev/cu.usbserial-* /dev/cu.usbmodem*; do
    [ -c "$p" ] && ports+=("$p")
  done

  # Start a monitor for any new port
  for port in "${ports[@]}"; do
    pidfile="$TMPDIR_ROOT/$(basename "$port").pid"
    if ! is_alive "$pidfile"; then
      start_monitor "$port"
    fi
  done

  # Reap stale pid files whose port has disappeared for good
  for pidfile in "$TMPDIR_ROOT"/*.pid; do
    [ -f "$pidfile" ] || continue
    portname="${pidfile##*/}"
    portname="/dev/${portname%.pid}"
    if [ ! -c "$portname" ]; then
      pid=$(cat "$pidfile" 2>/dev/null)
      [ -n "$pid" ] && kill "$pid" 2>/dev/null
      rm -f "$pidfile"
    fi
  done

  sleep 2
done
