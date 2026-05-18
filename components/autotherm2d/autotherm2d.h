#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  Autotherm2D – ESPHome External Climate Component  (ESPHome 2026.x)
//  Protocol ref: helloworld.schlussdienst.net/blog/hacking-autoterm-planar-2d
//               github.com/kalutep/AutotermHeaterController
// ─────────────────────────────────────────────────────────────────────────────

#include "esphome.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/uart/uart.h"

namespace esphome {
namespace autotherm2d {

static const char *PRESET_T_HEATER = "By T Heater";
static const char *PRESET_T_PANEL  = "By T Panel";
static const char *PRESET_T_AIR    = "By T Air";
static const char *PRESET_POWER    = "By Power";

// ── Command IDs ──────────────────────────────────────────────────────────────
static constexpr uint8_t CMD_START         = 0x01;
static constexpr uint8_t CMD_SETTINGS      = 0x02;
static constexpr uint8_t CMD_STOP          = 0x03;
static constexpr uint8_t CMD_STATUS        = 0x0F;
static constexpr uint8_t CMD_PANEL_TEMP    = 0x11;

// ── Parser limits ─────────────────────────────────────────────────────────────
static constexpr int     MAX_BYTES_PER_LOOP = 64;
static constexpr int     MAX_PAYLOAD_LEN    = 64;
static constexpr uint32_t PARSER_TIMEOUT_MS = 500;


class Autotherm2DClimate : public climate::Climate,
                            public Component,
                            public uart::UARTDevice {
 public:
  Autotherm2DClimate() = default;

  // ── Sensor wiring ──────────────────────────────────────────────────────────
  void set_room_temperature_sensor(sensor::Sensor *s) {
    s->add_on_state_callback([this](float v) {
      this->current_temperature = v;
      this->publish_state();
    });
  }
  void set_heater_board_temperature_sensor(sensor::Sensor *s) { s_intake_temp_     = s; }
  void set_battery_voltage_sensor(sensor::Sensor *s)          { s_battery_voltage_ = s; }
  void set_air_temperature_sensor(sensor::Sensor *s)          { s_air_temp_        = s; }
  void set_panel_temperature_sensor(sensor::Sensor *s)        { s_panel_temp_      = s; }
  void set_power_level_sensor(sensor::Sensor *s)              { s_power_level_     = s; }
  void set_ventilation_power_sensor(sensor::Sensor *s)        { s_fan_actual_      = s; }
  void set_status_sensor(sensor::Sensor *s)                   { s_status_code_     = s; }

  // ── ESPHome Component ───────────────────────────────────────────────────────
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
    // uart_debug consumes bytes from the hardware buffer before available()
    // returns them to the component, so we don't call available()/read_byte() here.
    // We only handle the parser timeout.
    const uint32_t now = millis();
    if (read_state_ != 0 && (now - parse_start_ms_) > PARSER_TIMEOUT_MS) {
      ESP_LOGW("autotherm2d", "Parser timeout (state=%d) – reset", read_state_);
      reset_parser();
    }
  }

  // ── Climate traits ──────────────────────────────────────────────────────────
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

