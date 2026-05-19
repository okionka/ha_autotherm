# ha_autoterm – Vollständige Dokumentation

**ESPHome-Komponente für die Autoterm Air 2D Dieselstandheizung**  
Repository: [github.com/okionka/ha_autoterm](https://github.com/okionka/ha_autoterm)

---

## Inhaltsverzeichnis

1. [Systemübersicht](#1-systemübersicht)
2. [Hardware-Verkabelung](#2-hardware-verkabelung)
3. [Projektstruktur](#3-projektstruktur)
4. [secrets.yaml](#4-secretsyaml)
5. [Autoterm ESP – Konfiguration](#5-autotherm-esp--konfiguration)
6. [NSPanel – Konfiguration](#6-nspanel--konfiguration)
7. [ESP-NOW: Direkte Geräteverbindung](#7-esp-now-direkte-geräteverbindung)
8. [Blackymas Blueprint](#8-blackymas-blueprint)
9. [Betriebsmodi](#9-betriebsmodi)
10. [Technische Details der Komponente](#10-technische-details-der-komponente)
11. [Fehlerbehebung](#11-fehlerbehebung)

---

## 1. Systemübersicht

```
┌──────────────────────────────────────────────────────────┐
│                     Home Assistant                        │
│   climate.autotherm  ◄──────────────────────────────┐    │
│   sensor.nspanel_temperatur                          │    │
└───────────────┬──────────────────────────────────────┘   │
                │ Wi-Fi / Native API                        │
     ┌──────────┴───────────┐                              │
     │                      │                              │
     ▼                      ▼                              │
┌─────────────┐      ┌──────────────────┐                 │
│   NSPanel   │      │  Autoterm ESP   │─────────────────┘
│  (ESP32)    │      │    (ESP32)       │  Native API
│             │      │                  │
│  NTC-Sensor │      │  UART ──────────► Diesel-Heater
│  Blackymas  │      │                  │  Autoterm Air 2D
└──────┬──────┘      └──────────────────┘
       │                      ▲
       └──────────────────────┘
          ESP-NOW (direkt, ~5 ms)
          Temperatur: NSPanel → Autoterm
```

### Kommunikationswege

| Verbindung | Protokoll | Latenz | Verfügbar ohne Wi-Fi |
|---|---|---|---|
| Autoterm ESP ↔ Heizung | UART 9600 baud | <10 ms | ✅ |
| NSPanel → Autoterm (Temp) | ESP-NOW | ~5 ms | ✅ |
| NSPanel ↔ Home Assistant | Wi-Fi Native API | 100–500 ms | ❌ |
| Autoterm ↔ Home Assistant | Wi-Fi Native API | 100–500 ms | ❌ |
| NSPanel → Heizung (via HA) | Wi-Fi + API | 1–2 s | ❌ |

### Betrieb mit aktivem Wi-Fi und HA

ESP-NOW und Wi-Fi laufen **gleichzeitig** auf demselben ESP32-Funkkanal.
Im Normalbetrieb sind beide Wege aktiv:

- Die Raumtemperatur fließt direkt via ESP-NOW vom NSPanel zur Autoterm Air 2D
- Die Steuerung (Soll-Temperatur, Modus, Preset) erfolgt über HA und den Blackymas Blueprint
- Alle Diagnosedaten sind in HA als Sensoren sichtbar

---

## 2. Hardware-Verkabelung

### Autoterm Air 2D ↔ ESP32

```
ESP32 GPIO          Autoterm Air 2D Stecker
────────────        ──────────────────────
TX  (GPIO1)  ────►  RX  (UART Eingang)
RX  (GPIO3)  ◄────  TX  (UART Ausgang)
GND          ──────  GND
```

> ⚠️ **Spannungspegel prüfen** – Die Autoterm Air 2D arbeitet mit 12 V Versorgung.
> Die UART-Signale sind auf 3,3 V gepegelt und direkt mit dem ESP32 kompatibel.
> **Niemals die 12-V-Versorgung an den ESP32 anlegen.**
> Im Zweifelsfall Spannungspegel mit Multimeter messen, bevor die Verbindung hergestellt wird.

### UART-Parameter

| Parameter | Wert |
|---|---|
| Baudrate | 9600 |
| Datenbits | 8 |
| Parität | keine |
| Stoppbits | 1 |

### NSPanel ↔ Autoterm ESP (optional, bei direkter UART-Verbindung)

Bei einer zusätzlichen **kabelgebundenen Verbindung** (Alternative zu ESP-NOW):

```
NSPanel GPIO4   ────►  Autoterm ESP GPIO16  (TX→RX)
NSPanel GPIO5   ◄────  Autoterm ESP GPIO17  (RX→TX)
NSPanel GND     ──────  Autoterm ESP GND
```

> Nur relevant, wenn kein ESP-NOW verwendet wird. Bei ESP-NOW ist keine Kabelverbindung
> zwischen NSPanel und Autoterm ESP erforderlich – sie kommunizieren drahtlos.

---

## 3. Projektstruktur

```
ha_autoterm/
├── components/
│   └── autotherm2d/
│       ├── __init__.py        # ESPHome Namespace-Deklaration
│       ├── climate.py         # Python-Codegen, Schema, Sensor-Config
│       └── autotherm2d.h      # C++ Treiberklasse (header-only)
├── docs/
│   └── DOCUMENTATION.md      # Diese Datei
├── autotherm.yaml             # Vollständiges Beispiel Autoterm ESP
├── nspanel.yaml               # Vollständiges Beispiel NSPanel
├── secrets.yaml.example       # Vorlage für Zugangsdaten
├── .gitignore
└── README.md
```

---

## 4. secrets.yaml

`secrets.yaml.example` in `secrets.yaml` kopieren und ausfüllen:

```yaml
# Netzwerk
wifi_ssid: "DeinNetzwerkname"
wifi_password: "DeinPasswort"

# ESPHome Sicherheit
ota_password: "sicheres-ota-passwort"
api_encryption_key: "base64-kodierten-32-byte-schluessel"  # esphome generate-key

# ESP-NOW MAC-Adressen
# Zu finden im ESPHome-Log beim Start:
# [I][wifi:052]: MAC: AA:BB:CC:DD:EE:FF
autotherm_mac: "AA:BB:CC:DD:EE:FF"
nspanel_mac:   "11:22:33:44:55:66"
```

---

## 5. Autoterm ESP – Konfiguration

### autotherm.yaml

```yaml
esphome:
  name: autotherm
  friendly_name: "Autoterm Air 2D"

esp32:
  board: esp32dev
  framework:
    type: arduino

# Externe Komponente aus diesem Repository
external_components:
  - source:
      type: git
      url: https://github.com/okionka/ha_autoterm
    components: [autotherm2d]

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  # Bei WLAN-Ausfall auf festen ESP-NOW-Kanal wechseln
  on_disconnect:
    - espnow.set_channel: 6

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:

# ── UART zur Heizung ────────────────────────────────────────
uart:
  id: heater_uart
  tx_pin: GPIO1
  rx_pin: GPIO3
  baud_rate: 9600

# ── ESP-NOW: Temperaturempfang vom NSPanel ──────────────────
espnow:
  auto_add_peer: true
  on_receive:
    - lambda: |-
        // Temperatur-Frame vom NSPanel erkennen (Kennung 0x54 = 'T')
        if (size < 4 || data[0] != 0x54) return;

        // CRC8 (XOR) prüfen
        uint8_t crc = data[0] ^ data[1] ^ data[2];
        if (crc != data[3]) {
          ESP_LOGW("espnow", "Temperaturdaten: CRC-Fehler");
          return;
        }

        // Temperatur aus zwei Bytes wiederherstellen (Wert × 100)
        float temp = (float)((int16_t)((data[1] << 8) | data[2])) / 100.0f;

        // Plausibilitätsprüfung
        if (temp < -20.0f || temp > 60.0f) {
          ESP_LOGW("espnow", "Temperatur außerhalb des Bereichs: %.1f°C", temp);
          return;
        }

        id(autotherm_climate).current_temperature = temp;
        id(autotherm_climate).publish_state();
        ESP_LOGD("espnow", "Raumtemperatur vom NSPanel: %.1f°C", temp);

# ── Climate-Entität ─────────────────────────────────────────
climate:
  - platform: autotherm2d
    id: autotherm_climate
    name: "Autoterm Air 2D"
    uart_id: heater_uart
    # Kein current_temperature_sensor – Temperatur kommt via ESP-NOW vom NSPanel

# ── Optionale Diagnosesensoren ──────────────────────────────
sensor:
  - platform: autotherm2d
    climate_id: autotherm_climate
    voltage:
      name: "Autoterm Spannung"
    temp_heater:
      name: "Autoterm Brennertemperatur"
    temp_panel:
      name: "Autoterm Gehäusetemperatur"
    fan_speed:
      name: "Autoterm Lüfterdrehzahl"
    error_code:
      name: "Autoterm Fehlercode"
    power:
      name: "Autoterm Leistung"
    runtime:
      name: "Autoterm Betriebszeit"

# ── Watchdog ────────────────────────────────────────────────
interval:
  - interval: 5min
    then:
      - if:
          condition:
            lambda: 'return isnan(id(autotherm_climate).current_temperature);'
          then:
            - logger.log:
                level: WARN
                tag: "autotherm"
                format: "Keine Raumtemperatur empfangen – ESP-NOW aktiv?"
```

---

## 6. NSPanel – Konfiguration

Der NSPanel sendet seine interne Temperatur via ESP-NOW direkt an den Autoterm ESP.
Gleichzeitig veröffentlicht er den Sensorwert in Home Assistant für den Blackymas Blueprint.

### nspanel.yaml

```yaml
esphome:
  name: nspanel
  friendly_name: "NSPanel"

esp32:
  board: esp32dev
  framework:
    type: arduino

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  on_disconnect:
    - espnow.set_channel: 6   # Gleichen Fallback-Kanal wie Autoterm ESP

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:

# ── UART zum Nextion-Display (NSPanel intern) ───────────────
uart:
  id: nextion_uart
  tx_pin: GPIO16
  rx_pin: GPIO17
  baud_rate: 115200

# ── ESP-NOW: Temperatur an Autoterm senden ─────────────────
espnow:
  peers:
    - !secret autotherm_mac

# ── Interner Temperatursensor (NTC, GPIO38) ─────────────────
sensor:
  - platform: adc
    id: ntc_source
    pin: GPIO38
    attenuation: 11db
    update_interval: never

  - platform: resistance
    id: ntc_resistance
    sensor: ntc_source
    configuration: DOWNSTREAM
    resistor: 11.2kOhm

  - platform: ntc
    id: nspanel_temperature
    sensor: ntc_resistance
    name: "NSPanel Temperatur"
    unit_of_measurement: "°C"
    calibration:
      b_constant: 3950
      reference_resistance: 10kOhm
      reference_temperature: 25°C
    update_interval: 30s
    filters:
      - median:
          window_size: 5
          send_every: 5
    on_value:
      then:
        # Wert via ESP-NOW direkt an den Autoterm ESP senden
        # Frame: [0x54='T', temp_high_byte, temp_low_byte, crc8]
        - espnow.send:
            address: !secret autotherm_mac
            data: !lambda |-
              int16_t t = (int16_t)(x * 100.0f);
              std::vector<uint8_t> frame = {
                0x54,
                (uint8_t)(t >> 8),
                (uint8_t)(t & 0xFF),
                0x00
              };
              frame[3] = frame[0] ^ frame[1] ^ frame[2];
              return frame;
            on_error:
              - logger.log:
                  level: WARN
                  tag: "espnow"
                  format: "Temperatur konnte nicht gesendet werden"
```

---

## 7. ESP-NOW: Direkte Geräteverbindung

### Protokoll

Alle ESP-NOW-Frames sind 4 Byte lang:

```
Byte 0: Kennung   0x54 = Temperatur ('T')
                  0x01 = Soll-Temperatur setzen
                  0x02 = Modus setzen (0=Off, 1=Heat)
                  0x03 = Preset setzen (0–3)
Byte 1: Wert high byte
Byte 2: Wert low byte
Byte 3: CRC8 = Byte0 XOR Byte1 XOR Byte2
```

Temperaturwerte werden als Ganzzahl übertragen (Originalwert × 100):
- 21,5 °C → `0x0867` (2151 dezimal, aber skaliert als int: 2150)
- -5,0 °C → `0xFE0C` (vorzeichenbehaftet: -500)

### Kanal-Verhalten

| WLAN-Status | ESP-NOW-Kanal | Verhalten |
|---|---|---|
| Verbunden (Router aktiv) | Gleich wie Wi-Fi-Kanal | ✅ Automatisch korrekt |
| Verbunden (Router ausgefallen) | Letzter bekannter Kanal | ✅ Meist noch funktionsfähig |
| Getrennt (nach `on_disconnect`) | Kanal 6 (fest) | ✅ Beide ESPs wechseln koordiniert |

Beide Geräte müssen den **gleichen** Fallback-Kanal in `on_disconnect` konfigurieren.

### MAC-Adresse ermitteln

Im ESPHome-Log bei jedem Gerätestart:

```
[I][wifi:052]: MAC: AA:BB:CC:DD:EE:FF
```

Alternativ in Home Assistant: **ESPHome → Gerät → Diagnose → MAC-Adresse**

---

## 8. Blackymas Blueprint

### Voraussetzung

Das [NSPanel HA Blueprint](https://github.com/Blackymas/NSPanel_HA_Blueprint) muss installiert sein.

### Blueprint-Automation in Home Assistant

```yaml
alias: "NSPanel Heizungssteuerung"
use_blueprint:
  path: Blackymas/NSPanel_HA_Blueprint/nspanel_blueprint.yaml
  input:

    # Gerät
    nspanel_device: nspanel

    # Temperaturanzeige auf der NSPanel-Startseite
    # Zeigt den eigenen NTC-Sensor des NSPanel
    temp_sensor: sensor.nspanel_temperatur

    # Climate Add-on: Autoterm Air 2D als steuerbare Seite
    climate_entities:
      - entity: climate.autotherm
        friendly_name: "Autoterm Air 2D"
```

### Angezeigte Informationen auf dem NSPanel

| Element | Quelle |
|---|---|
| Raumtemperatur (Startseite) | `sensor.nspanel_temperatur` |
| Aktuelle Temperatur (Climate-Seite) | `climate.autotherm.current_temperature` (= NSPanel-Temp via ESP-NOW) |
| Soll-Temperatur | `climate.autotherm.target_temperature` |
| Modus (Heat / Off) | `climate.autotherm.hvac_mode` |
| Lüfter (On / Off) | `climate.autotherm.fan_mode` |
| Presets | `climate.autotherm.preset_mode` |

### Verfügbare Presets

| Preset | Bedeutung |
|---|---|
| `By T Heater` | Regelung nach Brennerkopftemperatur |
| `By T Panel` | Regelung nach Gehäusetemperatur |
| `By T Air` | Regelung nach Lufttemperatur |
| `By Power` | Leistungsregelung (manuell) |

---

## 9. Betriebsmodi

### Normalbetrieb (Wi-Fi + HA aktiv)

Beide Kommunikationswege sind gleichzeitig aktiv.
ESP-NOW und Wi-Fi koexistieren auf demselben Funkkanal des ESP32.

```
NSPanel-Sensor
      ├──► ESP-NOW → Autoterm ESP → current_temperature → Heizungsregler
      └──► Wi-Fi → HA → sensor.nspanel_temperatur → Blackymas-Anzeige

Blackymas Blueprint (HA)
      └──► Wi-Fi → climate.autotherm → Autoterm ESP → UART → Heizung
```

**Latenz Temperaturweg:** ~5 ms (ESP-NOW)  
**Latenz Steuerweg:** 100–500 ms (Wi-Fi → HA → ESPHome)

### Betrieb bei HA-Ausfall (Wi-Fi noch aktiv)

- Temperatur fließt weiter via ESP-NOW ✅
- Heizung arbeitet mit letztem Sollwert weiter ✅
- NSPanel-Display zeigt keine aktuellen Daten (Blackymas braucht HA) ⚠️
- Neue Steuerungskommandos können nicht über den Blueprint gesendet werden ⚠️

### Betrieb bei WLAN-Ausfall (Router ausgefallen)

- `on_disconnect` setzt ESP-NOW-Kanal auf 6 auf beiden Geräten
- Temperatur fließt weiter via ESP-NOW auf Kanal 6 ✅
- Heizung arbeitet mit letztem Sollwert weiter ✅
- Keine HA-Verbindung ❌

### Komplett offline (kein WLAN, kein HA)

- Heizung läuft mit letztem bekanntem Sollwert ✅
- Temperaturmessung via ESP-NOW aktiv (wenn beide ESPs erreichbar) ✅
- Keine Fernsteuerung möglich ❌

---

## 10. Technische Details der Komponente

### Klasse `Autotherm2D`

Die Klasse `autotherm2d.h` erbt von:
- `climate::Climate` – ESPHome Climate-Entität
- `Component` – ESPHome Komponentenlebenszyklus
- `uart::UARTDevice` – Serielle Kommunikation

### UART-Protokoll

Die Heizung kommuniziert über ein proprietäres binäres Protokoll:

```
Byte 0:    Startbyte (0xAA)
Byte 1:    Nachrichtenlänge (1–64, validiert)
Byte 2..N: Nutzlast
Byte N+1:  Checksumme
```

### Schutzmaßnahmen gegen Watchdog-Reset

Aus früheren Erfahrungen mit dem Inline-Lambda-Ansatz wurden folgende Schutzmaßnahmen
in die Komponente eingebaut:

| Problem | Lösung |
|---|---|
| Unbegrenztes `while(available())` | Max. 64 Bytes pro Loop-Tick verarbeitet |
| Unkontrolliertes Vektorwachstum | Feste Maximalgröße für `message_data` |
| Fehlender Bounds-Check | Zugriff auf Payload nur nach Längenvalidierung |
| Ungültige `message_length` vom Gerät | Validierung: 1 ≤ Länge ≤ 64 |
| Parser bleibt in fehlerhaftem Zustand | 500 ms Timeout mit automatischem Reset |

### Unterstützte Climate-Features

```cpp
// Betriebsmodi
CLIMATE_MODE_OFF
CLIMATE_MODE_HEAT

// Lüftermodi
CLIMATE_FAN_ON
CLIMATE_FAN_OFF

// Presets
"By T Heater"
"By T Panel"
"By T Air"
"By Power"
```

---

## 11. Fehlerbehebung

### Heizung antwortet nicht auf UART

1. Verkabelung prüfen: TX→RX, RX→TX (gekreuzt), GND verbunden
2. Baudrate prüfen: muss exakt 9600 sein
3. ESPHome-Log auf UART-Fehler prüfen:
   ```bash
   esphome logs autotherm.yaml
   ```
4. Spannungspegel prüfen (3,3 V Logikpegel erforderlich)

### ESP-NOW Temperatur kommt nicht an

1. MAC-Adressen in `secrets.yaml` auf Tippfehler prüfen
2. Im ESPHome-Log des Autoterm ESP nach ESP-NOW-Empfang suchen:
   ```
   [D][espnow:???]: Raumtemperatur vom NSPanel: 21.5°C
   ```
3. Sicherstellen, dass beide Geräte im gleichen Wi-Fi-Kanal eingebucht sind
4. `auto_add_peer: true` im Autoterm ESP gesetzt?

### NSPanel-Temperatur erscheint nicht in HA

1. Sensorname prüfen: `sensor.nspanel_temperatur` (Kleinbuchstaben, Leerzeichen → Unterstrich)
2. ESPHome-Gerät in HA: **Einstellungen → Geräte & Dienste → ESPHome**
3. Sensor im ESPHome-Log prüfen:
   ```
   [D][ntc:???]: 'NSPanel Temperatur': 21.5°C
   ```

### Blackymas zeigt Autoterm Air 2D nicht

1. `climate.autotherm` muss in HA verfügbar sein (ESPHome-Integration aktiv)
2. Blueprint-Automation neu laden nach Konfigurationsänderung
3. NSPanel-Gerätename in der Blueprint-Automation exakt wie in `esphome: name:` schreiben

### Heizung läuft nach WLAN-Ausfall nicht weiter

Die Heizung läuft mit dem **letzten aktiven Sollwert** weiter.
Ein Neustart des Autoterm ESP ohne WLAN setzt `current_temperature` auf `NaN`.
→ Fallback-Wert in der Komponente kann bei Bedarf konfiguriert werden.

---

*Dokumentation zu ha_autoterm – Stand Mai 2026*
