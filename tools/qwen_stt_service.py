#!/usr/bin/env python3
"""Persistent local Qwen3-ASR service for the robot's final PTT utterances."""

from __future__ import annotations

import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME_DIR = Path(
    os.environ.get("QWEN_STT_RUNTIME_DIR", HERE.parent / ".robot-runtime" / "qwen-stt")
).resolve()
RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
os.chdir(RUNTIME_DIR)

import numpy as np
from mlx_qwen3_asr import Session


HOST = os.environ.get("QWEN_STT_HOST", "127.0.0.1")
PORT = int(os.environ.get("QWEN_STT_PORT", "3002"))
MODEL = os.environ.get("QWEN_STT_MODEL", "Qwen/Qwen3-ASR-0.6B")
SAMPLE_RATE = 16000
MAX_AUDIO_BYTES = int(os.environ.get("QWEN_STT_MAX_BYTES", str(SAMPLE_RATE * 2 * 60)))

print(f"[Qwen STT] loading {MODEL}...", flush=True)
load_started = time.perf_counter()
SESSION = Session(model=MODEL)
INFERENCE_LOCK = threading.Lock()
print(
    f"[Qwen STT] ready model={MODEL} load={time.perf_counter() - load_started:.2f}s",
    flush=True,
)


class Handler(BaseHTTPRequestHandler):
    server_version = "QwenRobotSTT/1.0"

    def do_GET(self) -> None:
        if self.path == "/health":
            self._json(
                200,
                {
                    "ok": True,
                    "service": "qwen-robot-stt",
                    "model": MODEL,
                },
            )
            return
        self._json(404, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:
        if self.path != "/transcribe":
            self._json(404, {"ok": False, "error": "not_found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._json(400, {"ok": False, "error": "invalid_content_length"})
            return
        if length < 2:
            self._json(400, {"ok": False, "error": "empty_pcm"})
            return
        if length > MAX_AUDIO_BYTES:
            self._json(413, {"ok": False, "error": "audio_too_long"})
            return

        pcm = self.rfile.read(length)
        if len(pcm) % 2:
            pcm = pcm[:-1]
        audio = np.frombuffer(pcm, dtype="<i2")
        started = time.perf_counter()
        try:
            with INFERENCE_LOCK:
                result = SESSION.transcribe((audio, SAMPLE_RATE), language="Korean")
            elapsed = time.perf_counter() - started
            text = str(result.text or "").strip()
            print(
                f"[Qwen STT] transcribed audio={len(audio) / 16000:.2f}s "
                f"elapsed={elapsed:.3f}s text={text!r}",
                flush=True,
            )
            self._json(
                200,
                {
                    "ok": True,
                    "model": MODEL,
                    "text": text,
                    "seconds": round(elapsed, 3),
                },
            )
        except Exception as exc:
            print(f"[Qwen STT] failed: {exc!r}", flush=True)
            self._json(
                500,
                {"ok": False, "error": f"{type(exc).__name__}: {exc}"},
            )

    def log_message(self, format: str, *args: object) -> None:
        print(f"[Qwen STT HTTP] {format % args}", flush=True)

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"[Qwen STT] listening on http://{HOST}:{PORT}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
