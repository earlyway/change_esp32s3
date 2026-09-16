#!/usr/bin/env python3
"""Ask the running STT server to speak a sentence on the Mac speaker.

Usage:
    python3 tools/speak.py 안녕하세요
    python3 tools/speak.py --emotion positive "오늘 날씨 정말 좋다"

The server (tools/stt_server.py) must already be running. The ESP32 only
mirrors the state/emotion on its face via GET /speaker/pull.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("text", nargs="+", help="Sentence to speak")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=3000)
    parser.add_argument("--emotion", default="none")
    args = parser.parse_args()
    text = " ".join(args.text).strip()

    url = f"http://{args.host}:{args.port}/speaker/say"
    payload = json.dumps({"text": text, "emotion": args.emotion}).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=payload,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
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
