#pragma once

// Minimal CST816D/CST816S touch driver (Waveshare 2inch Capacitive Touch LCD).
// The controller sits on the shared I2C bus at 0x15 after TP_RST is released.

#include <Arduino.h>
#include <Wire.h>

class CST816D {
 public:
  explicit CST816D(uint8_t addr = 0x15, TwoWire& wire = Wire)
      : addr_(addr), wire_(wire) {}

  // Probe the chip and disable auto-sleep so polling works without a GPIO IRQ.
  bool begin() {
    chip_id_ = 0;
    if (!readReg(REG_CHIP_ID, &chip_id_, 1)) return false;
    if (!knownChip(chip_id_)) return false;
    // Any non-zero DisAutoSleep value keeps the controller in dynamic mode.
    return writeReg(REG_DIS_AUTOSLEEP, 0x07);
  }

  uint8_t chipId() const { return chip_id_; }

  // True while a finger is on the panel. A dropped ACK keeps the last state
  // briefly so a glitch cannot end a PTT take early, but a stuck bus cannot
  // hold the mic open forever.
  bool touched() {
    uint8_t buf[7] = {};
    if (!readReg(REG_DATA, buf, sizeof(buf))) {
      if (last_touched_) {
        const uint32_t now = millis();
        if (fail_since_ms_ == 0) {
          fail_since_ms_ = now;
        } else if (now - fail_since_ms_ > 300) {
          last_touched_ = false;
          fail_since_ms_ = 0;
        }
      }
      return last_touched_;
    }
    fail_since_ms_ = 0;
    last_touched_ = buf[REG_FINGER_NUM] > 0;
    return last_touched_;
  }

 private:
  static constexpr uint8_t REG_DATA = 0x00;
  static constexpr uint8_t REG_FINGER_NUM = 0x02;
  static constexpr uint8_t REG_CHIP_ID = 0xA7;
  static constexpr uint8_t REG_DIS_AUTOSLEEP = 0xFE;

  static bool knownChip(uint8_t id) {
    return id == 0xB4 || id == 0xB5 || id == 0xB6 || id == 0x20;
  }

  bool readReg(uint8_t reg, uint8_t* data, uint8_t length) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    if (wire_.endTransmission(false) != 0) return false;
    if (wire_.requestFrom(addr_, length) != length) return false;
    for (uint8_t i = 0; i < length; ++i) {
      data[i] = static_cast<uint8_t>(wire_.read());
    }
    return true;
  }

  bool writeReg(uint8_t reg, uint8_t value) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    wire_.write(value);
    return wire_.endTransmission() == 0;
  }

  uint8_t addr_;
  TwoWire& wire_;
  uint8_t chip_id_ = 0;
  bool last_touched_ = false;
  uint32_t fail_since_ms_ = 0;
};
