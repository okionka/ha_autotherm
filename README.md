# ha_autotherm – ESPHome Diesel Heater Climate Component

An ESPHome **external component** that integrates diesel / fuel heaters
(Autotherm 2D and compatible) with Home Assistant as a full **Climate** entity.

---

## Features

| Feature | Detail |
|---|---|
| Climate modes | `OFF`, `HEAT` |
| Fan modes | `ON` (ventilator active) / `OFF` |
| Presets | `By T Heater` · `By T Panel` · `By T Air` · `By Power` |
| Target temperature | 0–30 °C in 1 °C steps |
| Current temperature | Any ESPHome sensor – including HA entities |
| Diagnostic sensors | Battery voltage, board temp, air temp, panel temp, power level, ventilator power, status code |
| Safety | Watchdog-safe parser, 500 ms timeout, bounds-checked payload access |

---

## Repository layout

```
ha_autotherm/
├── autotherm2d.yaml          # Main ESPHome config (edit this)
├── secrets.yaml.example      # Template – copy to secrets.yaml and fill in
└── components/
    └── autotherm2d/
        ├── __init__.py       # Component namespace declaration
        ├── climate.py        # ESPHome codegen (climate platform)
        └── autotherm2d.h     # C++ implementation (header-only)
```

---

## Hardware wiring

```
ESP32 dev board
  GPIO1  (TX) ──→ Controller panel RX
  GPIO3  (RX) ←── Controller panel TX

  GPIO17 (TX) ──→ Heater UART RX
  GPIO16 (RX) ←── Heater UART TX
```

The ESP32 sits transparently between the physical controller panel and the
heater, forwarding bytes in both directions while also parsing heater
responses and accepting override commands from Home Assistant.

---

## Quick start

### 1. Clone the repository

```bash
git clone https://github.com/okionka/ha_autotherm.git
cd ha_autotherm
```

### 2. Create your secrets file

```bash
cp secrets.yaml.example secrets.yaml
# Edit secrets.yaml with your WiFi credentials, OTA password, and API key
```

### 3. Set your room temperature sensor

Open `autotherm2d.yaml` and change the entity_id under the
`homeassistant` sensor platform to point to your actual HA temperature sensor:

```yaml
sensor:
  - platform: homeassistant
    id: room_temperature
    entity_id: sensor.living_room_temperature   # ← your sensor here
```

You can use **any** ESPHome-compatible sensor as the room thermometer:
- A sensor from Home Assistant (`platform: homeassistant`)
- A local sensor on the ESP32 (e.g. `platform: dht`, `platform: dallas`)
- Any sensor with a numeric temperature state

### 4. Flash

```bash
esphome run autotherm2d.yaml
```

---

## Climate entity in Home Assistant

Once flashed and adopted, the device exposes:

- **Climate card** – full thermostat control (mode, temperature, fan, preset)
- **Sensor entities** – battery voltage, temperatures, power level, status code

### Preset → heater regulation mode

| HA Preset | Heater mode | Description |
|---|---|---|
| `By T Heater` | Mode 1 | Regulates using heater's own board sensor |
| `By T Panel` | Mode 2 | Regulates using the (optional) panel sensor |
| `By T Air` | Mode 3 | Regulates using the inlet air sensor |
| `By Power` | Mode 4 | Fixed power level, no temperature regulation |

---

## UART protocol notes

- Baud rate: **9600**
- Frame format: `AA <len> <msglen> 00 <cmd_id> [payload…] <CRC16-hi> <CRC16-lo>`
- CRC: **CRC-16 Modbus** (poly `0xA001`, init `0xFFFF`), appended big-endian (MSB first)

### Received command IDs

| ID | Description |
|---|---|
| `0x0F` (15) | Live heater status (temperatures, battery, fan power, status byte) |
| `0x11` (17) | Panel temperature |
| `0x02` (2) | Settings echo – heater confirms current operating parameters |

---

## License

MIT
