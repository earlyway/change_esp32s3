#!/bin/bash

# Generate macOS say and Supertonic 3 comparison files without changing
# the robot's active TTS backend.

set -u

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENV_DIR="$PROJECT_DIR/.venv-supertonic"
OUTPUT_DIR="$PROJECT_DIR/.robot-runtime/tts-ab/$(date +%Y%m%d-%H%M%S)"

fail() {
  printf '\n[ERROR] %s\n' "$1"
  printf 'Press Return to close this window...'
  read -r _
  exit 1
}

cd "$PROJECT_DIR"
mkdir -p "$OUTPUT_DIR"

if command -v python3.11 >/dev/null 2>&1; then
  BASE_PYTHON="$(command -v python3.11)"
elif command -v python3.12 >/dev/null 2>&1; then
  BASE_PYTHON="$(command -v python3.12)"
else
  fail "Python 3.11 or 3.12 is required for the isolated Supertonic test."
fi

if [[ ! -x "$VENV_DIR/bin/python" ]]; then
  echo "[Setup] Creating isolated Supertonic environment..."
  "$BASE_PYTHON" -m venv "$VENV_DIR" || fail "Could not create the Python environment."
fi

if ! "$VENV_DIR/bin/python" -c \
  "from importlib.metadata import version; raise SystemExit(version('supertonic') != '1.3.1')" \
  >/dev/null 2>&1; then
  echo "[Setup] Installing Supertonic..."
  "$VENV_DIR/bin/python" -m pip install -r "$PROJECT_DIR/tools/requirements-supertonic.txt" \
    || fail "Could not install Supertonic."
fi

echo "[A/B] Generating comparison audio. The first model download is about 400 MB."
"$VENV_DIR/bin/python" "$PROJECT_DIR/tools/tts_ab_test.py" \
  --output-dir "$OUTPUT_DIR" \
  || fail "A/B audio generation failed."

open "$OUTPUT_DIR/index.html"
echo
echo "READY: The comparison page has opened."
echo "Results: $OUTPUT_DIR"
printf 'Press Return to close this window...'
read -r _
