#pragma once

// Minimal TCA9555 16-bit I2C IO expander driver (Waveshare ESP32-S3-AUDIO-Board).
// Pins are numbered 0..15 (EXIO0..EXIO15): 0..7 = port 0, 8..15 = port 1.

#include <Arduino.h>
#include <Wire.h>

class TCA9555 {
 public:
  explicit TCA9555(uint8_t addr = 0x20, TwoWire& wire = Wire) : addr_(addr), wire_(wire) {}

  // Returns false if the chip does not ACK.
  bool begin() {
    wire_.beginTransmission(addr_);
    if (wire_.endTransmission() != 0) return false;
    // Cache current output / config registers.
    output_ = read16(REG_OUTPUT0);
    config_ = read16(REG_CONFIG0);  // 1 = input (power-on default 0xFFFF)
    return true;
  }

  // mode: OUTPUT or INPUT
  void pinMode(uint8_t pin, uint8_t mode) {
    if (pin > 15) return;
    if (mode == OUTPUT) config_ &= ~(1u << pin);
    else                config_ |=  (1u << pin);
    write16(REG_CONFIG0, config_);
  }

  void digitalWrite(uint8_t pin, uint8_t level) {
    if (pin > 15) return;
    if (level) output_ |=  (1u << pin);
    else       output_ &= ~(1u << pin);
    write16(REG_OUTPUT0, output_);
  }

  int digitalRead(uint8_t pin) {
    if (pin > 15) return LOW;
    return (read16(REG_INPUT0) >> pin) & 1u ? HIGH : LOW;
  }

  uint16_t readAll()   { return read16(REG_INPUT0); }
  uint16_t configAll() { return config_; }
  uint16_t outputAll() { return output_; }

 private:
  static constexpr uint8_t REG_INPUT0  = 0x00;
  static constexpr uint8_t REG_OUTPUT0 = 0x02;
  static constexpr uint8_t REG_CONFIG0 = 0x06;

  uint16_t read16(uint8_t reg) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    if (wire_.endTransmission(false) != 0) return 0;
    if (wire_.requestFrom(addr_, static_cast<uint8_t>(2)) != 2) return 0;
    uint16_t lo = wire_.read();
    uint16_t hi = wire_.read();
    return static_cast<uint16_t>(lo | (hi << 8));
  }

  void write16(uint8_t reg, uint16_t value) {
    wire_.beginTransmission(addr_);
    wire_.write(reg);
    wire_.write(static_cast<uint8_t>(value & 0xFF));
    wire_.write(static_cast<uint8_t>(value >> 8));
    wire_.endTransmission();
  }

  uint8_t addr_;
  TwoWire& wire_;
  uint16_t output_ = 0xFFFF;
  uint16_t config_ = 0xFFFF;
};
