#!/usr/bin/env python3
"""
KabelCam-Testprogramm: prüft die USB-Verbindung zur iPhone-App, ohne die Windows-App.

  py tools\\kabelcam_test.py --list                         verbundene iPhones anzeigen
  py tools\\kabelcam_test.py                                Stream empfangen, Statistik anzeigen
  py tools\\kabelcam_test.py --save test.h264 --seconds 10  Rohvideo speichern (z. B. mit VLC abspielen)

Braucht nur Python 3.8+ und den „Apple Mobile Device Service“ (kommt mit iTunes bzw. „Apple Geräte“).
Protokoll: docs/protokoll.md
"""
import argparse
import json
import plistlib
import socket
import struct
import sys
import time

USBMUX_ADDRESS = ("127.0.0.1", 27015)
KABELCAM_PORT = 7878

MSG_HELLO = 0x01
MSG_VIDEO_FRAME = 0x02
MSG_COMMAND = 0x10

# Antwortcodes von usbmux auf "Connect"
USBMUX_RESULTS = {
    0: "OK",
    2: "Gerät nicht gefunden",
    3: "Verbindung abgelehnt: KabelCam läuft nicht oder ist nicht im Vordergrund",
    6: "Falsche Protokollversion",
}


class KabelCamError(Exception):
    pass


# --------------------------------------------------------------------------- usbmux

def recv_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("Verbindung wurde geschlossen")
        data += chunk
    return bytes(data)


def usbmux_request(sock, payload, tag=1):
    """Schickt eine plist-Nachricht an usbmux und liest die Antwort."""
    body = plistlib.dumps(payload)
    # Kopf: Länge, Version (1 = plist), Nachrichtentyp (8 = plist), Tag. Little Endian.
    sock.sendall(struct.pack("<IIII", 16 + len(body), 1, 8, tag) + body)
    length, _version, _type, _tag = struct.unpack("<IIII", recv_exact(sock, 16))
    return plistlib.loads(recv_exact(sock, length - 16))


def usbmux_base(message_type):
    return {
        "MessageType": message_type,
        "ClientVersionString": "kabelcam-test",
        "ProgName": "kabelcam-test",
    }


def open_usbmux():
    try:
        return socket.create_connection(USBMUX_ADDRESS, timeout=5)
    except OSError as error:
        raise KabelCamError(
            "Apple Mobile Device Service nicht erreichbar (127.0.0.1:27015). "
            "Ist iTunes bzw. „Apple Geräte“ installiert?"
        ) from error


def list_devices():
    with open_usbmux() as sock:
        reply = usbmux_request(sock, usbmux_base("ListDevices"))
    devices = []
    for entry in reply.get("DeviceList", []):
        props = entry.get("Properties", {})
        devices.append({
            "id": entry.get("DeviceID", props.get("DeviceID")),
            "connection": props.get("ConnectionType", "?"),
            "serial": props.get("SerialNumber", "?"),
            "product_id": props.get("ProductID"),
        })
    return devices


def connect_to_device(device_id, port):
    sock = open_usbmux()
    request = usbmux_base("Connect")
    request["DeviceID"] = device_id
    request["PortNumber"] = socket.htons(port)  # usbmux erwartet den Port in Netzwerk-Byte-Reihenfolge
    reply = usbmux_request(sock, request)
    number = reply.get("Number", -1)
    if reply.get("MessageType") != "Result" or number != 0:
        sock.close()
        raise KabelCamError(f"usbmux: {USBMUX_RESULTS.get(number, f'Fehler {number}')}")
    sock.settimeout(10)
    return sock  # ab jetzt ein direkter TCP-Tunnel zur App


def pick_usb_device():
    devices = [d for d in list_devices() if d["connection"] == "USB"]
    if not devices:
        raise KabelCamError(
            "Kein iPhone per USB gefunden. Kabel prüfen, iPhone entsperren und "
            "„Diesem Computer vertrauen“ bestätigen."
        )
    return devices[0]


# --------------------------------------------------------------------------- KabelCam-Protokoll

def send_command(sock, command):
    payload = json.dumps(command).encode("utf-8")
    sock.sendall(struct.pack(">IB", 1 + len(payload), MSG_COMMAND) + payload)


