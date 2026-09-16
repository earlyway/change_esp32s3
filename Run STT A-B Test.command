#!/bin/bash

# Run an isolated Qwen3-ASR 0.6B vs Moonshine Korean Tiny comparison server.
# The normal robot server must be stopped because the firmware uploads to 3000.

set -u

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$PROJECT_DIR/.venv-stt-ab"
RUNTIME_DIR="$PROJECT_DIR/.robot-runtime/stt-ab"
SERVER_LOG="$RUNTIME_DIR/server.log"
SERVER_PID_FILE="$RUNTIME_DIR/server.pid"
WEB_URL="http://localhost:3000"
SERVER_PID=""

fail() {
  printf '\n[ERROR] %s\n' "$1"
  printf 'Press Return to close this window...'
  read -r _
  exit 1
}

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -f "$SERVER_PID_FILE"
}
trap cleanup EXIT INT TERM

cd "$PROJECT_DIR"
mkdir -p "$RUNTIME_DIR"

if curl -fsS --max-time 2 "$WEB_URL/health" 2>/dev/null \
  | python3 -c 'import json,sys; raise SystemExit(json.load(sys.stdin).get("service") != "robot-stt-ab")' \
  >/dev/null 2>&1; then
  open "$WEB_URL"
  fail "The STT A/B server is already running. Its existing page was opened."
fi

if lsof -tiTCP:3000 -sTCP:LISTEN >/dev/null 2>&1; then
  fail "Port 3000 is in use. Double-click Stop Robot.command first, then run this file again."
fi

if command -v python3.11 >/dev/null 2>&1; then
  BASE_PYTHON="$(command -v python3.11)"
elif command -v python3.12 >/dev/null 2>&1; then
  BASE_PYTHON="$(command -v python3.12)"
else
  fail "Python 3.11 or 3.12 is required for the isolated STT comparison."
fi

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  echo "[Setup] Creating isolated STT A/B environment..."
  "$BASE_PYTHON" -m venv "$VENV_DIR" \
    || fail "Could not create the STT A/B Python environment."
fi

if ! "$VENV_DIR/bin/python" - <<'PY' >/dev/null 2>&1
from importlib.metadata import version
for package in ("fastapi", "uvicorn", "numpy"):
    version(package)
raise SystemExit(
    version("mlx-qwen3-asr") != "0.4.0"
    or version("moonshine-voice") != "0.1.5"
)
PY
then
  echo "[Setup] Installing Qwen3-ASR and Moonshine..."
  "$VENV_DIR/bin/python" -m pip install \
    -r "$PROJECT_DIR/tools/requirements-stt-ab.txt" \
    || fail "Could not install the STT comparison dependencies."
fi

echo "[Server] Starting the robot microphone comparison server..."
: >"$SERVER_LOG"
env STT_AB_RUNTIME_DIR="$RUNTIME_DIR" \
  "$VENV_DIR/bin/python" -u "$PROJECT_DIR/tools/stt_ab_server.py" \
  >"$SERVER_LOG" 2>&1 &
SERVER_PID="$!"
echo "$SERVER_PID" >"$SERVER_PID_FILE"

for _ in {1..30}; do
  if curl -fsS --max-time 2 "$WEB_URL/health" >/dev/null 2>&1; then
    open "$WEB_URL"
    echo
    echo "READY: The comparison page has opened."
    echo "The first Qwen model download is large and may take several minutes."
    echo "Hold the LCD while speaking, then lift your finger to compare both results."
    echo
    printf 'Press Return here when testing is finished to stop the A/B server...'
    read -r _
    exit 0
  fi
  if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG"
    fail "The STT A/B server stopped during startup."
  fi
  sleep 1
done

cat "$SERVER_LOG"
fail "Timed out waiting for the STT A/B server."
