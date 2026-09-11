#!/bin/bash

# Safely stop only the STT/LLM/TTS server belonging to this project.
# The ESP32 firmware and the shared Ollama service are intentionally left alone.

set -u

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_DIR="$PROJECT_DIR/.robot-runtime"
PID_FILE="$RUNTIME_DIR/server.pid"
TARGET_SCRIPT="$PROJECT_DIR/tools/stt_server.py"

stop_pid() {
  local pid="$1"
  local command
  local process_cwd
  command="$(ps -p "$pid" -o command= 2>/dev/null || true)"
  process_cwd="$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | awk 'substr($0,1,1)=="n" {print substr($0,2); exit}')"

  if [[ -z "$command" ]]; then
    return 1
  fi
  if [[ "$command" != *"$TARGET_SCRIPT"* ]] \
      && ! { [[ "$process_cwd" == "$PROJECT_DIR" ]] && [[ "$command" == *"tools/stt_server.py"* ]]; }; then
    echo "[Skip] PID $pid does not belong to this project."
    return 1
  fi

  echo "[Server] Stopping PID $pid..."
  kill "$pid" 2>/dev/null || return 1
  for _ in {1..20}; do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.25
  done

  echo "[Server] Graceful stop timed out; forcing shutdown."
  kill -9 "$pid" 2>/dev/null || true
  return 0
}

echo "=================================================="
echo " Stop Waveshare ESP32-S3 Voice Assistant"
echo "=================================================="

stopped=0

if [[ -f "$PID_FILE" ]]; then
  pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [[ "$pid" =~ ^[0-9]+$ ]] && stop_pid "$pid"; then
    stopped=1
  fi
  rm -f "$PID_FILE"
fi

# Fallback for a server started manually or after a stale PID file.
while IFS= read -r pid; do
  [[ -n "$pid" ]] || continue
  if stop_pid "$pid"; then
    stopped=1
  fi
done < <(lsof -tiTCP:3000 -sTCP:LISTEN 2>/dev/null || true)

if [[ "$stopped" -eq 1 ]]; then
  echo "[Server] Stopped. Port 3000 is now free."
else
  echo "[Server] This project's server was not running."
fi

echo
echo "The ESP32 may remain connected. Ollama was not stopped because other apps may use it."
printf 'Press Return to close this window...'
read -r _
