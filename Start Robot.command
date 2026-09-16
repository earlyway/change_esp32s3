#!/bin/bash

# One-click launcher for the Waveshare ESP32-S3 voice assistant.
# Double-click this file in Finder after connecting the board over USB-C.

set -u

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_DIR="$PROJECT_DIR/.robot-runtime"
SERVER_LOG="$RUNTIME_DIR/server.log"
SERVER_PID="$RUNTIME_DIR/server.pid"
SUPERTONIC_LOG="$RUNTIME_DIR/supertonic.log"
SUPERTONIC_PID="$RUNTIME_DIR/supertonic.pid"
SUPERTONIC_URL="http://127.0.0.1:3001"
SUPERTONIC_VENV="$PROJECT_DIR/.venv-supertonic"
TTS_BACKEND_VALUE="macos"
QWEN_STT_URL="http://127.0.0.1:3002"
QWEN_STT_VENV="$PROJECT_DIR/.venv-qwen-stt"
STT_AB_VENV="$PROJECT_DIR/.venv-stt-ab"
QWEN_STT_LOG="$RUNTIME_DIR/qwen-stt.log"
QWEN_STT_PID="$RUNTIME_DIR/qwen-stt.pid"
STT_BACKEND_VALUE="faster-whisper"
WEB_URL="http://localhost:3000"

mkdir -p "$RUNTIME_DIR"
cd "$PROJECT_DIR"

exec > >(tee -a "$RUNTIME_DIR/launcher.log") 2>&1

fail() {
  printf '\n[ERROR] %s\n' "$1"
  printf 'Press Return to close this window...'
  read -r _
  exit 1
}

find_pio() {
  if command -v pio >/dev/null 2>&1; then
    command -v pio
  elif [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    printf '%s\n' "$HOME/.platformio/penv/bin/pio"
  else
    return 1
  fi
}

current_lan_ip() {
  local ip
  ip="$(ipconfig getifaddr en0 2>/dev/null || true)"
  if [[ -z "$ip" ]]; then
    ip="$(ifconfig en0 2>/dev/null | awk '/inet / {print $2; exit}')"
  fi
  printf '%s\n' "$ip"
}

update_server_ip() {
  local ip="$1"
  ROBOT_SERVER_IP="$ip" python3 - "$PROJECT_DIR/include/server_config.h" <<'PY'
import os
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
updated, count = re.subn(
    r'(#define\s+POST_SERVER_HOST\s+)"[^"]*"',
    rf'\1"{os.environ["ROBOT_SERVER_IP"]}"',
    text,
    count=1,
)
if count != 1:
    raise SystemExit("POST_SERVER_HOST was not found in include/server_config.h")
path.write_text(updated)
PY
}

ensure_python_deps() {
  if python3 -c "import fastapi, uvicorn, faster_whisper, numpy" >/dev/null 2>&1; then
    PYTHON_BIN="python3"
    return
  fi

  echo "[Setup] Python STT dependencies are missing; creating .venv..."
  if [[ ! -x "$PROJECT_DIR/.venv/bin/python" ]]; then
    python3 -m venv "$PROJECT_DIR/.venv" || fail "Could not create Python virtual environment."
  fi
  "$PROJECT_DIR/.venv/bin/python" -m pip install --upgrade pip
  "$PROJECT_DIR/.venv/bin/python" -m pip install -r "$PROJECT_DIR/tools/requirements.txt" \
    || fail "Could not install STT dependencies."
  PYTHON_BIN="$PROJECT_DIR/.venv/bin/python"
}

ensure_ollama() {
  if curl -fsS --max-time 2 http://127.0.0.1:11434/api/tags >/dev/null 2>&1; then
    echo "[Ollama] Service ready."
    return
  fi

  if command -v ollama >/dev/null 2>&1; then
    echo "[Ollama] Starting local service..."
    nohup ollama serve >"$RUNTIME_DIR/ollama.log" 2>&1 &
  elif [[ -d "/Applications/Ollama.app" ]]; then
    echo "[Ollama] Opening Ollama.app..."
    open -a Ollama
  else
    echo "[WARN] Ollama is not installed. STT and manual TTS will work, but automatic LLM replies will not."
    return
  fi

  for _ in {1..30}; do
    curl -fsS --max-time 2 http://127.0.0.1:11434/api/tags >/dev/null 2>&1 && {
      echo "[Ollama] Service ready."
      return
    }
    sleep 1
  done
  echo "[WARN] Ollama did not become ready. The launcher will continue with STT/TTS only."
}

supertonic_ready() {
  curl -fsS --max-time 2 "$SUPERTONIC_URL/health" 2>/dev/null \
    | python3 -c 'import json,sys; data=json.load(sys.stdin); raise SystemExit(data.get("service") != "supertonic-robot-tts")' \
      >/dev/null 2>&1
}

