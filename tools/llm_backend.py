#!/usr/bin/env python3
"""LLM backends for Phase 3 robot replies.

Switch with env (or tools/llm.env, which does not override already-set env vars):

    LLM_BACKEND=ollama          # default
    OLLAMA_MODEL=qwen3:30b-a3b   # default; qwen2.5:14b also works
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
from typing import Optional, Sequence

HERE = Path(__file__).resolve().parent
PERSONA_PATH = HERE / "mallow_persona.txt"
# qwen3:30b-a3b is MoE (3B active): faster than qwen2.5:14b and noticeably
# better in Korean. Needs think=false so the JSON reply is not preceded by a
# <think> block.
DEFAULT_OLLAMA_MODEL = "qwen3:30b-a3b"
# Ollama unloads a model after 5 min by default; reloading the 18 GB model
# costs ~9 s on the first reply. Keep it resident between turns.
OLLAMA_KEEP_ALIVE = os.environ.get("OLLAMA_KEEP_ALIVE") or "30m"
EMOTIONS = ("positive", "neutral", "sad", "angry", "surprised")

REPLY_RULES = """응답 규칙:
- 반드시 JSON 객체 하나만 출력한다. 마크다운, 코드 블록, 설명을 덧붙이지 않는다.
- 키는 "reply"와 "emotion" 두 개만 사용한다.
- "reply"는 TTS가 읽기 좋은 짧은 한두 문장으로 쓴다. 이모지, 목록, 괄호 남용, 불필요한 영어를 피한다.
- 사용자가 말한 구체적인 내용을 하나 이상 반영하고, 상투적인 위로나 같은 말을 반복하지 않는다.
- 최근 대화가 메시지 목록으로 주어지면 지시어("그거", "아까", "아니 그게 아니라")를 그 맥락에서 해석한다.
- 장기 기억과 최근 대화에 없는 사실·이름·약속은 지어내지 않는다.
- 말이 불완전하거나 여러 뜻으로 해석되어 확신할 수 없으면 추측하지 말고 짧게 되묻는다.
- "emotion"은 positive, neutral, sad, angry, surprised 중 하나만 사용한다.
  기쁨·격려는 positive, 평범한 정보는 neutral, 슬픔·위로는 sad,
  명백한 분노에는 angry, 놀라운 소식에는 surprised를 고른다.

좋은 응답 예시:
사용자: 오늘 회사에서 발표를 망쳐서 속상해.
응답: {"reply":"발표를 망쳤다고 느껴져서 많이 속상했겠다. 그래도 오늘 버틴 것만으로 충분히 애썼어.","emotion":"sad"}

사용자: 부산 내일 비 와?
응답: {"reply":"지금은 실시간 날씨를 확인할 수 없어. 날씨 앱에서 부산의 내일 예보를 확인해 줘.","emotion":"neutral"}

사용자: 아까 그거 말이야.
(직전 대화에 발표 이야기가 있을 때)
응답: {"reply":"회사에서 발표 망쳤다고 했던 그 얘기 맞지? 그 뒤로 좀 괜찮아졌어?","emotion":"sad"}

사용자: 아까 그거 말이야.
(직전에 무슨 얘긴지 모를 때)
응답: {"reply":"어떤 내용을 말하는지 조금만 더 알려 줄래?","emotion":"neutral"}"""


def _load_persona() -> str:
    try:
        text = PERSONA_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        text = ""
    return text or "너는 멜로우봇이다. 책상 위 작은 반려 로봇이다. 친근한 반말로 말한다."


def build_system_prompt(memory_notes: str = "") -> str:
    parts = [_load_persona(), REPLY_RULES]
    notes = " ".join((memory_notes or "").split())
    if notes:
        # Keep a bounded block so a large archive cannot dominate the prompt.
        parts.append("장기 기억 (오래된 대화에서 건진 사실. 없는 내용은 지어내지 말 것):\n" + memory_notes.strip()[:4000])
    return "\n\n".join(parts)


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
    return os.environ.get("OLLAMA_MODEL") or DEFAULT_OLLAMA_MODEL


def _messages(
    user_text: str,
    history: Optional[Sequence[dict[str, str]]],
    memory_notes: str = "",
) -> list[dict[str, str]]:
    text = " ".join((user_text or "").split())
    if not text:
        raise LlmError("empty user text")
    if len(text) > 800:
        text = text[:800]
    messages = [{"role": "system", "content": build_system_prompt(memory_notes)}]
    for item in history or ():
        role = str(item.get("role") or "")
        content = " ".join(str(item.get("content") or "").split())
        if role not in ("user", "assistant") or not content:
            continue
        # The Hub already limits recent turns; these caps also protect direct callers.
        messages.append({"role": role, "content": content[:1000]})
    messages.append({"role": "user", "content": text})
    return messages


def complete(
    user_text: str,
    history: Optional[Sequence[dict[str, str]]] = None,
    memory_notes: str = "",
) -> LlmReply:
    messages = _messages(user_text, history, memory_notes)
    name = backend_name()
    if name in ("openai", "gpt", "gpt-4o-mini", "4o-mini"):
        return _complete_openai(messages)
    return _complete_ollama(messages)


def _complete_ollama(messages: Sequence[dict[str, str]]) -> LlmReply:
    host = (os.environ.get("OLLAMA_HOST") or "http://127.0.0.1:11434").rstrip("/")
    model = os.environ.get("OLLAMA_MODEL") or DEFAULT_OLLAMA_MODEL
    payload = {
        "model": model,
        "messages": list(messages),
        "stream": False,
        "format": "json",
        # Disable qwen3 thinking mode (ignored by models without it).
        "think": False,
        "keep_alive": OLLAMA_KEEP_ALIVE,
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


def _complete_openai(messages: Sequence[dict[str, str]]) -> LlmReply:
    api_key = (os.environ.get("OPENAI_API_KEY") or "").strip()
    if not api_key:
        raise LlmError("OPENAI_API_KEY가 없습니다. tools/llm.env에 넣거나 환경변수로 설정하세요.")
    model = os.environ.get("OPENAI_MODEL") or "gpt-4o-mini"
    payload = {
        "model": model,
        "temperature": 0.6,
        "max_tokens": 160,
        "response_format": {"type": "json_object"},
        "messages": list(messages),
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
