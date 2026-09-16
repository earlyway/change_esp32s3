#!/usr/bin/env python3
"""Push 16 kHz / 16-bit / mono PCM to the STT server to play on the Mac speaker.

Usage:
    python3 tools/send_pcm.py                  # 1 s 440 Hz tone
    python3 tools/send_pcm.py path/to/file.wav # WAV or raw PCM
    python3 tools/send_pcm.py --emotion positive file.wav

The server must already be running (python3 tools/stt_server.py). Useful to
check Mac playback and the ESP32 face sync (X-Robot-State / X-Emotion).
"""

from __future__ import annotations

import argparse
import math
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 16000


def tone_pcm(freq_hz: float = 440.0, seconds: float = 1.0, amplitude: int = 12000) -> bytes:
    n = int(SAMPLE_RATE * seconds)
    buf = bytearray()
    for i in range(n):
        s = int(amplitude * math.sin(2 * math.pi * freq_hz * i / SAMPLE_RATE))
        buf += struct.pack("<h", s)
    return bytes(buf)


def load_pcm(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) >= 12 and data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        return data  # server unwraps WAV
    if len(data) % 2 == 1:
        data = data[:-1]
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", nargs="?", help="WAV or raw 16-bit little-endian PCM. Omit for a 440 Hz beep.")
    parser.add_argument("--host", default="127.0.0.1", help="Mac server host (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=3000)
    parser.add_argument("--emotion", default="none", help="X-Emotion header for later face mapping")
    args = parser.parse_args()

    if args.file:
        body = load_pcm(Path(args.file))
        label = args.file
    else:
        body = tone_pcm()
        label = "generated 440 Hz 1s"

    url = f"http://{args.host}:{args.port}/speaker/push"
    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "X-Emotion": args.emotion,
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            print(f"pushed {label}: {resp.status} {resp.read().decode()}")
    except urllib.error.URLError as exc:
        print(f"failed to POST {url}: {exc}", file=sys.stderr)
        print("Is tools/stt_server.py running?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
