#!/usr/bin/env python3
"""Local robot-microphone A/B server for Qwen3-ASR and Moonshine Korean."""

from __future__ import annotations

import asyncio
import io
import json
import os
import time
import wave
from datetime import datetime
from pathlib import Path
from typing import Any

import numpy as np
from fastapi import FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse, Response

from stt_ab_backends import SttBackends


HERE = Path(__file__).resolve().parent
PROJECT_DIR = HERE.parent
WEB_DIR = HERE / "stt_ab_web"
RUNTIME_DIR = Path(
    os.environ.get(
        "STT_AB_RUNTIME_DIR",
        PROJECT_DIR / ".robot-runtime" / "stt-ab",
    )
).resolve()
PORT = int(os.environ.get("STT_AB_PORT", "3000"))
SAMPLE_RATE = 16000
BYTES_PER_SECOND = SAMPLE_RATE * 2
MAX_UTTERANCE_SECONDS = 60
MAX_PCM_BYTES = BYTES_PER_SECOND * MAX_UTTERANCE_SECONDS

app = FastAPI(title="Robot STT A/B Test")
models = SttBackends()
pcm_buffer = bytearray()
results: list[dict[str, Any]] = []
prepare_task: asyncio.Task | None = None
compare_lock = asyncio.Lock()
utterance_number = 0


@app.on_event("startup")
async def on_startup() -> None:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    print(f"[STT A/B] http://localhost:{PORT}")
    print(f"[STT A/B] results: {RUNTIME_DIR}")


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(WEB_DIR / "index.html")


@app.get("/health")
async def health() -> JSONResponse:
    return JSONResponse({"ok": True, "service": "robot-stt-ab"})


@app.post("/api/prepare")
async def prepare() -> JSONResponse:
    global prepare_task
    if models.state == "ready":
        return JSONResponse({"ok": True, "models": models.status()})
    if prepare_task is None or prepare_task.done():
        prepare_task = asyncio.create_task(_prepare_models())
    return JSONResponse({"ok": True, "models": models.status()})


async def _prepare_models() -> None:
    try:
        await asyncio.to_thread(models.load)
        print("[STT A/B] Both models are ready.", flush=True)
    except Exception as exc:
        print(f"[STT A/B] Model preparation failed: {exc!r}", flush=True)


@app.get("/api/status")
async def status() -> JSONResponse:
    return JSONResponse(
        {
            "ok": True,
            "models": models.status(),
            "capture": {
                "bytes": len(pcm_buffer),
                "seconds": round(len(pcm_buffer) / BYTES_PER_SECOND, 2),
            },
            "busy": compare_lock.locked(),
            "results": [_public_job(job) for job in results[-20:]],
        }
    )


@app.post("/upload")
async def robot_upload(request: Request) -> PlainTextResponse:
    """Receive the same 16 kHz, mono, signed-int16 PCM sent to the normal server."""
    body = await request.body()
    if not body:
        return PlainTextResponse("upload_ok\n")
    if len(body) % 2:
        body = body[:-1]
    if len(pcm_buffer) + len(body) > MAX_PCM_BYTES:
        pcm_buffer.clear()
        return PlainTextResponse(
            f"utterance_too_long_max_{MAX_UTTERANCE_SECONDS}s\n",
            status_code=413,
        )
    pcm_buffer.extend(body)
    return PlainTextResponse("upload_ok\n")


@app.post("/speaker/flush")
async def speaker_flush_compat() -> JSONResponse:
    """Keep the production firmware's PTT barge-in request error-free."""
    return JSONResponse({"ok": True, "dropped": 0})


@app.get("/speaker/pull")
async def speaker_pull_compat() -> Response:
    """The A/B server never plays replies, but firmware continues polling."""
    return Response(status_code=204, headers={"X-Robot-State": "idle"})


@app.post("/utterance/end")
async def robot_utterance_end() -> PlainTextResponse:
    """Finalize the robot's current PTT recording and queue both transcriptions."""
    if not pcm_buffer:
        return PlainTextResponse("no_utterance\n")
    pcm = bytes(pcm_buffer)
    pcm_buffer.clear()
    job = _new_job(pcm, source="robot")
    asyncio.create_task(_process_job(job))
    return PlainTextResponse("utterance_queued\n")


@app.post("/api/audio")
async def browser_audio(request: Request) -> JSONResponse:
    """Queue a 16 kHz/16-bit/mono WAV uploaded from the comparison page."""
    body = await request.body()
    try:
        pcm = _pcm_from_wav(body)
    except ValueError as exc:
        return JSONResponse({"ok": False, "error": str(exc)}, status_code=400)
    job = _new_job(pcm, source="wav_upload")
    asyncio.create_task(_process_job(job))
    return JSONResponse({"ok": True, "job": job})


