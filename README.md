# change_esp32s3

Voice assistant firmware (push-to-talk mic → STT → LLM → TTS playback, with an
animated LCD face) migrated from a classic ESP32 DevKit to the
**Waveshare ESP32-S3-AUDIO-Board**.

This repository continues the work from `speaker_connect_tts`, but on completely
new hardware. The microphone, speaker, display and button interfaces have been
rewritten and verified on the new board. TTS audio is received into a
PSRAM-backed three-second ring buffer and played by a separate task to avoid
gaps at one-second HTTP chunk boundaries.

## Hardware

| Part | Model | Notes |
|---|---|---|
| Main board | [Waveshare ESP32-S3-AUDIO-Board](https://www.waveshare.com/wiki/ESP32-S3-AUDIO-Board) | ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, dual digital mic array (ES7210), audio codec (ES8311), built-in speaker + amp, TCA9555 IO expander, RTC, 7× RGB LEDs |
| Display | [Waveshare 2inch Capacitive Touch LCD](https://www.waveshare.com/wiki/2inch_Capacitive_Touch_LCD) | ST7789T3, 240×320, CST816D touch, connected via the board's 18-pin FPC display connector |
| Host connection | USB-C (native USB CDC, no UART bridge) | |

The speaker is factory-mounted inside the board housing; nothing external is wired.

### Pin map (summary)

| Bus | Pins |
|---|---|
| I2C (shared) | SDA 11, SCL 10 — ES8311 0x18, TCA9555 0x20, ES7210 0x40, PCF85063 0x51, CST816D 0x15 |
| I2S (shared TX+RX) | MCLK 12, BCLK 13, LRCLK 14, DIN 15 (mic), DOUT 16 (speaker) |
| LCD SPI | CS 3, SCK 4, MOSI 9, MISO 8, DC 7, BL 5, RST → TCA9555 EXIO0 |
| TCA9555 | EXIO0 LCD_RST, EXIO1 TP_RST, EXIO2 TP_INT, EXIO8 PA_CTRL (amp enable), EXIO9 K1 unused, EXIO10–11 K2/K3 volume |
| Buttons | PTT = full-screen LCD touch; K2 = volume up, K3 = volume down; BOOT = GPIO0 (firmware recovery) |

Full details, board layout photo and the step-by-step bring-up checklist are in
[`docs/hardware_migration_esp32s3.md`](docs/hardware_migration_esp32s3.md) (Korean).

## Project layout

```
platformio.ini             board config: esp32-s3-devkitc-1 + 16MB/qio_opi overrides
include/hardware_pins.h    pin map for the new board
include/tca9555.h          minimal TCA9555 IO-expander driver
include/cst816d.h          minimal CST816D touch driver
include/*.example.h        templates for WiFi / server secrets
src/main.cpp               integrated Waveshare application (stage 7)
src/main.cpp.full          untouched legacy application reference
tools/                     FastAPI STT, Ollama LLM, macOS TTS and web UI
Start Robot.command        macOS double-click launcher
Stop Robot.command         stops this project's local voice server
docs/                      migration guide and images
```

## One-click start on macOS

1. Connect the ESP32-S3 to the Mac with its USB-C data cable.
2. Double-click **`Start Robot.command`** in Finder.
3. Wait for `READY`, then use the browser page opened at
   `http://localhost:3000`.
4. Hold a finger on the LCD while speaking and lift it when finished.

The launcher automatically:

- detects the USB board and current Mac LAN IP;
- updates the local firmware server address;
- builds and uploads the latest PlatformIO firmware;
- checks or installs Python STT dependencies;
- starts Ollama when available;
- starts the isolated Supertonic 3 F1 TTS service;
- starts the isolated Qwen3-ASR 0.6B STT service;
- starts the Korean STT/LLM/TTS server; and
- opens the local web interface.

On first use, macOS may require right-clicking the command and choosing **Open**.
The first Qwen and Whisper model/dependency setup can also take several minutes.
Runtime logs are written under the git-ignored `.robot-runtime/` directory.

The robot's default STT is Qwen3-ASR 0.6B. faster-whisper stays loaded as an
automatic fallback whenever the local Qwen sidecar is unavailable or a
transcription fails. Quiet chunks below `-46` dBFS are still ignored.

To stop the local pipeline, double-click **`Stop Robot.command`**. It stops this
project's port-3000 server, port-3001 Supertonic service, and port-3002 Qwen
service. The ESP32 may remain connected, and Ollama is intentionally left
running because other applications may use it.

## Character display

The character face is now the default UI: neutral while booting/idle/thinking,
surprised while listening, and the LLM-selected emotion while speaking. Each
emotion uses eight 240×240 source frames.

In the PlatformIO serial monitor, press **Control+W** to toggle the developer
status screen. Press it again to return to the character UI.

## TTS A/B comparison

Double-click **`Run TTS A-B Test.command`** to compare the current macOS
`say` voice with Supertonic 3 F1. It generates matching native and
robot-format 16 kHz WAV files, records synthesis times, and opens a local
listening page. Results are stored under the git-ignored
`.robot-runtime/tts-ab/` directory.

The A/B result selected Supertonic 3 F1 as the robot's default TTS. The
integration still keeps macOS `say` as an automatic fallback whenever the local
Supertonic service is unavailable or synthesis fails. It uses an isolated
Python 3.11/3.12 environment; the first run downloads roughly 400 MB of model
assets.

## STT A/B comparison

Double-click **`Run STT A-B Test.command`** to compare Qwen3-ASR 0.6B with
Moonshine Korean Tiny using the exact same robot-microphone recording. Stop the
normal robot pipeline first because both servers receive firmware uploads on
port 3000. The comparison page opens automatically, prepares both models, and
queues a test whenever the LCD finger is lifted. A 16 kHz, 16-bit, mono WAV can also be
uploaded from the page.

Each result includes both transcripts, inference times, the captured audio and
an optional character error rate (CER) calculated from the sentence you
actually spoke. Execution order alternates between tests to reduce first-run
and thermal bias. Recordings and JSON results are kept under the git-ignored
`.robot-runtime/stt-ab/` directory.

The test uses an isolated `.venv-stt-ab` Python environment and does not change
the production default. Production now uses Qwen3-ASR 0.6B, with faster-whisper
kept only as fallback. Qwen3-ASR uses the Apache-2.0 model via an Apple-Silicon
MLX runtime. The current Korean Moonshine Tiny checkpoint uses the
non-commercial Moonshine Community License. Its first download is small; the
Qwen model download is substantially larger and may take several minutes.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (VS Code extension or CLI).

```bash
cp include/wifi_credentials.example.h include/wifi_credentials.h
cp include/server_config.example.h    include/server_config.h
# edit both files with your WiFi and server details

pio run -e esp32s3_audio -t upload
pio device monitor -b 115200
```

`wifi_credentials.h` and `server_config.h` are git-ignored.

If the board is not detected for upload, hold **BOOT** (right edge, second
button below RESET) while plugging in USB, then release.

## Migration status

| Stage | Description | Status |
|---|---|---|
| 0 | `platformio.ini` for ESP32-S3R8 (16 MB flash, 8 MB OPI PSRAM, USB CDC) | done |
| 1 | Boot, USB serial, flash/PSRAM verification | done |
| 2 | I2C bus + TCA9555; all 5 I2C devices detected, FPC link verified | done |
| 3 | LCD bring-up (ST7789, backlight, 240×320) | done |
| 4 | Speaker output via ES8311 codec + PA enable | done |
| 5 | Microphone input via ES7210 ADC (live stereo level meter) | done |
| 6 | Push-to-talk using built-in K1 (TCA9555 EXIO9) | superseded |
| 7 | Full app: K1 → STT → Ollama → TTS → emotion face | done |
| 8 | Full-screen LCD touch PTT (CST816D); RGB LED ring, echo cancellation | PTT done |

## Toolchain notes

- PlatformIO `espressif32` 6.x with Arduino core 2.0.17 (ESP-IDF 4.4). The
  legacy `driver/i2s.h` API is used on purpose; do not upgrade to Arduino
  core 3.x without revisiting the I2S code.
- `ARDUINO_USB_CDC_ON_BOOT=1` is required for `Serial` output on this board.