  // ── Climate control ─────────────────────────────────────────────────────────
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
      // Outgoing: 1=On, 2=Off  (helloworld controller encoding)
      vent_cmd_ = (fm == climate::CLIMATE_FAN_ON) ? 1 : 2;
      this->fan_mode = fm;
      needs_send = true;
    }
    auto preset_ref = call.get_custom_preset();
    if (!preset_ref.empty()) {
      power_mode_ = preset_to_power_mode(preset_ref.c_str());
      this->set_custom_preset_(preset_ref.c_str());
      needs_send = true;
    }
    if (needs_send && this->mode == climate::CLIMATE_MODE_HEAT)
      send_control_command();
    this->publish_state();
  }

 private:
  // ── Sensor pointers ─────────────────────────────────────────────────────────
  sensor::Sensor *s_intake_temp_{nullptr};    // Heater Board Temperature (intake)
  sensor::Sensor *s_battery_voltage_{nullptr};
  sensor::Sensor *s_air_temp_{nullptr};       // Ext/output temperature
  sensor::Sensor *s_panel_temp_{nullptr};     // Panel (controller) temperature
  sensor::Sensor *s_power_level_{nullptr};    // Current power level (0-10)
  sensor::Sensor *s_fan_actual_{nullptr};     // Fan actual Hz (= ventilation power)
  sensor::Sensor *s_status_code_{nullptr};    // Numeric status code

  // ── Heater control state ─────────────────────────────────────────────────────
  uint8_t temp_set_    {15};   // Target temperature °C
  uint8_t power_mode_  {1};    // 1=By T Heater … 4=By Power
  uint8_t vent_cmd_    {1};    // Outgoing vent: 1=On, 2=Off
  uint8_t power_level_ {4};    // Power level 0-9 (internal)
  uint8_t major_state_ {0};    // Status major byte from last 0x0F response

  // ── Parser state ────────────────────────────────────────────────────────────
  uint8_t  read_state_{0};
  int      message_length_{0};
  uint8_t  command_id_{0};
  std::vector<uint8_t> message_data_;
  uint32_t parse_start_ms_{0};

  // ── Protocol decode helpers ──────────────────────────────────────────────────

  static const char *state_description(uint8_t major, uint8_t minor) {
    if (major == 0 && minor == 0) return "Sleep/Off";
    if (major == 0 && minor == 1) return "Standby";
    if (major == 1 && minor == 0) return "Purge: cooling flame sensor";
    if (major == 1 && minor == 1) return "Purge: ventilating chamber";
    if (major == 2 && minor == 1) return "Pre-heat (glow plug on)";
    if (major == 2 && minor == 2) return "Ignition seq 1";
    if (major == 2 && minor == 3) return "Ignition seq 2";
    if (major == 2 && minor == 4) return "Ramp up";
    if (major == 3 && minor == 0) return "HEATING (PID active)";
    if (major == 3 && minor == 35) return "Ventilation only (fan)";
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
      case 13: return "No start (ignition failed)";
      case 15: return "Under voltage";
      case 16: return "Ventilation duration exceeded";
      case 17: return "Fuel pump fault";
      case 20: return "No communication";
      case 29: return "Flame blowout";
      case 30: return "Flame detected before start";
      case 33: return "Control lockout";
      case 37: return "HARD LOCKOUT – send unlock cmd";
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

  // ── General helpers ──────────────────────────────────────────────────────────
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

  static uint8_t preset_to_power_mode(const char *preset) {
    if (strcmp(preset, PRESET_T_PANEL) == 0) return 2;
    if (strcmp(preset, PRESET_T_AIR)   == 0) return 3;
    if (strcmp(preset, PRESET_POWER)   == 0) return 4;
    return 1;
  }
  static const char *power_mode_to_preset(uint8_t mode) {
    switch (mode) {
      case 2: return PRESET_T_PANEL;
      case 3: return PRESET_T_AIR;
      case 4: return PRESET_POWER;
      default: return PRESET_T_HEATER;
    }
  }

  // ── CRC-16 Modbus ─────────────────────────────────────────────────────────
  static uint16_t crc16_modbus(const std::vector<uint8_t> &data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t b : data) {
      crc ^= b;
      for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
  }

  // ── Frame builder ─────────────────────────────────────────────────────────
  void send_frame(uint8_t cmd) {
    // Controller→Heater start/settings: [0xFF 0xFF] [mode] [temp] [vent] [level]
    // Time bytes set to 0xFF 0xFF = unlimited runtime
    std::vector<uint8_t> frame = {
        0xAA, 0x03, 0x06, 0x00, cmd, 0xFF, 0xFF,
        power_mode_, temp_set_, vent_cmd_, power_level_};
    uint16_t crc = crc16_modbus(frame);
    frame.push_back((crc >> 8) & 0xFF);  // MSB first (helloworld convention)
    frame.push_back(crc & 0xFF);
    write_array(frame.data(), frame.size());
    ESP_LOGD("autotherm2d", "TX %-8s | mode=%s temp=%d°C vent=%s level=%d",
             cmd == CMD_START ? "START" : "UPDATE",
             mode_description(power_mode_), temp_set_,
             vent_cmd_ == 1 ? "On" : "Off", power_level_ + 1);
  }
  void send_control_command() { send_frame(CMD_START); }
  void send_update_command()  { send_frame(CMD_SETTINGS); }

  void send_shutdown_command() {
    const uint8_t raw[] = {0xAA, 0x03, 0x00, 0x00, 0x03, 0x5D, 0x7C};
    write_array(raw, sizeof(raw));
    ESP_LOGD("autotherm2d", "TX SHUTDOWN");
  }

  // ── State machine ─────────────────────────────────────────────────────────
  // Called from YAML debug lambda (bytes consumed by uart_debug before loop())
  void process_incoming_byte(uint8_t byte) { process_byte(byte); }

  void process_byte(uint8_t byte) {
    switch (read_state_) {
      case 0:
        if (byte == 0xAA) { message_data_.clear(); parse_start_ms_ = millis(); read_state_ = 1; }
        break;
      case 1: read_state_ = 2; break;  // sender byte (0x03/0x04), ignored
      case 2:
        if (byte == 0 || byte > MAX_PAYLOAD_LEN) {
          ESP_LOGW("autotherm2d", "Bad length 0x%02X", byte); reset_parser();
        } else { message_length_ = byte; read_state_ = 3; }
        break;
      case 3: read_state_ = 4; break;  // 0x00 reserved
      case 4: command_id_ = byte; read_state_ = 5; break;
      case 5:
        message_data_.push_back(byte);
        if (message_data_.size() > MAX_PAYLOAD_LEN) { reset_parser(); break; }
        if ((int)message_data_.size() >= message_length_) { process_message(); reset_parser(); }
        break;
      default: reset_parser(); break;
    }
  }

  // ── Message dispatcher ────────────────────────────────────────────────────
  void process_message() {
    switch (command_id_) {
      case CMD_STATUS:     handle_status();      break;
      case CMD_PANEL_TEMP: handle_panel_temp();  break;
      case CMD_SETTINGS:   handle_settings();    break;
      default:             handle_unknown();     break;
    }
  }

  // ── 0x0F – Status ─────────────────────────────────────────────────────────
  // Payload (19 bytes):
  //  [0]   Major state
  //  [1]   Minor state
  //  [2]   Error code
  //  [3]   Temp1: intake air (int8 °C)
  //  [4]   Temp2: ext sensor / output (int8, 0x7F = disconnected)
  //  [5-6] Supply voltage uint16 BE / 10
  //  [7-8] Flame temp uint16 BE in Kelvin
  //  [11]  Fan setpoint (Hz)
  //  [12]  Fan actual   (Hz)
  //  [14]  Fuel pump frequency (Hz * 10)
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
    float   pump_hz  = safe_byte(14) / 10.0f;

    bool t2_ok = (t2_raw != 0x7F);
    int  t2    = to_signed_temp(t2_raw);

    // ── Human-readable log ────────────────────────────────────────────────
    if (t2_ok) {
      ESP_LOGD("autotherm2d",
        "STATUS %d.%d (%s) | Err: %d (%s) | "
        "T-intake: %d°C | T-output: %d°C | "
        "Volt: %.1fV | Flame: %.0f°C | "
        "Fan: %dHz/%dHz (%dRPM/%dRPM) | Pump: %.1fHz",
        major, minor, state_description(major, minor),
        error, error_description(error),
        t1, t2,
        volts, flame_c,
        fan_sp, fan_act, fan_sp * 60, fan_act * 60,
        pump_hz);
    } else {
      ESP_LOGD("autotherm2d",
        "STATUS %d.%d (%s) | Err: %d (%s) | "
        "T-intake: %d°C | T-output: n/a | "
        "Volt: %.1fV | Flame: %.0f°C | "
        "Fan: %dHz/%dHz (%dRPM/%dRPM) | Pump: %.1fHz",
        major, minor, state_description(major, minor),
        error, error_description(error),
        t1,
        volts, flame_c,
        fan_sp, fan_act, fan_sp * 60, fan_act * 60,
        pump_hz);
    }

    // ── Publish sensors ───────────────────────────────────────────────────
    major_state_ = major;
    publish_if(s_status_code_,     static_cast<float>((major << 8) | minor));
    publish_if(s_intake_temp_,     static_cast<float>(t1));
    if (t2_ok) publish_if(s_air_temp_, static_cast<float>(t2));
    publish_if(s_battery_voltage_, volts);
    publish_if(s_fan_actual_,      static_cast<float>(fan_act));

    // ── Sync Climate mode ─────────────────────────────────────────────────
    auto new_mode = (major != 0)
                        ? climate::CLIMATE_MODE_HEAT
                        : climate::CLIMATE_MODE_OFF;
    if (new_mode != this->mode) {
      this->mode = new_mode;
      this->publish_state();
    }
  }

  // ── 0x11 – Panel temperature ──────────────────────────────────────────────
  // Payload (1 byte): [0] Panel temp int8 °C
  void handle_panel_temp() {
    int t = to_signed_temp(safe_byte(0));
    ESP_LOGD("autotherm2d", "PANEL TEMP  %d°C", t);
    publish_if(s_panel_temp_, static_cast<float>(t));
  }

  // ── 0x02 – Settings echo ──────────────────────────────────────────────────
  // Payload (6 bytes):
  //  [0-1] Work time minutes uint16 BE
  //  [2]   Mode (1-4)
  //  [3]   Target temperature °C
  //  [4]   Ventilation: 0=Off, 1=On  (heater encoding)
  //  [5]   Power level 0-9
  void handle_settings() {
    if (major_state_ == 0) return;   // ignore while off

    uint16_t work_min   = (safe_byte(0) << 8) | safe_byte(1);
    uint8_t  mode       = safe_byte(2);
    uint8_t  target     = safe_byte(3);
    uint8_t  vent       = safe_byte(4);  // 0=Off, 1=On
    uint8_t  level      = safe_byte(5);  // 0-9

    ESP_LOGD("autotherm2d",
      "SETTINGS    mode=%-12s  target=%d°C  vent=%-3s  level=%d/10  time=%dmin",
      mode_description(mode),
      target,
      vent == 1 ? "On" : "Off",
      level + 1,
      work_min);

    publish_if(s_power_level_, static_cast<float>(level + 1));

    bool changed = false;
    if (temp_set_ != target) {
      temp_set_ = target;
      this->target_temperature = target;
      changed = true;
    }
    if (power_level_ != level) { power_level_ = level; changed = true; }

    if (power_mode_ != mode) {
      power_mode_ = mode;
      this->set_custom_preset_(power_mode_to_preset(mode));
      changed = true;
    }

    // Incoming vent: 0=Off, 1=On  (kalutep heater response encoding)
    auto new_fan = (vent == 1) ? climate::CLIMATE_FAN_ON : climate::CLIMATE_FAN_OFF;
    if (this->fan_mode != new_fan) {
      // Sync vent_cmd_ to helloworld controller encoding (1=On, 2=Off)
      vent_cmd_ = (vent == 1) ? 1 : 2;
      this->fan_mode = new_fan;
      changed = true;
    }

    if (changed) this->publish_state();
  }

  // ── Unknown command ───────────────────────────────────────────────────────
  void handle_unknown() {
    std::string hex;
    for (size_t i = 0; i < message_data_.size() && i < 12; i++) {
      char buf[4];
      snprintf(buf, sizeof(buf), i ? ":%02X" : "%02X", message_data_[i]);
      hex += buf;
    }
    if (message_data_.size() > 12) hex += "...";
    ESP_LOGD("autotherm2d", "CMD 0x%02X    len=%-2d  %s",
             command_id_, message_length_, hex.c_str());
  }
};

}  // namespace autotherm2d
}  // namespace esphome