def _new_job(pcm: bytes, source: str) -> dict[str, Any]:
    global utterance_number
    utterance_number += 1
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    wav_name = f"{stamp}.wav"
    wav_path = RUNTIME_DIR / wav_name
    _write_wav(wav_path, pcm)
    job: dict[str, Any] = {
        "id": utterance_number,
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "source": source,
        "audio_url": f"/recordings/{wav_name}",
        "audio_seconds": round(len(pcm) / BYTES_PER_SECOND, 2),
        "rms_dbfs": round(_rms_dbfs(pcm), 1),
        "state": "queued",
        "error": "",
        "transcriptions": [],
        "_wav_path": str(wav_path),
    }
    results.append(job)
    return _public_job(job)


async def _process_job(public_job: dict[str, Any]) -> None:
    job = next(item for item in results if item["id"] == public_job["id"])
    if prepare_task is not None and not prepare_task.done():
        job["state"] = "waiting_for_models"
        await prepare_task
    if models.state != "ready":
        job["state"] = "error"
        job["error"] = models.error or "STT 모델이 준비되지 않았습니다."
        _save_job(job)
        return

    async with compare_lock:
        job["state"] = "processing"
        started = time.perf_counter()
        try:
            # Alternate execution order so repeated tests do not systematically
            # favor the model that always runs first on a cooler machine.
            qwen_first = job["id"] % 2 == 1
            transcriptions = await asyncio.to_thread(
                models.compare,
                Path(job["_wav_path"]),
                qwen_first,
            )
            job["transcriptions"] = [
                {
                    "backend": item.backend,
                    "model": item.model,
                    "text": item.text,
                    "seconds": round(item.seconds, 3),
                }
                for item in transcriptions
            ]
            job["execution_order"] = [item.backend for item in transcriptions]
            job["total_seconds"] = round(time.perf_counter() - started, 3)
            job["state"] = "done"
            print(
                f"[STT A/B] job={job['id']} audio={job['audio_seconds']}s "
                f"order={job['execution_order']} total={job['total_seconds']}s",
                flush=True,
            )
        except Exception as exc:
            job["state"] = "error"
            job["error"] = f"{type(exc).__name__}: {exc}"
            print(f"[STT A/B] job={job['id']} failed: {exc!r}", flush=True)
        _save_job(job)


@app.get("/recordings/{filename}")
async def recording(filename: str):
    if Path(filename).name != filename or not filename.endswith(".wav"):
        return PlainTextResponse("not_found\n", status_code=404)
    path = RUNTIME_DIR / filename
    if not path.is_file():
        return PlainTextResponse("not_found\n", status_code=404)
    return FileResponse(path, media_type="audio/wav")


def _public_job(job: dict[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in job.items() if not key.startswith("_")}


def _save_job(job: dict[str, Any]) -> None:
    path = Path(job["_wav_path"]).with_suffix(".json")
    path.write_text(
        json.dumps(_public_job(job), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )


def _write_wav(path: Path, pcm: bytes) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(pcm)


def _pcm_from_wav(body: bytes) -> bytes:
    try:
        with wave.open(io.BytesIO(body), "rb") as wav:
            if wav.getnchannels() != 1:
                raise ValueError("WAV는 mono여야 합니다.")
            if wav.getsampwidth() != 2:
                raise ValueError("WAV는 16-bit PCM이어야 합니다.")
            if wav.getframerate() != SAMPLE_RATE:
                raise ValueError("WAV 샘플레이트는 16 kHz여야 합니다.")
            if wav.getcomptype() != "NONE":
                raise ValueError("압축되지 않은 PCM WAV만 지원합니다.")
            pcm = wav.readframes(wav.getnframes())
    except (wave.Error, EOFError) as exc:
        raise ValueError("유효한 WAV 파일이 아닙니다.") from exc
    if not pcm:
        raise ValueError("WAV 파일에 오디오가 없습니다.")
    if len(pcm) > MAX_PCM_BYTES:
        raise ValueError(f"오디오는 최대 {MAX_UTTERANCE_SECONDS}초까지 지원합니다.")
    return pcm


def _rms_dbfs(pcm: bytes) -> float:
    samples = np.frombuffer(pcm, dtype=np.int16).astype(np.float32)
    if samples.size == 0:
        return -120.0
    rms = float(np.sqrt(np.mean(np.square(samples))))
    if rms <= 0:
        return -120.0
    return float(20.0 * np.log10(rms / 32768.0))


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="info")
