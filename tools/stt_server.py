#!/usr/bin/env python3
"""Real-time STT server for the ESP32 audio streamer.

Pipeline:
    ESP32 mic --POST /upload (1s PCM)--> this server --> rolling buffer
        --> Qwen3-ASR 0.6B sidecar (final PTT text) with faster-whisper
            live preview + fallback --> WebSocket /ws
        --> local web page text input (tools/web/index.html)

    Mac --POST /speaker/say (text)--> selectable TTS --> Mac speaker (afplay)
        ESP32 --GET /speaker/pull--> X-Robot-State / X-Emotion only (face sync)

    STT utterance end (or POST /robot/reply) --> LLM (Ollama qwen2.5:14b
        or OpenAI gpt-4o-mini) --> TTS --> Mac speaker

The board's speaker wiring is gone, so all TTS audio plays on the Mac. The
ESP32 keeps polling /speaker/pull, but only to mirror robot state on the LCD.

Run:
    pip install -r tools/requirements.txt
    python tools/stt_server.py
    # then open http://localhost:3000

Environment variables:
    PORT          (default 3000)
    STT_BACKEND   qwen (default) or faster-whisper
    QWEN_STT_URL  local Qwen sidecar URL (default http://127.0.0.1:3002)
    STT_MODEL     faster-whisper model size (default "base")
    STT_DEVICE    "auto" | "cpu" | "cuda"   (default "auto")
    STT_COMPUTE   "auto" | "int8" | "float16" ... (default "auto")
    STT_LANGUAGE  force a language code (e.g. "ko"); empty = auto-detect
    STT_PROMPT    optional initial_prompt to bias domain vocabulary
    SAVE_RAW      "1" to also save received chunks to tools/uploads/*.raw
    SILENCE_RMS_DBFS  skip chunks quieter than this RMS dBFS (default -46)
    END_SILENCE_S unused leftover (PTT ends via /utterance/end, not silence)
    SKIP_STT      "1" to skip loading faster-whisper (speaker endpoints still work)
    TTS_VOLUME    Mac playback gain for afplay, 0.0-1.0 (default 1.0)
    TTS_BACKEND   supertonic (default) or macos
    SUPERTONIC_URL local sidecar URL (default http://127.0.0.1:3001)
    TTS_VOICE     fallback macOS `say` voice (default: first ko_KR voice)
    TTS_RATE      fallback speaking rate for `say` (default 180)
    LLM_BACKEND   ollama (default) or openai
    OLLAMA_MODEL  default qwen2.5:14b
    OPENAI_MODEL  default gpt-4o-mini (requires OPENAI_API_KEY)
    ROBOT_AUTO_REPLY  "1" (default) to LLM+TTS after each STT utterance end
    Optional file tools/llm.env (gitignored) sets the same keys if unset.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import warnings
import wave
from contextlib import asynccontextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional, Set

# Python 3.14 deprecates asyncio.iscoroutinefunction; fastapi/starlette still
# call it. Harmless for us: hide it so the startup log stays readable.
warnings.filterwarnings(
    "ignore",
    message=r".*asyncio\.iscoroutinefunction.*",
    category=DeprecationWarning,
)

import numpy as np
from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse, Response

from stt_processor import StreamingTranscriber, TranscriptState
from llm_backend import LlmError, backend_name, complete, model_name
from tts_backend import TtsError, backend_name as tts_backend_name, synthesize_pcm

HERE = Path(__file__).resolve().parent
WEB_DIR = HERE / "web"
UPLOAD_DIR = HERE / "uploads"

PORT = int(os.environ.get("PORT", "3000"))
MODEL_SIZE = os.environ.get("STT_MODEL", "base")
DEVICE = os.environ.get("STT_DEVICE", "auto")
COMPUTE_TYPE = os.environ.get("STT_COMPUTE", "auto")
LANGUAGE = os.environ.get("STT_LANGUAGE") or None
INITIAL_PROMPT = os.environ.get("STT_PROMPT") or None
STT_BACKEND = os.environ.get("STT_BACKEND", "qwen").strip().lower()
QWEN_STT_URL = os.environ.get("QWEN_STT_URL", "http://127.0.0.1:3002").rstrip("/")
MAX_UTTERANCE_BYTES = 16000 * 2 * 60
SAVE_RAW = os.environ.get("SAVE_RAW", "0") == "1"
SILENCE_RMS_DBFS = float(os.environ.get("SILENCE_RMS_DBFS", "-46"))
END_SILENCE_S = float(os.environ.get("END_SILENCE_S", "1.5"))
# Fallback: if the ESP32 stops uploading (PTT released, WiFi drop) and no end
# marker arrives, close the utterance after this many seconds without uploads.
UPLOAD_IDLE_END_S = float(os.environ.get("UPLOAD_IDLE_END_S", "3.0"))
AUDIO_CHUNK_S = 1.0
SKIP_STT = os.environ.get("SKIP_STT", "0") == "1"
ROBOT_AUTO_REPLY = os.environ.get("ROBOT_AUTO_REPLY", "1") == "1"
SPEAKER_BYTES_PER_SECOND = 32000  # 16 kHz / 16-bit / mono PCM
# TTS plays on the Mac speaker (the board's speaker wiring is gone). 0.0–1.0
# maps to `afplay -v`; values above 1.0 amplify.
TTS_VOLUME = float(os.environ.get("TTS_VOLUME", "1.0"))
# PTT release with no usable transcript still gets a short spoken retry prompt.
EMPTY_UTTERANCE_REPLY = "잘 못 들었어. 다시 말해 줄래?"


class MacPlayer:
    """Plays 16 kHz mono PCM through the Mac speaker with `afplay`.

    One utterance at a time: starting a new one stops the previous. The ESP32
    still polls /speaker/pull, but only for X-Robot-State / X-Emotion so the
    face can follow the reply; no audio goes back to the board.
    """

    def __init__(self) -> None:
        self.proc: Optional[subprocess.Popen] = None
        self.path: Optional[str] = None
        self.emotion = "none"
        self.started_mono = 0.0
        self.duration_s = 0.0
        self.play_count = 0
        self.afplay = shutil.which("afplay")

    # afplay lingers ~1 s after the last sample (CoreAudio teardown). Treat the
    # utterance as done once the audio itself must have ended so the face
    # returns to idle on time.
    END_GRACE_S = 0.5

    def playing(self) -> bool:
        if self.proc is None:
            return False
        if self.proc.poll() is not None:
            self._cleanup()
            return False
        if time.monotonic() - self.started_mono > self.duration_s + self.END_GRACE_S:
            self.stop()
            return False
        return True

    def remaining_ms(self) -> int:
        if not self.playing():
            return 0
        left = self.duration_s - (time.monotonic() - self.started_mono)
        return max(0, int(left * 1000))

    def play(self, pcm: bytes, emotion: str) -> dict:
        if self.afplay is None:
            raise RuntimeError("afplay_not_found (macOS required for Mac speaker output)")
        self.stop()
        fd, path = tempfile.mkstemp(prefix="mallow_tts_", suffix=".wav")
        os.close(fd)
        with wave.open(path, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(pcm)
        self.proc = subprocess.Popen(
            [self.afplay, "-v", f"{TTS_VOLUME:.2f}", path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.path = path
        self.emotion = emotion
        self.started_mono = time.monotonic()
        self.duration_s = len(pcm) / SPEAKER_BYTES_PER_SECOND
        self.play_count += 1
        return {"seconds": round(self.duration_s, 2), "emotion": emotion}

    def stop(self) -> bool:
        was_playing = self.proc is not None and self.proc.poll() is None
        if was_playing:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self._cleanup()
        return was_playing

    def _cleanup(self) -> None:
        self.proc = None
        if self.path:
            try:
                os.remove(self.path)
            except OSError:
                pass
        self.path = None

# `app` is created after the lifespan handlers below (see _lifespan).


class Hub:
    """Holds the transcriber, the audio queue, and connected websockets."""

    def __init__(self) -> None:
        self.transcriber: Optional[StreamingTranscriber] = None
        self.queue: Optional["asyncio.Queue[bytes]"] = None
        self.clients: Set[WebSocket] = set()
        self.last_state = TranscriptState(committed="", partial="")
        self.transcript_history = ""
        self.utterance_pcm = bytearray()
        self.worker_task: Optional[asyncio.Task] = None
        self.upload_count = 0
        self.silence_count = 0
        self.in_utterance = False
        # Bumped on utterance end / full reset; stale in-flight STT results are dropped.
        self.utterance_generation = 0
        # TTS output: Mac speaker. GET /speaker/pull only reports state now.
        self.player = MacPlayer()
        self.speaker_push_count = 0
        self.speaker_pull_count = 0
        self.last_robot: dict = {}
        # Phase 5: thinking until LLM/TTS finishes enqueue (or fails).
        self.robot_busy = False
        # Bumped by /speaker/flush (barge-in): an in-flight LLM/TTS reply whose
        # generation no longer matches is dropped instead of being queued.
        self.reply_generation = 0
        # PTT release handling: explicit end marker from the ESP32, plus a
        # fallback timer for when uploads simply stop arriving.
        self.last_upload_mono = 0.0
        self.idle_task: Optional[asyncio.Task] = None
        # Guards transcriber use between the worker and utterance finalization.
        self.stt_lock = asyncio.Lock()
        # Serializes LLM+TTS so two PTT turns cannot interleave speaker chunks.
        self.reply_lock = asyncio.Lock()
        # Debounces empty-PTT fallback so a retried /utterance/end after a real
        # turn does not speak the canned prompt on top of the actual reply.
        self.last_utterance_end_mono = 0.0

    async def broadcast(self, state: TranscriptState) -> None:
        self.last_state = state
        payload = {"committed": state.committed, "partial": state.partial}
        if self.last_robot:
            payload.update(self.last_robot)
        dead = []
        for ws in self.clients:
            try:
                await ws.send_json(payload)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)


hub = Hub()


async def on_startup() -> None:
    if STT_BACKEND not in ("qwen", "faster-whisper"):
        raise RuntimeError(
            f"Unsupported STT_BACKEND={STT_BACKEND!r}; use qwen or faster-whisper"
        )
    if SAVE_RAW:
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)

    # Create asyncio objects after uvicorn has created the running loop.
    hub.queue = asyncio.Queue()
    if hub.player.afplay is None:
        print("[Speaker] WARNING: afplay not found; TTS playback disabled", flush=True)

    if SKIP_STT:
        print("[STT] SKIP_STT=1: not loading faster-whisper. /upload will 503; /speaker/* works.")
    else:
        role = "fallback" if STT_BACKEND == "qwen" else "primary"
        print(f"[STT] Loading faster-whisper {role} model '{MODEL_SIZE}' "
              f"(device={DEVICE}, compute={COMPUTE_TYPE})... this can take a while")
        loop = asyncio.get_running_loop()
        hub.transcriber = await loop.run_in_executor(
            None,
            lambda: StreamingTranscriber(
                model_size=MODEL_SIZE,
                device=DEVICE,
                compute_type=COMPUTE_TYPE,
                language=LANGUAGE,
                initial_prompt=INITIAL_PROMPT,
            ),
        )
        print(f"[STT] backend={STT_BACKEND} faster-whisper {role}=ready")
        if STT_BACKEND == "qwen":
            print(f"[STT] Qwen sidecar={QWEN_STT_URL} (final PTT text; Whisper fallback)")
        hub.worker_task = asyncio.create_task(_worker())
        hub.worker_task.add_done_callback(_worker_done)
        hub.idle_task = asyncio.create_task(_upload_idle_watch())
    print(f"[STT] Listening on http://0.0.0.0:{PORT}  (open http://localhost:{PORT})")
    print(f"[Speaker] Mac speaker output (afplay, volume={TTS_VOLUME:.2f}): "
          "POST /speaker/say  POST /speaker/push  POST /speaker/flush  GET /speaker/pull (state only)")
    print(f"[STT] PTT end marker: POST /utterance/end  (fallback: no uploads for {UPLOAD_IDLE_END_S:.0f}s)")
    print(f"[LLM] backend={backend_name()} model={model_name()} auto_reply={int(ROBOT_AUTO_REPLY)}")
    try:
        print(f"[TTS] backend={tts_backend_name()}")
    except TtsError as exc:
        print(f"[TTS] configuration error: {exc}")


async def on_shutdown() -> None:
    if hub.worker_task:
        hub.worker_task.cancel()
    if hub.idle_task:
        hub.idle_task.cancel()
    hub.player.stop()


@asynccontextmanager
async def _lifespan(_: FastAPI):
    await on_startup()
    try:
        yield
    finally:
        await on_shutdown()


app = FastAPI(title="ESP32 Realtime STT", lifespan=_lifespan)


def _worker_done(task: asyncio.Task) -> None:
    if task.cancelled():
        return
    exc = task.exception()
    if exc is not None:
        print(f"[STT] worker crashed: {exc!r}", flush=True)


def _bump_utterance_generation() -> int:
    hub.utterance_generation += 1
    return hub.utterance_generation


def _drain_stt_queue() -> int:
    """Move every queued PCM chunk into the transcriber buffer. Caller holds stt_lock."""
    assert hub.queue is not None and hub.transcriber is not None
    drained = 0
    while not hub.queue.empty():
        try:
            hub.transcriber.accept_pcm(hub.queue.get_nowait())
            drained += 1
        except asyncio.QueueEmpty:
            break
    return drained


def _append_utterance_pcm(pcm: bytes) -> None:
    """Keep the current PTT take for the Qwen sidecar, capped at 60 seconds."""
    hub.utterance_pcm.extend(pcm)
    overflow = len(hub.utterance_pcm) - MAX_UTTERANCE_BYTES
    if overflow > 0:
        del hub.utterance_pcm[:overflow]


def _join_transcript(prefix: str, turn: str) -> str:
    prefix = prefix.strip()
    turn = turn.strip()
    if not prefix:
        return turn
    if not turn:
        return prefix
    return f"{prefix} {turn}"


def _with_history(state: TranscriptState) -> TranscriptState:
    """Render a current faster-whisper turn after finalized prior turns."""
    return TranscriptState(
        committed=_join_transcript(hub.transcript_history, state.committed),
        partial=state.partial,
        words=state.words,
        window_seconds=state.window_seconds,
        committed_delta=state.committed_delta,
    )


def _finalize_utterance() -> TranscriptState:
    """Last STT pass on buffered speech, then commit any leftover partial."""
    assert hub.transcriber is not None
    t = hub.transcriber
    state = t.process()
    flush_delta = t.flush_partial()
    final = TranscriptState(
        committed=state.committed + flush_delta,
        partial="",
        words=state.words,
        window_seconds=state.window_seconds,
        committed_delta=(state.committed_delta or "") + flush_delta,
    )
    # Each PTT press is a complete turn. History is owned by Hub so the
    # fallback transcriber can safely start from a clean acoustic/text state.
    t.reset()
    return final


class QwenSttError(RuntimeError):
    pass


def _qwen_transcribe_pcm(pcm: bytes) -> tuple[str, float]:
    if not pcm:
        raise QwenSttError("empty utterance PCM")
    request = urllib.request.Request(
        f"{QWEN_STT_URL}/transcribe",
        data=pcm,
        method="POST",
        headers={"Content-Type": "application/octet-stream"},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise QwenSttError(f"Qwen service unavailable: {exc}") from exc
    if not payload.get("ok"):
        raise QwenSttError(str(payload.get("error") or "Qwen transcription failed"))
    text = str(payload.get("text") or "").strip()
    if not text:
        raise QwenSttError("Qwen returned an empty transcript")
    return text, float(payload.get("seconds") or 0.0)


async def _end_utterance(silence_seconds: float, reason: str = "silence") -> None:
    if not hub.in_utterance:
        return
    # Clear first so a concurrent caller (silence upload / end marker / idle
    # timer) cannot finalize the same utterance twice while we await STT.
    hub.in_utterance = False
    hub.silence_count = 0
    t0 = time.perf_counter()
    gen = _bump_utterance_generation()
    assert hub.transcriber is not None
    loop = asyncio.get_running_loop()
    async with hub.stt_lock:
        # Chunks still waiting in the queue (e.g. the PTT tail chunk) belong to
        # this utterance: feed them in before the final pass.
        _drain_stt_queue()
        utterance_pcm = bytes(hub.utterance_pcm)
        hub.utterance_pcm.clear()
        whisper_state = await loop.run_in_executor(None, _finalize_utterance)

    turn = whisper_state.committed.strip()
    selected_backend = "faster-whisper"
    qwen_seconds = 0.0
    if STT_BACKEND == "qwen":
        try:
            turn, qwen_seconds = await loop.run_in_executor(
                None, lambda: _qwen_transcribe_pcm(utterance_pcm)
            )
            selected_backend = "qwen"
        except QwenSttError as exc:
            print(
                f"[STT] Qwen failed; using faster-whisper fallback: {exc}",
                flush=True,
            )

    hub.transcript_history = _join_transcript(hub.transcript_history, turn)
    state = TranscriptState(
        committed=hub.transcript_history,
        partial="",
        words=whisper_state.words,
        window_seconds=len(utterance_pcm) / (16000 * 2),
        committed_delta=turn,
    )
    print(
        "[STT] end_utterance "
        f"reason={reason} silence={silence_seconds:.1f}s gen={gen} reset_audio=yes "
        f"stt_final={time.perf_counter() - t0:.2f}s "
        f"backend={selected_backend} qwen={qwen_seconds:.3f}s "
        f"flush_delta={state.committed_delta!r} "
        f"committed_len={len(state.committed)}",
        flush=True,
    )
    await hub.broadcast(state)
    hub.last_utterance_end_mono = time.monotonic()
    if ROBOT_AUTO_REPLY and turn:
        asyncio.create_task(_robot_auto_from_utterance(turn, t0))
    elif ROBOT_AUTO_REPLY and reason == "ptt_release":
        asyncio.create_task(
            _robot_auto_from_utterance(
                EMPTY_UTTERANCE_REPLY, t0, canned_reply=EMPTY_UTTERANCE_REPLY
            )
        )


async def _upload_idle_watch() -> None:
    """Fallback end-of-utterance when uploads stop without an end marker."""
    while True:
        await asyncio.sleep(0.25)
        if not hub.in_utterance or hub.transcriber is None:
            continue
        idle = time.monotonic() - hub.last_upload_mono
        if idle >= UPLOAD_IDLE_END_S:
            try:
                await _end_utterance(idle, reason="upload_idle")
            except Exception as exc:
                print(f"[STT] idle end failed: {exc!r}", flush=True)


async def _worker() -> None:
    """Consume queued PCM, transcribe a sliding window, broadcast results."""
    loop = asyncio.get_running_loop()
    while True:
        assert hub.queue is not None
        pcm = await hub.queue.get()
        assert hub.transcriber is not None
        # Serialize with _end_utterance so the final pass never runs while a
        # sliding-window pass is still using the transcriber.
        async with hub.stt_lock:
            hub.transcriber.accept_pcm(pcm)
            drained = _drain_stt_queue()
            # faster-whisper is blocking; keep it off the event loop.
            gen_at_start = hub.utterance_generation
            start = time.perf_counter()
            print(
                "[STT] start "
                f"gen={gen_at_start} "
                f"drained={drained} "
                f"pending_samples={hub.transcriber.pending_samples()} "
                f"queue_after_drain={hub.queue.qsize()}",
                flush=True,
            )
            state = await loop.run_in_executor(None, hub.transcriber.process)
        elapsed = time.perf_counter() - start
        if gen_at_start != hub.utterance_generation:
            print(
                "[STT] stale_result=dropped "
                f"gen={gen_at_start}->{hub.utterance_generation} "
                f"took={elapsed:.2f}s "
                f"partial={state.partial!r}",
                flush=True,
            )
            continue
        print(
            "[STT] done "
            f"gen={gen_at_start} "
            f"took={elapsed:.2f}s "
            f"window={state.window_seconds:.1f}s "
            f"words={state.words} "
            f"delta={state.committed_delta!r} "
            f"partial={state.partial!r} "
            f"committed_len={len(state.committed)} "
            f"clients={len(hub.clients)}",
            flush=True,
        )
        await hub.broadcast(state)


@app.post("/upload")
async def upload(request: Request) -> PlainTextResponse:
    body = await request.body()
    if body:
        if hub.queue is None or hub.transcriber is None:
            print("[UPLOAD] rejected: STT is not ready", flush=True)
            return PlainTextResponse("stt_queue_not_ready\n", status_code=503)

        hub.upload_count += 1
        hub.last_upload_mono = time.monotonic()
        stats = _pcm_stats(body)
        is_silence = stats["rms_dbfs"] < SILENCE_RMS_DBFS
        print(
            "[UPLOAD] "
            f"#{hub.upload_count} "
            f"bytes={len(body)} "
            f"samples={stats['samples']} "
            f"rms={stats['rms_dbfs']:.1f}dBFS "
            f"peak={stats['peak_dbfs']:.1f}dBFS "
            f"queue={hub.queue.qsize()} "
            f"skipped={'silence' if is_silence else 'no'}",
            flush=True,
        )

        if is_silence:
            hub.silence_count += 1
            # PTT is edge-triggered by /utterance/end. Do not finalize while the
            # user is still holding the screen; quiet 1 s chunks are just skipped.
            if hub.silence_count >= 2 and hub.last_state.partial:
                await hub.broadcast(
                    TranscriptState(committed=hub.last_state.committed, partial="")
                )
            if SAVE_RAW:
                await asyncio.to_thread(_save_raw, body)
            return PlainTextResponse("upload_ok\n")

        hub.silence_count = 0
        hub.in_utterance = True
        _append_utterance_pcm(body)
        hub.queue.put_nowait(bytes(body))
        if SAVE_RAW:
            await asyncio.to_thread(_save_raw, body)
    return PlainTextResponse("upload_ok\n")


def _maybe_empty_utterance_fallback() -> None:
    """Speak the retry prompt when PTT ended with no usable speech."""
    if not ROBOT_AUTO_REPLY:
        return
    now = time.monotonic()
    if now - hub.last_utterance_end_mono < 1.0:
        return
    hub.last_utterance_end_mono = now
    t0 = time.perf_counter()
    asyncio.create_task(
        _robot_auto_from_utterance(
            EMPTY_UTTERANCE_REPLY, t0, canned_reply=EMPTY_UTTERANCE_REPLY
        )
    )


@app.post("/utterance/end")
async def utterance_end() -> PlainTextResponse:
    """ESP32 PTT released: finalize the current utterance right away instead of
    waiting for silence chunks that will never come while the button is up."""
    if hub.transcriber is None:
        return PlainTextResponse("stt_not_ready\n", status_code=503)
    if not hub.in_utterance:
        _maybe_empty_utterance_fallback()
        return PlainTextResponse("no_utterance\n")
    await _end_utterance(0.0, reason="ptt_release")
    return PlainTextResponse("utterance_ended\n")


@app.post("/ping")
async def ping(request: Request) -> PlainTextResponse:
    body = await request.body()
    print(f"[ping] {len(body)} bytes from {request.client.host if request.client else '?'}")
    return PlainTextResponse("ok\n")


@app.post("/speaker/push")
async def speaker_push(request: Request):
    """Play 16 kHz / 16-bit / mono PCM (or a WAV wrapping that format) on the Mac speaker."""
    body = await request.body()
    emotion = (request.headers.get("x-emotion") or "none").strip() or "none"
    pcm = _pcm_from_body(body)
    if len(pcm) < 2:
        return PlainTextResponse("empty_pcm\n", status_code=400)
    try:
        result = _enqueue_speaker_pcm(pcm, emotion)
    except RuntimeError as exc:
        return PlainTextResponse(str(exc) + "\n", status_code=503)
    hub.speaker_push_count += 1
    stats = _pcm_stats(pcm)
    print(
        "[SPEAKER] push "
        f"#{hub.speaker_push_count} "
        f"bytes={len(pcm)} "
        f"seconds={result['seconds']} "
        f"emotion={emotion!r} "
        f"rms={stats['rms_dbfs']:.1f}dBFS",
        flush=True,
    )
    return JSONResponse({"ok": True, "bytes": len(pcm), **result})


@app.post("/speaker/say")
async def speaker_say(request: Request):
    """Synthesize a sentence with the selected TTS backend and play it on the Mac."""
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse({"ok": False, "error": "expected JSON {text, emotion?}"}, status_code=400)
    text = str(payload.get("text") or "").strip()
    emotion = str(payload.get("emotion") or "none").strip() or "none"
    if not text:
        return JSONResponse({"ok": False, "error": "empty text"}, status_code=400)

    loop = asyncio.get_running_loop()
    hub.robot_busy = True
    try:
        pcm = await loop.run_in_executor(None, lambda: synthesize_pcm(text))
    except TtsError as exc:
        hub.robot_busy = False
        print(f"[TTS] failed: {exc}", flush=True)
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)

    try:
        result = _enqueue_speaker_pcm(pcm, emotion)
    except RuntimeError as exc:
        hub.robot_busy = False
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=503)

    hub.speaker_push_count += 1
    hub.robot_busy = False
    stats = _pcm_stats(pcm)
    print(
        "[TTS] manual "
        f"text={text!r} "
        f"bytes={len(pcm)} "
        f"seconds={result['seconds']} "
        f"emotion={emotion!r} "
        f"rms={stats['rms_dbfs']:.1f}dBFS",
        flush=True,
    )
    return JSONResponse({"ok": True, "text": text, "bytes": len(pcm), **result})


class ReplyCancelled(Exception):
    """Barge-in flushed the speaker while this reply was still being produced."""


async def _robot_auto_from_utterance(
    turn: str,
    t_stt_end: float,
    canned_reply: Optional[str] = None,
) -> None:
    # Stop any reply still playing so two turns never overlap on the speaker.
    _flush_speaker_queue()
    my_gen = hub.reply_generation
    try:
        async with hub.reply_lock:
            if my_gen != hub.reply_generation:
                return
            if canned_reply:
                hub.robot_busy = True
                try:
                    spoken = await _speak_robot_reply(canned_reply, "sad", gen=my_gen)
                finally:
                    if my_gen == hub.reply_generation:
                        hub.robot_busy = False
                print(
                    f"[TTS] empty-utterance fallback bytes={spoken.get('bytes')} "
                    f"gen={my_gen}",
                    flush=True,
                )
                return
            await _robot_respond(turn, speak=True, source="utterance", t_stt_end=t_stt_end)
    except ReplyCancelled:
        pass
    except Exception as exc:
        print(f"[LLM] auto_reply failed: {exc}", flush=True)
        if my_gen == hub.reply_generation:
            hub.robot_busy = False


async def _speak_robot_reply(text: str, emotion: str, gen: Optional[int] = None) -> dict:
    loop = asyncio.get_running_loop()
    pcm = await loop.run_in_executor(None, lambda: synthesize_pcm(text))
    if gen is not None and gen != hub.reply_generation:
        raise ReplyCancelled("barge-in during TTS")
    t_q0 = time.perf_counter()
    result = _enqueue_speaker_pcm(pcm, emotion)
    queue_s = time.perf_counter() - t_q0
    hub.speaker_push_count += 1
    stats = _pcm_stats(pcm)
    print(
        "[TTS] robot "
        f"text={text!r} "
        f"bytes={len(pcm)} "
        f"seconds={result['seconds']} "
        f"emotion={emotion!r} "
        f"rms={stats['rms_dbfs']:.1f}dBFS",
        flush=True,
    )
    return {
        "bytes": len(pcm),
        "queue_s": round(queue_s, 3),
        **result,
    }


async def _robot_respond(
    user_text: str,
    speak: bool,
    source: str,
    t_stt_end: Optional[float] = None,
) -> dict:
    loop = asyncio.get_running_loop()
    t0 = t_stt_end if t_stt_end is not None else time.perf_counter()
    gen = hub.reply_generation
    hub.robot_busy = True
    t_llm0 = time.perf_counter()
    try:
        llm = await loop.run_in_executor(None, lambda: complete(user_text))
    except Exception:
        if gen == hub.reply_generation:
            hub.robot_busy = False
        raise
    if gen != hub.reply_generation:
        # Barge-in arrived while the LLM was running: the user moved on.
        print(f"[LLM] reply dropped (barge-in) source={source} reply={llm.reply!r}", flush=True)
        raise ReplyCancelled("barge-in during LLM")
    print(
        f"[LLM] {llm.backend}/{llm.model} "
        f"{llm.elapsed_s:.2f}s source={source} "
        f"emotion={llm.emotion} "
        f"user={user_text!r} "
        f"reply={llm.reply!r}",
        flush=True,
    )
    spoken = None
    t_tts = 0.0
    t_queue = 0.0
    if speak:
        try:
            t_tts0 = time.perf_counter()
            spoken = await _speak_robot_reply(llm.reply, llm.emotion, gen=gen)
            t_tts = time.perf_counter() - t_tts0
            t_queue = float(spoken.get("queue_s") or 0.0)
        except ReplyCancelled:
            print(f"[TTS] reply dropped (barge-in) source={source} reply={llm.reply!r}", flush=True)
            raise
        except TtsError as exc:
            print(f"[TTS] robot failed: {exc}", flush=True)
            if gen == hub.reply_generation:
                hub.robot_busy = False
            raise
    if gen == hub.reply_generation:
        hub.robot_busy = False
    print(
        "[TIMING] "
        f"stt_end_to_llm={t_llm0 - t0:.2f}s "
        f"llm={llm.elapsed_s:.2f}s "
        f"tts={t_tts:.2f}s "
        f"queue={t_queue:.2f}s "
        f"source={source}",
        flush=True,
    )
    hub.last_robot = {
        "robot_reply": llm.reply,
        "robot_emotion": llm.emotion,
        "llm_backend": llm.backend,
        "llm_model": llm.model,
        "llm_seconds": round(llm.elapsed_s, 2),
        "user_text": user_text,
    }
    await hub.broadcast(hub.last_state)
    out = {
        "ok": True,
        "reply": llm.reply,
        "emotion": llm.emotion,
        "backend": llm.backend,
        "model": llm.model,
        "elapsed_s": round(llm.elapsed_s, 2),
        "user_text": user_text,
        "source": source,
        "spoken": spoken,
    }
    return out


@app.post("/robot/reply")
async def robot_reply(request: Request):
    """LLM reply (and optional TTS) for a user sentence. Switch backend via LLM_BACKEND."""
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse({"ok": False, "error": "expected JSON {text, speak?}"}, status_code=400)
    text = str(payload.get("text") or "").strip()
    speak = payload.get("speak", True)
    if isinstance(speak, str):
        speak = speak.strip().lower() not in ("0", "false", "no")
    if not text:
        return JSONResponse({"ok": False, "error": "empty text"}, status_code=400)
    try:
        async with hub.reply_lock:
            return JSONResponse(await _robot_respond(text, bool(speak), source="api"))
    except ReplyCancelled:
        return JSONResponse({"ok": False, "error": "cancelled_by_barge_in"}, status_code=409)
    except LlmError as exc:
        print(f"[LLM] failed: {exc}", flush=True)
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=502)
    except TtsError as exc:
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=500)
    except RuntimeError as exc:
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=503)


@app.get("/robot/status")
async def robot_status():
    return JSONResponse(
        {
            "backend": backend_name(),
            "model": model_name(),
            "stt_backend": STT_BACKEND,
            "qwen_stt_url": QWEN_STT_URL,
            "tts_backend": tts_backend_name(),
            "auto_reply": ROBOT_AUTO_REPLY,
            "robot_state": _robot_state(),
            "last": hub.last_robot or None,
        }
    )


def _robot_state() -> str:
    # "speaking" lasts until afplay actually finishes, so the face on the
    # board stays in sync with the Mac speaker.
    if hub.player.playing():
        return "speaking"
    if hub.robot_busy:
        return "thinking"
    return "idle"


def _state_headers() -> dict[str, str]:
    state = _robot_state()
    headers = {"X-Robot-State": state}
    if state == "speaking":
        headers["X-Emotion"] = hub.player.emotion
        headers["X-Speak-Remaining-Ms"] = str(hub.player.remaining_ms())
    return headers


def _flush_speaker_queue() -> int:
    stopped = 1 if hub.player.stop() else 0
    # Invalidate any LLM/TTS reply still being produced for the old turn.
    hub.reply_generation += 1
    hub.robot_busy = False
    return stopped


@app.post("/speaker/flush")
async def speaker_flush():
    """ESP32 barge-in: stop Mac playback so a new PTT turn can start."""
    dropped = _flush_speaker_queue()
    print(f"[SPEAKER] flush stopped={dropped}", flush=True)
    return JSONResponse({"ok": True, "dropped": dropped})


@app.get("/speaker/pull")
async def speaker_pull():
    """ESP32 polls this for robot state. Audio plays on the Mac, so always 204."""
    hub.speaker_pull_count += 1
    return Response(status_code=204, headers=_state_headers())


@app.get("/speaker/status")
async def speaker_status():
    return JSONResponse(
        {
            "state": _robot_state(),
            "playing": hub.player.playing(),
            "emotion": hub.player.emotion,
            "remaining_ms": hub.player.remaining_ms(),
            "play_count": hub.player.play_count,
            "push_count": hub.speaker_push_count,
            "pull_count": hub.speaker_pull_count,
            "volume": TTS_VOLUME,
        }
    )


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(WEB_DIR / "index.html")


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    hub.clients.add(ws)
    # Send the current state immediately so a fresh tab is not blank.
    hello = {"committed": hub.last_state.committed, "partial": hub.last_state.partial}
    if hub.last_robot:
        hello.update(hub.last_robot)
    await ws.send_json(hello)
    try:
        while True:
            msg = await ws.receive_text()
            if msg == "reset" and hub.transcriber is not None:
                gen = _bump_utterance_generation()
                print(f"[STT] full_reset gen={gen}", flush=True)
                hub.in_utterance = False
                hub.silence_count = 0
                hub.transcript_history = ""
                async with hub.stt_lock:
                    _drain_stt_queue()
                    hub.utterance_pcm.clear()
                    hub.transcriber.reset()
                await hub.broadcast(TranscriptState(committed="", partial=""))
    except WebSocketDisconnect:
        pass
    finally:
        hub.clients.discard(ws)


def _enqueue_speaker_pcm(pcm: bytes, emotion: str) -> dict:
    """Play one utterance on the Mac speaker (stops whatever was playing)."""
    return hub.player.play(pcm, emotion)


def _save_raw(body: bytes) -> None:
    when = datetime.now(timezone.utc).isoformat().replace(":", "-").replace(".", "-")
    path = UPLOAD_DIR / f"chunk_{when}_{len(body)}.raw"
    path.write_bytes(body)


def _pcm_from_body(body: bytes) -> bytes:
    """Accept headerless PCM or a 16-bit WAV; return raw little-endian int16 samples."""
    if len(body) >= 12 and body[:4] == b"RIFF" and body[8:12] == b"WAVE":
        return _pcm_from_wav(body)
    if len(body) % 2 == 1:
        body = body[:-1]
    return body


def _pcm_from_wav(body: bytes) -> bytes:
    pos = 12
    data = b""
    while pos + 8 <= len(body):
        chunk_id = body[pos:pos + 4]
        chunk_len = int.from_bytes(body[pos + 4:pos + 8], "little")
        pos += 8
        payload = body[pos:pos + chunk_len]
        pos += chunk_len
        if chunk_len % 2 == 1:
            pos += 1
        if chunk_id == b"data":
            data = payload
            break
    if len(data) % 2 == 1:
        data = data[:-1]
    return data


def _pcm_stats(body: bytes) -> dict[str, float]:
    """Return quick loudness stats for ESP32 int16 PCM chunks."""
    if len(body) < 2:
        return {"samples": 0, "rms_dbfs": -120.0, "peak_dbfs": -120.0}

    if len(body) % 2 != 0:
        body = body[:-1]

    samples = np.frombuffer(body, dtype=np.int16).astype(np.float32)
    if samples.size == 0:
        return {"samples": 0, "rms_dbfs": -120.0, "peak_dbfs": -120.0}

    rms = float(np.sqrt(np.mean(np.square(samples))))
    peak = float(np.max(np.abs(samples)))
    return {
        "samples": int(samples.size),
        "rms_dbfs": _dbfs(rms),
        "peak_dbfs": _dbfs(peak),
    }


def _dbfs(value: float) -> float:
    if value <= 0:
        return -120.0
    return float(20.0 * np.log10(value / 32768.0))


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="info")
