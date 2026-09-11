"""Streaming transcription over a rolling PCM buffer using faster-whisper.

The ESP32 firmware streams 1-second chunks of 16 kHz / 16-bit / mono PCM.
Transcribing each 1-second chunk in isolation produces poor results because
words get cut at chunk boundaries. Instead we keep a rolling audio buffer,
re-transcribe a sliding window every time new audio arrives, and use a
LocalAgreement-2 policy to decide which words are stable enough to "commit".

LocalAgreement-2 (Liu et al.): a word is committed once two consecutive
hypotheses agree on it. The agreed prefix becomes committed text; the rest is
returned as a volatile "partial" that may still change.

This module is engine logic only and does no I/O beyond model loading.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass
from typing import List

import numpy as np

SAMPLE_RATE = 16000
# int16 little-endian PCM, the format produced by the ESP32 firmware.
PCM_DTYPE = np.int16
INT16_FULL_SCALE = 32768.0


@dataclass
class _Word:
    text: str
    start: float  # absolute seconds since stream start
    end: float


@dataclass
class TranscriptState:
    """Snapshot returned to callers after each processing pass."""

    committed: str
    partial: str
    words: int = 0
    window_seconds: float = 0.0
    committed_delta: str = ""


class StreamingTranscriber:
    """Rolling-buffer streaming transcriber with LocalAgreement-2 commit.

    Not thread-safe across :meth:`accept_pcm` / :meth:`process` unless the
    caller serializes them; an internal lock guards the buffer so audio can be
    appended from one thread while another thread transcribes.
    """

    def __init__(
        self,
        model_size: str = "base",
        device: str = "auto",
        compute_type: str = "auto",
        language: str | None = None,
        beam_size: int = 1,
        min_window_s: float = 1.0,
        max_window_s: float = 6.0,
        keep_context_s: float = 0.0,
        initial_prompt: str | None = None,
    ) -> None:
        # Imported lazily so the module can be inspected without the heavy
        # faster-whisper / CTranslate2 dependency installed.
        from faster_whisper import WhisperModel

        self._model = WhisperModel(model_size, device=device, compute_type=compute_type)
        self._language = language
        self._beam_size = beam_size
        self._min_window_samples = int(min_window_s * SAMPLE_RATE)
        self._max_window_samples = int(max_window_s * SAMPLE_RATE)
        self._keep_context_samples = int(keep_context_s * SAMPLE_RATE)
        self._initial_prompt = initial_prompt

        self._lock = threading.Lock()
        # Rolling audio buffer of float32 samples in [-1, 1].
        self._audio = np.zeros(0, dtype=np.float32)
        # Absolute time (seconds since stream start) of self._audio[0].
        self._buffer_offset_s = 0.0
        # Time up to which words have been committed.
        self._committed_end_s = 0.0
        # Committed transcript text (already final).
        self._committed_text = ""
        # Previous run's uncommitted word tail, for LocalAgreement comparison.
        self._prev_tail: List[_Word] = []

    # -- audio ingestion -------------------------------------------------
    def accept_pcm(self, pcm_bytes: bytes) -> None:
        """Append raw int16 little-endian PCM bytes to the rolling buffer."""
        if not pcm_bytes:
            return
        # Drop a trailing odd byte defensively (int16 frames are 2 bytes).
        if len(pcm_bytes) % 2 != 0:
            pcm_bytes = pcm_bytes[:-1]
        samples = np.frombuffer(pcm_bytes, dtype=PCM_DTYPE).astype(np.float32)
        samples /= INT16_FULL_SCALE
        with self._lock:
            self._audio = np.concatenate((self._audio, samples))

    def pending_samples(self) -> int:
        with self._lock:
            return self._audio.shape[0]

    # -- transcription ---------------------------------------------------
    def process(self) -> TranscriptState:
        """Transcribe the current window and advance committed text.

        Returns the latest committed + partial transcript. Safe to call when
        the buffer is short; it simply returns the current state unchanged.
        """
        with self._lock:
            self._limit_buffer_locked()
            audio = self._audio.copy()
            buffer_offset_s = self._buffer_offset_s

        window_seconds = audio.shape[0] / SAMPLE_RATE
        if audio.shape[0] < self._min_window_samples:
            return TranscriptState(
                self._committed_text,
                self._partial_text(),
                window_seconds=window_seconds,
            )

        words = self._transcribe(audio, buffer_offset_s)

        # Only consider words that start after what we've already committed.
        eps = 1e-3
        tail = [w for w in words if w.end > self._committed_end_s + eps]

        newly_committed = self._local_agreement(tail)

        committed_delta = "".join(w.text for w in newly_committed)

        if newly_committed:
            self._committed_text += committed_delta
            self._committed_end_s = newly_committed[-1].end
            self._trim_buffer()
        else:
            with self._lock:
                self._limit_buffer_locked()

        # Remaining (still-volatile) tail becomes both the partial output and
        # the comparison baseline for the next LocalAgreement pass.
        remaining = [w for w in tail if w.end > self._committed_end_s + eps]
        self._prev_tail = remaining

        return TranscriptState(
            self._committed_text,
            self._partial_text(),
            words=len(words),
            window_seconds=window_seconds,
            committed_delta=committed_delta,
        )

    def reset(self) -> None:
        with self._lock:
            self._audio = np.zeros(0, dtype=np.float32)
        self._buffer_offset_s = 0.0
        self._committed_end_s = 0.0
        self._committed_text = ""
        self._prev_tail = []

    def reset_audio_buffer(self) -> None:
        """Start a new utterance while keeping already committed text."""
        with self._lock:
            self._audio = np.zeros(0, dtype=np.float32)
        self._buffer_offset_s = self._committed_end_s
        self._prev_tail = []

    def flush_partial(self) -> str:
        """Commit the remaining volatile tail (e.g. when the speaker stops).

        LocalAgreement-2 only promotes words seen in two consecutive passes.
        The last words of an utterance often never get a second pass once
        silence stops new audio from being queued, so they would stay in
        partial forever unless flushed here.
        """
        if not self._prev_tail:
            return ""
        delta = self._partial_text()
        self._committed_text += delta
        self._committed_end_s = self._prev_tail[-1].end
        self._prev_tail = []
        return delta

    # -- internals -------------------------------------------------------
    def _transcribe(self, audio: np.ndarray, buffer_offset_s: float) -> List[_Word]:
        segments, _info = self._model.transcribe(
            audio,
            language=self._language,
            beam_size=self._beam_size,
            word_timestamps=True,
            condition_on_previous_text=False,
            initial_prompt=self._initial_prompt,
            vad_filter=False,
        )
        words: List[_Word] = []
        for segment in segments:
            if not segment.words:
                continue
            for w in segment.words:
                words.append(
                    _Word(
                        text=w.word,
                        start=buffer_offset_s + w.start,
                        end=buffer_offset_s + w.end,
                    )
                )
        return words

    def _local_agreement(self, tail: List[_Word]) -> List[_Word]:
        """Commit the common word prefix shared with the previous hypothesis."""
        committed: List[_Word] = []
        for i, word in enumerate(tail):
            if i >= len(self._prev_tail):
                break
            if _norm(word.text) == _norm(self._prev_tail[i].text):
                committed.append(word)
            else:
                break
        return committed

    def _trim_buffer(self) -> None:
        """Drop audio that precedes the committed boundary (keep optional context)."""
        with self._lock:
            cut_time = self._committed_end_s - (self._keep_context_samples / SAMPLE_RATE)
            drop = int((cut_time - self._buffer_offset_s) * SAMPLE_RATE)
            if drop > 0:
                drop = min(drop, self._audio.shape[0])
                self._audio = self._audio[drop:]
                self._buffer_offset_s += drop / SAMPLE_RATE
                self._drop_stale_prev_tail()
            self._limit_buffer_locked()

    def _limit_buffer_locked(self) -> None:
        """Keep only the latest max_window_s of audio for low-latency STT."""
        if self._audio.shape[0] <= self._max_window_samples:
            return

        drop = self._audio.shape[0] - self._max_window_samples
        self._audio = self._audio[drop:]
        self._buffer_offset_s += drop / SAMPLE_RATE
        self._drop_stale_prev_tail()

    def _drop_stale_prev_tail(self) -> None:
        self._prev_tail = [
            word for word in self._prev_tail
            if word.end > self._buffer_offset_s
        ]

    def _partial_text(self) -> str:
        return "".join(w.text for w in self._prev_tail)


def _norm(text: str) -> str:
    return text.strip().lower()
