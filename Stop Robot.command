#!/bin/bash

# Safely stop this project's main/A-B server and local TTS/STT sidecars.
# The ESP32 firmware and the shared Ollama service are intentionally left alone.

set -u

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_DIR="$PROJECT_DIR/.robot-runtime"
SERVER_PID_FILE="$RUNTIME_DIR/server.pid"
SUPERTONIC_PID_FILE="$RUNTIME_DIR/supertonic.pid"
QWEN_STT_PID_FILE="$RUNTIME_DIR/qwen-stt.pid"
STT_AB_PID_FILE="$RUNTIME_DIR/stt-ab/server.pid"
SERVER_SCRIPT="$PROJECT_DIR/tools/stt_server.py"
STT_AB_SCRIPT="$PROJECT_DIR/tools/stt_ab_server.py"
SUPERTONIC_SCRIPT="$PROJECT_DIR/tools/supertonic_tts_service.py"
QWEN_STT_SCRIPT="$PROJECT_DIR/tools/qwen_stt_service.py"

stop_pid() {
  local pid="$1"
  local target_script="$2"
  local relative_script="$3"
  local label="$4"
  local expected_cwd="$5"
  local command
  local process_cwd
  command="$(ps -p "$pid" -o command= 2>/dev/null || true)"
  process_cwd="$(lsof -a -p "$pid" -d cwd -Fn 2>/dev/null | awk 'substr($0,1,1)=="n" {print substr($0,2); exit}')"

  if [[ -z "$command" ]]; then
    return 1
  fi
  if [[ "$command" != *"$target_script"* ]] \
      && ! { [[ "$process_cwd" == "$expected_cwd" ]] && [[ "$command" == *"$relative_script"* ]]; }; then
    echo "[Skip] PID $pid does not belong to this project."
    return 1
  fi

  echo "[$label] Stopping PID $pid..."
  kill "$pid" 2>/dev/null || return 1
  for _ in {1..20}; do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.25
  done

  echo "[$label] Graceful stop timed out; forcing shutdown."
  kill -9 "$pid" 2>/dev/null || true
  return 0
}

stop_from_pid_file() {
  local pid_file="$1"
  local target_script="$2"
  local relative_script="$3"
  local label="$4"
  local expected_cwd="$5"
  if [[ -f "$pid_file" ]]; then
    local pid
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [[ "$pid" =~ ^[0-9]+$ ]]; then
      stop_pid "$pid" "$target_script" "$relative_script" "$label" "$expected_cwd" || true
    fi
    rm -f "$pid_file"
  fi
}

stop_from_port() {
  local port="$1"
  local target_script="$2"
  local relative_script="$3"
  local label="$4"
  local expected_cwd="$5"
  while IFS= read -r pid; do
    [[ -n "$pid" ]] || continue
    stop_pid "$pid" "$target_script" "$relative_script" "$label" "$expected_cwd" || true
  done < <(lsof -tiTCP:"$port" -sTCP:LISTEN 2>/dev/null || true)
}

echo "=================================================="
echo " Stop Waveshare ESP32-S3 Voice Assistant"
echo "=================================================="

stop_from_pid_file "$SERVER_PID_FILE" "$SERVER_SCRIPT" "tools/stt_server.py" \
  "Server" "$PROJECT_DIR"
stop_from_port 3000 "$SERVER_SCRIPT" "tools/stt_server.py" "Server" "$PROJECT_DIR"
stop_from_pid_file "$STT_AB_PID_FILE" "$STT_AB_SCRIPT" "tools/stt_ab_server.py" \
  "STT A/B" "$PROJECT_DIR"
stop_from_port 3000 "$STT_AB_SCRIPT" "tools/stt_ab_server.py" \
  "STT A/B" "$PROJECT_DIR"
stop_from_pid_file "$SUPERTONIC_PID_FILE" "$SUPERTONIC_SCRIPT" \
  "tools/supertonic_tts_service.py" "Supertonic" "$RUNTIME_DIR/supertonic"
stop_from_port 3001 "$SUPERTONIC_SCRIPT" \
  "tools/supertonic_tts_service.py" "Supertonic" "$RUNTIME_DIR/supertonic"
stop_from_pid_file "$QWEN_STT_PID_FILE" "$QWEN_STT_SCRIPT" \
  "tools/qwen_stt_service.py" "Qwen STT" "$RUNTIME_DIR/qwen-stt"
stop_from_port 3002 "$QWEN_STT_SCRIPT" \
  "tools/qwen_stt_service.py" "Qwen STT" "$RUNTIME_DIR/qwen-stt"

for port in 3000 3001 3002; do
  if lsof -tiTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "[WARN] Port $port is still occupied; unrelated processes were not stopped."
  else
    echo "[Stop] Port $port is free."
  fi
done

echo
echo "The ESP32 may remain connected. Ollama was not stopped because other apps may use it."
printf 'Press Return to close this window...'
read -r _