ensure_supertonic() {
  if supertonic_ready; then
    echo "[TTS] Supertonic service ready."
    TTS_BACKEND_VALUE="supertonic"
    return
  fi

  if lsof -tiTCP:3001 -sTCP:LISTEN >/dev/null 2>&1; then
    echo "[WARN] Port 3001 is occupied by another program. Using macOS say fallback."
    return
  fi

  local base_python=""
  if command -v python3.11 >/dev/null 2>&1; then
    base_python="$(command -v python3.11)"
  elif command -v python3.12 >/dev/null 2>&1; then
    base_python="$(command -v python3.12)"
  else
    echo "[WARN] Python 3.11/3.12 is unavailable. Using macOS say fallback."
    return
  fi

  if [[ ! -x "$SUPERTONIC_VENV/bin/python" ]]; then
    echo "[TTS] Creating isolated Supertonic environment..."
    "$base_python" -m venv "$SUPERTONIC_VENV" || {
      echo "[WARN] Could not create Supertonic environment. Using macOS say fallback."
      return
    }
  fi

  if ! "$SUPERTONIC_VENV/bin/python" -c \
    "from importlib.metadata import version; raise SystemExit(version('supertonic') != '1.3.1')" \
    >/dev/null 2>&1; then
    echo "[TTS] Installing Supertonic 3..."
    "$SUPERTONIC_VENV/bin/python" -m pip install \
      -r "$PROJECT_DIR/tools/requirements-supertonic.txt" || {
      echo "[WARN] Supertonic installation failed. Using macOS say fallback."
      return
    }
  fi

  echo "[TTS] Starting Supertonic 3 F1 service..."
  : >"$SUPERTONIC_LOG"
  nohup env \
    SUPERTONIC_RUNTIME_DIR="$RUNTIME_DIR/supertonic" \
    SUPERTONIC_VOICE=F1 \
    SUPERTONIC_STEPS=8 \
    SUPERTONIC_SPEED=1.0 \
    "$SUPERTONIC_VENV/bin/python" -u "$PROJECT_DIR/tools/supertonic_tts_service.py" \
    >"$SUPERTONIC_LOG" 2>&1 &
  echo "$!" >"$SUPERTONIC_PID"

  # A fresh installation may need to download roughly 400 MB of model assets.
  for _ in {1..180}; do
    if supertonic_ready; then
      echo "[TTS] Supertonic service ready."
      TTS_BACKEND_VALUE="supertonic"
      return
    fi
    if ! kill -0 "$(cat "$SUPERTONIC_PID")" 2>/dev/null; then
      tail -30 "$SUPERTONIC_LOG"
      rm -f "$SUPERTONIC_PID"
      echo "[WARN] Supertonic stopped during startup. Using macOS say fallback."
      return
    fi
    sleep 1
  done

  kill "$(cat "$SUPERTONIC_PID")" 2>/dev/null || true
  rm -f "$SUPERTONIC_PID"
  echo "[WARN] Supertonic startup timed out. Using macOS say fallback."
}

qwen_stt_ready() {
  curl -fsS --max-time 2 "$QWEN_STT_URL/health" 2>/dev/null \
    | python3 -c 'import json,sys; data=json.load(sys.stdin); raise SystemExit(data.get("service") != "qwen-robot-stt")' \
      >/dev/null 2>&1
}

qwen_python_ok() {
  local python_bin="$1"
  [[ -x "$python_bin" ]] || return 1
  "$python_bin" -c \
    "from importlib.metadata import version; raise SystemExit(version('mlx-qwen3-asr') != '0.4.0')" \
    >/dev/null 2>&1
}

ensure_qwen_stt() {
  if qwen_stt_ready; then
    echo "[STT] Qwen3-ASR service ready."
    STT_BACKEND_VALUE="qwen"
    return
  fi

  if lsof -tiTCP:3002 -sTCP:LISTEN >/dev/null 2>&1; then
    echo "[WARN] Port 3002 is occupied by another program. Using faster-whisper fallback."
    return
  fi

  local qwen_python=""
  if qwen_python_ok "$STT_AB_VENV/bin/python"; then
    qwen_python="$STT_AB_VENV/bin/python"
  else
    local base_python=""
    if command -v python3.11 >/dev/null 2>&1; then
      base_python="$(command -v python3.11)"
    elif command -v python3.12 >/dev/null 2>&1; then
      base_python="$(command -v python3.12)"
    else
      echo "[WARN] Python 3.11/3.12 is unavailable. Using faster-whisper fallback."
      return
    fi

    if [[ ! -x "$QWEN_STT_VENV/bin/python" ]]; then
      echo "[STT] Creating isolated Qwen3-ASR environment..."
      "$base_python" -m venv "$QWEN_STT_VENV" || {
        echo "[WARN] Could not create Qwen environment. Using faster-whisper fallback."
        return
      }
    fi

    if ! qwen_python_ok "$QWEN_STT_VENV/bin/python"; then
      echo "[STT] Installing Qwen3-ASR 0.6B..."
      "$QWEN_STT_VENV/bin/python" -m pip install \
        -r "$PROJECT_DIR/tools/requirements-qwen-stt.txt" || {
        echo "[WARN] Qwen installation failed. Using faster-whisper fallback."
        return
      }
    fi
    qwen_python="$QWEN_STT_VENV/bin/python"
  fi

  echo "[STT] Starting Qwen3-ASR 0.6B service..."
  : >"$QWEN_STT_LOG"
  nohup env \
    QWEN_STT_RUNTIME_DIR="$RUNTIME_DIR/qwen-stt" \
    QWEN_STT_MODEL="Qwen/Qwen3-ASR-0.6B" \
    "$qwen_python" -u "$PROJECT_DIR/tools/qwen_stt_service.py" \
    >"$QWEN_STT_LOG" 2>&1 &
  echo "$!" >"$QWEN_STT_PID"

  # A cached model still needs time to load into MLX.
  for _ in {1..180}; do
    if qwen_stt_ready; then
      echo "[STT] Qwen3-ASR service ready."
      STT_BACKEND_VALUE="qwen"
      return
    fi
    if ! kill -0 "$(cat "$QWEN_STT_PID")" 2>/dev/null; then
      tail -30 "$QWEN_STT_LOG"
      rm -f "$QWEN_STT_PID"
      echo "[WARN] Qwen stopped during startup. Using faster-whisper fallback."
      return
    fi
    sleep 1
  done

  kill "$(cat "$QWEN_STT_PID")" 2>/dev/null || true
  rm -f "$QWEN_STT_PID"
  echo "[WARN] Qwen startup timed out. Using faster-whisper fallback."
}

