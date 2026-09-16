#pragma once

// Waveshare ESP32-S3-AUDIO-Board + 2inch Capacitive Touch LCD (ST7789T3 / CST816D)
// Pin map. Reference: docs/hardware_migration_esp32s3.md
//
// Everything on this board hangs off one shared I2C bus (GPIO10/11):
//   ES8311  0x18  audio codec / DAC  (speaker)
//   TCA9555 0x20  16-bit IO expander (LCD_RST, TP_RST, PA_CTRL, buttons ...)
//   ES7210  0x40  4ch audio ADC      (dual mic array)
//   PCF85063 0x51 RTC
//   CST816D 0x15  touch controller   (on the LCD, only after TP_RST released)
//
// Audio uses ONE I2S bus shared by codec (TX) and ADC (RX):
//   MCLK 12 / BCLK 13 / LRCLK 14 / DIN 15 (mic) / DOUT 16 (speaker)

// ---------------------------------------------------------------- I2C
#ifndef PIN_I2C_SDA
#define PIN_I2C_SDA 11
#endif
#ifndef PIN_I2C_SCL
#define PIN_I2C_SCL 10
#endif

#define I2C_ADDR_ES8311   0x18
#define I2C_ADDR_TCA9555  0x20
#define I2C_ADDR_ES7210   0x40
#define I2C_ADDR_PCF85063 0x51
#define I2C_ADDR_CST816D  0x15

// ---------------------------------------------------------------- I2S (shared)
#ifndef PIN_I2S_MCLK
#define PIN_I2S_MCLK 12
#endif
#ifndef PIN_I2S_BCLK
#define PIN_I2S_BCLK 13
#endif
#ifndef PIN_I2S_LRCLK
#define PIN_I2S_LRCLK 14
#endif
#ifndef PIN_I2S_DIN            // ES7210 -> ESP32 (microphones)
#define PIN_I2S_DIN 15
#endif
#ifndef PIN_I2S_DOUT           // ESP32 -> ES8311 (speaker)
#define PIN_I2S_DOUT 16
#endif

#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE 16000
#endif

// ---------------------------------------------------------------- LCD (18-pin FPC, SPI)
#ifndef PIN_TFT_CS
#define PIN_TFT_CS 3
#endif
#ifndef PIN_TFT_SCK
#define PIN_TFT_SCK 4
#endif
#ifndef PIN_TFT_BL             // backlight, HIGH = on
#define PIN_TFT_BL 5
#endif
#ifndef PIN_TFT_DC
#define PIN_TFT_DC 7
#endif
#ifndef PIN_TFT_MISO           // shared with battery ADC
#define PIN_TFT_MISO 8
#endif
#ifndef PIN_TFT_MOSI
#define PIN_TFT_MOSI 9
#endif
#define PIN_TFT_RST (-1)       // driven via TCA9555 EXIO0, not a GPIO

#define TFT_WIDTH  240         // panel native size (portrait)
#define TFT_HEIGHT 320
#define TFT_ROTATION 1         // landscape 320x240, confirmed on hardware 2026-09-11

// ---------------------------------------------------------------- TCA9555 expander pins (EXIOx)
#define EXIO_LCD_RST   0
#define EXIO_TP_RST    1
#define EXIO_TP_INT    2
#define EXIO_SD_CS     3
#define EXIO_CAM_PWDN  5
#define EXIO_CAM_SEL   6
#define EXIO_USB_MUX   7
#define EXIO_PA_CTRL   8   // speaker amp enable, HIGH = on
#define EXIO_KEY1      9   // physical K1; unused by the app
#define EXIO_KEY2      10  // volume up, active LOW
#define EXIO_KEY3      11  // volume down, active LOW

// ---------------------------------------------------------------- Misc GPIO
#define PIN_BUTTON_BOOT 0   // active LOW, firmware recovery only
#define PIN_RGB_LED     38  // WS2812 x7
#define PIN_SD_CLK      40
#define PIN_SD_D0       41
#define PIN_SD_CMD      42
#define PIN_BAT_ADC     8

// Push-to-talk is the full LCD surface (CST816D). K1 is unused.
// K2 raises speaker volume; K3 lowers it.
