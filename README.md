# KabelCam

Mit KabelCam wird die iPhone-Kamera zur Webcam für Windows, und zwar **über das USB-Kabel**, ohne WLAN.

```
iPhone                                       Windows-PC
┌───────────────────────────┐   USB-Kabel   ┌──────────────────────────────────────┐
│ Kamera → H.264-Hardware-  │ ────────────▶ │ Apple Mobile Device Service (usbmux) │
│ Encoder → TCP 127.0.0.1:  │    usbmux     │ → Empfänger → GPU-Decoder            │
│ 7878 (KabelCam-App)       │               │ → virtuelle Kamera „KabelCam“        │
└───────────────────────────┘               └──────────────────────────────────────┘
```

**Wenig Last, gute Qualität:**
- Kodiert und dekodiert wird in den Video-Chips von iPhone und Grafikkarte.
- Über USB passt viel Bitrate durch (1080p30 mit 12 Mbit/s).
- Kodiert wird nur, wenn ein PC verbunden ist.

## Ordner

| Ordner | Inhalt | Stand |
|--------|--------|-------|
| `ios/` | iPhone-App (Swift, AVFoundation, VideoToolbox) | geschrieben, wird in der Cloud gebaut |
| `tools/kabelcam_test.py` | Testprogramm: empfängt den Stream über USB, zeigt FPS/Bitrate, speichert `.h264` | fertig |
| `docs/protokoll.md` | Wie iPhone und PC miteinander reden | fertig |
| `windows/` | Windows-Programm in C++ mit virtueller Kamera | nächster Schritt |
| `.github/workflows/ios-build.yml` | Baut die App auf einem Mac in der Cloud | fertig |

## iPhone-App bauen (ohne Mac)

1. Projekt auf GitHub hochladen. Bei jedem Push, der `ios/` ändert, baut GitHub Actions die App.
   Von Hand starten kannst du den Build unter **Actions → „iPhone-App bauen“ → Run workflow**.
2. Ist der Build fertig, das Artefakt **KabelCam-ipa** herunterladen und entpacken.
   Darin liegt `KabelCam-unsigned.ipa`.

## App auf das iPhone bringen

1. **[Sideloadly](https://sideloadly.io)** installieren.
   Findet Sideloadly das iPhone nicht, iTunes und iCloud direkt von apple.com installieren
   statt aus dem Microsoft Store.
2. iPhone per Kabel anschließen, entsperren und **„Diesem Computer vertrauen“** bestätigen.
3. In Sideloadly die `.ipa` hineinziehen, deine Apple-ID eintragen und auf **Start** klicken.
4. Auf dem iPhone:
   - **Einstellungen → Datenschutz & Sicherheit → Entwicklermodus** einschalten (ganz unten, Abschnitt „Sicherheit“).
     Der Schalter erscheint erst, wenn Sideloadly die App installiert hat. Einen Mac brauchst du dafür nicht.
     Das iPhone startet danach neu, anschließend mit „Aktivieren“ bestätigen.
   - **Einstellungen → Allgemein → VPN & Geräteverwaltung** öffnen, deine Apple-ID antippen und **vertrauen**.
5. KabelCam öffnen und den Kamerazugriff erlauben.

> Mit einer **kostenlosen Apple-ID** läuft die App 7 Tage, danach musst du sie mit Sideloadly
> neu aufspielen. Mit dem Apple-Entwicklerkonto (99 $ pro Jahr) hält sie ein Jahr.

## Verbindung testen

Solange es das Windows-Programm noch nicht gibt, prüft das Testprogramm die ganze Strecke.
Es braucht nur Python, keine Zusatzpakete:

```powershell
py tools\kabelcam_test.py --list
```

```powershell
py tools\kabelcam_test.py
```

```powershell
py tools\kabelcam_test.py --save test.h264 --seconds 10
```

- `--list` zeigt das iPhone mit `USB` an.
- Ohne Parameter erscheinen jede Sekunde FPS und Mbit/s.
- `test.h264` lässt sich mit VLC abspielen.

Die KabelCam-App muss dabei geöffnet sein, im Hintergrund gibt iOS die Kamera nicht frei.
Mit „Bildschirm abdunkeln“ in der App sparst du Akku, die Übertragung läuft weiter.

## Nächste Schritte

1. **Windows-Empfänger (C++):**
   - Verbindet sich über usbmux.
   - Dekodiert mit Media Foundation auf der GPU.
   - Zeigt eine Vorschau an.
2. **Virtuelle Kamera:**
   - Nutzt die Windows-11-Schnittstelle `MFCreateVirtualCamera`.
   - Damit erscheint „KabelCam“ in Zoom, Teams, Discord, OBS und der Windows-Kamera-App.
3. Steuerung vom PC aus: Objektiv, Zoom, Fokus.
