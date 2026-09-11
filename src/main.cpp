// Stage 3 bring-up test: 2inch Capacitive Touch LCD (ST7789T3, 240x320) over SPI.
// LCD_RST lives behind the TCA9555 (EXIO0); backlight is GPIO5.
// Landscape orientation (320x240): rotation 1. Use 3 if the image is upside down.
// Expected: backlight on, 4 color bars, white text, a moving counter.

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#include "hardware_pins.h"
#include "tca9555.h"

static constexpr uint8_t kRotation = 1;  // 1 or 3 = landscape (320x240)

static TCA9555 exio(I2C_ADDR_TCA9555);
// RST = -1: reset is driven through the IO expander, not a GPIO.
static Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

static bool lcdHardwareReset() {
  if (!exio.begin()) return false;
  exio.pinMode(EXIO_LCD_RST, OUTPUT);
  exio.pinMode(EXIO_TP_RST, OUTPUT);
  exio.pinMode(EXIO_PA_CTRL, OUTPUT);
  exio.digitalWrite(EXIO_PA_CTRL, LOW);   // keep amp off in this stage

  exio.digitalWrite(EXIO_LCD_RST, LOW);
  exio.digitalWrite(EXIO_TP_RST, LOW);
  delay(20);
  exio.digitalWrite(EXIO_LCD_RST, HIGH);
  exio.digitalWrite(EXIO_TP_RST, HIGH);
  delay(150);
  return true;
}

static void drawTestScreen() {
  tft.fillScreen(ST77XX_BLACK);

  // Four color bars across the top quarter.
  const int16_t w = tft.width();
  const int16_t barH = 40;
  const uint16_t colors[] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_WHITE};
  for (int i = 0; i < 4; ++i) {
    tft.fillRect(i * w / 4, 0, w / 4, barH, colors[i]);
  }

  // Border to verify the full 240x320 area is addressable (no offset issues).
  tft.drawRect(0, 0, tft.width(), tft.height(), ST77XX_YELLOW);

  tft.setTextWrap(false);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(8, barH + 14);
  tft.println("ESP32-S3-AUDIO");
  tft.setTextSize(2);
  tft.setCursor(8, barH + 46);
  tft.println("Stage 3: LCD landscape OK");

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(8, barH + 76);
  tft.printf("ST7789T3 %dx%d  rotation=%d\n", tft.width(), tft.height(), tft.getRotation());
  tft.setCursor(8, barH + 88);
  tft.printf("SPI SCK=%d MOSI=%d CS=%d DC=%d  RST=EXIO%d  BL=GPIO%d\n",
             PIN_TFT_SCK, PIN_TFT_MOSI, PIN_TFT_CS, PIN_TFT_DC, EXIO_LCD_RST, PIN_TFT_BL);

  // Orientation guide: arrow pointing to the top edge + labels on each edge.
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(w / 2 - 9, barH + 4);
  tft.print("TOP");
  tft.setCursor(w / 2 - 20, tft.height() - 12);
  tft.print("BOTTOM");
  tft.setCursor(3, tft.height() / 2 - 4);
  tft.print("L");
  tft.setCursor(w - 9, tft.height() / 2 - 4);
  tft.print("R");

  // Corner markers.
  tft.fillCircle(6, tft.height() - 7, 4, ST77XX_MAGENTA);
  tft.fillCircle(tft.width() - 7, tft.height() - 7, 4, ST77XX_MAGENTA);
}

void setup() {
  Serial.begin(115200);
  const uint32_t start = millis();
  while (!Serial && millis() - start < 5000) delay(10);
  delay(300);

  Serial.println("==============================================");
  Serial.println(" ESP32-S3-AUDIO-Board  stage 3: LCD (ST7789)");
  Serial.println("==============================================");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  if (!lcdHardwareReset()) {
    Serial.println("!! TCA9555 not found - cannot reset LCD. Stop.");
    return;
  }
  Serial.println("TCA9555 ok, LCD_RST pulsed");

  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, HIGH);
  Serial.printf("Backlight GPIO%d = HIGH\n", PIN_TFT_BL);

  SPI.begin(PIN_TFT_SCK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(TFT_WIDTH, TFT_HEIGHT);
  tft.setSPISpeed(40000000);
  tft.setRotation(kRotation);
  Serial.printf("tft.init(%d, %d) done, SPI 40MHz, rotation=%d -> %dx%d\n",
                TFT_WIDTH, TFT_HEIGHT, kRotation, tft.width(), tft.height());

  // Quick full-screen flashes so a wrong-but-alive panel is still visible.
  tft.fillScreen(ST77XX_RED);   delay(250);
  tft.fillScreen(ST77XX_GREEN); delay(250);
  tft.fillScreen(ST77XX_BLUE);  delay(250);

  drawTestScreen();
  Serial.println("Test screen drawn. Counter running in loop().");
}

void loop() {
  static uint32_t frame = 0;
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (now - lastMs < 500) { delay(5); return; }
  lastMs = now;

  // Counter box near the bottom (left of the BOTTOM label).
  const int16_t y = tft.height() - 40;
  tft.fillRect(8, y, 160, 20, ST77XX_BLACK);
  tft.setCursor(8, y + 2);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_GREEN);
  tft.printf("frame %lu", frame);

  if (frame % 10 == 0) {
    Serial.printf("[lcd] frame=%lu uptime=%lu ms\n", frame, now);
  }
  ++frame;
}
