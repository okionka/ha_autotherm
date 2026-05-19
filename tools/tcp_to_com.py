#!/usr/bin/env python3
"""
tcp_to_com.py  –  Virtueller COM-Port-Bridge für ha_autoterm Diagnose
======================================================================
Erstellt unter Windows einen virtuellen COM-Port (via com0com oder
direkt als Named Pipe) und leitet ihn an den ESP32-TCP-Diagnoseserver.

Voraussetzungen (Windows):
  pip install pyserial

Voraussetzungen (Linux/macOS):
  pip install pyserial
  socat muss installiert sein (apt install socat / brew install socat)

Verwendung:
  python tcp_to_com.py --host 192.168.7.6 --port 8888

Danach in Autoterm Test: den neu erstellten COM-Port auswählen.
"""

import argparse
import socket
import threading
import sys
import time
import os

# ---------------------------------------------------------------------------
#  Argumente
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="Virtueller COM-Port → ESP32 TCP Diagnose-Proxy"
)
parser.add_argument("--host", default="192.168.7.6",
                    help="IP-Adresse des ESP32 (Standard: 192.168.7.6)")
parser.add_argument("--port", type=int, default=8888,
                    help="TCP-Port des Diagnoseservers (Standard: 8888)")
parser.add_argument("--com",  default=None,
                    help="Zu verwendender COM-Port (Windows: COM10, Linux: /tmp/ttyHeizung)")
parser.add_argument("--baud", type=int, default=9600,
                    help="Baud-Rate (Standard: 9600, muss mit Heizung übereinstimmen)")
args = parser.parse_args()

# ---------------------------------------------------------------------------
#  Plattform-Erkennung
# ---------------------------------------------------------------------------
IS_WINDOWS = sys.platform == "win32"
IS_LINUX   = sys.platform.startswith("linux")
IS_MAC     = sys.platform == "darwin"


# ---------------------------------------------------------------------------
#  TCP-Verbindung zur Heizung
# ---------------------------------------------------------------------------
def connect_tcp(host: str, port: int, retries: int = 10) -> socket.socket:
    for attempt in range(1, retries + 1):
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.settimeout(5.0)
            sock.connect((host, port))
            sock.settimeout(None)
            print(f"[OK] TCP-Verbindung zu {host}:{port} hergestellt")
            return sock
        except OSError as e:
            print(f"[Versuch {attempt}/{retries}] Verbindung fehlgeschlagen: {e}")
            time.sleep(2)
    print("[FEHLER] Kann nicht mit ESP32 verbinden. Prüfe IP, Port und WiFi.")
    sys.exit(1)


# ---------------------------------------------------------------------------
#  Bridge-Loop (bidirektional, zwei Threads)
# ---------------------------------------------------------------------------
def bridge(serial_port, tcp_sock: socket.socket):
    import serial as ser_mod

    def tcp_to_serial():
        while True:
            try:
                data = tcp_sock.recv(256)
                if not data:
                    break
                serial_port.write(data)
            except Exception as e:
                print(f"[tcp→serial] {e}")
                break

    def serial_to_tcp():
        while True:
            try:
                data = serial_port.read(serial_port.in_waiting or 1)
                if data:
                    tcp_sock.sendall(data)
            except Exception as e:
                print(f"[serial→tcp] {e}")
                break

    t1 = threading.Thread(target=tcp_to_serial,  daemon=True)
    t2 = threading.Thread(target=serial_to_tcp,  daemon=True)
    t1.start()
    t2.start()
    print("[Bridge] Datenübertragung aktiv. STRG+C zum Beenden.")
    try:
        t1.join()
        t2.join()
    except KeyboardInterrupt:
        pass
    finally:
        tcp_sock.close()
        serial_port.close()
        print("[Bridge] Beendet.")


# ---------------------------------------------------------------------------
#  Windows: com0com-Paar verwenden
# ---------------------------------------------------------------------------
def run_windows(host: str, port: int, com: str, baud: int):
    try:
        import serial
    except ImportError:
        print("pyserial fehlt. Bitte: pip install pyserial")
        sys.exit(1)

    if com is None:
        # com0com Standard-Paar: CNCA0 ↔ CNCB0
        # Autoterm Test nutzt CNCB0, dieses Skript nutzt CNCA0
        com = "CNCA0"
        print(f"[INFO] Kein --com angegeben. Versuche {com} (com0com).")
        print(f"       Autoterm Test dann mit CNCB0 verbinden.")

    tcp = connect_tcp(host, port)

    print(f"[INFO] Öffne seriellen Port {com} mit {baud} Baud …")
    try:
        sp = serial.Serial(com, baudrate=baud, timeout=0.05)
    except serial.SerialException as e:
        print(f"[FEHLER] Kann {com} nicht öffnen: {e}")
        print("         com0com installieren: https://com0com.sourceforge.net")
        tcp.close()
        sys.exit(1)

    print(f"[OK] {com} geöffnet. Autoterm Test mit dem Gegenstück verbinden.")
    bridge(sp, tcp)


# ---------------------------------------------------------------------------
#  Linux/macOS: PTY-Pseudoterminal verwenden
# ---------------------------------------------------------------------------
def run_posix(host: str, port: int, com: str, baud: int):
    try:
        import serial
        import pty
        import tty
    except ImportError as e:
        print(f"Fehlendes Modul: {e}. Bitte: pip install pyserial")
        sys.exit(1)

    if com is None:
        com = "/tmp/ttyHeizung"

    # Symlink zu einem PTY erstellen
    master_fd, slave_fd = pty.openpty()
    slave_name = os.ttyname(slave_fd)

    # Symlink anlegen
    if os.path.exists(com):
        os.unlink(com)
    os.symlink(slave_name, com)
    print(f"[OK] Virtueller Port: {com}  (→ {slave_name})")
    print(f"     In Autoterm Test: {com} mit {baud} Baud auswählen.")

    tcp = connect_tcp(host, port)

    import serial
    sp = serial.Serial()
    sp.fd = master_fd
    sp._isOpen = True
    sp.baudrate = baud
    sp.timeout = 0.05

    try:
        bridge(sp, tcp)
    finally:
        if os.path.islink(com):
            os.unlink(com)


# ---------------------------------------------------------------------------
#  Einstiegspunkt
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    print("=" * 60)
    print("  ha_autoterm TCP→COM Bridge")
    print(f"  ESP32: {args.host}:{args.port}   Baud: {args.baud}")
    print("=" * 60)

    if IS_WINDOWS:
        run_windows(args.host, args.port, args.com, args.baud)
    else:
        run_posix(args.host, args.port, args.com, args.baud)
