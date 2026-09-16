#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <freertos/stream_buffer.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#include "hardware_pins.h"
#include "tca9555.h"
#include "cst816d.h"
#include "es8311.h"
#include "es7210.h"
#include "server_config.h"
#include "wifi_credentials.h"
#include "animation_frames.h"

namespace {
// Waveshare 2inch ST7789T3: native 240x320, shown landscape as 320x240.
constexpr int kTftWidth = 320;
constexpr int kTftHeight = 240;
Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
TCA9555 exio(I2C_ADDR_TCA9555);
CST816D touch(I2C_ADDR_CST816D);
bool displayReady = false;
bool touchReady = false;

// Waveshare audio board: ES7210 microphones and ES8311 speaker share I2S0.
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr i2s_port_t SPK_I2S_PORT = I2S_PORT;
constexpr size_t kI2sReadFrames = 256;
int16_t i2sRawBuffer[kI2sReadFrames * 2]; // interleaved L/R from ES7210
es8311_handle_t speakerCodec = nullptr;
es7210_dev_handle_t micCodec = nullptr;
bool micReady = false;
bool speakerReady = false;
constexpr int kDefaultSpeakerVolume = 90;
constexpr int kSpeakerVolumeStep = 10;
volatile int speakerVolume = kDefaultSpeakerVolume;

enum TestTune : int {
  kTuneNone = 0,
  kTuneBeep = 1,
  kTuneRedRed = 2,
};
int testTunePlaying = kTuneNone;
int pendingSerialChar = -1;

int readSerialChar() {
  if (pendingSerialChar >= 0) {
    const int c = pendingSerialChar;
    pendingSerialChar = -1;
    return c;
  }
  if (Serial.available() > 0) return Serial.read();
  return -1;
}

bool testTuneStopRequested() {
  while (true) {
    const int c = (pendingSerialChar >= 0 || Serial.available() > 0)
                      ? readSerialChar()
                      : -1;
    if (c < 0) return false;
    const bool stopBeep = testTunePlaying == kTuneBeep && (c == 'b' || c == 'B');
    const bool stopRedRed =
        testTunePlaying == kTuneRedRed && (c == 'p' || c == 'P');
    if (!stopBeep && !stopRedRed) {
      pendingSerialChar = c;
      return false;
    }
    // Drop extra repeats of the same key so loop() does not restart playback.
    while (Serial.available() > 0) {
      const int next = Serial.peek();
      if ((stopBeep && (next == 'b' || next == 'B')) ||
          (stopRedRed && (next == 'p' || next == 'P'))) {
        Serial.read();
      } else {
        break;
      }
    }
    return true;
  }
}

// Speaker downlink uses a 3-second ring buffer. The network task fills it while
// a dedicated playback task drains it, so one-second HTTP chunk boundaries do
// not reset I2S or create audible gaps.
constexpr size_t kSpeakerBytesPerSecond = AUDIO_SAMPLE_RATE * sizeof(int16_t);
constexpr size_t kSpeakerRingBytes = kSpeakerBytesPerSecond * 3;
constexpr size_t kSpeakerPrebufferBytes = kSpeakerBytesPerSecond / 4; // 250 ms
StreamBufferHandle_t speakerPcmStream = nullptr;
StaticStreamBuffer_t speakerPcmStreamControl;
uint8_t* speakerPcmStorage = nullptr;
volatile bool speakerDownlinkPlaying = false;
volatile bool speakerStreamOpen = false;
volatile bool speakerStreamEndPending = false;
volatile uint32_t speakerLastDataMs = 0;
char speakerEmotion[24] = "none";
// Phase 4: -1 = no face. faceHoldUntilMs is millis() deadline (Arduino wrap-safe).
volatile int faceEmotionIndex = -1;
volatile uint32_t faceHoldUntilMs = 0;

enum RobotUi : int {
  kUiIdle = 0,
  kUiListen = 1,
  kUiThink = 2,
  kUiSpeak = 3,
};
volatile int serverRobotState = kUiIdle;
volatile bool speakerBargeIn = false;
// Set by audioTask once the PTT tail chunk has been handed off; networkTask
// then POSTs /utterance/end so the Mac finalizes STT without waiting.
volatile bool pttEndPending = false;
int lastLoggedUi = -1;

// What networkTask is doing right now; printed when a mic chunk overflows so
// the cause (slow POST vs. speaker pull vs. flush) is visible in the log.
enum NetActivity : int {
  kNetIdle = 0,
  kNetPost = 1,
  kNetPull = 2,
  kNetFlush = 3,
  kNetEnd = 4,
};
volatile int netActivity = kNetIdle;

const char* netActivityName(int a) {
  switch (a) {
    case kNetPost: return "POST";
    case kNetPull: return "speaker pull";
    case kNetFlush: return "speaker flush";
    case kNetEnd: return "utterance/end";
    default: return "idle";
  }
}

// 16kHz, 16-bit Mono PCM. Three buffers let one chunk POST while another waits
// and capture continues, so a slow upload no longer drops the next second or
// the PTT tail.
constexpr size_t kChunkSamples = 16000;
int16_t micBuffers[3][kChunkSamples];
int16_t* activeWriteBuffer = micBuffers[0];
volatile int16_t* activeSendBuffer = nullptr;
volatile int16_t* pendingSendBuffer = nullptr;
volatile size_t writeIndex = 0;
volatile size_t sendBufferSize = 0;
volatile size_t pendingSendSize = 0;

SemaphoreHandle_t xSendSemaphore = nullptr;

int16_t* unusedMicBuffer() {
  for (int i = 0; i < 3; ++i) {
    int16_t* candidate = micBuffers[i];
    if (candidate != activeWriteBuffer &&
        candidate != activeSendBuffer &&
        candidate != pendingSendBuffer) {
      return candidate;
    }
  }
  return nullptr;
}

// Queue a captured buffer for POST. If both slots are full, keep the in-flight
// POST and replace the waiting chunk with fresher audio (especially the PTT
// tail) instead of dropping the newest second.
bool enqueueMicChunk(int16_t* buf, size_t bytes, bool isTail) {
  if (bytes < 2) {
    return false;
  }
  if (activeSendBuffer == nullptr) {
    activeSendBuffer = buf;
    sendBufferSize = bytes;
    xSemaphoreGive(xSendSemaphore);
    return true;
  }
  if (pendingSendBuffer == nullptr) {
    pendingSendBuffer = buf;
    pendingSendSize = bytes;
    return true;
  }
  pendingSendBuffer = buf;
  pendingSendSize = bytes;
  Serial.printf("[Task] Replaced waiting chunk with %s\n",
                isTail ? "PTT tail" : "newer audio");
  return true;
}

// dBFS calculations for 16-bit PCM from ES7210.
constexpr int32_t kFullScale = 32768;
constexpr float kFullScaleF = static_cast<float>(kFullScale);
constexpr float kFloorDbfs = -80.0f;
constexpr int kBarWidth = 40;

struct LevelStats {
  int64_t sumAbs = 0;
  uint32_t sampleCount = 0;
  int32_t peak = 0;
};
LevelStats stats;
uint32_t lastPrintMs = 0;
float lastPeakDbfs = kFloorDbfs;
float lastAvgDbfs = kFloorDbfs;
volatile uint32_t postedChunks = 0;
volatile int lastHttpCode = 0;
volatile uint32_t lastHttpDurationMs = 0;

// Push-to-talk: audio is captured/uploaded only while a finger is on the LCD.
volatile bool micActive = false;

// Character UI is the default. Ctrl+W toggles the developer status screen;
// 'q' still runs the full emotion demo while idle.
constexpr uint32_t kSpeakFrameIntervalMs = 70;
constexpr uint32_t kListenFrameIntervalMs = 80;
constexpr uint32_t kThinkFrameIntervalMs = 100;
constexpr uint32_t kIdleFrameIntervalMs = 120;
volatile bool diagnosticDisplayMode = false;
volatile bool animationMode = false;

float magnitudeToDbfs(int32_t magnitude) {
  if (magnitude <= 0) return kFloorDbfs;
  float dbfs = 20.0f * log10f(static_cast<float>(magnitude) / kFullScaleF);
  if (dbfs < kFloorDbfs) return kFloorDbfs;
  if (dbfs > 0.0f) return 0.0f;
  return dbfs;
}

const char* labelFor(float peakDbfs) {
  if (peakDbfs >= -3.0f)  return "CLIPPING!";
  if (peakDbfs >= -10.0f) return "VERY LOUD";
  if (peakDbfs >= -25.0f) return "LOUD     ";
  if (peakDbfs >= -50.0f) return "NORMAL   ";
  return                          "QUIET    ";
}

void printDbfsBar() {
  if (stats.sampleCount == 0) return;
  int32_t average = static_cast<int32_t>(stats.sumAbs / stats.sampleCount);
  float peakDbfs = magnitudeToDbfs(stats.peak);
  float avgDbfs = magnitudeToDbfs(average);
  lastPeakDbfs = peakDbfs;
  lastAvgDbfs = avgDbfs;

  int bars = static_cast<int>((peakDbfs - kFloorDbfs) / -kFloorDbfs * kBarWidth);
  if (bars < 0) bars = 0;
  if (bars > kBarWidth) bars = kBarWidth;

  char meter[kBarWidth + 1];
  for (int i = 0; i < kBarWidth; ++i) {
    meter[i] = (i < bars) ? '#' : '.';
  }
  meter[kBarWidth] = '\0';

  Serial.printf("[Mic] Peak: %6.1f dB | Avg: %6.1f dB | %s | %s\n",
                static_cast<double>(peakDbfs),
                static_cast<double>(avgDbfs),
                meter,
                labelFor(peakDbfs));

  stats = LevelStats{};
}

void drawEmotionFrame(int emotion, size_t localIndex);

bool initDisplay() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!exio.begin()) {
    Serial.println("[Board] TCA9555 not found at 0x20");
    return false;
  }

  exio.pinMode(EXIO_LCD_RST, OUTPUT);
  exio.pinMode(EXIO_TP_RST, OUTPUT);
  exio.pinMode(EXIO_TP_INT, INPUT);
  exio.pinMode(EXIO_PA_CTRL, OUTPUT);
  exio.pinMode(EXIO_KEY1, INPUT);
  exio.pinMode(EXIO_KEY2, INPUT);
  exio.pinMode(EXIO_KEY3, INPUT);
  exio.digitalWrite(EXIO_PA_CTRL, LOW);
  exio.digitalWrite(EXIO_LCD_RST, LOW);
  exio.digitalWrite(EXIO_TP_RST, LOW);
  delay(20);
  exio.digitalWrite(EXIO_LCD_RST, HIGH);
  exio.digitalWrite(EXIO_TP_RST, HIGH);
  delay(150);

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  SPI.begin(PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setSPISpeed(40000000);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(ST77XX_BLACK);

  displayReady = true;
  touchReady = touch.begin();
  if (touchReady) {
    Serial.printf("[Touch] CST816D ready chip=0x%02X (hold the LCD to capture)\n",
                  touch.chipId());
  } else {
    Serial.println("[Touch] CST816D not found; PTT disabled");
  }
  drawEmotionFrame(ANIM_EMOTION_NEUTRAL, 0);
  Serial.printf("[TFT] Init OK (%dx%d) CS=%d DC=%d RST=%d\n",
                tft.width(), tft.height(),
                PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
  return true;
}

const char* uiName(int ui) {
  switch (ui) {
    case kUiListen: return "listen";
    case kUiThink: return "think";
    case kUiSpeak: return "speak";
    default: return "idle";
  }
}

int parseRobotState(const char* tag) {
  if (tag == nullptr || tag[0] == '\0') {
    return kUiIdle;
  }
  if (strcasecmp(tag, "thinking") == 0) {
    return kUiThink;
  }
  if (strcasecmp(tag, "speaking") == 0) {
    return kUiSpeak;
  }
  return kUiIdle;
}

void applyServerRobotState(const String& header) {
  if (header.length() == 0) {
    return;
  }
  serverRobotState = parseRobotState(header.c_str());
}

bool faceHoldActive(uint32_t now) {
  const int face = faceEmotionIndex;
  return face >= 0 && face < ANIM_EMOTION_COUNT && (int32_t)(now - faceHoldUntilMs) < 0;
}

int effectiveUi(uint32_t now) {
  if (micActive) {
    return kUiListen;
  }
  // Keep the face up while PCM is being received, buffered, or played.
  if (speakerStreamOpen || speakerDownlinkPlaying || faceHoldActive(now)) {
    return kUiSpeak;
  }
  if (serverRobotState == kUiThink || serverRobotState == kUiSpeak) {
    return kUiThink;
  }
  return kUiIdle;
}

void logUiIfChanged(int ui) {
  if (ui == lastLoggedUi) {
    return;
  }
  lastLoggedUi = ui;
  Serial.printf("[State] %s\n", uiName(ui));
}

void drawThinkingScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(20, 110);
  tft.println("THINKING");
}

