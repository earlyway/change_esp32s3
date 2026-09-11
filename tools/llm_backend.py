#!/usr/bin/env python3
"""LLM backends for Phase 3 robot replies.

Switch with env (or tools/llm.env, which does not override already-set env vars):

    LLM_BACKEND=ollama          # default
    OLLAMA_MODEL=qwen2.5:14b
    OLLAMA_HOST=http://127.0.0.1:11434

    LLM_BACKEND=openai
    OPENAI_API_KEY=sk-...
    OPENAI_MODEL=gpt-4o-mini

Both backends use the same prompt and must return JSON:
    {"reply": "<one or two Korean sentences>", "emotion": "positive|neutral|sad|angry|surprised"}
"""

from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
EMOTIONS = ("positive", "neutral", "sad", "angry", "surprised")

SYSTEM_PROMPT = """You are a small desk companion robot. Reply in spoken Korean.
Return ONLY a JSON object with two keys:
  "reply": one or two short sentences, no quotes around the whole answer, no markdown
  "emotion": exactly one of positive, neutral, sad, angry, surprised
Match emotion to the user's mood. Be warm and concise. Do not mention that you are an AI."""


def load_llm_env_file() -> None:
    path = HERE / "llm.env"
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


load_llm_env_file()


@dataclass
class LlmReply:
    reply: str
    emotion: str
    backend: str
    model: str
    elapsed_s: float
    raw: str = ""


class LlmError(RuntimeError):
    pass


def backend_name() -> str:
    return (os.environ.get("LLM_BACKEND") or "ollama").strip().lower()


def model_name() -> str:
    if backend_name() in ("openai", "gpt", "gpt-4o-mini", "4o-mini"):
        return os.environ.get("OPENAI_MODEL") or "gpt-4o-mini"
    return os.environ.get("OLLAMA_MODEL") or "qwen2.5:14b"


def complete(user_text: str) -> LlmReply:
    text = " ".join((user_text or "").split())
    if not text:
        raise LlmError("empty user text")
    if len(text) > 800:
        text = text[:800]
    name = backend_name()
    if name in ("openai", "gpt", "gpt-4o-mini", "4o-mini"):
        return _complete_openai(text)
    return _complete_ollama(text)


def _complete_ollama(user_text: str) -> LlmReply:
    host = (os.environ.get("OLLAMA_HOST") or "http://127.0.0.1:11434").rstrip("/")
    model = os.environ.get("OLLAMA_MODEL") or "qwen2.5:14b"
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_text},
        ],
        "stream": False,
        "format": "json",
        "options": {"temperature": 0.6, "num_predict": 160},
    }
    url = host + "/api/chat"
    started = time.perf_counter()
    try:
        data = _http_json(url, payload, timeout=120)
    except LlmError as exc:
        raise LlmError(
            f"Ollama 호출 실패 ({model}). `ollama pull {model}` 후 `ollama serve`가 켜져 있는지 확인하세요. {exc}"
        ) from exc
    elapsed = time.perf_counter() - started
    raw = ""
    message = data.get("message") or {}
    if isinstance(message, dict):
        raw = str(message.get("content") or "")
    parsed = _parse_reply_json(raw)
    return LlmReply(
        reply=parsed["reply"],
        emotion=parsed["emotion"],
        backend="ollama",
        model=str(data.get("model") or model),
        elapsed_s=elapsed,
        raw=raw,
    )


def _complete_openai(user_text: str) -> LlmReply:
    api_key = (os.environ.get("OPENAI_API_KEY") or "").strip()
    if not api_key:
        raise LlmError("OPENAI_API_KEY가 없습니다. tools/llm.env에 넣거나 환경변수로 설정하세요.")
    model = os.environ.get("OPENAI_MODEL") or "gpt-4o-mini"
    payload = {
        "model": model,
        "temperature": 0.6,
        "max_tokens": 160,
        "response_format": {"type": "json_object"},
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_text},
        ],
    }
    url = os.environ.get("OPENAI_BASE_URL") or "https://api.openai.com/v1/chat/completions"
    started = time.perf_counter()
    data = _http_json(
        url,
        payload,
        timeout=60,
        headers={"Authorization": "Bearer " + api_key},
    )
    elapsed = time.perf_counter() - started
    raw = ""
    try:
        raw = data["choices"][0]["message"]["content"] or ""
    except (KeyError, IndexError, TypeError) as exc:
        raise LlmError(f"OpenAI 응답 형식이 예상과 다릅니다: {data!r}") from exc
    parsed = _parse_reply_json(raw)
    return LlmReply(
        reply=parsed["reply"],
        emotion=parsed["emotion"],
        backend="openai",
        model=str(data.get("model") or model),
        elapsed_s=elapsed,
        raw=raw,
    )


def _parse_reply_json(raw: str) -> dict[str, str]:
    obj = _extract_json_object(raw)
    reply = str(obj.get("reply") or obj.get("text") or obj.get("response") or "").strip()
    emotion = str(obj.get("emotion") or "neutral").strip().lower()
    if emotion not in EMOTIONS:
        emotion = "neutral"
    if not reply:
        raise LlmError(f"LLM JSON에 reply가 없습니다: {raw[:300]!r}")
    reply = " ".join(reply.split())
    return {"reply": reply, "emotion": emotion}


def _extract_json_object(raw: str) -> dict:
    text = (raw or "").strip()
    if text.startswith("```"):
        lines = text.splitlines()
        if lines and lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        text = "\n".join(lines).strip()
    try:
        obj = json.loads(text)
        if isinstance(obj, dict):
            return obj
    except json.JSONDecodeError:
        pass
    start = text.find("{")
    end = text.rfind("}")
    if start >= 0 and end > start:
        obj = json.loads(text[start:end + 1])
        if isinstance(obj, dict):
            return obj
    raise LlmError(f"LLM 출력이 JSON이 아닙니다: {raw[:300]!r}")


def _http_json(url: str, payload: dict, timeout: float, headers: dict | None = None) -> dict:
    body = json.dumps(payload).encode("utf-8")
    req_headers = {"Content-Type": "application/json", **(headers or {})}
    req = urllib.request.Request(url, data=body, headers=req_headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:500]
        raise LlmError(f"HTTP {exc.code} {url}: {detail}") from exc
    except urllib.error.URLError as exc:
        raise LlmError(f"연결 실패 {url}: {exc}") from exc
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise LlmError(f"JSON 파싱 실패: {raw[:300]!r}") from exc
    if not isinstance(data, dict):
        raise LlmError(f"JSON 객체가 아닙니다: {raw[:300]!r}")
    return data
