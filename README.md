# ha_autotherm – ESPHome Diesel Heater Climate Component

An ESPHome **external component** that integrates Autotherm Air 2D / Planar 2D
diesel heaters with Home Assistant as a full **Climate** entity.

---

## Features

| Feature | Detail |
|---|---|
| Climate modes | `OFF`, `HEAT` |
| Fan modes | `ON` (ventilator active) / `OFF` |
| Presets | `By T Heater` · `By T Panel` · `By T Air` · `By Power` |
| Target temperature | 0–30 °C in 1 °C steps |
| Current temperature | Any ESPHome sensor – including HA entities |
| Diagnostic sensors | Battery voltage, intake temp, output temp, panel temp, flame temp, fan RPM, fuel pump Hz, status code |
| Safety | Watchdog-safe parser, 500 ms timeout, bounds-checked payload |

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
  GPIO1  (TX) ──→ Controller panel RX   (5V level shift required!)
  GPIO3  (RX) ←── Controller panel TX

  GPIO17 (TX) ──→ Heater UART RX
  GPIO16 (RX) ←── Heater UART TX
```

> **⚠️ Voltage:** The heater bus runs at **5V TTL**. A logic-level shifter
> (e.g. TXS0108E) is required when connecting to a 3.3V ESP32.

The ESP32 sits transparently between the physical controller panel and the
heater, forwarding bytes in both directions while parsing heater responses
and accepting override commands from Home Assistant.

---

## Quick start

### 1. Add to your ESPHome config

```yaml
external_components:
  - source: github://okionka/ha_autotherm@main
    components: [autotherm2d]
```

ESPHome fetches the component automatically on every compile.

### 2. Create your secrets file

```bash
curl -o secrets.yaml https://raw.githubusercontent.com/okionka/ha_autotherm/main/secrets.yaml.example
# Edit secrets.yaml with your credentials
```

### 3. Set your room temperature sensor

```yaml
sensor:
  - platform: homeassistant
    id: room_temperature
    entity_id: sensor.your_temperature_sensor   # ← change this
