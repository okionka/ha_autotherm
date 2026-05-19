# USB-Proxy – Diagnosesoftware über WiFi anschließen

**Projekt:** ha_autoterm · ESP32 + ESPHome · Autoterm Air 2D / Planar 2D  
**Dokument:** Einrichtung des TCP-Diagnoseproxys und der Autoterm-Test-Software  

---

## Überblick

Der ESP32 öffnet nach dem Firmware-Update einen TCP-Server auf **Port 8888**.
Auf dem Laptop wird ein **virtueller COM-Port** erstellt, der diesen TCP-Socket
transparent durchleitet. Die Autoterm-Test-Software sieht dann denselben
seriellen Datenstrom, den sie sonst über den offiziellen USB-Adapter (Assy 2135)
erhalten würde – ohne dass du diesen teuren Adapter benötigst.

```
Autoterm Test (Laptop)
        │
   Virtueller COM-Port
   (com0com / PTY / socat)
        │
     TCP 9600 8N1
        │
  ESP32 192.168.7.6:8888
        │
   Level Shifter 3.3V↔5V
        │
   Autoterm Air 2D Heizung
```

HA-Klimaentität läuft **parallel** weiter. Diagnosesessions beeinflussen den
Normalbetrieb nicht – nach Trennung des Diagnose-Clients nimmt der ESP32 den
eigenen Poll-Zyklus automatisch wieder auf.

---

## Teil 1 – Firmware-Update

### 1.1 Dateien bereitstellen

Füge folgende neue Datei dem Repository hinzu:

```
components/autoterm2d/
  __init__.py        (unverändert)
  climate.py         (unverändert)
  autoterm2d.h       (→ 4 Ergänzungen, siehe PATCH_ANLEITUNG.md)
  panel.h            (unverändert)
  diag_proxy.h       (NEU – diese Datei kopieren)
```

### 1.2 autoterm2d.h patchen

Öffne `components/autoterm2d/autoterm2d.h` und führe die vier Änderungen aus
`PATCH_ANLEITUNG.md` durch:

| Schritt | Was | Wo |
|---------|-----|----|
| 1 | `#include "diag_proxy.h"` | Nach bestehenden Includes |
| 2 | `, public DiagProxyMixin` | In die Klassendeklaration |
| 3 | `diag_setup_();` | Am Ende von `setup()` |
| 4a | `diag_loop_tick_();` | Ganz oben in `loop()` |
| 4b | `diag_forward_rx_(b);` | Nach jedem `read_byte(&b)` |
| 4c | `diag_drain_tx_(...)` + `if (is_diagnostic_active()) return;` | Vor dem Poll-Zyklus |

### 1.3 autoterm2d.yaml ergänzen

Füge den Binary-Sensor aus `YAML_ERGAENZUNGEN.yaml` hinzu:

```yaml
binary_sensor:
  - platform: template
    name: "Diagnose-Client verbunden"
    id: diag_client_active
    icon: mdi:lan-connect
    entity_category: diagnostic
    device_class: connectivity
    lambda: return id(diesel_heater)->is_diagnostic_active();
```

### 1.4 Flashen

```bash
esphome run autoterm2d.yaml
```

Nach dem Start erscheint im Log:

```
[I][diag_proxy]: TCP-Diagnoseserver läuft auf Port 8888  (Baud 9600 8N1 ...)
```

---

## Teil 2 – Autoterm-Test-Software herunterladen

1. Öffne **https://autoterm.com/support/downloads**
2. Filter: Software → **Autoterm Test**
3. Lade **Autoterm Test V1.15** herunter
4. Installiere die Software und den COM-Treiber (falls mitgeliefert)

---

## Teil 3 – Virtuellen COM-Port einrichten

Wähle die Methode passend zu deinem Betriebssystem.

---

### Windows – Methode A: Python-Skript (empfohlen)

