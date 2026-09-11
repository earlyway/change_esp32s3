// Stage 6 bring-up test: push-to-talk using the built-in K1 button.
// The shared I2S0 bus remains full-duplex compatible with stage 4, but the
// speaker amplifier stays off during this test to prevent acoustic feedback.
// Hold K1 (bottom-most user key on the right edge) to capture microphone data.
// Expected: IDLE -> LISTENING while held -> session summary after release.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <driver/i2s.h>
#include <math.h>

#include "hardware_pins.h"
#include "tca9555.h"
#include "es7210.h"

static constexpr i2s_port_t kI2sPort = I2S_NUM_0;
static constexpr uint32_t kSampleRate = AUDIO_SAMPLE_RATE;
static constexpr uint32_t kMclkMultiple = 256;
static constexpr size_t kFramesPerRead = 512;
static constexpr float kDbFloor = -90.0f;

static TCA9555 exio(I2C_ADDR_TCA9555);
static Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
static es7210_dev_handle_t micCodec = nullptr;
static int16_t samples[kFramesPerRead * 2];  // stereo interleaved
static bool pushToTalkActive = false;
static uint32_t pressStartedMs = 0;
static uint32_t capturedFrames = 0;
static float sessionMaxDbL = kDbFloor;
static float sessionMaxDbR = kDbFloor;

struct Levels {
  float rmsDbL;
  float rmsDbR;
  int16_t peakL;
  int16_t peakR;
};

static void showFatal(const char* message) {
  Serial.printf("!! %s\n", message);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_RED);
  tft.setCursor(8, 8);
  tft.println("Stage 5 FAILED");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 40);
  tft.println(message);
}

static bool initBoardAndLcd() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!exio.begin()) return false;

  exio.pinMode(EXIO_LCD_RST, OUTPUT);
  exio.pinMode(EXIO_TP_RST, OUTPUT);
  exio.pinMode(EXIO_PA_CTRL, OUTPUT);
  exio.digitalWrite(EXIO_PA_CTRL, LOW);  // speaker off: avoid mic feedback

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
  return true;
}

static bool initI2sFullDuplex() {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
  cfg.sample_rate = kSampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  esp_err_t err = i2s_driver_install(kI2sPort, &cfg, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("i2s_driver_install: %s\n", esp_err_to_name(err));
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_I2S_MCLK;
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRCLK;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = PIN_I2S_DIN;
  err = i2s_set_pin(kI2sPort, &pins);
  if (err != ESP_OK) {
    Serial.printf("i2s_set_pin: %s\n", esp_err_to_name(err));
    return false;
  }

  i2s_zero_dma_buffer(kI2sPort);
  return true;
}

static bool initEs7210() {
  es7210_i2c_config_t i2cConfig = {};
  i2cConfig.i2c_port = I2C_NUM_0;
  i2cConfig.i2c_addr = I2C_ADDR_ES7210;

  esp_err_t err = es7210_new_codec(&i2cConfig, &micCodec);
  if (err != ESP_OK) {
    Serial.printf("es7210_new_codec: %s\n", esp_err_to_name(err));
    return false;
  }

  es7210_codec_config_t codecConfig = {};
  codecConfig.sample_rate_hz = kSampleRate;
  codecConfig.mclk_ratio = kMclkMultiple;
  codecConfig.i2s_format = ES7210_I2S_FMT_I2S;
  codecConfig.bit_width = ES7210_I2S_BITS_16B;
  codecConfig.mic_bias = ES7210_MIC_BIAS_2V87;
  codecConfig.mic_gain = ES7210_MIC_GAIN_30DB;
  codecConfig.flags.tdm_enable = true;  // official Waveshare dual-mic setting

  err = es7210_config_codec(micCodec, &codecConfig);
  if (err != ESP_OK) {
    Serial.printf("es7210_config_codec: %s\n", esp_err_to_name(err));
    return false;
  }
  err = es7210_config_volume(micCodec, 0);
  if (err != ESP_OK) {
    Serial.printf("es7210_config_volume: %s\n", esp_err_to_name(err));
    return false;
  }
  return true;
}

static float toDbFs(double sumSquares, size_t count) {
  if (count == 0) return kDbFloor;
  const double rms = sqrt(sumSquares / count);
  if (rms < 1.0) return kDbFloor;
  return max(kDbFloor, static_cast<float>(20.0 * log10(rms / 32768.0)));
}

static Levels analyze(const int16_t* data, size_t frames) {
  double sumL = 0.0;
  double sumR = 0.0;
  int32_t peakL = 0;
  int32_t peakR = 0;
  for (size_t i = 0; i < frames; ++i) {
    const int32_t left = data[i * 2];
    const int32_t right = data[i * 2 + 1];
    sumL += static_cast<double>(left) * left;
    sumR += static_cast<double>(right) * right;
    peakL = max(peakL, abs(left));
    peakR = max(peakR, abs(right));
  }
  return {toDbFs(sumL, frames), toDbFs(sumR, frames),
          static_cast<int16_t>(min(peakL, 32767)),
          static_cast<int16_t>(min(peakR, 32767))};
}

static int16_t dbToPixels(float db, int16_t width) {
  constexpr float kMinDb = -60.0f;
  const float normalized = constrain((db - kMinDb) / -kMinDb, 0.0f, 1.0f);
  return static_cast<int16_t>(normalized * width);
}

static uint16_t levelColor(float db) {
  if (db > -6.0f) return ST77XX_RED;
  if (db > -18.0f) return ST77XX_YELLOW;
  return ST77XX_GREEN;
}

static void drawStaticScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(8, 8);
  tft.println("Stage 6: Push-to-talk");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(8, 32);
  tft.print("Hold K1 (bottom-right key) and speak");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 215);
  tft.print("Mic is analyzed only while K1 is held");
}