def read_message(sock):
    (length,) = struct.unpack(">I", recv_exact(sock, 4))
    if length < 1 or length > 64 * 1024 * 1024:
        raise KabelCamError(f"Ungültige Nachrichtenlänge {length}")
    body = recv_exact(sock, length)
    return body[0], body[1:]


def receive_stream(sock, seconds, save_path):
    output = open(save_path, "wb") if save_path else None
    send_command(sock, {"cmd": "keyframe"})

    start = window_start = time.monotonic()
    total_frames = keyframes = 0
    window_frames = window_bytes = 0
    first_pts = None
    try:
        while True:
            msg_type, payload = read_message(sock)
            if msg_type == MSG_HELLO:
                info = json.loads(payload.decode("utf-8"))
                print(
                    f"HELLO: {info.get('width')}x{info.get('height')} @ {info.get('fps')} FPS, "
                    f"{info.get('bitrate', 0) / 1e6:.0f} Mbit/s, Codec {info.get('codec')}, "
                    f"Objektiv {info.get('lens')}"
                )
            elif msg_type == MSG_VIDEO_FRAME:
                pts_us, flags = struct.unpack(">qB", payload[:9])
                data = payload[9:]
                is_key = bool(flags & 1)
                if total_frames == 0 and not is_key:
                    print("WARNUNG: Der erste Frame ist kein Keyframe")
                if is_key and not data.startswith(b"\x00\x00\x00\x01"):
                    print("WARNUNG: Keyframe beginnt nicht mit einem Annex-B-Startcode")
                first_pts = pts_us if first_pts is None else first_pts
                total_frames += 1
                keyframes += is_key
                window_frames += 1
                window_bytes += len(data)
                if output:
                    output.write(data)
            else:
                print(f"Unbekannter Nachrichtentyp 0x{msg_type:02x} ({len(payload)} Bytes)")

            now = time.monotonic()
            if now - window_start >= 1.0:
                elapsed = now - window_start
                print(
                    f"{window_frames / elapsed:5.1f} FPS   {window_bytes * 8 / elapsed / 1e6:6.2f} Mbit/s   "
                    f"Frames: {total_frames}   Keyframes: {keyframes}"
                )
                window_start, window_frames, window_bytes = now, 0, 0
            if seconds and now - start >= seconds:
                break
    finally:
        if output:
            output.close()
            print(f"Gespeichert: {save_path} (abspielen z. B. mit VLC oder ffplay)")
    return total_frames


def main():
    parser = argparse.ArgumentParser(description="KabelCam-Verbindung über USB testen")
    parser.add_argument("--list", action="store_true", help="verbundene Geräte anzeigen und beenden")
    parser.add_argument("--seconds", type=float, default=0, help="nach so vielen Sekunden beenden (0 = nie)")
    parser.add_argument("--save", metavar="DATEI", help="H.264-Rohdaten in diese Datei schreiben")
    parser.add_argument("--port", type=int, default=KABELCAM_PORT, help="Port der App auf dem iPhone")
    parser.add_argument("--direct", metavar="HOST:PORT", help="ohne usbmux direkt verbinden (nur zum Entwickeln)")
    args = parser.parse_args()

    try:
        if args.list:
            devices = list_devices()
            if not devices:
                print("Keine Geräte gefunden.")
            for device in devices:
                print(f"ID {device['id']}: {device['connection']}, Seriennummer {device['serial']}")
            return 0

        if args.direct:
            host, port = args.direct.rsplit(":", 1)
            sock = socket.create_connection((host, int(port)), timeout=10)
            print(f"Direkt verbunden mit {args.direct}")
        else:
            device = pick_usb_device()
            print(f"iPhone gefunden (ID {device['id']}), verbinde mit Port {args.port} …")
            sock = connect_to_device(device["id"], args.port)
            print("Verbunden. Strg+C beendet.")

        with sock:
            frames = receive_stream(sock, args.seconds, args.save)
        print(f"Fertig, {frames} Frames empfangen.")
        return 0 if frames > 0 else 1
    except KabelCamError as error:
        print(f"Fehler: {error}", file=sys.stderr)
        return 2
    except (ConnectionError, socket.timeout) as error:
        print(f"Verbindung verloren: {error}", file=sys.stderr)
        return 3
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
