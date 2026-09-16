#!/usr/bin/env python3
"""Generate a local macOS say vs. Supertonic 3 Korean TTS comparison."""

from __future__ import annotations

import argparse
import html
import json
import os
import subprocess
import time
from datetime import datetime
from importlib.metadata import version
from pathlib import Path


TEST_SENTENCES = [
    ("greeting", "안녕하세요. 만나서 반가워요."),
    ("flow", "오늘은 따뜻한 바람이 불어서 기분이 정말 좋아요."),
    ("numbers", "지금은 오전 열한 시 사십이 분이고, 배터리는 팔십칠 퍼센트 남아 있어요."),
    ("mixed", "와이파이와 ESP32 연결 상태를 다시 확인했어요."),
    ("emotion", "정말요? 와, 그건 아주 멋진 생각이에요!"),
    (
        "long",
        "천천히 이야기해도 괜찮아요. 저는 당신의 말을 끝까지 듣고, 가장 알맞은 답을 생각해 볼게요.",
    ),
]


def run(command: list[str]) -> None:
    subprocess.run(command, check=True, capture_output=True, text=True)


def korean_voice() -> str:
    listing = subprocess.check_output(
        ["say", "-v", "?"], text=True, stderr=subprocess.DEVNULL
    )
    first = ""
    for line in listing.splitlines():
        if "ko_KR" not in line:
            continue
        name = line.split()[0]
        if name.lower() == "yuna":
            return name
        if not first:
            first = name
    return first or "Yuna"


def convert_wav(source: Path, destination: Path, sample_rate: int | None) -> None:
    data_format = "LEI16" if sample_rate is None else f"LEI16@{sample_rate}"
    run(
        [
            "afconvert",
            "-f",
            "WAVE",
            "-d",
            data_format,
            "-c",
            "1",
            str(source),
            str(destination),
        ]
    )


def audio_player(label: str, path: Path) -> str:
    return (
        f'<div class="player"><strong>{html.escape(label)}</strong>'
        f'<audio controls preload="none" src="{html.escape(path.name)}"></audio></div>'
    )