**Voraussetzungen:**
- Python 3.x installiert
- [com0com](https://sourceforge.net/projects/com0com/) installiert
  *(Nullmodemtreiber, kostenlos, schafft COM-Paare im System)*

**com0com einrichten (einmalig):**

1. com0com-Setup ausführen
2. Im Setup-Dialog ein COM-Paar erstellen, z. B. `CNCA0 ↔ CNCB0`
3. Beide Ports erscheinen im Gerätemanager unter *Ports (COM & LPT)*

**Bridge starten:**

```cmd
pip install pyserial
python tools\tcp_to_com.py --host 192.168.7.6 --port 8888 --com CNCA0 --baud 9600
```

Ausgabe:
```
============================================================
  ha_autoterm TCP→COM Bridge
  ESP32: 192.168.7.6:8888   Baud: 9600
============================================================
[OK] CNCA0 geöffnet. Autoterm Test mit dem Gegenstück verbinden.
[OK] TCP-Verbindung zu 192.168.7.6:8888 hergestellt
[Bridge] Datenübertragung aktiv. STRG+C zum Beenden.
```

→ In Autoterm Test dann **CNCB0** auswählen (das andere Ende des COM-Paares).

---

### Windows – Methode B: hub4com (ohne Python)

1. [hub4com](https://sourceforge.net/projects/com0com/files/hub4com/) herunterladen
2. com0com-Paar erstellen (siehe Methode A, Schritt 1–3)

```cmd
hub4com.exe --baud=9600 --route=0:All --octs=off \\.\CNCA0 TCP:192.168.7.6:8888
```

→ In Autoterm Test dann **CNCB0** auswählen.

---

### Linux

**Voraussetzung:** `socat` installiert (`sudo apt install socat` oder `sudo dnf install socat`)

**Option A – socat (direkt):**

```bash
socat PTY,link=/tmp/ttyHeizung,raw,echo=0,b9600 TCP:192.168.7.6:8888 &
```

→ In Autoterm Test (unter Wine): `/tmp/ttyHeizung`

**Option B – Python-Skript:**

```bash
pip install pyserial
python3 tools/tcp_to_com.py --host 192.168.7.6 --port 8888
```

Erstellt `/tmp/ttyHeizung` automatisch.

---

### macOS

```bash
brew install socat
socat PTY,link=/tmp/ttyHeizung,raw,echo=0,b9600 TCP:192.168.7.6:8888 &
```

→ In Autoterm Test (unter Wine): `/tmp/ttyHeizung`

---

## Teil 4 – Autoterm Test in Betrieb nehmen

### 4.1 Verbindung herstellen

1. **TCP-Bridge starten** (Teil 3) – Verbindung zum ESP32 prüfen
2. **Autoterm Test öffnen**
3. Im Hauptfenster: **Verbindung → COM-Port auswählen**
   - Windows: `CNCB0` (oder der im Gerätemanager angezeigte Name)
   - Linux/macOS: `/tmp/ttyHeizung`
4. Baud-Rate: **9600**, Parität: **Keine**, Stopbits: **1**
5. Klick auf **Verbinden**

Die Software erkennt die Heizung und zeigt den Status-Bildschirm.

### 4.2 Erfolgskontrolle in Home Assistant

Im HA-Dashboard erscheint nach Verbindungsaufbau:

- **Diagnose-Client verbunden**: `Ein`
- Alle Heizungs-Sensoren werden weiter aktualisiert

Im ESPHome-Log siehst du:

```
[I][diag_proxy]: Diagnose-Client verbunden: 192.168.1.x  –  Poll-Zyklus pausiert
```

### 4.3 Verfügbare Diagnose-Funktionen

| Funktion | Beschreibung |
|----------|-------------|
| **Status** | Betriebszustand, Fehlercodes, Temperaturen, Spannung |
| **Diagramm** | Verlauf von Temperaturen, Drehzahl, Spannung über Zeit |
| **Einstellungen** | Betriebsmodus, Zieltemperatur, Leistungsstufe |
| **Fehlerprotokoll** | Liste der letzten Fehlercodes mit Zeitstempel |
| **Firmware** | Firmware-Version des Heizungssteuergeräts |

### 4.4 Sitzung beenden

1. In Autoterm Test: **Verbindung → Trennen**
2. TCP-Bridge beenden: **STRG+C** im Terminal

Im ESPHome-Log:

```
[I][diag_proxy]: Diagnose-Client getrennt  –  normaler Betrieb wird fortgesetzt
```

HA-Entität **Diagnose-Client verbunden** geht auf `Aus`.

---

## Teil 5 – Verhalten im Bridge-Modus

Der ESP32 läuft im **Bridge-Modus** (physisches Bedienteil an UART1 angeschlossen).
Das Bedienteil treibt den Poll-Zyklus (0x0F → 0x11 → 0x02 alle ~2 Sekunden).

Während einer Diagnosesession gilt:

| | Normalbetrieb | Diagnosesession |
|---|---|---|
| Bedienteil | aktiv, steuert Heizung | weiterhin aktiv |
| ESP32 Poll-Zyklus | bridge-modus: kein eigener | entfällt (physisches Panel aktiv) |
| HA Klimaentität | aktualisiert | weiterhin aktualisiert |
| Diagnose-TCP | kein Client | Client empfängt alle Heizungsbytes |
| Befehle senden | nur über HA | über Autoterm Test möglich |

> ⚠️ **Gleichzeitige Bus-Aktivität:** Das Bedienteil sendet alle 2 Sekunden
> einen Poll. Befehle aus Autoterm Test können selten kollidieren. Das
> Autoterm-Protokoll toleriert das – ein kollisionsbedingter CRC-Fehler führt
> lediglich dazu, dass die nächste Antwort etwas verzögert kommt.
> In der Praxis ist dies bei kurzen Diagnosesessions kein Problem.

---

## Teil 6 – Fehlersuche

| Symptom | Ursache | Lösung |
|---------|---------|--------|
| Autoterm Test zeigt „Kein Gerät" | COM-Port nicht verbunden | Bridge-Script läuft? TCP-Verbindung prüfen |
| Verbindung bricht nach 5 s ab | Heizung antwortet nicht | Heizung eingeschaltet? Verkabelung prüfen |
| `diag_proxy`-Log fehlt | Patch nicht angewendet | `autoterm2d.h` nochmals prüfen |
| Sehr viele CRC-Fehler im Log | Baud-Rate falsch | Sicherstellen: 9600 in Bridge UND Autoterm Test |
| ESP32 startet nicht | `WiFiServer` nicht verfügbar | Framework muss `type: arduino` sein |
| Nur Lesen, kein Schreiben | Bridge unidirektional | Python-Skript erwartet bidirektionale Verbindung |

### ESP32-Log prüfen (live)

```bash
esphome logs autoterm2d.yaml
```

Relevante Tags: `diag_proxy`, `autoterm2d`, `heater`

### Netzwerk-Verbindbarkeit testen

```bash
# Prüfen ob Port 8888 erreichbar ist (kein Client darf gerade verbunden sein):
nc -zv 192.168.7.6 8888
```

```cmd
# Windows:
Test-NetConnection 192.168.7.6 -Port 8888
```

---

## Anhang – Schnellreferenz

### Protokoll-Parameter
- Baud-Rate: **9600**
- Datenbits: **8**
- Parität: **Keine**
- Stopbits: **1**
- Spannung an der Heizung: **5 V TTL** (Level Shifter erforderlich)

### Netzwerk
- ESP32-IP: `192.168.7.6` (laut `autoterm2d.yaml`)
- TCP-Port: `8888`
- Protokoll: roher Byte-Stream (kein Framing auf TCP-Ebene)

### Wichtige Statusmeldungen im Log

```
[I][diag_proxy]: TCP-Diagnoseserver läuft auf Port 8888
[I][diag_proxy]: Diagnose-Client verbunden: 192.168.x.x  –  Poll-Zyklus pausiert
[I][diag_proxy]: Diagnose-Client getrennt  –  normaler Betrieb wird fortgesetzt
```
