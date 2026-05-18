#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Autotherm2D – ESPHome External Climate Component  (ESPHome 2026.x)
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include "esphome.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace autotherm2d {

static const char *PRESET_T_HEATER = "By T Heater";
static const char *PRESET_T_PANEL  = "By T Panel";
static const char *PRESET_T_AIR    = "By T Air";
static const char *PRESET_POWER    = "By Power";

static constexpr uint8_t  CMD_START        = 0x01;
static constexpr uint8_t  CMD_SETTINGS     = 0x02;
static constexpr uint8_t  CMD_STOP         = 0x03;
static constexpr uint8_t  CMD_VERSION      = 0x06;
static constexpr uint8_t  CMD_STATUS       = 0x0F;
static constexpr uint8_t  CMD_PANEL_TEMP   = 0x11;

static constexpr int      MAX_BYTES_PER_LOOP = 64;
static constexpr int      MAX_PAYLOAD_LEN    = 64;
static constexpr uint32_t PARSER_TIMEOUT_MS  = 500;
static constexpr float    AIR_TEMP_MAX_JUMP  = 2.0f;   // °C per update


class Autotherm2DClimate : public climate::Climate,
                            public Component,
                            public uart::UARTDevice {
 public:
  Autotherm2DClimate() = default;

  // ── Sensor wiring (called from climate.py generated code) ──────────────────

  // Room temperature for current_temperature display
  void set_room_temperature_sensor(sensor::Sensor *s) {
    s->add_on_state_callback([this](float v) {
      this->current_temperature = v;
      this->publish_state();
    });
  }

  // HA sensor used as temperature source in "By T Air" mode
  void set_air_temp_source_sensor(sensor::Sensor *s) {
    s_air_temp_source_ = s;
    s->add_on_state_callback([this](float temp) {
      on_air_temp_source_update(temp);
    });
  }

  // Diagnostic / informational sensors
  void set_heater_board_temperature_sensor(sensor::Sensor *s) { s_intake_temp_     = s; }
  void set_battery_voltage_sensor(sensor::Sensor *s)          { s_battery_voltage_ = s; }
  void set_air_temperature_sensor(sensor::Sensor *s)          { s_air_temp_        = s; }
  void set_panel_temperature_sensor(sensor::Sensor *s)        { s_panel_temp_      = s; }
  void set_power_level_sensor(sensor::Sensor *s)              { s_power_level_     = s; }
  void set_ventilation_power_sensor(sensor::Sensor *s)        { s_fan_actual_      = s; }
  void set_status_sensor(sensor::Sensor *s)                   { s_status_code_     = s; }
  void set_error_code_sensor(sensor::Sensor *s)               { s_error_code_      = s; }

  void set_status_text_sensor(text_sensor::TextSensor *s)     { s_status_text_     = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s)      { s_error_text_      = s; }
  void set_software_version_sensor(text_sensor::TextSensor *s){ s_sw_version_      = s; }

  // Called from UART debug lambda (bytes consumed by uart_debug before loop())
  void process_incoming_byte(uint8_t byte) { process_byte(byte); }

  // ── ESPHome Component ──────────────────────────────────────────────────────
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    reset_parser();
    this->set_supported_custom_presets({
        PRESET_T_HEATER, PRESET_T_PANEL, PRESET_T_AIR, PRESET_POWER});
    this->mode               = climate::CLIMATE_MODE_OFF;
    this->target_temperature = 15.0f;
    this->fan_mode           = climate::CLIMATE_FAN_ON;
    this->set_custom_preset_(PRESET_T_HEATER);
    this->publish_state();
    ESP_LOGD("autotherm2d", "Autotherm2D ready");
  }

  void loop() override {
    // Bytes are fed via process_incoming_byte() from the UART debug lambda.
    // We only handle the parser timeout here.
    const uint32_t now = millis();
    if (read_state_ != 0 && (now - parse_start_ms_) > PARSER_TIMEOUT_MS) {
      ESP_LOGW("autotherm2d", "Parser timeout (state=%d) – reset", read_state_);
      reset_parser();
    }
  }

  // ── Climate traits ─────────────────────────────────────────────────────────
  climate::ClimateTraits traits() override {
    auto t = climate::ClimateTraits();
    t.set_supports_current_temperature(true);
    t.set_supported_modes({climate::CLIMATE_MODE_OFF, climate::CLIMATE_MODE_HEAT});
    t.set_supported_fan_modes({climate::CLIMATE_FAN_ON, climate::CLIMATE_FAN_OFF});
    t.set_visual_min_temperature(0.0f);
    t.set_visual_max_temperature(30.0f);
    t.set_visual_temperature_step(1.0f);
    return t;
  }

  // ── Climate control ────────────────────────────────────────────────────────
  void control(const climate::ClimateCall &call) override {
    bool needs_send = false;
    if (call.get_mode().has_value()) {
      auto m = *call.get_mode();
      if (m == climate::CLIMATE_MODE_OFF) {
        send_shutdown_command();
        this->mode = climate::CLIMATE_MODE_OFF;
      } else {
        this->mode = climate::CLIMATE_MODE_HEAT;
        needs_send = true;
      }
    }
    if (call.get_target_temperature().has_value()) {
      temp_set_ = static_cast<uint8_t>(*call.get_target_temperature());
      this->target_temperature = temp_set_;
      needs_send = true;
    }
    if (call.get_fan_mode().has_value()) {
      auto fm = *call.get_fan_mode();
      vent_cmd_ = (fm == climate::CLIMATE_FAN_ON) ? 1 : 2;
      this->fan_mode = fm;
      needs_send = true;
    }
    auto preset_ref = call.get_custom_preset();
    if (!preset_ref.empty()) {
      power_mode_ = preset_to_power_mode(preset_ref.c_str());
      this->set_custom_preset_(preset_ref.c_str());
      last_sent_air_temp_ = NAN;   // reset smoothing on preset change
      needs_send = true;
    }
    if (needs_send && this->mode == climate::CLIMATE_MODE_HEAT)
      send_control_command();
    this->publish_state();
  }

 private:
  // ── Sensor pointers ────────────────────────────────────────────────────────
  sensor::Sensor *s_intake_temp_{nullptr};
  sensor::Sensor *s_battery_voltage_{nullptr};
  sensor::Sensor *s_air_temp_{nullptr};
  sensor::Sensor *s_panel_temp_{nullptr};
  sensor::Sensor *s_power_level_{nullptr};
  sensor::Sensor *s_fan_actual_{nullptr};
  sensor::Sensor *s_status_code_{nullptr};
  sensor::Sensor *s_error_code_{nullptr};
  sensor::Sensor *s_air_temp_source_{nullptr};

  text_sensor::TextSensor *s_status_text_{nullptr};
  text_sensor::TextSensor *s_error_text_{nullptr};
  text_sensor::TextSensor *s_sw_version_{nullptr};

  // ── Control state ──────────────────────────────────────────────────────────
  uint8_t temp_set_    {15};
  uint8_t power_mode_  {1};
  uint8_t vent_cmd_    {1};    // outgoing vent: 1=On, 2=Off
  uint8_t power_level_ {4};
  uint8_t major_state_ {0};

  float   last_sent_air_temp_{NAN};
  bool    version_requested_{false};

  // ── Parser state ───────────────────────────────────────────────────────────
  uint8_t  read_state_{0};
  int      message_length_{0};
  uint8_t  command_id_{0};
  std::vector<uint8_t> message_data_;
  uint32_t parse_start_ms_{0};

  // ── Protocol string helpers ────────────────────────────────────────────────
  static const char *state_description(uint8_t major, uint8_t minor) {
    if (major == 0 && minor == 0) return "Sleep/Off";
    if (major == 0 && minor == 1) return "Standby";
    if (major == 1 && minor == 0) return "Purge: cooling flame sensor";
    if (major == 1 && minor == 1) return "Purge: ventilating chamber";
    if (major == 2 && minor == 1) return "Pre-heat (glow plug on)";
    if (major == 2 && minor == 2) return "Ignition seq 1";
    if (major == 2 && minor == 3) return "Ignition seq 2";
    if (major == 2 && minor == 4) return "Ramp up";
    if (major == 3 && minor == 0) return "Heating (PID active)";
    if (major == 3 && minor == 35) return "Ventilation only";
    if (major == 3 && minor == 4) return "Cooling down";
    if (major == 4 && minor == 0) return "Shutdown complete";
    return "Unknown state";
  }

  static const char *error_description(uint8_t err) {
    switch (err) {
      case  0: return "OK";
      case  1: return "Overheat";
      case  2: return "Potential overheat";
      case  5: return "Flame sensor fault";
      case  6: return "Temperature sensor fault";
      case  9: return "Glow plug fault";
      case 10: return "Motor RPM fault";
      case 11: return "Air temperature fault";
      case 12: return "Over voltage";
      case 13: return "No start – ignition failed";
      case 15: return "Under voltage";
      case 16: return "Ventilation duration exceeded";
      case 17: return "Fuel pump fault";
      case 20: return "No communication";
      case 29: return "Flame blowout";
      case 30: return "Flame detected before start";
      case 33: return "Control lockout (3× overheat)";
      case 37: return "Hard lockout – send unlock cmd";
      default: return "Unknown error";
    }
  }

  static const char *mode_description(uint8_t mode) {
    switch (mode) {
      case 1: return "By T Heater";
      case 2: return "By T Panel";
      case 3: return "By T Air";
      case 4: return "By Power";
      default: return "Unknown";
    }
  }

  // ── Helpers ────────────────────────────────────────────────────────────────
  void reset_parser() { read_state_ = 0; message_data_.clear(); }

  uint8_t safe_byte(size_t i) const {
    return i < message_data_.size() ? message_data_[i] : 0;
  }
  static int to_signed_temp(uint8_t raw) {
    return (raw > 127) ? (static_cast<int>(raw) - 256) : static_cast<int>(raw);
  }
  template<typename T>
  static void publish_if(sensor::Sensor *s, T v) {
    if (s) s->publish_state(static_cast<float>(v));
  }
  static void publish_text(text_sensor::TextSensor *s, const char *v) {
    if (s) s->publish_state(v);
  }

  static uint8_t preset_to_power_mode(const char *p) {
    if (strcmp(p, PRESET_T_PANEL) == 0) return 2;
    if (strcmp(p, PRESET_T_AIR)   == 0) return 3;
    if (strcmp(p, PRESET_POWER)   == 0) return 4;
    return 1;
  }
  static const char *power_mode_to_preset(uint8_t m) {
    switch (m) {
      case 2: return PRESET_T_PANEL;
      case 3: return PRESET_T_AIR;
      case 4: return PRESET_POWER;
      default: return PRESET_T_HEATER;
    }
  }

  // ── CRC-16 Modbus ──────────────────────────────────────────────────────────
  static uint16_t crc16_modbus(const std::vector<uint8_t> &data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t b : data) {
      crc ^= b;
      for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
  }

  // ── Frame builders ─────────────────────────────────────────────────────────
  void send_frame(uint8_t cmd) {
    std::vector<uint8_t> frame = {
        0xAA, 0x03, 0x06, 0x00, cmd, 0xFF, 0xFF,
        power_mode_, temp_set_, vent_cmd_, power_level_};
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
    ESP_LOGD("autotherm2d", "TX %-8s | mode=%s temp=%d°C vent=%s level=%d",
             cmd == CMD_START ? "START" : "UPDATE",
             mode_description(power_mode_), temp_set_,
             vent_cmd_ == 1 ? "On" : "Off", power_level_ + 1);
  }

  void send_control_command() { send_frame(CMD_START); }

  void send_shutdown_command() {
    const uint8_t raw[] = {0xAA, 0x03, 0x00, 0x00, 0x03, 0x5D, 0x7C};
    write_array(raw, sizeof(raw));
    ESP_LOGD("autotherm2d", "TX SHUTDOWN");
  }

  // Send 0x11 (panel/air temperature) to heater
  void send_temp_0x11(int8_t temp_c, const char *source = "") {
    std::vector<uint8_t> frame = {
        0xAA, 0x03, 0x01, 0x00, CMD_PANEL_TEMP,
        static_cast<uint8_t>(temp_c)};
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
    ESP_LOGD("autotherm2d", "TX 0x11 %d°C%s", temp_c, source);
  }

  // Request firmware version from heater (once)
  void request_version() {
    const uint8_t req[] = {0xAA, 0x03, 0x00, 0x00, 0x06, 0x5E, 0xBC};
    write_array(req, sizeof(req));
    ESP_LOGD("autotherm2d", "TX 0x06 (version request)");
  }

  // ── "By T Air" HA-sensor callback ─────────────────────────────────────────
  void on_air_temp_source_update(float new_temp) {
    if (power_mode_ != 3) return;          // only active in By T Air mode
    if (major_state_ == 0) return;          // only when heater is active
    if (!std::isfinite(new_temp)) return;   // reject NaN / inf
    if (new_temp < -40.0f || new_temp > 80.0f) return;  // sanity bounds

    // Smooth: limit jump to AIR_TEMP_MAX_JUMP per update
    float target = new_temp;
    if (std::isfinite(last_sent_air_temp_)) {
      float delta = target - last_sent_air_temp_;
      if (std::abs(delta) > AIR_TEMP_MAX_JUMP)
        target = last_sent_air_temp_ + (delta > 0 ? AIR_TEMP_MAX_JUMP : -AIR_TEMP_MAX_JUMP);
    }

    int8_t t_byte = static_cast<int8_t>(std::round(target));
    send_temp_0x11(t_byte, " (By T Air / HA sensor)");
    last_sent_air_temp_ = static_cast<float>(t_byte);
  }

  // ── State machine ──────────────────────────────────────────────────────────
  void process_byte(uint8_t byte) {
    switch (read_state_) {
      case 0:
        if (byte == 0xAA) { message_data_.clear(); parse_start_ms_ = millis(); read_state_ = 1; }
        break;
      case 1: read_state_ = 2; break;
      case 2:
        if (byte == 0 || byte > MAX_PAYLOAD_LEN) {
          ESP_LOGW("autotherm2d", "Bad length 0x%02X", byte); reset_parser();
        } else { message_length_ = byte; read_state_ = 3; }
        break;
      case 3: read_state_ = 4; break;
      case 4: command_id_ = byte; read_state_ = 5; break;
      case 5:
        message_data_.push_back(byte);
        if (message_data_.size() > MAX_PAYLOAD_LEN) { reset_parser(); break; }
        if ((int)message_data_.size() >= message_length_) { process_message(); reset_parser(); }
        break;
      default: reset_parser(); break;
    }
  }

  void process_message() {
    switch (command_id_) {
      case CMD_STATUS:    handle_status();    break;
      case CMD_PANEL_TEMP:handle_panel_temp(); break;
      case CMD_SETTINGS:  handle_settings();  break;
      case CMD_VERSION:   handle_version();   break;
      default:            handle_unknown();   break;
    }
  }

  // ── 0x0F – Status ──────────────────────────────────────────────────────────
  void handle_status() {
    uint8_t major  = safe_byte(0);
    uint8_t minor  = safe_byte(1);
    uint8_t error  = safe_byte(2);
    int     t1     = to_signed_temp(safe_byte(3));
    uint8_t t2_raw = safe_byte(4);
    float   volts  = static_cast<float>((safe_byte(5) << 8) | safe_byte(6)) / 10.0f;
    uint16_t flame_k = (safe_byte(7) << 8) | safe_byte(8);
    float   flame_c  = (flame_k > 273) ? (flame_k - 273.15f) : 0.0f;
    uint8_t fan_sp   = safe_byte(11);
    uint8_t fan_act  = safe_byte(12);
    float   pump_hz  = safe_byte(14) / 100.0f;   // ÷100 per schroeder-robert
    bool    t2_ok    = (t2_raw != 0x7F);
    int     t2       = to_signed_temp(t2_raw);

    // Log
    if (t2_ok) {
      ESP_LOGD("autotherm2d",
        "STATUS %d.%d (%s) | Err:%d (%s) | T-intake:%d°C T-out:%d°C | "
        "%.1fV Flame:%.0f°C | Fan:%d/%dHz Pump:%.2fHz",
        major, minor, state_description(major, minor), error, error_description(error),
        t1, t2, volts, flame_c, fan_sp, fan_act, pump_hz);
    } else {
      ESP_LOGD("autotherm2d",
        "STATUS %d.%d (%s) | Err:%d (%s) | T-intake:%d°C T-out:n/a | "
        "%.1fV Flame:%.0f°C | Fan:%d/%dHz Pump:%.2fHz",
        major, minor, state_description(major, minor), error, error_description(error),
        t1, volts, flame_c, fan_sp, fan_act, pump_hz);
    }

    // Request firmware version once after first valid status
    if (!version_requested_) {
      version_requested_ = true;
      request_version();
    }

    major_state_ = major;

    // Numeric status code (major * 256 + minor)
    publish_if(s_status_code_,   static_cast<float>((major << 8) | minor));
    // Textual status
    publish_text(s_status_text_, state_description(major, minor));
    // Error (separate sensor + text)
    publish_if(s_error_code_,    static_cast<float>(error));
    publish_text(s_error_text_,  error_description(error));

    // Physical sensors
    publish_if(s_intake_temp_,     static_cast<float>(t1));
    // Air/output temp: publish NAN when disconnected (HA shows "unavailable")
    if (s_air_temp_) s_air_temp_->publish_state(t2_ok ? static_cast<float>(t2) : NAN);
    publish_if(s_battery_voltage_, volts);
    publish_if(s_fan_actual_,      static_cast<float>(fan_act));

    // Sync Climate mode with heater state
    auto new_mode = (major != 0)
                        ? climate::CLIMATE_MODE_HEAT
                        : climate::CLIMATE_MODE_OFF;
    if (new_mode != this->mode) {
      this->mode = new_mode;
      this->publish_state();
    }
  }

  // ── 0x11 – Panel temperature ───────────────────────────────────────────────
  void handle_panel_temp() {
    int t = to_signed_temp(safe_byte(0));
    ESP_LOGD("autotherm2d", "PANEL TEMP  %d°C", t);
    publish_if(s_panel_temp_, static_cast<float>(t));
  }

  // ── 0x02 – Settings echo ───────────────────────────────────────────────────
  void handle_settings() {
    if (major_state_ == 0) return;

    bool    use_work_time = (safe_byte(0) == 0);
    uint8_t work_min      = safe_byte(1);
    uint8_t mode          = safe_byte(2);
    uint8_t target        = safe_byte(3);
    uint8_t vent          = safe_byte(4);   // 0=Off, 1=On (heater encoding)
    uint8_t level         = safe_byte(5);

    ESP_LOGD("autotherm2d",
      "SETTINGS    mode=%-12s target=%d°C vent=%-3s level=%d/10 time=%s",
      mode_description(mode), target,
      vent == 1 ? "On" : "Off", level + 1,
      use_work_time ? (std::to_string(work_min) + "min").c_str() : "unlimited");

    publish_if(s_power_level_, static_cast<float>(level + 1));

    bool changed = false;
    if (temp_set_ != target)  { temp_set_ = target; this->target_temperature = target; changed = true; }
    if (power_level_ != level) { power_level_ = level; changed = true; }
    if (power_mode_ != mode) {
      power_mode_ = mode;
      this->set_custom_preset_(power_mode_to_preset(mode));
      changed = true;
    }
    auto new_fan = (vent == 1) ? climate::CLIMATE_FAN_ON : climate::CLIMATE_FAN_OFF;
    if (this->fan_mode != new_fan) {
      vent_cmd_ = (vent == 1) ? 1 : 2;
      this->fan_mode = new_fan;
      changed = true;
    }
    if (changed) this->publish_state();
  }

  // ── 0x06 – Software version response ──────────────────────────────────────
  void handle_version() {
    if (message_data_.size() < 4) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
             safe_byte(0), safe_byte(1), safe_byte(2), safe_byte(3));
    ESP_LOGI("autotherm2d", "Firmware: %s (bootloader v%d)", buf, safe_byte(4));
    publish_text(s_sw_version_, buf);
  }

  // ── Unknown command ────────────────────────────────────────────────────────
  void handle_unknown() {
    std::string hex;
    for (size_t i = 0; i < message_data_.size() && i < 12; i++) {
      char b[4]; snprintf(b, sizeof(b), i ? ":%02X" : "%02X", message_data_[i]);
      hex += b;
    }
    if (message_data_.size() > 12) hex += "...";
    ESP_LOGD("autotherm2d", "CMD 0x%02X len=%-2d %s",
             command_id_, message_length_, hex.c_str());
  }
};

}  // namespace autotherm2d
}  // namespace esphome