def write_report(
    output_dir: Path,
    rows: list[dict],
    mac_voice: str,
    model_load_seconds: float,
    supertonic_version: str,
) -> Path:
    cards = []
    for index, row in enumerate(rows, 1):
        cards.append(
            f"""
            <section class="card">
              <h2>{index}. {html.escape(row["text"])}</h2>
              <p class="timing">생성 시간 — macOS say {row["say_seconds"]:.2f}초 ·
                 Supertonic {row["supertonic_seconds"]:.2f}초</p>
              <h3>실제 로봇 조건: 16kHz mono</h3>
              <div class="players">
                {audio_player("A · macOS say", Path(row["say_16k"]))}
                {audio_player("B · Supertonic 3 F1", Path(row["supertonic_16k"]))}
              </div>
              <details>
                <summary>원본 음질도 비교</summary>
                <div class="players">
                  {audio_player("macOS say 원본", Path(row["say_native"]))}
                  {audio_player("Supertonic 44.1kHz 원본", Path(row["supertonic_native"]))}
                </div>
              </details>
            </section>
            """
        )

    report = f"""<!doctype html>
<html lang="ko">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>한국어 TTS A/B 테스트</title>
  <style>
    :root {{ color-scheme: light dark; font-family: -apple-system, sans-serif; }}
    body {{ max-width: 920px; margin: 0 auto; padding: 28px 18px 60px; }}
    h1 {{ margin-bottom: 8px; }}
    .intro, .timing {{ color: #888; }}
    .card {{ border: 1px solid #7776; border-radius: 16px; padding: 18px;
             margin-top: 18px; background: #8881; }}
    .card h2 {{ font-size: 18px; line-height: 1.55; }}
    .card h3 {{ font-size: 14px; margin-top: 20px; }}
    .players {{ display: grid; grid-template-columns: repeat(auto-fit,minmax(280px,1fr));
                gap: 12px; }}
    .player {{ display: grid; gap: 8px; padding: 12px; border-radius: 12px;
               background: #8882; }}
    audio {{ width: 100%; }}
    details {{ margin-top: 16px; }}
    code {{ background: #8882; padding: 2px 5px; border-radius: 5px; }}
  </style>
</head>
<body>
  <h1>한국어 TTS A/B 테스트</h1>
  <p class="intro">먼저 16kHz 버전을 들어 실제 로봇 조건을 비교하세요.
  발음 누락, 음절 연결, 억양, 시작 지연을 확인한 뒤 원본 음질도 들어보세요.</p>
  <p class="intro">A: macOS {html.escape(mac_voice)} · B: Supertonic 3 F1
  · Supertonic {html.escape(supertonic_version)}
  · 최초 모델 준비 {model_load_seconds:.2f}초</p>
  {''.join(cards)}
</body>
</html>
"""
    report_path = output_dir / "index.html"
    report_path.write_text(report, encoding="utf-8")
    return report_path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--voice", default="F1", help="Supertonic voice style")
    parser.add_argument("--steps", type=int, default=8, help="quality steps")
    parser.add_argument("--speed", type=float, default=1.0)
    parser.add_argument("--say-rate", type=int, default=180)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()

    project_dir = Path(__file__).resolve().parent.parent
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = (
        args.output_dir or project_dir / ".robot-runtime" / "tts-ab" / stamp
    ).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    mac_voice = korean_voice()
    rows: list[dict] = []

    print(f"[A/B] Output: {output_dir}")
    print(f"[A/B] macOS voice={mac_voice} rate={args.say_rate}")
    for index, (slug, text) in enumerate(TEST_SENTENCES, 1):
        prefix = f"{index:02d}_{slug}"
        say_aiff = output_dir / f"{prefix}_macos.aiff"
        say_native = output_dir / f"{prefix}_macos_native.wav"
        say_16k = output_dir / f"{prefix}_macos_16k.wav"
        started = time.perf_counter()
        run(
            [
                "say",
                "-v",
                mac_voice,
                "-r",
                str(args.say_rate),
                "-o",
                str(say_aiff),
                text,
            ]
        )
        say_seconds = time.perf_counter() - started
        convert_wav(say_aiff, say_native, None)
        convert_wav(say_aiff, say_16k, 16000)
        say_aiff.unlink()
        rows.append(
            {
                "text": text,
                "say_seconds": say_seconds,
                "say_native": say_native.name,
                "say_16k": say_16k.name,
            }
        )

    print("[A/B] Loading Supertonic 3 (first run downloads model assets)...")
    load_started = time.perf_counter()
    # ONNX Runtime may create a tiny telemetry session file in the process CWD.
    # Keep any such runtime artifact beside the git-ignored comparison output.
    os.chdir(output_dir)
    from supertonic import TTS

    supertonic_version = version("supertonic")
    tts = TTS(model="supertonic-3", auto_download=True)
    style = tts.get_voice_style(args.voice)
    model_load_seconds = time.perf_counter() - load_started
    print(
        f"[A/B] Supertonic ready in {model_load_seconds:.2f}s "
        f"voice={args.voice} sample_rate={tts.sample_rate}"
    )

    for index, ((slug, text), row) in enumerate(zip(TEST_SENTENCES, rows), 1):
        prefix = f"{index:02d}_{slug}"
        native = output_dir / f"{prefix}_supertonic_native.wav"
        converted = output_dir / f"{prefix}_supertonic_16k.wav"
        started = time.perf_counter()
        wav, _ = tts.synthesize(
            text,
            voice_style=style,
            lang="ko",
            total_steps=args.steps,
            speed=args.speed,
            silence_duration=0.2,
        )
        synthesis_seconds = time.perf_counter() - started
        tts.save_audio(wav, str(native))
        convert_wav(native, converted, 16000)
        row.update(
            {
                "supertonic_seconds": synthesis_seconds,
                "supertonic_native": native.name,
                "supertonic_16k": converted.name,
            }
        )
        print(
            f"[A/B] {index}/{len(TEST_SENTENCES)} Supertonic "
            f"{synthesis_seconds:.2f}s — {text}"
        )

    metadata = {
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "macos": {"voice": mac_voice, "rate": args.say_rate},
        "supertonic": {
            "version": supertonic_version,
            "model": "supertonic-3",
            "voice": args.voice,
            "steps": args.steps,
            "speed": args.speed,
            "model_load_seconds": round(model_load_seconds, 3),
        },
        "rows": rows,
    }
    (output_dir / "results.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    report = write_report(
        output_dir, rows, mac_voice, model_load_seconds, supertonic_version
    )
    print(f"[A/B] READY: {report}")


if __name__ == "__main__":
    main()
