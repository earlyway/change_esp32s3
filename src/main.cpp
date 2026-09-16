#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <driver/i2s.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#include "hardware_pins.h"
#include "tca9555.h"
#include "cst816d.h"
#include "es7210.h"
#include "server_config.h"
#include "wifi_credentials.h"
#include "animation_frames.h"

// Roles: this board captures the microphones (ES7210) and shows the face.
// All TTS audio plays on the Mac speaker; the board only mirrors the robot
// state (idle / thinking / speaking + emotion) that the server reports on
// GET /speaker/pull. The onboard ES8311 speaker path is not used.

namespace {
// Waveshare 2inch ST7789T3: native 240x320, shown landscape as 320x240.
constexpr int kTftWidth = 320;
constexpr int kTftHeight = 240;
Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
TCA9555 exio(I2C_ADDR_TCA9555);
CST816D touch(I2C_ADDR_CST816D);
bool displayReady = false;
bool touchReady = false;

// I2S0 runs RX-only for the ES7210 microphone ADC.
constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
constexpr size_t kI2sReadFrames = 256;
int16_t i2sRawBuffer[kI2sReadFrames * 2]; // interleaved L/R from ES7210
es7210_dev_handle_t micCodec = nullptr;
bool micReady = false;

// Face shown while the Mac is speaking (from X-Emotion). -1 = none.
volatile int faceEmotionIndex = -1;

enum RobotUi : int {
  kUiIdle = 0,
  kUiListen = 1,
  kUiThink = 2,
  kUiSpeak = 3,
};
// Last state reported by the server and when it was received. A stale value
// (Mac unreachable mid-reply) must not leave the face stuck in speak/think.
volatile int serverRobotState = kUiIdle;
volatile uint32_t serverStateUpdatedMs = 0;
constexpr uint32_t kServerStateStaleMs = 5000;
// PTT during a reply: networkTask POSTs /speaker/flush so the Mac stops.
volatile bool bargeInPending = false;
// Set by audioTask once the PTT tail chunk has been handed off; networkTask
// then POSTs /utterance/end so the Mac finalizes STT without waiting.
volatile bool pttEndPending = false;
int lastLoggedUi = -1;

// What networkTask is doing right now; printed when a mic chunk overflows so
// the cause (slow POST vs. state poll vs. flush) is visible in the log.
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
    case kNetPull: return "state poll";
    case kNetFlush: return "barge-in flush";
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
  // Speaker amplifier stays off: the board's speaker is not used.
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
  serverStateUpdatedMs = millis();
}

bool serverStateFresh(uint32_t now) {
  return (now - serverStateUpdatedMs) < kServerStateStaleMs;
}