static void drawChannel(char channel, int16_t y, float db, int16_t peak) {
  constexpr int16_t kBarX = 42;
  constexpr int16_t kBarWidth = 266;
  constexpr int16_t kBarHeight = 24;

  tft.fillRect(0, y, tft.width(), 58, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, y + 4);
  tft.printf("%c", channel);

  tft.drawRect(kBarX, y, kBarWidth, kBarHeight, ST77XX_WHITE);
  const int16_t pixels = dbToPixels(db, kBarWidth - 2);
  tft.fillRect(kBarX + 1, y + 1, pixels, kBarHeight - 2, levelColor(db));

  tft.setCursor(kBarX, y + 31);
  tft.printf("%6.1f dBFS  peak %5d", db, peak);
}

static void drawPttState(const char* state, uint16_t color) {
  tft.fillRect(0, 45, tft.width(), 32, ST77XX_BLACK);
  tft.setTextSize(3);
  tft.setTextColor(color);
  tft.setCursor(8, 49);
  tft.print(state);
}

static void clearMeters() {
  tft.fillRect(0, 82, tft.width(), 125, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 92);
  tft.println("Press and hold K1");
  tft.setCursor(8, 120);
  tft.println("to start listening.");
}

static void showSessionSummary(uint32_t durationMs) {
  tft.fillRect(0, 82, tft.width(), 125, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(8, 88);
  tft.printf("Duration: %lu ms", durationMs);
  tft.setCursor(8, 114);
  tft.printf("Frames:   %lu", capturedFrames);
  tft.setCursor(8, 140);
  tft.printf("Max L: %5.1f dBFS", sessionMaxDbL);
  tft.setCursor(8, 166);
  tft.printf("Max R: %5.1f dBFS", sessionMaxDbR);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(8, 192);
  tft.print("Hold K1 for another test");
}

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && millis() - start < 5000) delay(10);
  delay(300);

  Serial.println("==============================================");
  Serial.println(" ESP32-S3-AUDIO-Board  stage 6: push-to-talk");
  Serial.println("==============================================");

  if (!initBoardAndLcd()) {
    Serial.println("!! TCA9555 not found. Stop.");
    return;
  }
  drawStaticScreen();

  if (!initI2sFullDuplex()) {
    showFatal("I2S init failed");
    return;
  }
  Serial.printf("I2S0 RX ready: %luHz, 16-bit stereo, DIN GPIO%d, MCLK %luHz\n",
                kSampleRate, PIN_I2S_DIN, kSampleRate * kMclkMultiple);

  if (!initEs7210()) {
    showFatal("ES7210 init failed");
    return;
  }
  Serial.println("ES7210 ready: gain=30dB, ADC volume=0dB, TDM enabled");
  Serial.println("Hold K1 (TCA9555 EXIO9, active LOW) and speak.");
  exio.pinMode(EXIO_KEY1, INPUT);
  drawPttState("IDLE", ST77XX_CYAN);
  clearMeters();
}

void loop() {
  // K1 is active LOW. Debounce both edges before changing PTT state.
  static bool rawPressed = false;
  static bool stablePressed = false;
  static uint32_t rawChangedMs = 0;
  const bool currentRaw = ((exio.readAll() & (1u << EXIO_KEY1)) == 0);
  const uint32_t now = millis();
  if (currentRaw != rawPressed) {
    rawPressed = currentRaw;
    rawChangedMs = now;
  }
  if (rawPressed != stablePressed && now - rawChangedMs >= 30) {
    stablePressed = rawPressed;
    pushToTalkActive = stablePressed;
    if (pushToTalkActive) {
      pressStartedMs = now;
      capturedFrames = 0;
      sessionMaxDbL = kDbFloor;
      sessionMaxDbR = kDbFloor;
      drawPttState("LISTENING", ST77XX_GREEN);
      tft.fillRect(0, 82, tft.width(), 125, ST77XX_BLACK);
      Serial.println("[PTT] pressed -> capture START");
    } else {
      const uint32_t durationMs = now - pressStartedMs;
      drawPttState("RELEASED", ST77XX_YELLOW);
      showSessionSummary(durationMs);
      Serial.printf("[PTT] released -> capture STOP, duration=%lu ms, frames=%lu, max L/R=%.1f/%.1f dBFS\n",
                    durationMs, capturedFrames, sessionMaxDbL, sessionMaxDbR);
    }
  }

  // Always drain the RX DMA. Data is counted/analyzed only while K1 is held.
  size_t bytesRead = 0;
  const esp_err_t err = i2s_read(kI2sPort, samples, sizeof(samples),
                                 &bytesRead, pdMS_TO_TICKS(250));
  if (err != ESP_OK) {
    Serial.printf("i2s_read: %s\n", esp_err_to_name(err));
    delay(100);
    return;
  }
  const size_t frames = bytesRead / (2 * sizeof(int16_t));
  if (frames == 0) return;
  if (!pushToTalkActive) return;

  const Levels levels = analyze(samples, frames);
  capturedFrames += frames;
  sessionMaxDbL = max(sessionMaxDbL, levels.rmsDbL);
  sessionMaxDbR = max(sessionMaxDbR, levels.rmsDbR);
  drawChannel('L', 82, levels.rmsDbL, levels.peakL);
  drawChannel('R', 147, levels.rmsDbR, levels.peakR);

  static uint32_t lastSerialMs = 0;
  if (now - lastSerialMs >= 250) {
    lastSerialMs = now;
    Serial.printf("[mic] L %6.1f dBFS peak=%5d | R %6.1f dBFS peak=%5d | frames=%u\n",
                  levels.rmsDbL, levels.peakL, levels.rmsDbR, levels.peakR,
                  static_cast<unsigned>(frames));
  }
}
