"""Persistent Qwen3-ASR and Moonshine adapters for the local STT A/B test."""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any


QWEN_MODEL = "Qwen/Qwen3-ASR-0.6B"
MOONSHINE_LANGUAGE = "ko"


@dataclass(frozen=True)
class Transcription:
    backend: str
    model: str
    text: str
    seconds: float


class SttBackends:
    """Load both models once and run fair, sequential comparisons."""

    def __init__(self) -> None:
        self.state = "not_loaded"
        self.detail = "비교 모델을 아직 준비하지 않았습니다."
        self.error = ""
        self._load_lock = threading.Lock()
        self._inference_lock = threading.Lock()
        self._qwen: Any = None
        self._moonshine: Any = None

    def status(self) -> dict[str, str]:
        return {
            "state": self.state,
            "detail": self.detail,
            "error": self.error,
            "qwen_model": QWEN_MODEL,
            "moonshine_model": "Moonshine Korean Tiny",
        }

    def load(self) -> None:
        if self.state == "ready":
            return
        if not self._load_lock.acquire(blocking=False):
            return
        try:
            self.state = "loading"
            self.error = ""

            self.detail = "Qwen3-ASR 0.6B를 준비하는 중입니다."
            try:
                from mlx_qwen3_asr import Session
            except (ImportError, RuntimeError) as exc:
                raise RuntimeError(
                    "Qwen3-ASR MLX를 불러오지 못했습니다. "
                    "Apple Silicon Mac과 Run STT A-B Test.command 전용 환경이 필요합니다."
                ) from exc
            self._qwen = Session(model=QWEN_MODEL)

            self.detail = "Moonshine 한국어 Tiny를 준비하는 중입니다."
            try:
                from moonshine_voice import Transcriber, get_model_for_language
            except ImportError as exc:
                raise RuntimeError(
                    "Moonshine을 불러오지 못했습니다. "
                    "Run STT A-B Test.command로 전용 환경을 설치해 주세요."
                ) from exc
            model_path, model_arch = get_model_for_language(MOONSHINE_LANGUAGE)
            self._moonshine = Transcriber(model_path, model_arch)

            self.state = "ready"
            self.detail = "두 모델이 준비되었습니다. 화면을 누른 채 말해보세요."
        except Exception as exc:
            self.state = "error"
            self.error = f"{type(exc).__name__}: {exc}"
            self.detail = "모델 준비에 실패했습니다."
            raise
        finally:
            self._load_lock.release()

    def compare(self, wav_path: Path, qwen_first: bool) -> list[Transcription]:
        if self.state != "ready":
            raise RuntimeError("두 STT 모델이 아직 준비되지 않았습니다.")
        order = (
            (self._transcribe_qwen, self._transcribe_moonshine)
            if qwen_first
            else (self._transcribe_moonshine, self._transcribe_qwen)
        )
        with self._inference_lock:
            return [transcribe(wav_path) for transcribe in order]

    def _transcribe_qwen(self, wav_path: Path) -> Transcription:
        started = time.perf_counter()
        result = self._qwen.transcribe(str(wav_path), language="Korean")
        return Transcription(
            backend="qwen",
            model=QWEN_MODEL,
            text=str(result.text or "").strip(),
            seconds=time.perf_counter() - started,
        )

    def _transcribe_moonshine(self, wav_path: Path) -> Transcription:
        from moonshine_voice import load_wav_file

        audio, sample_rate = load_wav_file(wav_path)
        started = time.perf_counter()
        self._moonshine.start()
        self._moonshine.add_audio(audio, sample_rate)
        transcript = self._moonshine.stop()
        lines = transcript.lines if transcript is not None else []
        text = " ".join(line.text.strip() for line in lines if line.text.strip())
        return Transcription(
            backend="moonshine",
            model="Moonshine Korean Tiny",
            text=text,
            seconds=time.perf_counter() - started,
        )
