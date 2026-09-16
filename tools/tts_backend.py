#!/usr/bin/env python3
"""Selectable robot TTS backend with a safe macOS `say` fallback."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from dataclasses import dataclass

from mac_tts import (
    SAMPLE_RATE as MACOS_SAMPLE_RATE,
    TtsError,
    default_korean_voice,
    synthesize_pcm as synthesize_macos_pcm,
)


@dataclass
class TtsClip:
    pcm: bytes
    sample_rate: int

    @property
    def duration_s(self) -> float:
        if self.sample_rate <= 0:
            return 0.0
        return len(self.pcm) / (self.sample_rate * 2)

TTS_BACKEND = (os.environ.get("TTS_BACKEND") or "supertonic").strip().lower()
SUPERTONIC_URL = (
    os.environ.get("SUPERTONIC_URL") or "http://127.0.0.1:3001"
).rstrip("/")
SUPERTONIC_TIMEOUT_S = float(os.environ.get("SUPERTONIC_TIMEOUT_S", "30"))


def backend_name() -> str:
    if TTS_BACKEND == "supertonic":
        return "supertonic-3/F1 (macOS say fallback)"
    if TTS_BACKEND == "macos":
        return f"macOS say/{default_korean_voice()}"
    return f"invalid:{TTS_BACKEND}"


def supertonic_health(timeout: float = 2.0) -> dict:
    request = urllib.request.Request(f"{SUPERTONIC_URL}/health", method="GET")
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = json.loads(response.read())
    except (OSError, ValueError, urllib.error.URLError) as exc:
        raise TtsError(f"Supertonic service unavailable: {exc}") from exc
    if not payload.get("ok") or payload.get("service") != "supertonic-robot-tts":
        raise TtsError("Unexpected Supertonic health response")
    return payload


def synthesize_pcm(text: str) -> bytes:
    """Backward-compatible helper: PCM bytes only."""
    return synthesize(text).pcm


def synthesize(text: str) -> TtsClip:
    if TTS_BACKEND == "macos":
        pcm = synthesize_macos_pcm(text)
        return TtsClip(pcm=pcm, sample_rate=MACOS_SAMPLE_RATE)
    if TTS_BACKEND != "supertonic":
        raise TtsError(f"Unsupported TTS_BACKEND: {TTS_BACKEND}")

    try:
        return synthesize_supertonic(text)
    except TtsError as exc:
        print(f"[TTS] Supertonic failed; using macOS say fallback: {exc}", flush=True)
        pcm = synthesize_macos_pcm(text)
        return TtsClip(pcm=pcm, sample_rate=MACOS_SAMPLE_RATE)


def synthesize_supertonic(text: str) -> TtsClip:
    body = json.dumps({"text": text}, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        f"{SUPERTONIC_URL}/synthesize",
        data=body,
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(
            request, timeout=SUPERTONIC_TIMEOUT_S
        ) as response:
            pcm = response.read()
            elapsed = response.headers.get("X-Synthesis-Seconds", "?")
            rate_header = response.headers.get("X-Sample-Rate", "44100")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise TtsError(f"Supertonic HTTP {exc.code}: {detail}") from exc
    except (OSError, urllib.error.URLError) as exc:
        raise TtsError(f"Supertonic request failed: {exc}") from exc

    if len(pcm) < 2:
        raise TtsError("Supertonic returned empty audio")
    if len(pcm) % 2:
        pcm = pcm[:-1]
    try:
        sample_rate = int(rate_header)
    except (TypeError, ValueError):
        sample_rate = 44100
    if sample_rate < 8000:
        sample_rate = 44100
    print(
        f"[TTS] Supertonic ready seconds={elapsed} bytes={len(pcm)} rate={sample_rate}",
        flush=True,
    )
    return TtsClip(pcm=pcm, sample_rate=sample_rate)
