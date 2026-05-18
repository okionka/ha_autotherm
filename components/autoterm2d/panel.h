#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  ControllerPanelComponent
//  Handles UART1 (physical controller panel side):
//    • Reads bytes from panel (UART1 RX)
//    • Forwards every byte unchanged to heater (UART2 TX)
//    • Logs complete frames at DEBUG level
//    • NO command injection, NO protocol modification
// ─────────────────────────────────────────────────────────────────────────────

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace autoterm2d {

class ControllerPanelComponent : public Component,
                                  public uart::UARTDevice {
 public:
  ControllerPanelComponent() = default;

  // Reference to UART2 (heater TX) for transparent forwarding
  void set_heater_uart(uart::UARTComponent *u) { heater_uart_ = u; }

  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    ESP_LOGD("controller", "ControllerPanelComponent ready (panel → heater bridge)");
  }

  void loop() override {
    const uint32_t now = millis();
    bool got_byte = false;

    while (available()) {
      uint8_t b;
      read_byte(&b);
      frame_buf_.push_back(b);
      // Transparent forward – no modification
      if (heater_uart_) heater_uart_->write_byte(b);
      got_byte = true;
    }
    if (got_byte) last_rx_ms_ = now;

    // Log complete frame after 50 ms inter-frame gap
    if (!frame_buf_.empty() && (now - last_rx_ms_) > 50) {
      log_frame();
      frame_buf_.clear();
    }
  }

 private:
  uart::UARTComponent  *heater_uart_{nullptr};
  std::vector<uint8_t>  frame_buf_;
  uint32_t              last_rx_ms_{0};

  void log_frame() {
    std::string hex;
    hex.reserve(frame_buf_.size() * 3);
    for (size_t i = 0; i < frame_buf_.size(); i++) {
      char b[4];
      snprintf(b, sizeof(b), i ? ":%02X" : "%02X", frame_buf_[i]);
      hex += b;
    }
    ESP_LOGD("controller", "→ heater: %s", hex.c_str());
  }
};

}  // namespace autoterm2d
}  // namespace esphome
