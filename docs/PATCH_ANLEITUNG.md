# PATCH_ANLEITUNG.md
## autoterm2d.h – 4 gezielte Ergänzungen für den TCP-Diagnose-Proxy

> Datei: `components/autoterm2d/autoterm2d.h`  
> Alle Änderungen sind **additive** – kein vorhandener Code wird entfernt.

---

### Schritt 1 – Header einbinden

Ganz oben in der Datei, nach den vorhandenen `#include`-Zeilen:

```cpp
// ── NEU ──
#include "diag_proxy.h"
```

---

### Schritt 2 – Basisklasse hinzufügen

Die Klassen-Deklaration lautet bisher ungefähr:

```cpp
class Autoterm2DClimate : public climate::Climate,
                          public UARTDevice,
                          public Component {
```

Ergänze `DiagProxyMixin` als weitere Basisklasse:

```cpp
class Autoterm2DClimate : public climate::Climate,
                          public UARTDevice,
                          public Component,
                          public DiagProxyMixin {          // ← NEU
```

---

### Schritt 3 – TCP-Server in setup() starten

Am Ende der `setup()`-Methode, vor der schließenden Klammer:

```cpp
void setup() override {
  // ... vorhandener Code ...

  diag_setup_();   // ← NEU: TCP-Server auf Port 8888 starten
}
```

---

### Schritt 4 – Loop-Integration

Die `loop()`-Methode braucht drei gezielte Eingriffe.

#### 4a – Client-Verbindung prüfen (ganz oben in loop())

```cpp
void loop() override {
  diag_loop_tick_();   // ← NEU: Verbindungen verwalten
  // ... vorhandener Code folgt ...
```

#### 4b – Jedes vom Heizungs-UART gelesene Byte auch an TCP weiterleiten

Suche die Stelle wo bytes von UART2 gelesen werden, typischerweise:

```cpp
uint8_t b;
read_byte(&b);
```

Ergänze direkt danach:

```cpp
uint8_t b;
read_byte(&b);
diag_forward_rx_(b);   // ← NEU: Byte auch an Diagnose-Client senden
```

Wenn Bytes in einem Puffer gesammelt werden (z. B. `rx_buf_.push_back(b)`),
muss `diag_forward_rx_(b)` trotzdem *nach* `read_byte` aufgerufen werden,
damit der TCP-Client exakt denselben rohen Byte-Strom sieht wie die Hardware.

#### 4c – Bytes vom Diagnose-Client an Heizung senden + Poll-Zyklus pausieren

Suche die Stelle am Ende von `loop()` wo der virtuelle Poll-Zyklus gesteuert
wird (Bedingung wie `if (millis() - last_poll_ms_ > POLL_INTERVAL_MS)`).

Füge direkt davor ein:

```cpp
  // ── NEU: Diagnose-Proxy TX + Poll-Zyklus-Sperre ──────────────────────
  diag_drain_tx_([this](uint8_t b) { write_byte(b); });
  if (is_diagnostic_active()) return;   // Poll-Zyklus pausieren
  // ─────────────────────────────────────────────────────────────────────

  // ... vorhandener Virtual-Panel-Poll-Code ...
  if (millis() - last_poll_ms_ > POLL_INTERVAL_MS) {
    // ...
  }
```

> **Hinweis Bridge-Modus:** Im Bridge-Modus hat der ESP32 keinen eigenen
> Poll-Zyklus – das physische Bedienteil übernimmt diese Rolle. Das `return`
> ist dann wirkungslos, schadet aber nicht. Der TCP-Diagnose-Client kann
> trotzdem Befehle senden (gleichzeitig mit dem Bedienteil – Kollisionen
> selten und harmlos).

---

### Fertig

Nach diesen vier Änderungen und einem `esphome run autoterm2d.yaml` ist der
TCP-Diagnoseserver aktiv. Statusmeldungen erscheinen im ESPHome-Log unter
dem Tag `diag_proxy`.

Nächster Schritt: `autoterm2d.yaml` um den `binary_sensor` für den
Verbindungsstatus erweitern (→ `YAML_ERGAENZUNGEN.yaml`).
