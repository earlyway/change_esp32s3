"""Persistent conversation memory for 멜로우봇.

All turns are stored on disk under .robot-runtime/ so a server restart keeps
them. Only the newest N turns go into the next LLM prompt; older turns are
folded into a plain-text long-term notes field that is also always sent.
"""

from __future__ import annotations

import json
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
DEFAULT_PATH = HERE.parent / ".robot-runtime" / "chat_memory.json"
NOTES_CHAR_CAP = 8000


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class ChatStore:
    def __init__(self, path: Path | None = None) -> None:
        self.path = Path(path) if path is not None else DEFAULT_PATH
        self.notes = ""
        self.turns: list[dict[str, str]] = []

    @property
    def turn_count(self) -> int:
        return len(self.turns) // 2

    def load(self) -> None:
        if not self.path.is_file():
            return
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"[LLM] chat memory unreadable ({self.path}): {exc}", flush=True)
            return
        if not isinstance(raw, dict):
            return
        self.notes = str(raw.get("notes") or "")
        turns = raw.get("turns")
        if not isinstance(turns, list):
            return
        cleaned: list[dict[str, str]] = []
        for item in turns:
            if not isinstance(item, dict):
                continue
            role = str(item.get("role") or "")
            content = " ".join(str(item.get("content") or "").split())
            if role not in ("user", "assistant") or not content:
                continue
            cleaned.append({
                "role": role,
                "content": content,
                "ts": str(item.get("ts") or ""),
            })
        self.turns = cleaned

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload: dict[str, Any] = {
            "version": 1,
            "updated_at": _now_iso(),
            "notes": self.notes,
            "turns": self.turns,
        }
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(
            json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(tmp, self.path)

    def clear(self) -> None:
        self.notes = ""
        self.turns = []
        if self.path.is_file():
            try:
                self.path.unlink()
            except OSError:
                self.save()

    def append(self, user_text: str, reply: str, max_recent_turns: int) -> None:
        user = " ".join((user_text or "").split())
        assistant = " ".join((reply or "").split())
        if not user or not assistant:
            return
        ts = _now_iso()
        self.turns.append({"role": "user", "content": user[:800], "ts": ts})
        self.turns.append({"role": "assistant", "content": assistant[:1000], "ts": ts})
        max_msgs = max(1, max_recent_turns) * 2
        if len(self.turns) > max_msgs:
            overflow, self.turns = self.turns[:-max_msgs], self.turns[-max_msgs:]
            self._archive(overflow)

    def recent_messages(self, max_turns: int) -> list[dict[str, str]]:
        max_msgs = max(1, max_turns) * 2
        recent = self.turns[-max_msgs:]
        return [{"role": item["role"], "content": item["content"]} for item in recent]

    def _archive(self, overflow: list[dict[str, str]]) -> None:
        lines: list[str] = []
        if self.notes.strip():
            lines.append(self.notes.strip())
        for item in overflow:
            who = "사용자" if item["role"] == "user" else "멜로우봇"
            ts = (item.get("ts") or "")[:16].replace("T", " ")
            prefix = f"{ts} " if ts else ""
            lines.append(f"{prefix}{who}: {item['content']}")
        text = "\n".join(lines).strip()
        if len(text) > NOTES_CHAR_CAP:
            text = text[-NOTES_CHAR_CAP:]
            cut = text.find("\n")
            if cut > 0:
                text = text[cut + 1:]
        self.notes = text