start_stt_server() {
  if curl -fsS --max-time 2 "$WEB_URL/robot/status" >/dev/null 2>&1; then
    echo "[Server] Already running on port 3000."
    echo "[Server] Stop Robot.command first if this process should pick up the Qwen STT sidecar."
    return
  fi

  if lsof -tiTCP:3000 -sTCP:LISTEN >/dev/null 2>&1; then
    fail "Port 3000 is occupied by another program."
  fi

  echo "[Server] Starting Korean STT / LLM / TTS pipeline..."
  : >"$SERVER_LOG"
  nohup env \
    STT_LANGUAGE=ko \
    STT_BACKEND="$STT_BACKEND_VALUE" \
    QWEN_STT_URL="$QWEN_STT_URL" \
    TTS_BACKEND="$TTS_BACKEND_VALUE" \
    SUPERTONIC_URL="$SUPERTONIC_URL" \
    "$PYTHON_BIN" -u "$PROJECT_DIR/tools/stt_server.py" \
    >"$SERVER_LOG" 2>&1 &
  echo "$!" >"$SERVER_PID"

  # First model load can take longer, so allow up to two minutes.
  for _ in {1..120}; do
    if curl -fsS --max-time 2 "$WEB_URL/robot/status" >/dev/null 2>&1; then
      echo "[Server] Ready."
      return
    fi
    if [[ -f "$SERVER_PID" ]] && ! kill -0 "$(cat "$SERVER_PID")" 2>/dev/null; then
      tail -30 "$SERVER_LOG"
      fail "The STT server stopped during startup."
    fi
    sleep 1
  done
  tail -30 "$SERVER_LOG"
  fail "Timed out waiting for the STT server."
}

echo "=================================================="
echo " Waveshare ESP32-S3 Voice Assistant"
echo "=================================================="

[[ -f "$PROJECT_DIR/include/wifi_credentials.h" ]] \
  || fail "Missing include/wifi_credentials.h. Copy the example and enter the 2.4GHz Wi-Fi details."
if [[ ! -f "$PROJECT_DIR/include/server_config.h" ]]; then
  cp "$PROJECT_DIR/include/server_config.example.h" "$PROJECT_DIR/include/server_config.h" \
    || fail "Could not create include/server_config.h."
fi

BOARD_PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -n 1 || true)"
[[ -n "$BOARD_PORT" ]] || fail "ESP32-S3 was not found. Connect its USB-C data cable and try again."
echo "[Board] Found: $BOARD_PORT"

MAC_IP="$(current_lan_ip)"
[[ -n "$MAC_IP" ]] || fail "Could not determine the Mac LAN IP. Connect the Mac to Wi-Fi and try again."
echo "[Network] Mac LAN IP: $MAC_IP"
update_server_ip "$MAC_IP" || fail "Could not update the firmware server address."

PIO_BIN="$(find_pio)" || fail "PlatformIO CLI was not found. Install the PlatformIO VS Code extension first."
echo "[Board] Building and uploading the latest firmware..."
"$PIO_BIN" run -e esp32s3_audio -t upload --upload-port "$BOARD_PORT" \
  || fail "Firmware upload failed. Close any serial monitor and try again."
echo "[Board] Firmware ready."

ensure_python_deps
ensure_ollama
ensure_supertonic
ensure_qwen_stt
start_stt_server

echo "[Web] Opening $WEB_URL"
open "$WEB_URL"

echo
echo "READY: Hold the LCD, speak, then lift your finger."
echo "The server continues running after this window closes."
echo "Server log: $SERVER_LOG"
echo
printf 'Press Return to close this launcher window...'
read -r _
