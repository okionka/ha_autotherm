#pragma once
// ============================================================================
//  diag_proxy.h  –  TCP-Diagnosedurchleitung für ha_autoterm
//  Fasst die Heizungs-UART in einen WiFi-TCP-Socket, sodass die offizielle
//  Autoterm Test-Software über einen virtuellen COM-Port zugreifen kann.
//
//  Nutzung:
//    1. Diese Datei in components/autoterm2d/ ablegen.
//    2. In autoterm2d.h: DiagProxyMixin als Basisklasse hinzufügen,
//       drei Aufrufe in setup()/loop() einfügen (siehe PATCH_ANLEITUNG.md).
//    3. autoterm2d.yaml um den binary_sensor erweitern (Vorlage weiter unten).
//
//  Protokoll:
//    – Port 8888 (konfigurierbar per set_diag_port())
//    – Roher Byte-Stream, kein Framing – identisch zum physischen COM-Port
//    – Baud-Rate 9600 8N1 in der Diagnosesoftware einstellen
//
//  Verhalten je nach Betriebsmodus:
//    Bridge-Modus (physisches Bedienteil vorhanden):
//      Bedienteil treibt weiterhin den Poll-Zyklus. Der TCP-Client empfängt
//      alle Heizungsantworten und kann eigene Befehle einspeisen.
//      Beide Bus-Teilnehmer schreiben selten gleichzeitig – Kollisionen sind
//      unwahrscheinlich und führen nur zu einem verworfenen Frame.
//
//    Virtuell-Panel-Modus (kein physisches Bedienteil):
//      Sobald ein TCP-Client verbunden ist, pausiert der interne Poll-Zyklus
//      vollständig. Die Diagnosesoftware übernimmt die Bus-Steuerung.
//      Nach Trennung des Clients nimmt der ESP32 den Poll-Betrieb wieder auf.
//
//  Sicherheit:
//    Der Server akzeptiert genau einen Client gleichzeitig.
//    Kein Auth – Zugriffsschutz über Netzwerksegmentierung (VLAN/Firewall).
// ============================================================================

#include <WiFiServer.h>
#include <WiFiClient.h>
#include <functional>
#include "esphome/core/log.h"

namespace esphome {
namespace autoterm2d {

static const char *const DIAG_TAG = "diag_proxy";

// ---------------------------------------------------------------------------
//  Mixin-Klasse  –  in Autoterm2DClimate als zusätzliche Basis einbinden
// ---------------------------------------------------------------------------
class DiagProxyMixin {
 public:
  // ── Konfiguration (optional, Standard: Port 8888) ────────────────────────
  void set_diag_port(uint16_t port) { diag_port_ = port; }

  // ── Status-Abfrage ────────────────────────────────────────────────────────
  /// true solange ein TCP-Diagnoseclient verbunden ist
  bool is_diagnostic_active() const { return diagnostic_active_; }

  /// IP-Adresse des verbundenen Clients (oder "" wenn keiner)
  std::string diag_client_ip() const {
    if (!diagnostic_active_) return "";
    return diag_client_ip_;
  }

 protected:
  // ── Lifecycle ─────────────────────────────────────────────────────────────

  /// In setup() aufrufen – startet den TCP-Server
  void diag_setup_() {
    diag_server_ = new WiFiServer(diag_port_);
    diag_server_->begin();
    diag_server_->setNoDelay(true);
    ESP_LOGI(DIAG_TAG,
             "TCP-Diagnoseserver läuft auf Port %u  (Baud 9600 8N1 in der "
             "Diagnosesoftware einstellen)",
             diag_port_);
  }

  /// Am Anfang von loop() aufrufen – verwaltet Client-Verbindungen
  /// Gibt true zurück wenn ein Client verbunden ist
  bool diag_loop_tick_() {
    if (!diag_server_) return false;

    // ── Neue Verbindung annehmen ──────────────────────────────────────────
    if (!diag_connected_()) {
      WiFiClient incoming = diag_server_->accept();
      if (incoming) {
        if (diag_client_) diag_client_.stop();
        diag_client_ = incoming;
        diag_client_.setNoDelay(true);
        diag_client_ip_ = diag_client_.remoteIP().toString().c_str();
        diagnostic_active_ = true;
        ESP_LOGI(DIAG_TAG,
                 "Diagnose-Client verbunden: %s  –  Poll-Zyklus pausiert",
                 diag_client_ip_.c_str());
      }
    }

    // ── Verbindungsabbruch erkennen ───────────────────────────────────────
    if (diagnostic_active_ && !diag_connected_()) {
      diag_client_.stop();
      diag_client_ip_ = "";
      diagnostic_active_ = false;
      ESP_LOGI(DIAG_TAG,
               "Diagnose-Client getrennt  –  normaler Betrieb wird fortgesetzt");
    }

    return diagnostic_active_;
  }

  /// Nach dem Lesen eines UART-Bytes aus der Heizung aufrufen:
  ///   read_byte(&b);
  ///   diag_forward_rx_(b);     ← dieses hier
  /// Leitet das Byte zusätzlich an den TCP-Client weiter.
  void diag_forward_rx_(uint8_t byte) {
    if (diagnostic_active_ && diag_connected_()) {
      diag_client_.write(byte);
    }
  }

  /// Liest Bytes vom TCP-Client und übergibt sie an uart_write_fn.
  /// uart_write_fn(b) soll das Byte auf UART2 TX senden.
  /// Max. 64 Bytes pro Loop-Tick (kein Watchdog-Risk).
  void diag_drain_tx_(const std::function<void(uint8_t)> &uart_write_fn) {
    if (!diagnostic_active_ || !diag_connected_()) return;
    uint8_t count = 0;
    while (diag_client_.available() && count < 64) {
      uint8_t b = static_cast<uint8_t>(diag_client_.read());
      uart_write_fn(b);
      ++count;
    }
  }

 private:
  uint16_t    diag_port_{8888};
  WiFiServer *diag_server_{nullptr};
  WiFiClient  diag_client_;
  bool        diagnostic_active_{false};
  std::string diag_client_ip_;

  bool diag_connected_() {
    return diag_client_ && diag_client_.connected();
  }
};

}  // namespace autoterm2d
}  // namespace esphome
