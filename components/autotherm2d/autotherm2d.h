#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Autotherm2D – ESPHome External Climate Component
//  Controls a diesel / fuel-burning heater via UART.
//
//  Climate features
//  ─────────────────
//  Modes    : OFF, HEAT
//  Fan modes: ON (ventilator active), OFF (ventilator inactive)
//  Presets  : "By T Heater" | "By T Panel" | "By T Air" | "By Power"
//             (select which sensor the heater uses to regulate itself)
//  Target T : 0–30 °C, 1 °C steps
//  Current T: fed by any ESPHome sensor (e.g. homeassistant platform sensor)
//
//  Protocol
//  ─────────
//  9600 baud, frames: AA <len> <msglen> 00 <cmd_id> [payload…] <CRC16-hi> <CRC16-lo>
//  CRC-16 Modbus (poly 0xA001, init 0xFFFF), appended big-endian (MSB first).
//
//  Parser safety
//  ─────────────
//  • Max 64 bytes processed per loop() to avoid watchdog reset
//  • 500 ms timeout → automatic parser reset
//  • message_length validated (1–64)
//  • All payload accesses bounds-checked
// ─────────────────────────────────────────────────────────────────────────────

#include "esphome.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace autotherm2d {

// ── Power-mode ↔ preset name mapping ─────────────────────────────────────────
static const char *PRESET_T_HEATER = "By T Heater";
static const char *PRESET_T_PANEL  = "By T Panel";
static const char *PRESET_T_AIR    = "By T Air";
static const char *PRESET_POWER    = "By Power";

// ── UART command IDs received from heater ────────────────────────────────────
static constexpr uint8_t CMD_LIVE_STATUS   = 15;
static constexpr uint8_t CMD_PANEL_TEMP    = 17;
static constexpr uint8_t CMD_SETTINGS_ECHO = 2;

// ── Parser limits ─────────────────────────────────────────────────────────────
static constexpr int     MAX_BYTES_PER_LOOP = 64;
static constexpr int     MAX_PAYLOAD_LEN    = 64;
static constexpr uint32_t PARSER_TIMEOUT_MS = 500;


