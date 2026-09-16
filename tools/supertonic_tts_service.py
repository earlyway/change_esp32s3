#!/usr/bin/env python3
"""Persistent local Supertonic 3 service returning Mac-ready 44.1 kHz PCM."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib.metadata import version
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME_DIR = Path(
    os.environ.get("SUPERTONIC_RUNTIME_DIR", HERE.parent / ".robot-runtime" / "supertonic")
).resolve()
RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
# Keep any ONNX Runtime telemetry/session artifact out of the repository.
os.chdir(RUNTIME_DIR)

from supertonic import TTS  # noqa: E402

HOST = os.environ.get("SUPERTONIC_HOST", "127.0.0.1")
PORT = int(os.environ.get("SUPERTONIC_PORT", "3001"))
VOICE = os.environ.get("SUPERTONIC_VOICE", "F1")
STEPS = int(os.environ.get("SUPERTONIC_STEPS", "32"))
SPEED = float(os.environ.get("SUPERTONIC_SPEED", "1.0"))
MAX_TEXT_LENGTH = 500
# Mac speaker playback; no longer downsampled for the ESP32 speaker.
TARGET_SAMPLE_RATE = int(os.environ.get("SUPERTONIC_SAMPLE_RATE", "44100"))


def wav_to_pcm(body: bytes) -> bytes:
    pos = 12
    while pos + 8 <= len(body):
        chunk_id = body[pos : pos + 4]
        chunk_len = int.from_bytes(body[pos + 4 : pos + 8], "little")
        pos += 8
        payload = body[pos : pos + chunk_len]
        pos += chunk_len + (chunk_len % 2)
        if chunk_id == b"data":
            return payload[: len(payload) - (len(payload) % 2)]
    return b""


class Engine:
    def __init__(self) -> None:
        started = time.perf_counter()
        self.tts = TTS(model="supertonic-3", auto_download=True)
        self.style = self.tts.get_voice_style(VOICE)
        self.lock = threading.Lock()
        self.load_seconds = time.perf_counter() - started
        print(
            f"[Supertonic] ready version={version('supertonic')} voice={VOICE} "
            f"steps={STEPS} native_rate={self.tts.sample_rate} "
            f"output_rate={TARGET_SAMPLE_RATE} load={self.load_seconds:.2f}s",
            flush=True,
        )

    def synthesize_pcm(self, text: str) -> tuple[bytes, float]:
        started = time.perf_counter()
        with self.lock:
            wav, _ = self.tts.synthesize(
                text,
                voice_style=self.style,
                lang="ko",
                total_steps=STEPS,
                speed=SPEED,
                silence_duration=0.2,
            )
            with tempfile.TemporaryDirectory(
                prefix="synthesis-", dir=RUNTIME_DIR
            ) as temporary:
                native_path = Path(temporary) / "native.wav"
                converted_path = Path(temporary) / "robot.wav"
                self.tts.save_audio(wav, str(native_path))
                subprocess.run(
                    [
                        "afconvert",
                        "-f",
                        "WAVE",
                        "-d",
                        f"LEI16@{TARGET_SAMPLE_RATE}",
                        "-c",
                        "1",
                        str(native_path),
                        str(converted_path),
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                pcm = wav_to_pcm(converted_path.read_bytes())

        if not pcm:
            raise RuntimeError("Supertonic produced no PCM audio")
        return pcm, time.perf_counter() - started


ENGINE: Engine


class Handler(BaseHTTPRequestHandler):
    server_version = "SupertonicRobot/1.0"

    def send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path != "/health":
            self.send_json(404, {"ok": False, "error": "not_found"})
            return
        self.send_json(
            200,
            {
                "ok": True,
                "service": "supertonic-robot-tts",
                "version": version("supertonic"),
                "model": "supertonic-3",
                "voice": VOICE,
                "sample_rate": TARGET_SAMPLE_RATE,
                "native_sample_rate": ENGINE.tts.sample_rate,
                "steps": STEPS,
            },
        )

    def do_POST(self) -> None:
        if self.path != "/synthesize":
            self.send_json(404, {"ok": False, "error": "not_found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 16_384:
                raise ValueError("invalid request size")
            payload = json.loads(self.rfile.read(length))
            text = " ".join(str(payload.get("text") or "").split())
            if not text:
                raise ValueError("empty text")
            if len(text) > MAX_TEXT_LENGTH:
                raise ValueError(f"text too long (max {MAX_TEXT_LENGTH} characters)")

            pcm, elapsed = ENGINE.synthesize_pcm(text)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(pcm)))
            self.send_header("X-Sample-Rate", str(TARGET_SAMPLE_RATE))
            self.send_header("X-Channels", "1")
            self.send_header("X-Bits", "16")
            self.send_header("X-Synthesis-Seconds", f"{elapsed:.3f}")
            self.end_headers()
            self.wfile.write(pcm)
            print(
                f"[Supertonic] synth seconds={elapsed:.2f} bytes={len(pcm)} "
                f"text={text!r}",
                flush=True,
            )
        except (BrokenPipeError, ConnectionResetError):
            print("[Supertonic] client disconnected during response", flush=True)
        except Exception as exc:
            print(f"[Supertonic] failed: {exc}", flush=True)
            try:
                self.send_json(500, {"ok": False, "error": str(exc)})
            except (BrokenPipeError, ConnectionResetError):
                pass

    def log_message(self, format: str, *args: object) -> None:
        if self.path != "/health":
            super().log_message(format, *args)


def main() -> None:
    global ENGINE
    print("[Supertonic] loading model...", flush=True)
    ENGINE = Engine()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"[Supertonic] listening on http://{HOST}:{PORT}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
