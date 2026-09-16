#!/usr/bin/env python3
"""Selectable robot TTS backend with a safe macOS `say` fallback."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.request

from mac_tts import TtsError, default_korean_voice, synthesize_pcm as synthesize_macos_pcm

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
    if TTS_BACKEND == "macos":
        return synthesize_macos_pcm(text)
    if TTS_BACKEND != "supertonic":
        raise TtsError(f"Unsupported TTS_BACKEND: {TTS_BACKEND}")

    try:
        return synthesize_supertonic_pcm(text)
    except TtsError as exc:
        print(f"[TTS] Supertonic failed; using macOS say fallback: {exc}", flush=True)
        return synthesize_macos_pcm(text)


def synthesize_supertonic_pcm(text: str) -> bytes:
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
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise TtsError(f"Supertonic HTTP {exc.code}: {detail}") from exc
    except (OSError, urllib.error.URLError) as exc:
        raise TtsError(f"Supertonic request failed: {exc}") from exc

    if len(pcm) < 2:
        raise TtsError("Supertonic returned empty audio")
    if len(pcm) % 2:
        pcm = pcm[:-1]
    print(
        f"[TTS] Supertonic ready seconds={elapsed} bytes={len(pcm)}",
        flush=True,
    )
    return pcm