class Autotherm2DClimate : public climate::Climate,
                            public Component,
                            public uart::UARTDevice {
 public:
  explicit Autotherm2DClimate(uart::UARTComponent *parent)
      : UARTDevice(parent) {}

  // ── Sensor wiring (called by climate.py generated code) ─────────────────────
  void set_room_temperature_sensor(sensor::Sensor *s) {
    s->add_on_state_callback([this](float v) {
      this->current_temperature = v;
      this->publish_state();
    });
  }
  void set_heater_board_temperature_sensor(sensor::Sensor *s) { s_heater_board_temp_ = s; }
  void set_battery_voltage_sensor(sensor::Sensor *s)          { s_battery_voltage_   = s; }
  void set_air_temperature_sensor(sensor::Sensor *s)          { s_air_temperature_   = s; }
  void set_panel_temperature_sensor(sensor::Sensor *s)        { s_panel_temperature_ = s; }
  void set_power_level_sensor(sensor::Sensor *s)              { s_power_level_       = s; }
  void set_ventilation_power_sensor(sensor::Sensor *s)        { s_ventilation_power_ = s; }
  void set_status_sensor(sensor::Sensor *s)                   { s_status_code_       = s; }

  // ── ESPHome Component ────────────────────────────────────────────────────────
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    reset_parser();
    // Initial climate state
    this->mode               = climate::CLIMATE_MODE_OFF;
    this->target_temperature = 15.0f;
    this->fan_mode           = climate::CLIMATE_FAN_ON;
    this->custom_preset      = PRESET_T_HEATER;
    this->publish_state();
    ESP_LOGD("autotherm2d", "Autotherm2D climate component ready");
  }

  void loop() override {
    const uint32_t now = millis();

    // Parser timeout guard
    if (read_state_ != 0 && (now - parse_start_ms_) > PARSER_TIMEOUT_MS) {
      ESP_LOGW("autotherm2d", "Parser timeout (state=%d) – reset", read_state_);
      reset_parser();
    }

    int budget = MAX_BYTES_PER_LOOP;
    while (available() && budget-- > 0) {
      uint8_t byte;
      read_byte(&byte);
      process_byte(byte);
    }
  }

  // ── Climate::traits – advertise capabilities to HA ──────────────────────────
  climate::ClimateTraits traits() override {
    auto t = climate::ClimateTraits();
    t.set_supports_current_temperature(true);
    t.set_supported_modes({
        climate::CLIMATE_MODE_OFF,
        climate::CLIMATE_MODE_HEAT,
    });
    t.set_supported_fan_modes({
        climate::CLIMATE_FAN_ON,
        climate::CLIMATE_FAN_OFF,
    });
    t.set_supported_custom_presets({
        PRESET_T_HEATER,
        PRESET_T_PANEL,
        PRESET_T_AIR,
        PRESET_POWER,
    });
    t.set_visual_min_temperature(0.0f);
    t.set_visual_max_temperature(30.0f);
    t.set_visual_temperature_step(1.0f);
    t.set_supports_two_point_target_temperature(false);
    return t;
  }

  // ── Climate::control – handle commands from HA ──────────────────────────────
  void control(const climate::ClimateCall &call) override {
    bool needs_send = false;

    if (call.get_mode().has_value()) {
      auto new_mode = *call.get_mode();
      if (new_mode == climate::CLIMATE_MODE_OFF) {
        send_shutdown_command();
        this->mode = climate::CLIMATE_MODE_OFF;
      } else if (new_mode == climate::CLIMATE_MODE_HEAT) {
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
      ventilation_ = (fm == climate::CLIMATE_FAN_ON) ? 1 : 2;
      this->fan_mode = fm;
      needs_send = true;
    }

    if (call.get_custom_preset().has_value()) {
      const std::string &preset = *call.get_custom_preset();
      power_mode_       = preset_to_power_mode(preset);
      this->custom_preset = preset;
      needs_send = true;
    }

    if (needs_send && this->mode == climate::CLIMATE_MODE_HEAT)
      send_control_command();

    this->publish_state();
  }

 private:
  // ── Optional sensor pointers ─────────────────────────────────────────────────
  sensor::Sensor *s_heater_board_temp_{nullptr};
  sensor::Sensor *s_battery_voltage_{nullptr};
  sensor::Sensor *s_air_temperature_{nullptr};
  sensor::Sensor *s_panel_temperature_{nullptr};
  sensor::Sensor *s_power_level_{nullptr};
  sensor::Sensor *s_ventilation_power_{nullptr};
  sensor::Sensor *s_status_code_{nullptr};

  // ── Heater control state ─────────────────────────────────────────────────────
  uint8_t temp_set_    {15};  // 0–30 °C
  uint8_t power_mode_  {1};   // 1=By T Heater, 2=By T Panel, 3=By T Air, 4=By Power
  uint8_t ventilation_ {1};   // 1=On, 2=Off
  uint8_t power_level_ {4};   // 0-based (0–9), sent as power_level_ to heater
  int     heater_status_{0};  // last raw status byte from heater

  // ── Parser state ─────────────────────────────────────────────────────────────
  uint8_t  read_state_{0};
  int      message_length_{0};
  uint8_t  command_id_{0};
  std::vector<uint8_t> message_data_;
  uint32_t parse_start_ms_{0};

  // ── Helpers ──────────────────────────────────────────────────────────────────
  void reset_parser() {
    read_state_ = 0;
    message_data_.clear();
  }

  uint8_t safe_byte(size_t i) const {
    return i < message_data_.size() ? message_data_[i] : 0;
  }

  static int to_signed_temp(uint8_t raw) {
    return (raw > 127) ? (static_cast<int>(raw) - 256) : static_cast<int>(raw);
  }

  template<typename T>
  static void publish_if(sensor::Sensor *s, T value) {
    if (s) s->publish_state(static_cast<float>(value));
  }

  static uint8_t preset_to_power_mode(const std::string &preset) {
    if (preset == PRESET_T_PANEL)  return 2;
    if (preset == PRESET_T_AIR)    return 3;
    if (preset == PRESET_POWER)    return 4;
    return 1;  // default: PRESET_T_HEATER
  }

  static const char *power_mode_to_preset(uint8_t mode) {
    switch (mode) {
      case 2:  return PRESET_T_PANEL;
      case 3:  return PRESET_T_AIR;
      case 4:  return PRESET_POWER;
      default: return PRESET_T_HEATER;
    }
  }

  // ── CRC-16 Modbus ─────────────────────────────────────────────────────────────
  static uint16_t crc16_modbus(const std::vector<uint8_t> &data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t b : data) {
      crc ^= b;
      for (int i = 0; i < 8; i++) {
        if (crc & 0x0001) {
          crc = (crc >> 1) ^ 0xA001;
        } else {
          crc >>= 1;
        }
      }
    }
    return crc;
  }

  // ── Frame builder ─────────────────────────────────────────────────────────────
  void send_frame(uint8_t cmd) {
    std::vector<uint8_t> frame = {
        0xAA, 0x03, 0x06, 0x00, cmd, 0xFF, 0xFF,
        power_mode_,
        temp_set_,
        ventilation_,
        power_level_,
    };
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);  // MSB first (protocol convention)
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
    ESP_LOGD("autotherm2d",
             "TX cmd=0x%02X pm=%d t=%d v=%d p=%d",
             cmd, power_mode_, temp_set_, ventilation_, power_level_);
  }

  void send_control_command()  { send_frame(0x01); }
  void send_update_command()   { send_frame(0x02); }

  void send_shutdown_command() {
    const uint8_t raw[] = {0xAA, 0x03, 0x00, 0x00, 0x03, 0x5D, 0x7C};
    write_array(raw, sizeof(raw));
    ESP_LOGD("autotherm2d", "TX shutdown");
  }

  // ── Byte-level state machine ──────────────────────────────────────────────────
  void process_byte(uint8_t byte) {
    switch (read_state_) {
      case 0:  // Wait for 0xAA start byte
        if (byte == 0xAA) {
          message_data_.clear();
          parse_start_ms_ = millis();
          read_state_ = 1;
        }
        break;

      case 1:  // Second header byte (ignored)
        read_state_ = 2;
        break;

      case 2:  // Message length
        if (byte == 0 || byte > MAX_PAYLOAD_LEN) {
          ESP_LOGW("autotherm2d", "Invalid length 0x%02X – reset", byte);
          reset_parser();
        } else {
          message_length_ = byte;
          read_state_ = 3;
        }
        break;

      case 3:  // Reserved 0x00 (ignored)
        read_state_ = 4;
        break;

      case 4:  // Command ID
        command_id_ = byte;
        read_state_ = 5;
        break;

      case 5:  // Payload bytes
        message_data_.push_back(byte);
        if (message_data_.size() > MAX_PAYLOAD_LEN) {
          ESP_LOGW("autotherm2d", "Payload overflow – reset");
          reset_parser();
          break;
        }
        if ((int)message_data_.size() >= message_length_) {
          process_message();
          reset_parser();
        }
        break;

      default:
        reset_parser();
        break;
    }
  }

  // ── Message dispatcher ────────────────────────────────────────────────────────
  void process_message() {
    switch (command_id_) {
      case CMD_LIVE_STATUS:   handle_live_status();   break;
      case CMD_PANEL_TEMP:    handle_panel_temp();    break;
      case CMD_SETTINGS_ECHO: handle_settings_echo(); break;
      default:
        ESP_LOGV("autotherm2d", "Unknown cmd_id=%d len=%d", command_id_, message_length_);
        break;
    }
  }

  // cmd 15 – live heater status ─────────────────────────────────────────────────
  void handle_live_status() {
    publish_if(s_heater_board_temp_, to_signed_temp(safe_byte(3)));
    publish_if(s_air_temperature_,   to_signed_temp(safe_byte(4)));
    publish_if(s_battery_voltage_,   safe_byte(6) / 10.0f);

    uint8_t raw_status = safe_byte(9);
    heater_status_ = raw_status;
    publish_if(s_status_code_, raw_status);

    if (message_data_.size() > 16)
      publish_if(s_ventilation_power_, safe_byte(16));

    // Sync Climate mode with heater's actual run state
    auto new_mode = (heater_status_ != 0)
                        ? climate::CLIMATE_MODE_HEAT
                        : climate::CLIMATE_MODE_OFF;
    if (new_mode != this->mode) {
      this->mode = new_mode;
      this->publish_state();
    }
  }

  // cmd 17 – panel temperature ──────────────────────────────────────────────────
  void handle_panel_temp() {
    publish_if(s_panel_temperature_, to_signed_temp(safe_byte(0)));
  }

  // cmd 2 – settings echo (heater confirms current settings) ────────────────────
  void handle_settings_echo() {
    if (heater_status_ == 0) return;  // ignore while off

    uint8_t reported_temp    = safe_byte(3);
    uint8_t reported_power   = safe_byte(5);  // 0-based
    uint8_t reported_mode    = safe_byte(2);
    uint8_t reported_vent    = safe_byte(4);

    publish_if(s_power_level_, reported_power + 1);

    // Only update local state if it differs (avoid feedback loops)
    bool changed = false;

    if (temp_set_ != reported_temp) {
      temp_set_ = reported_temp;
      this->target_temperature = reported_temp;
      changed = true;
    }
    if (power_level_ != reported_power) {
      power_level_ = reported_power;
      changed = true;
    }
    if (power_mode_ != reported_mode) {
      power_mode_ = reported_mode;
      this->custom_preset = power_mode_to_preset(reported_mode);
      changed = true;
    }
    uint8_t new_vent_raw = reported_vent;
    auto new_fan = (new_vent_raw == 1)
                       ? climate::CLIMATE_FAN_ON
                       : climate::CLIMATE_FAN_OFF;
    if (this->fan_mode != new_fan) {
      ventilation_ = new_vent_raw;
      this->fan_mode = new_fan;
      changed = true;
    }

    if (changed)
      this->publish_state();
  }
};

}  // namespace autotherm2d
}  // namespace esphome
