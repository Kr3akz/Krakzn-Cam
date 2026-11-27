# KrakznCam-Protokoll (Version 1)

So reden die iPhone-App und das Windows-Programm miteinander.

## Transport

- Die iPhone-App lauscht auf **TCP-Port 7878**, und zwar nur auf `127.0.0.1`.
  Im WLAN ist der Port dadurch nicht erreichbar, nur über das USB-Kabel.
- Der PC erreicht den Port über **usbmux**. Das ist der USB-Tunnel des
  „Apple Mobile Device Service“, der unter Windows auf `127.0.0.1:27015` lauscht.
  - Der PC verbindet sich mit 27015 und schickt eine `Connect`-Anfrage mit
    `DeviceID` und Port 7878.
  - Danach ist der Socket ein normaler TCP-Tunnel zur App.
  - Eine Beispiel-Implementierung steht in `tools/krakzncam_test.py`.
- Es wird immer nur ein PC gleichzeitig bedient.

## Nachrichtenrahmen

Jede Nachricht in beide Richtungen hat diesen Aufbau (alle Zahlen Big Endian):

| Bytes | Inhalt |
|------:|--------|
| 4     | `UInt32` Länge = 1 + Länge der Nutzdaten |
| 1     | `UInt8` Nachrichtentyp |
| n     | Nutzdaten |

## Nachrichtentypen

### `0x01` HELLO (iPhone → PC)

Die Nutzdaten sind ein UTF-8-JSON-Objekt. Die App schickt es sofort nach dem
Verbindungsaufbau als erste Nachricht und erneut nach jeder Änderung der Einstellungen.

```json
{
  "version": 1,
  "app": "KrakznCam",
  "codec": "h264",
  "width": 1920,
  "height": 1080,
  "fps": 30,
  "bitrate": 12000000,
  "lens": "wide",
  "signatureExpires": 1765000000
}
```

- `signatureExpires` gibt das Ablaufdatum der App-Signatur in Unix-Sekunden an.
  Sideloadly signiert mit kostenloser Apple-ID für 7 Tage. Das Feld fehlt, wenn die App
  kein Signaturprofil hat.
- Unbekannte Felder werden ignoriert.

### `0x02` VIDEO_FRAME (iPhone → PC)

| Bytes | Inhalt |
|------:|--------|
| 8     | `Int64` Zeitstempel (PTS) in Mikrosekunden, Uhr des iPhones |
| 1     | Flags: Bit 0 = Keyframe (IDR) |
| n     | H.264 im **Annex-B-Format** (Startcode `00 00 00 01` vor jeder NAL-Einheit) |

- Jeder Keyframe enthält **SPS und PPS** direkt vor dem IDR-Bild. Ein Decoder kann
  also bei jedem Keyframe einsteigen.
- Es gibt keine B-Frames. Die Dekodierreihenfolge ist auch die Anzeigereihenfolge.
- Der erste Frame nach dem Verbindungsaufbau ist immer ein Keyframe.
- Kommt der PC nicht hinterher, verwirft die App Frames bis zum nächsten Keyframe.
  Die Verzögerung bleibt dadurch klein.

### `0x10` COMMAND (PC → iPhone)

Die Nutzdaten sind ein UTF-8-JSON-Objekt mit dem Feld `cmd` und sind höchstens 64 KB groß.

| `cmd`      | Wirkung |
|------------|---------|
| `keyframe` | Die App schickt so bald wie möglich einen Keyframe. |

Unbekannte Befehle werden ignoriert. Neue Befehle (Objektiv, Zoom, Fokus …) kommen
später dazu, ohne dass sich die Version ändern muss.
