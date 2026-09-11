# change_esp32s3

Voice assistant firmware (push-to-talk mic → STT → LLM → TTS playback, with an
animated LCD face) migrated from a classic ESP32 DevKit to the
**Waveshare ESP32-S3-AUDIO-Board**.

This repository continues the work from `speaker_connect_tts`, but on completely
new hardware. The microphone, speaker, display and button interfaces have been
rewritten and verified on the new board.

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
| TCA9555 | EXIO0 LCD_RST, EXIO1 TP_RST, EXIO2 TP_INT, EXIO8 PA_CTRL (amp enable), EXIO9–11 K1–K3 |
| Buttons | K1 = TCA9555 EXIO9 (push-to-talk); BOOT = GPIO0 (firmware recovery) |

Full details, board layout photo and the step-by-step bring-up checklist are in
[`docs/hardware_migration_esp32s3.md`](docs/hardware_migration_esp32s3.md) (Korean).

## Project layout

```
platformio.ini             board config: esp32-s3-devkitc-1 + 16MB/qio_opi overrides
include/hardware_pins.h    pin map for the new board
include/tca9555.h          minimal TCA9555 IO-expander driver
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
4. Hold the board's **K1** button while speaking and release it when finished.

The launcher automatically:

- detects the USB board and current Mac LAN IP;
- updates the local firmware server address;
- builds and uploads the latest PlatformIO firmware;
- checks or installs Python STT dependencies;
- starts Ollama when available;
- starts the Korean STT/LLM/TTS server; and
- opens the local web interface.

On first use, macOS may require right-clicking the command and choosing **Open**.
The first Whisper model/dependency setup can also take several minutes. Runtime
logs are written under the git-ignored `.robot-runtime/` directory.

To stop the local pipeline, double-click **`Stop Robot.command`**. It only stops
this project's port-3000 STT/LLM/TTS server. The ESP32 may remain connected, and
Ollama is intentionally left running because other applications may use it.

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
| 6 | Push-to-talk using built-in K1 (TCA9555 EXIO9) | done |
| 7 | Full app: K1 → STT → Ollama → TTS → emotion face | done |
| 8 | Optional: touch, RGB LED ring, echo cancellation | later |

## Toolchain notes

- PlatformIO `espressif32` 6.x with Arduino core 2.0.17 (ESP-IDF 4.4). The
  legacy `driver/i2s.h` API is used on purpose; do not upgrade to Arduino
  core 3.x without revisiting the I2S code.
- `ARDUINO_USB_CDC_ON_BOOT=1` is required for `Serial` output on this board.