int effectiveUi(uint32_t now) {
  if (micActive) {
    return kUiListen;
  }
  if (!serverStateFresh(now)) {
    return kUiIdle;
  }
  if (serverRobotState == kUiSpeak) {
    return kUiSpeak;
  }
  if (serverRobotState == kUiThink) {
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

  const uint32_t now = millis();
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const String ip = wifiConnected ? WiFi.localIP().toString() : "-";
  const bool robotFresh = serverStateFresh(now);
  drawStatusRow(0,   "",        "ESP32-S3 AUDIO", ST77XX_CYAN);
  drawStatusRow(24,  "Mode: ",  micActive ? "LISTEN" : uiName(effectiveUi(now)),
                micActive ? ST77XX_GREEN : ST77XX_WHITE);
  drawStatusRow(48,  "WiFi: ",  wifiConnected ? "OK" : "WAIT",
                wifiConnected ? ST77XX_GREEN : ST77XX_YELLOW);
  drawStatusRow(72,  "IP: ",    ip.c_str());
  drawStatusRow(96,  "Level: ", level);
  drawStatusRow(120, "Mic: ",   micActive ? "ON" : "OFF",
                micActive ? ST77XX_GREEN : ST77XX_WHITE);
  drawStatusRow(144, "POST: ",  post);
  drawStatusRow(168, "Chunks: ", chunks);
  drawStatusRow(192, "Robot: ", robotFresh ? uiName(serverRobotState) : "stale",
                robotFresh ? ST77XX_WHITE : ST77XX_YELLOW);
  drawStatusRow(216, "TTS: ",   "Mac speaker");
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

// RX-only I2S master: supplies MCLK/BCLK/LRCLK to the ES7210 and reads DIN.
bool installI2sDriver() {
  i2s_config_t i2sConfig = {};
  i2sConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  i2sConfig.sample_rate = AUDIO_SAMPLE_RATE;
  i2sConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  i2sConfig.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  i2sConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2sConfig.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  i2sConfig.dma_buf_count = 8; // Increased for extra safety margin during network load
  i2sConfig.dma_buf_len = 256;
  i2sConfig.use_apll = false;
  i2sConfig.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  esp_err_t result = i2s_driver_install(I2S_PORT, &i2sConfig, 0, nullptr);
  if (result != ESP_OK) return false;

  i2s_pin_config_t pinConfig = {};
  pinConfig.mck_io_num = PIN_I2S_MCLK;
  pinConfig.bck_io_num = PIN_I2S_BCLK;
  pinConfig.ws_io_num = PIN_I2S_LRCLK;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE; // ES8311 speaker path unused
  pinConfig.data_in_num = PIN_I2S_DIN;

  result = i2s_set_pin(I2S_PORT, &pinConfig);
  if (result != ESP_OK) return false;

  i2s_zero_dma_buffer(I2S_PORT);
  return true;
}

bool setupMicCodec() {
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
  if (result != ESP_OK) {
    Serial.printf("[Mic] ES7210 setup failed: %s\n", esp_err_to_name(result));
  }
  return result == ESP_OK;
}

// Barge-in: tell the Mac to stop the reply that is playing/being produced.
void flushRobotSpeech(const String& flushUrl) {
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
  Serial.printf("[Robot] flush HTTP %d %s\n", code, body.c_str());
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

// Poll the server for robot state (idle / thinking / speaking + emotion). The
// Mac plays the audio itself, so the response body (if any) is ignored.
void pollRobotState(const String& pullUrl) {
  netActivity = kNetPull;
  HTTPClient http;
  // Short connect timeout: a dead/unreachable Mac must not stall mic uploads.
  http.setConnectTimeout(1000);
  http.setTimeout(2000);
  if (!http.begin(pullUrl)) {
    netActivity = kNetIdle;
    return;
  }
  const char* headerKeys[] = {"X-Emotion", "X-Robot-State"};
  http.collectHeaders(headerKeys, 2);

  const int httpCode = http.GET();
  if (httpCode == 204 || httpCode == HTTP_CODE_NO_CONTENT || httpCode == 200) {
    applyServerRobotState(http.header("X-Robot-State"));
    if (serverRobotState == kUiSpeak) {
      String emotion = http.header("X-Emotion");
      faceEmotionIndex = emotionIndexFromTag(emotion.c_str());
    }
  } else if (httpCode < 0) {
    Serial.printf("[Robot] state poll failed: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("[Robot] state poll HTTP %d\n", httpCode);
  }
  http.end();
  netActivity = kNetIdle;
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
    esp_err_t result = i2s_read(
        I2S_PORT, i2sRawBuffer, sizeof(i2sRawBuffer), &bytesRead, pdMS_TO_TICKS(100));

    if (result != ESP_OK || bytesRead == 0) {
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

  String statePullUrl = "http://";
  statePullUrl += POST_SERVER_HOST;
  statePullUrl += ':';
  statePullUrl += POST_SERVER_PORT;
  statePullUrl += SPEAKER_PULL_PATH;

  String flushUrl = "http://";
  flushUrl += POST_SERVER_HOST;
  flushUrl += ':';
  flushUrl += POST_SERVER_PORT;
  flushUrl += SPEAKER_FLUSH_PATH;

  String utteranceEndUrl = "http://";
  utteranceEndUrl += POST_SERVER_HOST;
  utteranceEndUrl += ':';
  utteranceEndUrl += POST_SERVER_PORT;
  utteranceEndUrl += UTTERANCE_END_PATH;

  Serial.printf("[Robot] State poll target: %s\n", statePullUrl.c_str());

  uint32_t lastConnectAttemptMs = millis();
  // Idle: poll every 2 s. Server thinking/speaking: poll quickly so the face
  // follows the Mac's reply closely.
  constexpr uint32_t kPollIdleMs = 2000;
  constexpr uint32_t kPollBusyMs = 300;
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
    if (bargeInPending && WiFi.status() == WL_CONNECTED) {
      flushRobotSpeech(flushUrl);
      bargeInPending = false;
      // The Mac stopped; do not keep showing the old reply until the next poll.
      serverRobotState = kUiIdle;
      serverStateUpdatedMs = millis();
    }

    // Prefer mic upload; otherwise poll the Mac for robot state.
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
      // needs to be polled now (barge-in flush is handled above).
      pollWaitMs = kPollBusyMs;
    } else if (WiFi.status() == WL_CONNECTED) {
      pollRobotState(statePullUrl);
      if (bargeInPending) {
        flushRobotSpeech(flushUrl);
        bargeInPending = false;
        serverRobotState = kUiIdle;
        serverStateUpdatedMs = millis();
      }
      if (serverRobotState == kUiThink || serverRobotState == kUiSpeak) {
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
  Serial.println("Waveshare ESP32-S3 Mic Streamer (TTS plays on the Mac)");
  Serial.printf("I2S0 RX: MCLK=%d BCLK=%d LRCLK=%d MIC_DIN=%d\n",
                PIN_I2S_MCLK, PIN_I2S_BCLK, PIN_I2S_LRCLK, PIN_I2S_DIN);
  Serial.printf("TFT: CS=%d SCK=%d MOSI=%d DC=%d RST=EXIO%d BL=%d (%dx%d landscape)\n",
                PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI, PIN_TFT_DC,
                EXIO_LCD_RST, PIN_TFT_BL, kTftWidth, kTftHeight);
  Serial.printf("Server Target: http://%s:%d%s\n", POST_SERVER_HOST, POST_SERVER_PORT, POST_SERVER_PATH);
  Serial.printf("Robot State:   http://%s:%d%s\n", POST_SERVER_HOST, POST_SERVER_PORT, SPEAKER_PULL_PATH);
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

  // 3. RX-only I2S0, then the ES7210 microphone ADC.
  if (!installI2sDriver()) {
    Serial.println("[Audio] I2S0 RX setup FAILED");
  } else {
    Serial.println("[Audio] I2S0 RX ready");
    micReady = setupMicCodec();
  }
  Serial.printf("[Mic] ES7210 %s; PTT=LCD touch (hold the screen to capture)\n",
                micReady ? "ready" : "FAILED");

  // 4. Mic-upload semaphore.
  xSendSemaphore = xSemaphoreCreateBinary();

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
  const bool robotBusy =
      serverStateFresh(now) &&
      (serverRobotState == kUiThink || serverRobotState == kUiSpeak);
  if (pressed && lastPressed && robotBusy && !bargeLatched &&
      (now - pressStartMs) >= kBargeInMs) {
    bargeLatched = true;
    bargeInPending = true;
    Serial.printf("[Touch] Barge-in: stop Mac reply (state=%s)\n",
                  uiName(serverRobotState));
  }

  // Serial keyboard control:
  //   Ctrl+W (ASCII 0x17) -> toggle character/developer status screen,
  //   'q' -> full emotion demo.
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == 0x17) {
      diagnosticDisplayMode = !diagnosticDisplayMode;
      animationMode = false;
      Serial.printf("[Display] Ctrl+W -> %s\n",
                    diagnosticDisplayMode ? "developer status" : "character UI");
    } else if (c == 'q' || c == 'Q') {
      diagnosticDisplayMode = false;
      animationMode = true;
      Serial.println("[Display] Animation mode ON (q)");
    }
  }

  vTaskDelay(pdMS_TO_TICKS(10));
}