void drawStatusRow(int16_t y, const char* label, const char* value,
                   uint16_t valueColor = ST77XX_WHITE) {
  // Clear and redraw only this text row. Clearing the whole screen every
  // 500 ms caused a visible black flash on the ST7789.
  tft.fillRect(0, y, tft.width(), 21, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(0, 0);
  tft.setCursor(4, y + 2);
  tft.print(label);
  tft.setTextColor(valueColor);
  tft.print(value);
}

void drawStatusScreen() {
  char level[24];
  char post[32];
  char chunks[16];
  snprintf(level, sizeof(level), "%.1f dB", static_cast<double>(lastPeakDbfs));
  if (lastHttpCode > 0) {
    snprintf(post, sizeof(post), "%d %lums", lastHttpCode,
             static_cast<unsigned long>(lastHttpDurationMs));
  } else {
    strncpy(post, "-", sizeof(post));
    post[sizeof(post) - 1] = '\0';
  }
  snprintf(chunks, sizeof(chunks), "%lu", static_cast<unsigned long>(postedChunks));

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const String ip = wifiConnected ? WiFi.localIP().toString() : "-";
  drawStatusRow(0,   "",        "ESP32-S3 AUDIO", ST77XX_CYAN);
  drawStatusRow(24,  "Mode: ",  micActive ? "LISTEN" : uiName(effectiveUi(millis())),
                micActive ? ST77XX_GREEN : ST77XX_WHITE);
  drawStatusRow(48,  "WiFi: ",  wifiConnected ? "OK" : "WAIT",
                wifiConnected ? ST77XX_GREEN : ST77XX_YELLOW);
  drawStatusRow(72,  "IP: ",    ip.c_str());
  drawStatusRow(96,  "Level: ", level);
  drawStatusRow(120, "Mic: ",   micActive ? "ON" : "OFF",
                micActive ? ST77XX_GREEN : ST77XX_WHITE);
  drawStatusRow(144, "POST: ",  post);
  drawStatusRow(168, "Chunks: ", chunks);
  drawStatusRow(192, "Spk: ",   speakerDownlinkPlaying ? "PLAY" : "IDLE",
                speakerDownlinkPlaying ? ST77XX_GREEN : ST77XX_WHITE);
  char volume[16];
  snprintf(volume, sizeof(volume), "%d", speakerVolume);
  drawStatusRow(216, "Vol: ", volume);
}

void drawAnimationFrame(size_t frameIndex) {
  constexpr int16_t x = (kTftWidth - ANIM_FRAME_WIDTH) / 2;
  constexpr int16_t y = (kTftHeight - ANIM_FRAME_HEIGHT) / 2;
  tft.drawRGBBitmap(x, y, animFrames[frameIndex], ANIM_FRAME_WIDTH, ANIM_FRAME_HEIGHT);
}

int emotionIndexFromTag(const char* tag) {
  if (tag == nullptr || tag[0] == '\0') {
    return ANIM_EMOTION_NEUTRAL;
  }
  if (strcasecmp(tag, "positive") == 0) {
    return ANIM_EMOTION_POSITIVE;
  }
  if (strcasecmp(tag, "neutral") == 0 || strcasecmp(tag, "none") == 0) {
    return ANIM_EMOTION_NEUTRAL;
  }
  if (strcasecmp(tag, "sad") == 0) {
    return ANIM_EMOTION_SAD;
  }
  if (strcasecmp(tag, "angry") == 0) {
    return ANIM_EMOTION_ANGRY;
  }
  if (strcasecmp(tag, "surprised") == 0) {
    return ANIM_EMOTION_SURPRISED;
  }
  return ANIM_EMOTION_NEUTRAL;
}

void drawEmotionFrame(int emotion, size_t localIndex) {
  constexpr int16_t x = (kTftWidth - ANIM_FRAME_WIDTH) / 2;
  constexpr int16_t y = (kTftHeight - ANIM_FRAME_HEIGHT) / 2;
  if (emotion < 0 || emotion >= ANIM_EMOTION_COUNT) {
    emotion = ANIM_EMOTION_NEUTRAL;
  }
  localIndex %= ANIM_FRAMES_PER_EMOTION;
  tft.drawRGBBitmap(
      x, y,
      animEmotionFrames[emotion][localIndex],
      ANIM_FRAME_WIDTH, ANIM_FRAME_HEIGHT);
}

void displayTask(void* pvParameters) {
  Serial.println("[Task] Display task started on Core 0");

  size_t animFrameIndex = 0;
  size_t faceFrameIndex = 0;
  int lastFaceEmotion = -1;
  uint32_t lastStatusMs = 0;
  uint32_t lastAnimMs = 0;
  int lastDrawnUi = -1;
  bool lastDiagnosticMode = false;
  bool showingDemo = false;
  int thinkDirection = 1;

  while (true) {
    if (displayReady) {
      uint32_t now = millis();
      const int ui = effectiveUi(now);
      logUiIfChanged(ui);

      if (diagnosticDisplayMode) {
        if (!lastDiagnosticMode) {
          tft.fillScreen(ST77XX_BLACK);
        }
        if (!lastDiagnosticMode || now - lastStatusMs >= 500) {
          lastStatusMs = now;
          drawStatusScreen();
        }
      } else if (animationMode && ui == kUiIdle) {
        if (!showingDemo || lastDiagnosticMode) {
          tft.fillScreen(ST77XX_BLACK);
          showingDemo = true;
          animFrameIndex = 0;
        }
        if (now - lastAnimMs >= kSpeakFrameIntervalMs) {
          lastAnimMs = now;
          drawAnimationFrame(animFrameIndex);
          animFrameIndex++;
          if (animFrameIndex >= ANIM_FRAME_COUNT) {
            animFrameIndex = 0;
            animationMode = false;
            Serial.println("[Display] Animation finished, back to character UI");
          }
        }
      } else {
        int face = ANIM_EMOTION_NEUTRAL;
        if (ui == kUiListen) {
          face = ANIM_EMOTION_SURPRISED;
        } else if (ui == kUiSpeak) {
          face = faceEmotionIndex >= 0 ? faceEmotionIndex : ANIM_EMOTION_NEUTRAL;
        }

        const bool stateChanged =
            lastDiagnosticMode || showingDemo ||
            lastDrawnUi != ui || lastFaceEmotion != face;
        if (stateChanged) {
          tft.fillScreen(ST77XX_BLACK);
          faceFrameIndex = 0;
          thinkDirection = 1;
          drawEmotionFrame(face, faceFrameIndex);
          lastAnimMs = now;
        }

        uint32_t interval = kSpeakFrameIntervalMs;
        if (ui == kUiIdle) {
          interval = kIdleFrameIntervalMs;
        } else if (ui == kUiListen) {
          interval = kListenFrameIntervalMs;
        } else if (ui == kUiThink) {
          interval = kThinkFrameIntervalMs;
        }

        if (!stateChanged && now - lastAnimMs >= interval) {
          lastAnimMs = now;
          if (ui == kUiThink) {
            if (faceFrameIndex == ANIM_FRAMES_PER_EMOTION - 1) {
              thinkDirection = -1;
            } else if (faceFrameIndex == 0) {
              thinkDirection = 1;
            }
            faceFrameIndex =
                static_cast<size_t>(static_cast<int>(faceFrameIndex) + thinkDirection);
          } else {
            faceFrameIndex = (faceFrameIndex + 1) % ANIM_FRAMES_PER_EMOTION;
          }
          drawEmotionFrame(face, faceFrameIndex);
        }

        showingDemo = false;
        animFrameIndex = 0;
        lastFaceEmotion = face;
      }
      lastDiagnosticMode = diagnosticDisplayMode;
      lastDrawnUi = ui;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// One full-duplex I2S driver supplies clocks to both ES7210 (RX) and ES8311 (TX).
bool installI2sDriver() {
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX);
  i2sConfig.sample_rate = AUDIO_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8; // Increased for extra safety margin during network load
  i2sConfig.dma_buf_len = 256;
  i2sConfig.use_apll = false;
  i2sConfig.tx_desc_auto_clear = true;
  i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  esp_err_t result = i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr);
  if (result != ESP_OK) return false;

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = PIN_I2S_MCLK;
  pinConfig.bck_io_num = PIN_I2S_BCLK;
  pinConfig.ws_io_num = PIN_I2S_LRCLK;
  pinConfig.data_out_num = PIN_I2S_DOUT;
  pinConfig.data_in_num = PIN_I2S_DIN;

  result = i2s_set_pin(I2S_PORT, &pinConfig);
  if (result != ESP_OK) return false;

  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

bool setupAudioCodecs() {
  // Dual microphone ADC, matching the verified stage-5 configuration.
  es7210_i2c_config_t micI2c = {};
  micI2c.i2c_port = I2C_NUM_0;
  micI2c.i2c_addr = I2C_ADDR_ES7210;
  esp_err_t result = es7210_new_codec(&micI2c, &micCodec);
  if (result == ESP_OK) {
    es7210_codec_config_t micConfig = {};
    micConfig.sample_rate_hz = AUDIO_SAMPLE_RATE;
    micConfig.mclk_ratio = 256;
    micConfig.i2s_format = ES7210_I2S_FMT_I2S;
    micConfig.bit_width = ES7210_I2S_BITS_16B;
    micConfig.mic_bias = ES7210_MIC_BIAS_2V87;
    micConfig.mic_gain = ES7210_MIC_GAIN_30DB;
    micConfig.flags.tdm_enable = true;
    result = es7210_config_codec(micCodec, &micConfig);
    if (result == ESP_OK) {
      result = es7210_config_volume(micCodec, 0);
    }
  }
  micReady = (result == ESP_OK);
  if (!micReady) {
    Serial.printf("[Mic] ES7210 setup failed: %s\n", esp_err_to_name(result));
  }

  // Speaker DAC/codec, matching the verified stage-4 configuration.
  speakerCodec = es8311_create(I2C_NUM_0, ES8311_ADDRESS_0);
  if (speakerCodec == nullptr) {
    Serial.println("[Speaker] ES8311 create failed");
    return false;
  }
  es8311_clock_config_t speakerClock = {};
  speakerClock.mclk_inverted = false;
  speakerClock.sclk_inverted = false;
  speakerClock.mclk_from_mclk_pin = true;
  speakerClock.mclk_frequency = AUDIO_SAMPLE_RATE * 256;
  speakerClock.sample_frequency = AUDIO_SAMPLE_RATE;
  result = es8311_init(
      speakerCodec, &speakerClock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (result == ESP_OK) {
    int applied = kDefaultSpeakerVolume;
    result = es8311_voice_volume_set(
        speakerCodec, kDefaultSpeakerVolume, &applied);
    if (result == ESP_OK) {
      speakerVolume = applied;
    }
  }
  if (result == ESP_OK) {
    result = es8311_microphone_config(speakerCodec, false);
  }
  if (result != ESP_OK) {
    Serial.printf("[Speaker] ES8311 setup failed: %s\n", esp_err_to_name(result));
    return false;
  }

  // Prime TX with silence before enabling the onboard NS4150B amplifier.
  int16_t silence[128 * 2] = {};
  size_t bytesWritten = 0;
  i2s_write(I2S_PORT, silence, sizeof(silence), &bytesWritten, portMAX_DELAY);
  exio.digitalWrite(EXIO_PA_CTRL, HIGH);
  delay(50);
  return true;
}

// Play a single sine tone (freqHz == 0 -> silence/rest). Blocking.
// A short linear fade-in/out envelope avoids click noise between notes.
// Returns true if the matching serial key stopped playback early.
bool playTone(float freqHz, uint32_t durationMs, int16_t amplitude = 28000) {
  constexpr size_t kBlockFrames = 128;
  constexpr uint32_t kFadeMs = 6;

  const size_t totalFrames = static_cast<size_t>(AUDIO_SAMPLE_RATE) * durationMs / 1000;
  const size_t fadeFrames = static_cast<size_t>(AUDIO_SAMPLE_RATE) * kFadeMs / 1000;

  int16_t block[kBlockFrames * 2]; // interleaved stereo (L, R per frame)
  size_t written = 0;
  while (written < totalFrames) {
    if (testTuneStopRequested()) return true;
    size_t count = min(kBlockFrames, totalFrames - written);
    for (size_t i = 0; i < count; ++i) {
      size_t n = written + i;
      float sample = 0.0f;
      if (freqHz > 0.0f) {
        float t = static_cast<float>(n) / AUDIO_SAMPLE_RATE;
        float envelope = 1.0f;
        if (n < fadeFrames) {
          envelope = static_cast<float>(n) / fadeFrames;
        } else if (totalFrames - n <= fadeFrames) {
          envelope = static_cast<float>(totalFrames - n) / fadeFrames;
        }
        sample = envelope * amplitude * sinf(2.0f * PI * freqHz * t);
      }
      int16_t s = static_cast<int16_t>(sample);
      block[2 * i] = s;     // left slot
      block[2 * i + 1] = s; // right slot
    }
    size_t bytesWritten = 0;
    i2s_write(SPK_I2S_PORT, block, count * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    written += count;
  }
  return false;
}

void applySpeakerVolume(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  if (!speakerReady || speakerCodec == nullptr) {
    Serial.println("[Speaker] volume ignored; codec not ready");
    return;
  }
  int applied = volume;
  if (es8311_voice_volume_set(speakerCodec, volume, &applied) != ESP_OK) {
    Serial.println("[Speaker] volume set failed");
    return;
  }
  speakerVolume = applied;
  Serial.printf("[Speaker] volume=%d\n", applied);
}

// Play a short 440Hz sine beep (~0.5s) through ES8311. Blocking.
void playTestBeep() {
  if (!speakerReady) {
    Serial.println("[Speaker] Not initialized, cannot beep");
    return;
  }
  if (speakerStreamOpen || speakerDownlinkPlaying) {
    Serial.println("[Speaker] Downlink playing, skip test beep");
    return;
  }
  Serial.println("[Speaker] Test beep start (440Hz, 0.5s); press b again to stop");
  testTunePlaying = kTuneBeep;
  const bool stopped = playTone(440.0f, 500);
  testTunePlaying = kTuneNone;
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  Serial.println(stopped ? "[Speaker] Test beep stopped" : "[Speaker] Test beep done");
}

// Note frequencies (Hz), 4th-6th octave.
constexpr float NOTE_REST = 0.0f;
constexpr float NOTE_C5 = 523.25f;
constexpr float NOTE_D5 = 587.33f;

// ============================================================
// REDRED synth beat: the melody transcribed from the user's REDRED guitar
// sheet music (D minor, 121 BPM) played by a synth voice over a
// kick/snare/hi-hat drum grid. Synth engine ported from a standalone
// ESP_I2S.h sketch onto this project's shared driver/i2s.h + I2S_NUM_0 setup.
// Runs at AUDIO_SAMPLE_RATE (16kHz, same as the mic) so it reuses the
// speaker port config as-is instead of reinstalling the I2S driver.
// ============================================================
constexpr float PI2 = 2.0f * PI;
constexpr float kBeatBpm = 121.0f; // matches the REDRED sheet music tempo
constexpr float kBeatSec = 60.0f / kBeatBpm;
constexpr int kBeatsPerBar = 4;

constexpr float NOTE_G4 = 392.00f;
constexpr float NOTE_A4 = 440.00f;

struct MelodyNote {
  float freqHz;       // NOTE_* constant, NOTE_REST for silence
  uint16_t durationMs;
};

// CORTIS "REDRED", transcribed from all three pages of the guitar sheet music
// (arr. Woojeong Park, D minor, quarter note = 121 BPM).
// The vocal line is mostly a D-D-C triplet motif over a Dm/G/F progression
// (guitar TAB "3 3 1 3 3 1 3" on the B string), played one octave up (D5/C5)
// so it cuts through the small speaker. The bridge ("다시 배워봐") drops to
// the lower A4/G4 register (TAB "2 2 2 ... 0" on the G string).
// Timing at 121 BPM: triplet eighth = 165 ms, eighth = 248 ms, quarter = 496 ms,
// one 4/4 measure = 1984 ms.
constexpr MelodyNote kRedRedMelody[] = {
    // --- Verse (page 1, m5-7) ---
    // "따바라 한모금 sip"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 400}, {NOTE_REST, 594},
    // "카페인이 또 kickinin"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_C5, 248}, {NOTE_REST, 498},
    // "어젯밤에 만들던 beat"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 400}, {NOTE_REST, 594},

    // --- Hook (page 3, m26-28) ---
    // "주변을 살피기 that's red red"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_D5, 248}, {NOTE_D5, 400}, {NOTE_REST, 96},
    // "쿨한척 척하기 that's red red"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_D5, 248}, {NOTE_D5, 400}, {NOTE_REST, 96},
    // "you should come mess with the team-eam" (tail drops to C)
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_C5, 248}, {NOTE_C5, 248}, {NOTE_C5, 400}, {NOTE_REST, 96},

    // --- Verse 2 (page 3, m29-31) ---
    // "친구들 전부 한트럭에 다 담아서" (continuous triplets)
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    // "거리로 나가서 빙 빙"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_C5, 400}, {NOTE_REST, 96}, {NOTE_C5, 496},
    // "거리서 돌다가 돌아가 studio"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_D5, 248},

    // --- Pre-hook (page 3, m32-34) ---
    // "cookin up till we get stin-ky"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 496}, {NOTE_REST, 498},
    // "팔랑귀 팔랑귀 that's red red"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_D5, 248}, {NOTE_D5, 400}, {NOTE_REST, 96},
    // "눈치나 살피기 that's red red"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_D5, 248}, {NOTE_D5, 400}, {NOTE_REST, 96},

    // --- Bridge (page 5, m50-52, lower A4/G4 register) ---
    // "다시 배워봐 you gotta note down"
    {NOTE_A4, 165}, {NOTE_A4, 165}, {NOTE_A4, 165},
    {NOTE_A4, 165}, {NOTE_A4, 165}, {NOTE_G4, 165},
    {NOTE_A4, 248}, {NOTE_G4, 248}, {NOTE_REST, 498},
    // "불러와 버려 두번째 혼란"
    {NOTE_A4, 165}, {NOTE_A4, 165}, {NOTE_A4, 165},
    {NOTE_A4, 165}, {NOTE_A4, 165}, {NOTE_G4, 165},
    {NOTE_A4, 400}, {NOTE_G4, 496}, {NOTE_REST, 96},
    // "신호등 바꼈어 green green"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_C5, 248}, {NOTE_C5, 248}, {NOTE_REST, 498},

    // --- Outro (page 5, m53 + m58) ---
    // "you should come mess with the team-eam"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_C5, 248}, {NOTE_C5, 248}, {NOTE_C5, 400}, {NOTE_REST, 96},
    // closing "팔랑귀 팔랑귀 that's red red"
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 165}, {NOTE_D5, 165}, {NOTE_C5, 165},
    {NOTE_D5, 248}, {NOTE_D5, 248}, {NOTE_D5, 800},
};

uint32_t beatNoiseState = 0x12345678;

float beatNoiseSample() {
  beatNoiseState ^= beatNoiseState << 13;
  beatNoiseState ^= beatNoiseState >> 17;
  beatNoiseState ^= beatNoiseState << 5;
  return ((beatNoiseState & 0xFFFF) / 32767.5f) - 1.0f;
}

// Melody voice: sine + slight 2nd harmonic with attack/release envelope.
// Unlike the original sketch (fixed 0.5s beats), notes here have variable
// lengths from the REDRED table, so the envelope scales with note duration.
float synthRedRedVoice(float freqHz, float posSec, float noteSec) {
  if (freqHz <= 0.0f) return 0.0f;

  const float soundLen = noteSec * 0.92f; // small gap separates repeated notes
  if (posSec > soundLen) return 0.0f;

  float envelope = 1.0f;
  if (posSec < 0.008f) {
    envelope = posSec / 0.008f; // 8ms attack
  }
  const float releaseStart = soundLen * 0.70f;
  if (posSec > releaseStart) {
    envelope *= 1.0f - (posSec - releaseStart) / (soundLen - releaseStart); // release
  }
  envelope = constrain(envelope, 0.0f, 1.0f);

  float phase = PI2 * freqHz * posSec;
  float fundamental = sinf(phase);
  float harmonic = sinf(phase * 2.0f) * 0.18f; // slight second harmonic
  return (fundamental + harmonic) * envelope * 0.38f;
}

float synthBeatKick(float triggerPosition) {
  constexpr float kLength = 0.18f;
  if (triggerPosition < 0 || triggerPosition > kLength) return 0.0f;
  float freqHz = 45.0f + 105.0f * expf(-triggerPosition * 25.0f); // pitch drop 150Hz -> 45Hz
  float envelope = expf(-triggerPosition * 18.0f);
  return sinf(PI2 * freqHz * triggerPosition) * envelope * 0.72f;
}

float synthBeatSnare(float triggerPosition) {
  constexpr float kLength = 0.14f;
  if (triggerPosition < 0 || triggerPosition > kLength) return 0.0f;
  float envelope = expf(-triggerPosition * 24.0f);
  float noise = beatNoiseSample();
  float body = sinf(PI2 * 180.0f * triggerPosition); // body around 180Hz
  return (noise * 0.75f + body * 0.25f) * envelope * 0.44f;
}

float synthBeatHiHat(float triggerPosition) {
  constexpr float kLength = 0.055f;
  static float previousNoise = 0.0f;
  if (triggerPosition < 0 || triggerPosition > kLength) return 0.0f;
  float n = beatNoiseSample();
  float highPassed = n - previousNoise; // crude high-pass: current - previous
  previousNoise = n;
  float envelope = expf(-triggerPosition * 70.0f);
  return highPassed * envelope * 0.13f;
}

// Render and play REDRED (melody table + drum grid) once, blocking (~34s).
// The matching serial key ('p') can stop playback in the middle of a block.
void playRedRedBeat() {
  if (!speakerReady) {
    Serial.println("[Speaker] Not initialized, cannot play beat");
    return;
  }
  if (speakerStreamOpen || speakerDownlinkPlaying) {
    Serial.println("[Speaker] Downlink playing, skip REDRED");
    return;
  }

  constexpr size_t kBlockFrames = 128;
  constexpr size_t kNoteCount = sizeof(kRedRedMelody) / sizeof(kRedRedMelody[0]);
  const uint32_t samplesPerBeat = static_cast<uint32_t>(AUDIO_SAMPLE_RATE * kBeatSec);
  const uint32_t samplesPerEighth = samplesPerBeat / 2;

  uint32_t totalSamples = 0;
  for (size_t i = 0; i < kNoteCount; ++i) {
    totalSamples += static_cast<uint32_t>(AUDIO_SAMPLE_RATE) * kRedRedMelody[i].durationMs / 1000;
  }

  Serial.printf("[Speaker] REDRED beat start (%u notes, %.0f BPM, ~%us); "
                "press p again to stop\n",
                static_cast<unsigned>(kNoteCount), static_cast<double>(kBeatBpm),
                static_cast<unsigned>(totalSamples / AUDIO_SAMPLE_RATE));

  testTunePlaying = kTuneRedRed;
  int16_t block[kBlockFrames * 2]; // interleaved stereo (L, R per frame)
  size_t noteIndex = 0;
  uint32_t noteStartSample = 0;
  uint32_t noteSamples =
      static_cast<uint32_t>(AUDIO_SAMPLE_RATE) * kRedRedMelody[0].durationMs / 1000;
  uint32_t sampleIndex = 0;

  while (sampleIndex < totalSamples) {
    if (testTuneStopRequested()) {
      i2s_zero_dma_buffer(SPK_I2S_PORT);
      testTunePlaying = kTuneNone;
      Serial.println("[Speaker] REDRED beat stopped");
      return;
    }
    size_t count = min(kBlockFrames, static_cast<size_t>(totalSamples - sampleIndex));

    for (size_t i = 0; i < count; ++i) {
      uint32_t currentSample = sampleIndex + static_cast<uint32_t>(i);

      // Advance to the melody note containing this sample.
      while (noteIndex < kNoteCount && currentSample >= noteStartSample + noteSamples) {
        noteStartSample += noteSamples;
        noteIndex++;
        if (noteIndex < kNoteCount) {
          noteSamples =
              static_cast<uint32_t>(AUDIO_SAMPLE_RATE) * kRedRedMelody[noteIndex].durationMs / 1000;
        }
      }

      float audio = 0.0f;
      if (noteIndex < kNoteCount) {
        float posSec = static_cast<float>(currentSample - noteStartSample) / AUDIO_SAMPLE_RATE;
        float noteSec = kRedRedMelody[noteIndex].durationMs / 1000.0f;
        audio += synthRedRedVoice(kRedRedMelody[noteIndex].freqHz, posSec, noteSec);
      }

      // Drum grid at 121 BPM: 8 steps/bar. HH every 8th, KD on steps 0/4
      // (+ fill on the last 8th of every 4th bar), SN on steps 2/6.
      uint32_t eighthIndex = currentSample / samplesPerEighth;
      uint32_t eighthSample = currentSample % samplesPerEighth;
      float eighthPosition = static_cast<float>(eighthSample) / AUDIO_SAMPLE_RATE;
      int stepInBar = eighthIndex % 8;
      uint32_t beatIndex = currentSample / samplesPerBeat;
      uint32_t currentBar = beatIndex / kBeatsPerBar;

      audio += synthBeatHiHat(eighthPosition);
      if (stepInBar == 0 || stepInBar == 4) {
        audio += synthBeatKick(eighthPosition);
      }
      if ((currentBar % 4) == 3 && stepInBar == 7) {
        audio += synthBeatKick(eighthPosition);
      }
      if (stepInBar == 2 || stepInBar == 6) {
        audio += synthBeatSnare(eighthPosition);
      }

      audio = constrain(audio, -0.90f, 0.90f);
      int16_t s = static_cast<int16_t>(audio * 32767.0f);
      block[2 * i] = s;     // left slot
      block[2 * i + 1] = s; // right slot
    }

    size_t bytesWritten = 0;
    i2s_write(SPK_I2S_PORT, block, count * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    sampleIndex += count;
  }

  i2s_zero_dma_buffer(SPK_I2S_PORT);
  testTunePlaying = kTuneNone;
  Serial.println("[Speaker] REDRED beat done");
}

// Play one mono 16-bit PCM buffer as stereo (duplicated L/R) on shared I2S0.
void playPcmMono16(const int16_t* samples, size_t sampleCount) {
  constexpr size_t kBlockFrames = 128;
  int16_t block[kBlockFrames * 2];
  size_t written = 0;
  uint8_t stallTries = 0;
  while (written < sampleCount) {
    if (speakerBargeIn) {
      break;
    }
    size_t count = min(kBlockFrames, sampleCount - written);
    for (size_t i = 0; i < count; ++i) {
      int16_t s = samples[written + i];
      block[2 * i] = s;
      block[2 * i + 1] = s;
    }
    size_t bytesWritten = 0;
    const esp_err_t err = i2s_write(
        SPK_I2S_PORT, block, count * 2 * sizeof(int16_t), &bytesWritten, pdMS_TO_TICKS(100));
    if (err != ESP_OK || bytesWritten < 4) {
      vTaskDelay(pdMS_TO_TICKS(1));
      if (++stallTries >= 20) {
        break;
      }
      continue;
    }
    stallTries = 0;
    written += bytesWritten / (2 * sizeof(int16_t));
  }
}

void flushSpeakerQueue(const String& flushUrl) {
  netActivity = kNetFlush;
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(2000);
  if (!http.begin(flushUrl)) {
    netActivity = kNetIdle;
    return;
  }
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST("{}");
  String body = http.getString();
  body.trim();
  http.end();
  netActivity = kNetIdle;
  Serial.printf("[Speaker] flush HTTP %d %s\n", code, body.c_str());
}

bool postUtteranceEnd(const String& endUrl) {
  netActivity = kNetEnd;
  HTTPClient http;
  http.setConnectTimeout(1000);
  http.setTimeout(3000);
  if (!http.begin(endUrl)) {
    netActivity = kNetIdle;
    Serial.println("[HTTP] utterance/end begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  const uint32_t t0 = millis();
  const int code = http.POST("{}");
  String body = http.getString();
  body.trim();
  http.end();
  netActivity = kNetIdle;
  Serial.printf("[HTTP] utterance/end -> %d %s (took %u ms)\n",
                code, body.c_str(), static_cast<unsigned>(millis() - t0));
  return code == 200;
}

// Returns true when a PCM chunk was received (caller should poll again soon).
bool pollSpeakerPull(const String& pullUrl) {
  if (!speakerReady || speakerPcmStream == nullptr) {
    return false;
  }

  netActivity = kNetPull;
  HTTPClient http;
  // Short connect timeout: a dead/unreachable Mac must not stall mic uploads.
  http.setConnectTimeout(1000);
  http.setTimeout(2000);
  if (!http.begin(pullUrl)) {
    netActivity = kNetIdle;
    return false;
  }
  const char* headerKeys[] = {"X-Emotion", "X-Sample-Rate", "X-Robot-State"};
  http.collectHeaders(headerKeys, 3);

  int httpCode = http.GET();
  if (httpCode == 204 || httpCode == HTTP_CODE_NO_CONTENT) {
    applyServerRobotState(http.header("X-Robot-State"));
    http.end();
    netActivity = kNetIdle;
    return false;
  }
  if (httpCode != 200) {
    if (httpCode < 0) {
      Serial.printf("[Speaker] pull failed: %s\n", http.errorToString(httpCode).c_str());
    } else {
      Serial.printf("[Speaker] pull HTTP %d\n", httpCode);
    }
    http.end();
    netActivity = kNetIdle;
    return false;
  }

  String emotion = http.header("X-Emotion");
  if (emotion.length() > 0 && emotion.length() < sizeof(speakerEmotion)) {
    strncpy(speakerEmotion, emotion.c_str(), sizeof(speakerEmotion) - 1);
    speakerEmotion[sizeof(speakerEmotion) - 1] = '\0';
  } else {
    strncpy(speakerEmotion, "none", sizeof(speakerEmotion) - 1);
  }

  // Mark the stream open before applying a possible idle state so the face
  // does not flash think/idle for one frame on the last (or first) chunk.
  speakerStreamOpen = true;
  applyServerRobotState(http.header("X-Robot-State"));

  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    http.end();
    netActivity = kNetIdle;
    return false;
  }
  stream->setTimeout(50);

  speakerStreamEndPending = false;
  faceEmotionIndex = emotionIndexFromTag(speakerEmotion);
  faceHoldUntilMs = millis() + 60000;
  Serial.printf("[Speaker] receive start emotion=%s content_length=%d buffered=%u\n",
                speakerEmotion,
                len,
                static_cast<unsigned>(xStreamBufferBytesAvailable(speakerPcmStream)));

  constexpr size_t kScratchBytes = 1024;
  uint8_t scratch[kScratchBytes];
  size_t totalBytes = 0;
  uint32_t idleMs = 0;
  const uint32_t kIdleGiveUpMs = 400;
  int remaining = len;
  const uint32_t pullStartedMs = millis();
  const uint32_t pullBudgetMs = (len > 0)
      ? static_cast<uint32_t>(len) / 2 * 1000 / AUDIO_SAMPLE_RATE + 4000
      : 8000;

  while (true) {
    if (speakerBargeIn) {
      Serial.println("[Speaker] pull abort: barge-in");
      break;
    }
    if ((int32_t)(millis() - pullStartedMs) >= (int32_t)pullBudgetMs) {
      Serial.println("[Speaker] pull abort: timeout");
      break;
    }
    if (len > 0 && remaining <= 0) {
      break;
    }
    const int avail = stream->available();
    if (avail < 1) {
      vTaskDelay(pdMS_TO_TICKS(5));
      idleMs += 5;
      if (idleMs >= kIdleGiveUpMs) {
        break;
      }
      continue;
    }
    size_t want = sizeof(scratch);
    if (static_cast<size_t>(avail) < want) {
      want = static_cast<size_t>(avail);
    }
    if (len > 0 && static_cast<size_t>(remaining) < want) {
      want = static_cast<size_t>(remaining);
    }
    if (want % 2 == 1) {
      want -= 1;
    }
    if (want == 0) {
      break;
    }

    const int got = stream->read(scratch, want);
    if (got <= 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      idleMs += 5;
      if (idleMs >= kIdleGiveUpMs) {
        break;
      }
      continue;
    }
    idleMs = 0;
    int evenGot = got;
    if (evenGot % 2 == 1) {
      evenGot -= 1;
    }
    if (evenGot < 2) {
      break;
    }

    size_t queued = 0;
    while (queued < static_cast<size_t>(evenGot) && !speakerBargeIn) {
      const size_t sent = xStreamBufferSend(
          speakerPcmStream,
          scratch + queued,
          static_cast<size_t>(evenGot) - queued,
          pdMS_TO_TICKS(100));
      if (sent == 0) {
        continue;
      }
      queued += sent;
      speakerLastDataMs = millis();
    }
    totalBytes += queued;
    if (len > 0) {
      remaining -= evenGot;
    }
  }

  http.end();
  netActivity = kNetIdle;
  if (speakerBargeIn) {
    Serial.println("[Speaker] receive abort: barge-in");
  } else if (serverRobotState != kUiSpeak) {
    // The server removes a chunk from its queue before responding. If the
    // resulting state is no longer "speaking", this response is the final
    // chunk; playback owns the final drain and the single I2S reset.
    speakerStreamEndPending = true;
  }

  Serial.printf("[Speaker] receive done bytes=%u (~%u ms) buffered=%u end=%d\n",
                static_cast<unsigned>(totalBytes),
                static_cast<unsigned>(totalBytes / 2 * 1000 / AUDIO_SAMPLE_RATE),
                static_cast<unsigned>(xStreamBufferBytesAvailable(speakerPcmStream)),
                speakerStreamEndPending ? 1 : 0);
  return true;
}

void discardBufferedSpeakerPcm() {
  if (speakerPcmStream == nullptr) return;
  uint8_t discard[512];
  while (xStreamBufferReceive(speakerPcmStream, discard, sizeof(discard), 0) > 0) {
  }
}

void finishSpeakerPlayback(bool bargedIn) {
  discardBufferedSpeakerPcm();
  if (!bargedIn) {
    // i2s_write() returns after copying into the eight 256-frame DMA buffers.
    // Let their final ~128 ms reach the codec before clearing them, otherwise
    // the last Korean syllable can be clipped.
    for (int i = 0; i < 14; ++i) {
      if (speakerBargeIn) {
        bargedIn = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  i2s_zero_dma_buffer(SPK_I2S_PORT);
  speakerDownlinkPlaying = false;
  speakerStreamOpen = false;
  speakerStreamEndPending = false;

  if (bargedIn) {
    faceEmotionIndex = -1;
    faceHoldUntilMs = millis();
    serverRobotState = kUiIdle;
    Serial.println("[Speaker] buffered playback stopped by PTT");
  } else {
    faceHoldUntilMs = millis() + 400;
    Serial.println("[Speaker] buffered playback complete");
  }
}

void speakerPlaybackTask(void* pvParameters) {
  Serial.printf("[Task] Speaker playback task started on Core 1 (ring=%u bytes, prebuffer=%u ms)\n",
                static_cast<unsigned>(kSpeakerRingBytes),
                static_cast<unsigned>(kSpeakerPrebufferBytes * 1000 / kSpeakerBytesPerSecond));

  if (!speakerReady || speakerPcmStream == nullptr) {
    Serial.println("[Task] Speaker ring buffer unavailable! Task suspending.");
    vTaskDelete(nullptr);
    return;
  }

  constexpr size_t kPlaybackSamples = 256;
  int16_t samples[kPlaybackSamples];
  bool bargeHandled = false;

  while (true) {
    if (speakerBargeIn) {
      if (!bargeHandled) {
        finishSpeakerPlayback(true);
        bargeHandled = true;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    bargeHandled = false;

    const size_t buffered = xStreamBufferBytesAvailable(speakerPcmStream);
    if (!speakerDownlinkPlaying) {
      const bool readyToStart =
          speakerStreamOpen &&
          (buffered >= kSpeakerPrebufferBytes ||
           (speakerStreamEndPending && buffered > 0));
      if (!readyToStart) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }

      speakerDownlinkPlaying = true;
      faceEmotionIndex = emotionIndexFromTag(speakerEmotion);
      faceHoldUntilMs = millis() + 60000;
      // Fill the eight 256-frame TX DMA buffers (~128 ms) before any Serial
      // I/O. TX auto-clear would otherwise play zeros while this task logs,
      // which clips the first Korean syllable.
      constexpr size_t kPrimeBytes = 2048 * sizeof(int16_t);
      size_t primed = 0;
      while (primed < kPrimeBytes && !speakerBargeIn) {
        const size_t got = xStreamBufferReceive(
            speakerPcmStream, samples, sizeof(samples), 0);
        const size_t even = got & ~static_cast<size_t>(1);
        if (even < 2) {
          break;
        }
        playPcmMono16(samples, even / sizeof(int16_t));
        primed += even;
      }
      Serial.printf("[Speaker] buffered playback start bytes=%u primed=%u\n",
                    static_cast<unsigned>(buffered),
                    static_cast<unsigned>(primed));
    }

    const size_t received = xStreamBufferReceive(
        speakerPcmStream, samples, sizeof(samples), pdMS_TO_TICKS(20));
    const size_t evenBytes = received & ~static_cast<size_t>(1);
    if (evenBytes > 0) {
      playPcmMono16(samples, evenBytes / sizeof(int16_t));
      continue;
    }

    if (speakerStreamEndPending &&
        xStreamBufferBytesAvailable(speakerPcmStream) == 0) {
      finishSpeakerPlayback(false);
      continue;
    }

    if (speakerDownlinkPlaying && speakerStreamOpen && !speakerStreamEndPending) {
      static uint32_t lastUnderrunLogMs = 0;
      const uint32_t nowMs = millis();
      if (nowMs - lastUnderrunLogMs > 250) {
        lastUnderrunLogMs = nowMs;
        Serial.printf("[Speaker] ring underrun buffered=%u\n",
                      static_cast<unsigned>(xStreamBufferBytesAvailable(speakerPcmStream)));
      }
    }

    // Recover from a lost final response without leaving the UI permanently
    // speaking. Normal one-second chunk boundaries are far below this timeout.
    if (speakerStreamOpen && serverRobotState != kUiSpeak &&
        millis() - speakerLastDataMs > 2000) {
      speakerStreamEndPending = true;
    }
  }
}

// WiFi status helpers
void printWifiStatus() {
  wl_status_t status = WiFi.status();
  Serial.printf("[WiFi] Status: %d | RSSI: %d dBm\n", static_cast<int>(status), WiFi.RSSI());
  if (status == WL_CONNECTED) {
    Serial.printf("  IP: %s\n", WiFi.localIP().toString().c_str());
  }
}

// Tasks
void audioTask(void* pvParameters) {
  Serial.println("[Task] Audio capture task started on Core 1");

  if (!micReady) {
    Serial.println("[Task] ES7210 not ready! Task suspending.");
    vTaskDelete(nullptr);
    return;
  }
  Serial.println("[Task] ES7210 capture ready");

  bool wasCapturing = false;
  constexpr size_t kMinTailSamples = AUDIO_SAMPLE_RATE / 50; // 20 ms of real audio

  while (true) {
    size_t bytesRead = 0;
    // During speaker playback the same I2S port is also transmitting. A blocking
    // RX wait holds the driver lock and lets TX DMA auto-clear, which sounds
    // like the first 1–2 syllables dropping out.
    const TickType_t readWait =
        speakerDownlinkPlaying ? 0 : pdMS_TO_TICKS(100);
    esp_err_t result = i2s_read(
        I2S_PORT, i2sRawBuffer, sizeof(i2sRawBuffer), &bytesRead, readWait);

    if (result != ESP_OK || bytesRead == 0) {
      if (speakerDownlinkPlaying) {
        vTaskDelay(pdMS_TO_TICKS(1));
        continue;
      }
      Serial.printf("[Task] I2S read error: %d, bytes=%u\n", result, bytesRead);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t framesRead = bytesRead / (2 * sizeof(i2sRawBuffer[0]));

    // Push-to-talk gate: when the LCD is not being touched, keep draining I2S DMA
    // (so it never stalls) and keep the level meter alive, but do not accumulate
    // or transmit any audio chunks.
    bool capturing = micActive;

    // Finger just lifted: send the last partial second unpadded so trailing
    // speech is not turned into silence by zero fill.
    if (wasCapturing && !capturing) {
      if (writeIndex >= kMinTailSamples) {
        const size_t tailBytes = writeIndex * sizeof(int16_t);
        const unsigned tailMs =
            static_cast<unsigned>(writeIndex * 1000 / AUDIO_SAMPLE_RATE);
        if (enqueueMicChunk(activeWriteBuffer, tailBytes, true)) {
          int16_t* next = unusedMicBuffer();
          if (next != nullptr) {
            activeWriteBuffer = next;
          }
          Serial.printf("[Task] Tail chunk: %u ms unpadded\n", tailMs);
        } else {
          Serial.println("[Task] Tail chunk dropped");
        }
      }
      writeIndex = 0;
      pttEndPending = true;
    }
    wasCapturing = capturing;

    for (size_t i = 0; i < framesRead; ++i) {
      // ES7210 supplies interleaved 16-bit L/R data. Mix both onboard
      // microphones to mono for the existing 16-bit STT upload protocol.
      const int32_t left = i2sRawBuffer[i * 2];
      const int32_t right = i2sRawBuffer[i * 2 + 1];
      const int16_t sample16 = static_cast<int16_t>((left + right) / 2);

      int32_t magnitude = abs(static_cast<int32_t>(sample16));
      stats.sumAbs += magnitude;
      if (magnitude > stats.peak) {
        stats.peak = magnitude;
      }
      stats.sampleCount++;

      if (!capturing) {
        // Mic off: drop any partial chunk and skip accumulation/transmission.
        writeIndex = 0;
        continue;
      }

      if (writeIndex < kChunkSamples) {
        activeWriteBuffer[writeIndex++] = sample16;
      }

      // Check if buffer is full (1 second worth of audio)
      if (writeIndex >= kChunkSamples) {
        if (enqueueMicChunk(activeWriteBuffer, kChunkSamples * sizeof(int16_t), false)) {
          int16_t* next = unusedMicBuffer();
          if (next != nullptr) {
            activeWriteBuffer = next;
          }
          writeIndex = 0;
        } else {
          Serial.printf("[Task] Buffer overflow! Drop chunk. (network busy: %s)\n",
                        netActivityName(netActivity));
          writeIndex = 0;
        }
      }
    }

    // Print dBFS meter every 200ms
    uint32_t now = millis();
    if (now - lastPrintMs >= 200) {
      lastPrintMs = now;
      printDbfsBar();
    }
  }
}

void networkTask(void* pvParameters) {
  Serial.println("[Task] Network / HTTP POST task started on Core 0");

  String serverUrl = "http://";
  serverUrl += POST_SERVER_HOST;
  serverUrl += ':';
  serverUrl += POST_SERVER_PORT;
  serverUrl += POST_SERVER_PATH;

  String speakerPullUrl = "http://";
  speakerPullUrl += POST_SERVER_HOST;
  speakerPullUrl += ':';
  speakerPullUrl += POST_SERVER_PORT;
  speakerPullUrl += SPEAKER_PULL_PATH;

  String speakerFlushUrl = "http://";
  speakerFlushUrl += POST_SERVER_HOST;
  speakerFlushUrl += ':';
  speakerFlushUrl += POST_SERVER_PORT;
  speakerFlushUrl += SPEAKER_FLUSH_PATH;

  String utteranceEndUrl = "http://";
  utteranceEndUrl += POST_SERVER_HOST;
  utteranceEndUrl += ':';
  utteranceEndUrl += POST_SERVER_PORT;
  utteranceEndUrl += UTTERANCE_END_PATH;

  Serial.printf("[Speaker] Pull target: %s\n", speakerPullUrl.c_str());

  uint32_t lastConnectAttemptMs = millis();
  // Idle: poll every 2 s. Server thinking/speaking or a chunk just played:
  // poll quickly so 1 s TTS chunks play back-to-back and speech starts fast.
  constexpr uint32_t kPollIdleMs = 2000;
  constexpr uint32_t kPollBusyMs = 300;
  constexpr uint32_t kPollStreamMs = 20;
  uint32_t pollWaitMs = kPollIdleMs;

  while (true) {
    const uint32_t nowMs = millis();
    // WiFi.setAutoReconnect(true) already retries; this is a slow fallback so
    // we never restart an association that is still in progress.
    if (WiFi.status() != WL_CONNECTED && nowMs - lastConnectAttemptMs > 15000) {
      Serial.println("[WiFi] Force trigger WiFi.begin() for reconnection...");
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      lastConnectAttemptMs = nowMs;
    }
    if (speakerBargeIn && !speakerDownlinkPlaying && WiFi.status() == WL_CONNECTED) {
      flushSpeakerQueue(speakerFlushUrl);
      speakerBargeIn = false;
    }

    // Prefer mic upload; otherwise poll the Mac for a speaker chunk.
    // A pending end marker only waits briefly so it goes out right after the
    // tail chunk (the semaphore is given before pttEndPending is set).
    const uint32_t waitMs = pttEndPending ? 50 : pollWaitMs;
    if (xSemaphoreTake(xSendSemaphore, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Disconnected. Retrying queued mic chunk.");
        xSemaphoreGive(xSendSemaphore);
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }

      bool postFailed = false;
      while (activeSendBuffer != nullptr) {
        if (WiFi.status() != WL_CONNECTED) {
          postFailed = true;
          break;
        }
        uint32_t postStart = millis();
        netActivity = kNetPost;
        HTTPClient http;
        http.setConnectTimeout(2000);
        http.setTimeout(5000);

        int httpCode = -1;
        String response;
        if (http.begin(serverUrl)) {
          http.addHeader("Content-Type", "application/octet-stream");

          httpCode = http.POST(reinterpret_cast<uint8_t*>(const_cast<int16_t*>(activeSendBuffer)), sendBufferSize);
          response = http.getString();
          response.trim();
          http.end();

          uint32_t duration = millis() - postStart;
          lastHttpCode = httpCode;
          lastHttpDurationMs = duration;
          Serial.printf("[HTTP] POST chunk size=%d bytes -> Code=%d, Response=%s (took %d ms)\n",
                        sendBufferSize, httpCode, response.c_str(), duration);
        } else {
          lastHttpCode = -1;
          Serial.println("[HTTP] Begin failed");
        }

        if (httpCode != 200) {
          netActivity = kNetIdle;
          Serial.println("[HTTP] POST failed; will retry queued chunk");
          vTaskDelay(pdMS_TO_TICKS(200));
          continue;
        }

        postedChunks++;
        activeSendBuffer = nullptr;
        sendBufferSize = 0;
        if (pendingSendBuffer != nullptr) {
          activeSendBuffer = pendingSendBuffer;
          sendBufferSize = pendingSendSize;
          pendingSendBuffer = nullptr;
          pendingSendSize = 0;
        }
        netActivity = kNetIdle;
      }
      if (postFailed) {
        Serial.println("[WiFi] Disconnected during POST. Retrying queued mic chunk.");
        xSemaphoreGive(xSendSemaphore);
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
      pollWaitMs = kPollBusyMs;
      if (pttEndPending && WiFi.status() == WL_CONNECTED) {
        if (postUtteranceEnd(utteranceEndUrl)) {
          pttEndPending = false;
        }
      }
    } else if (pttEndPending) {
      if (activeSendBuffer != nullptr || pendingSendBuffer != nullptr) {
        // Tail is queued; wait for the POST path instead of ending early.
      } else if (WiFi.status() == WL_CONNECTED) {
        if (postUtteranceEnd(utteranceEndUrl)) {
          pttEndPending = false;
        }
        pollWaitMs = kPollBusyMs;
      }
    } else if (micActive) {
      // User is talking: keep the network task free for mic chunks. Nothing
      // needs to be pulled now (barge-in flush is handled above).
      pollWaitMs = kPollBusyMs;
    } else if (WiFi.status() == WL_CONNECTED) {
      const bool gotChunk = pollSpeakerPull(speakerPullUrl);
      if (speakerBargeIn) {
        flushSpeakerQueue(speakerFlushUrl);
        speakerBargeIn = false;
      }
      if (gotChunk) {
        pollWaitMs = kPollStreamMs;
      } else if (serverRobotState == kUiThink || serverRobotState == kUiSpeak) {
        pollWaitMs = kPollBusyMs;
      } else {
        pollWaitMs = kPollIdleMs;
      }
    } else {
      pollWaitMs = kPollIdleMs;
    }
  }
}
} // namespace

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=================================================");
  Serial.println("Waveshare ESP32-S3 Integrated Audio Streamer");
  Serial.printf("I2S0: MCLK=%d BCLK=%d LRCLK=%d MIC_DIN=%d SPK_DOUT=%d\n",
                PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DIN, PIN_I2S_DOUT);
  Serial.printf("TFT: CS=%d SCK=%d MOSI=%d DC=%d RST=EXIO%d BL=%d (%dx%d landscape)\n",
                PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI, PIN_TFT_DC,
                EXIO_LCD_RST, PIN_TFT_BL, kTftWidth, kTftHeight);
  Serial.printf("Server Target: http://%s:%d%s\n", POST_SERVER_HOST, POST_SERVER_PORT, POST_SERVER_PATH);
  Serial.printf("Speaker Pull:  http://%s:%d%s\n", POST_SERVER_HOST, POST_SERVER_PORT, SPEAKER_PULL_PATH);
  Serial.println("=================================================");

  // 1. Bring up the TFT first so boot / WiFi status is visible.
  if (!initDisplay()) {
    Serial.println("[TFT] Initialization failed; continuing headless");
  }

  // 2. WiFi Connect first (Avoids power/RF interference before turning on I2S)
  WiFi.mode(WIFI_STA);
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("[WiFi] Disconnected, reason=%u\n",
                      static_cast<unsigned>(info.wifi_sta_disconnected.reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[WiFi] Connecting to SSID: %s ", WIFI_SSID);

  uint32_t startMs = millis();
  size_t bootFaceFrame = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < 30000) {
    delay(500);
    Serial.print('.');
    drawEmotionFrame(ANIM_EMOTION_NEUTRAL, bootFaceFrame);
    bootFaceFrame = (bootFaceFrame + 1) % ANIM_FRAMES_PER_EMOTION;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Connected successfully!");
    printWifiStatus();
    drawEmotionFrame(ANIM_EMOTION_POSITIVE, 0);
  } else {
    Serial.println("[WiFi] Warning: Failed to connect initially. Auto-reconnect will keep trying in background.");
    Serial.println("[WiFi] Scanning nearby 2.4GHz networks for diagnostics...");
    WiFi.disconnect(false, false);
    delay(200);
    const int networkCount = WiFi.scanNetworks(false, true);
    Serial.printf("[WiFi] Scan result: %d network(s)\n", networkCount);
    for (int i = 0; i < networkCount; ++i) {
      Serial.printf("  [%d] %s  RSSI=%d dBm  channel=%d\n",
                    i + 1, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    }
    WiFi.scanDelete();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    drawEmotionFrame(ANIM_EMOTION_SAD, 0);
  }

  // 3. One shared I2S0 bus, then both onboard audio codecs.
  if (!installI2sDriver()) {
    Serial.println("[Audio] Shared I2S0 setup FAILED");
  } else {
    Serial.println("[Audio] Shared I2S0 TX/RX ready");
    speakerReady = setupAudioCodecs();
  }
  Serial.printf("[Mic] ES7210 %s; PTT=LCD touch (hold the screen to capture)\n",
                micReady ? "ready" : "FAILED");

  // 4. ES8311 + onboard NS4150B amplifier. Failure is non-fatal for STT.
  if (speakerReady) {
    Serial.printf("[Speaker] ES8311 ready: volume=%d DOUT=%d PA=EXIO%d "
                  "(K2=vol+, K3=vol-, 'p'=REDRED, 'b'=beep)\n",
                  speakerVolume, PIN_I2S_DOUT, EXIO_PA_CTRL);
  } else {
    Serial.println("[Speaker] ES8311 setup FAILED (playback disabled)");
  }

  // 5. Create the mic-upload semaphore and 3-second speaker ring buffer.
  xSendSemaphore = xSemaphoreCreateBinary();
  bool speakerRingInPsram = false;
  speakerPcmStorage = static_cast<uint8_t*>(ps_malloc(kSpeakerRingBytes + 1));
  if (speakerPcmStorage != nullptr) {
    speakerRingInPsram = true;
  } else {
    // Keep playback available on boards where PSRAM failed to initialize.
    speakerPcmStorage = static_cast<uint8_t*>(malloc(kSpeakerRingBytes + 1));
  }
  if (speakerPcmStorage != nullptr) {
    speakerPcmStream = xStreamBufferCreateStatic(
        kSpeakerRingBytes, 1, speakerPcmStorage, &speakerPcmStreamControl);
  }
  if (speakerPcmStream == nullptr) {
    Serial.printf("[Speaker] Failed to allocate %u-byte ring buffer\n",
                  static_cast<unsigned>(kSpeakerRingBytes));
  } else {
    Serial.printf("[Speaker] Ring buffer ready: %u bytes (3.0 s, %s)\n",
                  static_cast<unsigned>(kSpeakerRingBytes),
                  speakerRingInPsram ? "PSRAM" : "internal RAM");
  }

  xTaskCreatePinnedToCore(
      displayTask,
      "DisplayTFT",
      4096,
      nullptr,
      1,
      nullptr,
      0
  );

  // Audio capture runs on Core 1 (default App core) with higher priority
  xTaskCreatePinnedToCore(
      audioTask,
      "AudioCapture",
      4096,
      nullptr,
      5,
      nullptr,
      1
  );

  // Playback is independent from HTTP reception. Core 1 keeps I2S writes close
  // to the audio capture task, while the network task fills the ring on Core 0.
  xTaskCreatePinnedToCore(
      speakerPlaybackTask,
      "SpeakerPlayback",
      4096,
      nullptr,
      4,
      nullptr,
      1
  );

  // Network IO runs on Core 0 (default Protocol core) with normal priority
  xTaskCreatePinnedToCore(
      networkTask,
      "NetworkPOST",
      8192,
      nullptr,
      2,
      nullptr,
      0
  );
}

void loop() {
  // Push-to-talk: any contact on the CST816D panel, not the physical K1 key.
  static bool lastPressed = false;
  static bool bargeLatched = false;
  static uint32_t lastChangeMs = 0;
  static uint32_t pressStartMs = 0;
  constexpr uint32_t kMicPressMs = 50;
  constexpr uint32_t kBargeInMs = 100;

  bool pressed = touchReady && touch.touched();
  uint32_t now = millis();

  if (pressed != lastPressed && (now - lastChangeMs) > 30) {
    lastChangeMs = now;
    lastPressed = pressed;
    if (pressed) {
      pressStartMs = now;
      bargeLatched = false;
    } else {
      if (micActive) {
        Serial.println("[Touch] Mic capture OFF");
      }
      micActive = false;
      bargeLatched = false;
    }
  }

  // Ignore sub-50 ms contacts so CST816D ghost taps do not start a PTT turn.
  if (pressed && lastPressed && !micActive && (now - pressStartMs) >= kMicPressMs) {
    micActive = true;
    Serial.println("[Touch] Mic capture ON");
  }

  // Ignore sub-100 ms contacts as barge-in so CST816D glitches do not stop TTS.
  const bool speakingOrThinking =
      speakerStreamOpen || speakerDownlinkPlaying ||
      serverRobotState == kUiThink || serverRobotState == kUiSpeak;
  if (pressed && lastPressed && speakingOrThinking && !bargeLatched &&
      (now - pressStartMs) >= kBargeInMs) {
    bargeLatched = true;
    speakerBargeIn = true;
    Serial.printf("[Touch] Barge-in: stop speaker (playing=%d open=%d ui=%d)\n",
                  speakerDownlinkPlaying ? 1 : 0,
                  speakerStreamOpen ? 1 : 0,
                  serverRobotState);
  }

  static bool lastK2 = false;
  static bool lastK3 = false;
  static uint32_t lastVolumeKeyMs = 0;
  const uint16_t exioBits = exio.readAll();
  const bool k2Pressed = (exioBits & (1u << EXIO_KEY2)) == 0;
  const bool k3Pressed = (exioBits & (1u << EXIO_KEY3)) == 0;
  if (now - lastVolumeKeyMs > 30) {
    if (k2Pressed && !lastK2) {
      lastVolumeKeyMs = now;
      applySpeakerVolume(speakerVolume + kSpeakerVolumeStep);
    } else if (k3Pressed && !lastK3) {
      lastVolumeKeyMs = now;
      applySpeakerVolume(speakerVolume - kSpeakerVolumeStep);
    }
  }
  lastK2 = k2Pressed;
  lastK3 = k3Pressed;

  // Serial keyboard control:
  //   Ctrl+W (ASCII 0x17) -> toggle character/developer status screen,
  //   'q' -> full emotion demo,
  //   'p' -> REDRED beat (same key stops it), 'b' -> test beep (same key stops it).
  while (pendingSerialChar >= 0 || Serial.available() > 0) {
    int c = readSerialChar();
    if (c == 0x17) {
      diagnosticDisplayMode = !diagnosticDisplayMode;
      animationMode = false;
      Serial.printf("[Display] Ctrl+W -> %s\n",
                    diagnosticDisplayMode ? "developer status" : "character UI");
    } else if (c == 'q' || c == 'Q') {
      diagnosticDisplayMode = false;
      animationMode = true;
      Serial.println("[Display] Animation mode ON (q)");
    } else if (c == 'p' || c == 'P') {
      playRedRedBeat();
    } else if (c == 'b' || c == 'B') {
      playTestBeep();
    }
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}