```

Any ESPHome sensor works: `platform: homeassistant`, `platform: dht`, `platform: dallas`, etc.

### 4. Flash

```bash
esphome run autotherm2d.yaml
```

Or use the **ESPHome Dashboard** in Home Assistant → Install → Wirelessly.

---

## Climate entity in Home Assistant

| HA control | Effect |
|---|---|
| Mode `OFF` | Sends shutdown command |
| Mode `HEAT` | Starts heater with current settings |
| Target temperature | Sets temperature setpoint (0–30 °C) |
| Fan `ON` / `OFF` | Controls ventilator mode |
| Preset | Selects regulation strategy (see below) |

### Presets

| Preset | Mode | Description |
|---|---|---|
| `By T Heater` | 1 | Regulates via heater board sensor |
| `By T Panel` | 2 | Regulates via controller panel sensor |
| `By T Air` | 3 | Regulates via inlet air sensor |
| `By Power` | 4 | Fixed power level, no thermostat |

---

## Serial Protocol

> References:
> [helloworld.schlussdienst.net](https://helloworld.schlussdienst.net/blog/hacking-autoterm-planar-2d) ·
> [kalutep/AutotermHeaterController](https://github.com/kalutep/AutotermHeaterController/blob/main/serial_communication_protocol.md)

### Physical layer

| Parameter | Value |
|---|---|
| Interface | UART |
| Baud rate | 9600 (heater auto-negotiates 1200 / 2400 / 9600 at startup) |
| Data bits | 8 |
| Parity | None |
| Stop bits | 1 |
| Voltage | **5V TTL** |

All communication is initiated by the controller panel. The heater only responds; it never sends unsolicited messages.

---

### Frame structure

Every frame – in both directions – follows this layout:

```
Byte 0    Byte 1    Byte 2        Byte 3   Byte 4      Byte 5 … N   N+1   N+2
──────────────────────────────────────────────────────────────────────────────
0xAA      Sender    Payload len   0x00     Command ID  Payload      CRC16 (LE)
```

| Field | Length | Values |
|---|---|---|
| Preamble | 1 | Always `0xAA` |
| Sender | 1 | `0x03` = Controller panel · `0x04` = Heater |
| Payload length | 1 | Number of bytes in payload field |
| Reserved | 1 | Always `0x00` |
| Command ID | 1 | See table below |
| Payload | N | Command-specific (N = payload length) |
| CRC-16 | 2 | CRC-16 Modbus, **little-endian** (low byte first) |

**CRC-16 Modbus:** poly `0x8005`, init `0xFFFF`, reflect in/out, XOR out `0x0000`.
Calculated over all bytes from `0xAA` through the last payload byte.

---

### Command reference

| ID | Name | Direction | Payload len |
|---|---|---|---|
| `0x01` | Start | Controller → Heater | 6 |
| `0x02` | Settings | Controller ↔ Heater | 6 |
| `0x03` | Stop | Controller → Heater | 0 |
| `0x04` | Serial number | Controller ↔ Heater | 0 / 5 |
| `0x06` | SW version | Controller ↔ Heater | 0 / 5 |
| `0x0F` | Status | Controller ↔ Heater | 0 / 19 |
| `0x11` | Panel temperature | Controller ↔ Heater | 1 |
| `0x1C` | Handshake | Controller ↔ Heater | 0 |
| `0x23` | Ventilation (fan only) | Controller → Heater | 4 |

Regular polling cycle (heater off and on): `0x02` → `0x11` → `0x0F` → `0x11` → repeat (~every 2 s).

---

### `0x01` / `0x02` – Start / Settings payload

Same 6-byte structure for both commands:

| Byte | Field | Notes |
|---|---|---|
| 0–1 | Work time (minutes) | `0xFF 0xFF` = unlimited (controller convention) |
| 2 | Mode | 1 = By T Heater · 2 = By T Panel · 3 = By T Air · 4 = By Power |
| 3 | Target temperature | °C, range 1–30 |
| 4 | Ventilation | Controller→Heater: `1` = On · `2` = Off · Heater→Controller: `0` = Off · `1` = On |
| 5 | Power level | 0–9 (used when Mode = 4; also updated by PID in Modes 1–3) |

**`0x01`** triggers ignition. **`0x02`** without payload reads current settings; with payload updates them live.

#### Example – Start, By T Panel, 16 °C, vent on, level 3

```
Controller → AA 03 06 00 01 FF FF 02 10 01 03  [CRC]
Heater     ← AA 04 06 00 01 FF FF 02 10 01 03  [CRC]
```

---

### `0x03` – Stop

No payload. Controller sends this to initiate the shutdown/purge sequence.

```
Controller → AA 03 00 00 03 5D 7C
Heater     ← AA 04 00 00 03 29 7D
```

---

### `0x0F` – Status response payload (19 bytes)

| Offset | Field | Type | Formula / Notes |
|---|---|---|---|
| 0 | Status major | `uint8` | See state table below |
| 1 | Status minor | `uint8` | See state table below |
| 2 | Error code | `uint8` | 0 = OK, see error table below |
| 3 | Temp 1 (intake air) | `int8` | °C · `0x7F` = sensor disconnected |
| 4 | Temp 2 (output / ext) | `int8` | °C · `0x7F` = sensor disconnected |
| 5–6 | Supply voltage | `uint16` BE | `value / 10.0` → V |
| 7–8 | Flame temperature | `uint16` BE | Kelvin → subtract 273.15 for °C |
| 11 | Fan setpoint | `uint8` | Hz · `× 60` → RPM |
| 12 | Fan actual | `uint8` | Hz · `× 60` → RPM |
| 14 | Fuel pump | `uint8` | `value / 10.0` → Hz |

#### Status codes (Major.Minor)

| Major | Minor | State |
|---|---|---|
| 0 | 0 | Sleep / Off |
| 0 | 1 | Standby |
| 1 | 0 | Purge: cooling flame sensor |
| 1 | 1 | Purge: ventilating combustion chamber |
| 2 | 1 | Pre-heat (glow plug on) |
| 2 | 2 | Ignition sequence 1 |
| 2 | 3 | Ignition sequence 2 |
| 2 | 4 | Ramp up (stabilising combustion) |
| 3 | 0 | **Heating – PID active** |
| 3 | 35 | Ventilation only (fan mode) |
| 3 | 4 | Cooling down / stopping |
| 4 | 0 | Shutdown complete |

#### Error codes

| Code | Description |
|---|---|
| 0 | No error |
| 1 | Overheat |
| 2 | Potential overheat |
| 5 | Flame sensor fault |
| 6 | Temperature sensor fault |
| 9 | Glow plug fault |
| 10 | Motor / fan RPM fault |
| 11 | Air temperature sensor fault |
| 12 | Over voltage (> 16 V on 12 V system) |
| 13 | No start – ignition failed |
| 15 | Under voltage (< 10 V on 12 V system) |
| 17 | Fuel pump fault |
| 20 | No communication |
| 29 | Flame blowout during operation |
| 33 | Control lockout (3× overheat) |
| 37 | **Hard lockout** – send `0x0D` unlock command to reset |

---

### `0x11` – Panel temperature payload (1 byte)

| Byte | Field | Type |
|---|---|---|
| 0 | Panel temperature | `int8` °C |

The controller reports its ambient temperature to the heater once per poll cycle. The heater echoes the same value. Relevant when operating in **By T Panel** mode.

---

### Annotated log example (heater idle)

```
# Controller polls settings (no payload)
AA 03 00 00 02  [CRC]

# Heater responds: 120 min · By T Panel · 15 °C · vent off · level 5
AA 04 06 00 02  00 78  02  0F  00  05  [CRC]
                time   mode temp vent lvl

# Controller polls status (no payload)
AA 03 00 00 0F  [CRC]

# Heater responds: standby 0.1, no error, 20 °C intake, 13.2 V, flame 20 °C
AA 04 13 00 0F  00 01  00  14  7F  00 84  01 25  00 00  00  00  ...  [CRC]
                maj min err t1  t2  voltage    flame_K  ..  fan  pump

# Controller reports panel temp 13 °C
AA 03 01 00 11  0D  [CRC]

# Heater echoes panel temp
AA 04 01 00 11  0D  [CRC]
```

---

## License

MIT
