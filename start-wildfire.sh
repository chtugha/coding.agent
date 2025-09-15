#!/usr/bin/env bash
# start-wildfire.sh
# Comprehensive startup script to cleanup processes, reset database service states, and start the HTTP server
# Platform: macOS (tested), but uses only POSIX-ish tools

set -Eeuo pipefail

ROOT_DIR="/Users/whisper/Documents/augment-projects/clean-repo"
HTTP_SERVER_BIN="$ROOT_DIR/bin/http-server"
# Fallback to legacy location if CMake-built binary not found yet
if [ ! -x "$HTTP_SERVER_BIN" ] && [ -x "$ROOT_DIR/http-server" ]; then
  HTTP_SERVER_BIN="$ROOT_DIR/http-server"
fi
DB_PATH="$ROOT_DIR/whisper_talk.db"
SQLITE="/usr/bin/sqlite3"
PORT=8081

# -----------------------
# Logging helpers
# -----------------------
log() {
  printf "%s %s\n" "[$(date '+%Y-%m-%d %H:%M:%S')]" "$*"
}

hr() { printf '%*s\n' "${COLUMNS:-80}" '' | tr ' ' '-'; }

# -----------------------
# Process management
# -----------------------
# Send a signal to PIDs if any exist
send_signal_if_pids() {
  local sig="$1"; shift
  local pids=("$@")
  if (( ${#pids[@]} > 0 )); then
    log "Sending $sig to PIDs: ${pids[*]}"
    kill -s "$sig" "${pids[@]}" 2>/dev/null || true
  fi
}

# Find PIDs by pattern, excluding this script's PID
find_pids() {
  local pattern="$1"
  local self_pid=$$
  pgrep -f "$pattern" 2>/dev/null | awk -v self="$self_pid" '{ if ($1 != self) print $1 }'
}

# Terminate processes matching a pattern gracefully then forcefully if needed
terminate_pattern() {
  local pattern="$1"
  log "Scanning for processes matching: $pattern"

  # Collect PIDs (portable, no mapfile)
  local pids
  pids="$(find_pids "$pattern" || true)"

  if [ -z "$pids" ]; then
    log "No running processes for pattern: $pattern"
    return 0
  fi

  log "Found PIDs: $pids (pattern: $pattern)"
  # shellcheck disable=SC2086
  send_signal_if_pids TERM $pids

  # Wait up to ~5 seconds for graceful shutdown
  local i
  for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 0.5
    pids="$(find_pids "$pattern" || true)"
    if [ -z "$pids" ]; then
      log "All processes (pattern: $pattern) terminated gracefully"
      return 0
    fi
  done

  log "Forcing termination for remaining PIDs: $pids (pattern: $pattern)"
  # shellcheck disable=SC2086
  send_signal_if_pids KILL $pids

  # Final verification
  sleep 0.5
  pids="$(find_pids "$pattern" || true)"
  if [ -n "$pids" ]; then
    log "WARNING: Some processes still running for pattern $pattern: $pids"
    return 1
  fi
  log "Processes (pattern: $pattern) fully stopped"
}

cleanup_processes() {
  hr; log "PHASE 1: Process Cleanup"; hr

  # Specific port listener (HTTP server) first
  if command -v lsof >/dev/null 2>&1; then
    if lsof -tiTCP:$PORT -sTCP:LISTEN >/dev/null 2>&1; then
      lpids="$(lsof -tiTCP:$PORT -sTCP:LISTEN 2>/dev/null | sort -u || true)"
      if [ -n "$lpids" ]; then
        log "Found listeners on port $PORT: $lpids - sending INT"
        # shellcheck disable=SC2086
        kill -INT $lpids 2>/dev/null || true
        sleep 0.5
      fi
    fi
  fi

  # Kill by known process names
  terminate_pattern "$ROOT_DIR/http-server|(^|/)http-server( |$)"
  terminate_pattern "$ROOT_DIR/sip-client|(^|/)sip-client( |$)"
  terminate_pattern "$ROOT_DIR/audio-processor-service|(^|/)audio-processor-service( |$)"
  terminate_pattern "$ROOT_DIR/whisper-service|(^|/)whisper-service( |$)"
  terminate_pattern "$ROOT_DIR/llama-service|(^|/)llama-service( |$)"
  terminate_pattern "$ROOT_DIR/piper-service|(^|/)piper-service( |$)"
}

# -----------------------
# Database state reset
# -----------------------
reset_database_states() {
  hr; log "PHASE 2: Database State Reset"; hr

  if [ ! -x "$SQLITE" ]; then
    log "ERROR: sqlite3 not found at $SQLITE"
    return 1
  fi

  if [ ! -f "$DB_PATH" ]; then
    log "Database not found at $DB_PATH - skipping state reset (fresh start expected to be OK)"
    return 0
  fi

  log "Resetting service states in database: $DB_PATH"
  local sql
  sql='
BEGIN TRANSACTION;
-- Reset service status keys to stopped
UPDATE system_config SET value = "stopped", updated_at = CURRENT_TIMESTAMP
 WHERE key IN ("whisper_service_status", "llama_service_status", "piper_service_status");
-- Reset SIP line statuses and disable lines by default
UPDATE sip_lines SET status = "disconnected";
UPDATE sip_lines SET enabled = 0;
COMMIT;
'
  # Run and capture output/exit status
  if ! printf "%s" "$sql" | "$SQLITE" "$DB_PATH" >/dev/null 2>&1; then
    log "ERROR: Failed to reset database states"
    return 1
  fi
  log "Database service states reset to defaults (stopped/disconnected/disabled)"
}

# -----------------------
# Start HTTP server
# -----------------------
start_server() {
  hr; log "PHASE 3: Starting HTTP Server"; hr

  if [ ! -x "$HTTP_SERVER_BIN" ]; then
    log "ERROR: HTTP server binary not found or not executable: $HTTP_SERVER_BIN"
    return 1
  fi

  log "Launching: $HTTP_SERVER_BIN (port $PORT)"
  # Ensure dynamic libs resolve for Piper (libpiper + ONNXRuntime)
  ORT_LIB_DIR="$(ls -d "$ROOT_DIR/libpiper/lib"/onnxruntime-*/lib 2>/dev/null | head -n1 || true)"
  export DYLD_LIBRARY_PATH="$ROOT_DIR/libpiper/build${ORT_LIB_DIR:+:$ORT_LIB_DIR}:${DYLD_LIBRARY_PATH:-}"
  exec "$HTTP_SERVER_BIN"
}

main() {
  cleanup_processes || true
  reset_database_states
  start_server
}

main "$@"

