#!/usr/bin/env python3
"""macOS TTS → 44.1 kHz / 16-bit / mono PCM for Mac speaker playback.

Uses the built-in `say` command and `afconvert`. No extra pip packages.
Override the voice with TTS_VOICE (default: first ko_KR voice, usually Yuna).
Override speaking rate with TTS_RATE (default: 180 words/min).
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

SAMPLE_RATE = 44100


class TtsError(RuntimeError):
    pass


def default_korean_voice() -> str:
    env = (os.environ.get("TTS_VOICE") or "").strip()
    if env:
        return env
    try:
        listing = subprocess.check_output(["say", "-v", "?"], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise TtsError(f"say is not available: {exc}") from exc
    first = ""
    for line in listing.splitlines():
        if "ko_KR" not in line:
            continue
        name = line.split()[0]
        if name.lower() == "yuna":
            return name
        if not first:
            first = name
    return first or "Yuna"


def synthesize_pcm(text: str, voice: str | None = None, rate: int | None = None) -> bytes:
    """Return headerless little-endian int16 mono PCM at 44.1 kHz."""
    cleaned = " ".join((text or "").split())
    if not cleaned:
        raise TtsError("empty text")
    if len(cleaned) > 500:
        raise TtsError("text too long (max 500 characters)")

    voice = voice or default_korean_voice()
    rate = int(os.environ.get("TTS_RATE", "180")) if rate is None else rate

    with tempfile.TemporaryDirectory(prefix="esp32_tts_") as tmp:
        tmp_path = Path(tmp)
        aiff_path = tmp_path / "speech.aiff"
        wav_path = tmp_path / "speech.wav"
        try:
            subprocess.run(
                ["say", "-v", voice, "-r", str(rate), "-o", str(aiff_path), cleaned],
                check=True,
                capture_output=True,
                text=True,
            )
        except FileNotFoundError as exc:
            raise TtsError("say command not found (macOS only)") from exc
        except subprocess.CalledProcessError as exc:
            err = (exc.stderr or exc.stdout or "").strip()
            raise TtsError(f"say failed: {err or exc}") from exc

        try:
            subprocess.run(
                [
                    "afconvert",
                    "-f", "WAVE",
                    "-d", f"LEI16@{SAMPLE_RATE}",
                    "-c", "1",
                    str(aiff_path),
                    str(wav_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
        except FileNotFoundError as exc:
            raise TtsError("afconvert not found (macOS only)") from exc
        except subprocess.CalledProcessError as exc:
            err = (exc.stderr or exc.stdout or "").strip()
            raise TtsError(f"afconvert failed: {err or exc}") from exc

        wav = wav_path.read_bytes()

    pcm = _wav_to_pcm(wav)
    if len(pcm) < 2:
        raise TtsError("TTS produced no audio")
    return pcm


def _wav_to_pcm(body: bytes) -> bytes:
    pos = 12
    while pos + 8 <= len(body):
        chunk_id = body[pos:pos + 4]
        chunk_len = int.from_bytes(body[pos + 4:pos + 8], "little")
        pos += 8
        payload = body[pos:pos + chunk_len]
        pos += chunk_len
        if chunk_len % 2 == 1:
            pos += 1
        if chunk_id == b"data":
            if len(payload) % 2 == 1:
                payload = payload[:-1]
            return payload
    return b""
