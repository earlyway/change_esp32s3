#!/usr/bin/env python3
"""Send a user sentence through Phase 3 (LLM → TTS queue).

    python3 tools/robot_reply.py 오늘 날씨 정말 좋다

Requires tools/stt_server.py running. Default LLM is Ollama qwen2.5:14b.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text", nargs="+")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3000)
    parser.add_argument("--no-speak", action="store_true", help="LLM only, do not queue TTS")
    args = parser.parse_args()
    text = " ".join(args.text).strip()

    url = f"http://{args.host}:{args.port}/robot/reply"
    payload = json.dumps({"text": text, "speak": not args.no_speak}).encode("utf-8")
    req = urllib.request.Request(
        url, data=payload, method="POST", headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            print(resp.read().decode())
    except urllib.error.HTTPError as exc:
        print(exc.read().decode() or str(exc), file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"failed to POST {url}: {exc}", file=sys.stderr)
        print("Is tools/stt_server.py running?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
