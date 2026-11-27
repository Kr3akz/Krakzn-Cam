# KrakznCam

Mit KrakznCam wird die iPhone-Kamera zur Webcam für Windows, und zwar **über das USB-Kabel**, ohne WLAN.

```
iPhone                                       Windows-PC
┌───────────────────────────┐   USB-Kabel   ┌──────────────────────────────────────┐
│ Kamera → H.264-Hardware-  │ ────────────▶ │ Apple Mobile Device Service (usbmux) │
│ Encoder → TCP 127.0.0.1:  │    usbmux     │ → Empfänger → GPU-Decoder            │
│ 7878 (KrakznCam-App)      │ ◀──────────── │ → virtuelle Kamera „KrakznCam“       │
│ Einstellungen vom PC      │ Einstellungen │   (OBS, Discord, Teams …)            │
└───────────────────────────┘               └──────────────────────────────────────┘
```

**Wenig Last, gute Qualität:**
- Kodiert und dekodiert wird in den Video-Chips von iPhone und Grafikkarte.
- Über USB passt viel Bitrate durch (1080p60 mit etwa 20 Mbit/s).
- Kodiert wird nur, wenn ein PC verbunden ist. Das Windows-Programm zeichnet nur bei neuen Bildern.

## Ordner

| Ordner | Inhalt | Stand |
|--------|--------|-------|
| `ios/` | iPhone-App (Swift, AVFoundation, VideoToolbox) | wird in der Cloud gebaut |
| `windows/` | Windows-Programm (C++): Empfang per USB, Dekodierung auf der Grafikkarte, Vorschau, Kamera-Einstellungen, Einrichtungsprüfung | fertig |
| `windows/vcam/` | Virtuelle Kamera `krakzncam_vcam.dll` (Media Source für Windows 11) | fertig |
| `windows/tools/fakephone.cpp` | Testquelle: spielt das iPhone mit einem Testbild, reagiert auf Einstellungen | fertig |
| `windows/tools/vcamtest.cpp` | Liest die virtuelle Kamera wie OBS und speichert ein Bild | fertig |
| `windows/setup/` | `KrakznCam-Setup.exe`: installieren, aktualisieren, deinstallieren (im KrakznCam-Design) | fertig |
| `tools/krakzncam_test.py` | Testprogramm in Python: empfängt den Stream über USB, zeigt FPS/Bitrate | fertig |
| `docs/protokoll.md` | Wie iPhone und PC miteinander reden | fertig |
| `.github/workflows/` | Cloud-Builds für iPhone-App (macOS) und Windows-Programm, dazu `release.yml` für Updates | fertig |

## iPhone-App bauen (ohne Mac)

1. Projekt auf GitHub hochladen. Bei jedem Push, der `ios/` ändert, baut GitHub Actions die App.
   Von Hand starten kannst du den Build unter **Actions → „iPhone-App bauen“ → Run workflow**.
2. Ist der Build fertig, das Artefakt **KrakznCam-ipa** herunterladen und entpacken.
   Darin liegt `KrakznCam-unsigned.ipa`.

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
5. KrakznCam öffnen und den Kamerazugriff erlauben.

> Mit einer **kostenlosen Apple-ID** läuft die App 7 Tage, danach musst du sie mit Sideloadly
> neu aufspielen. Mit dem Apple-Entwicklerkonto (99 $ pro Jahr) hält sie ein Jahr.

## Windows-Programm

1. Unter **[Releases](https://github.com/Kr3akz/Krakzn-Cam/releases/latest)** `KrakznCam-Setup.exe` herunterladen
   und starten. Beim ersten Mal warnt Windows („Der Computer wurde durch Windows geschützt“), weil das
   Setup kein kostenpflichtiges Zertifikat hat. Dann **Weitere Informationen → Trotzdem ausführen**.
   - Installiert wird nur für dich nach `%LOCALAPPDATA%\Programs\KrakznCam`, ohne Administratorrechte.
   - Auf Wunsch richtet das Setup gleich die virtuelle Kamera ein (eine Windows-Abfrage).
   - Die iPhone-App `KrakznCam.ipa` liegt danach im Programmordner („App-Datei zeigen“ unter Einrichtung).
   - Entfernen über **Einstellungen → Apps → KrakznCam → Deinstallieren**.
2. KrakznCam aus dem Startmenü starten.
3. Rechts unter **Einrichtung** zeigt das Programm, was fehlt. Die Knöpfe starten im Browser direkt
   denselben Download wie die Knöpfe auf sideloadly.io (iTunes und iCloud als Web-Version von Apple).
   Kommst du nach der Installation ins Fenster zurück, prüft es von selbst neu.
4. iPhone anschließen, KrakznCam auf dem iPhone öffnen, und das Bild erscheint.
   Unter **Signatur** steht, wann du die App mit Sideloadly erneuern musst.
5. Rechts neben der Vorschau stellst du die Kamera ein: Auflösung, Bildrate, Qualität, Objektiv und
   Zoom, Belichtung (Korrektur, Verschlusszeit, ISO), Weißabgleich (Farbtemperatur, Tönung), Fokus,
   Bildstabilisierung, HDR, Spiegeln und Licht. Graue Regler zeigen, was die Automatik gerade wählt;
   sobald du einen ziehst, gilt dein Wert. Der Pfeil neben einer Überschrift schaltet zurück auf Automatik.
   Die Werte bleiben auf dem iPhone gespeichert.

### Updates

KrakznCam schaut beim Start und danach alle 6 Stunden, ob es ein neueres Release gibt. Dann erscheint
oben rechts **„Update 0.x.y“**. Ein Klick lädt das Setup, KrakznCam schließt sich, das Setup
aktualisiert und startet KrakznCam neu. Der Download wird an Größe und SHA-256-Prüfsumme geprüft.

So veröffentlichst du ein Update:
1. Auf GitHub **Releases → Draft a new release** öffnen.
2. Bei **Choose a tag** einen neuen Tag eintippen, z. B. `v0.2.0`. Er muss höher sein als der letzte.
3. Titel und Notizen eintragen, **Publish release** klicken.
4. Nach etwa 10 Minuten hängen `KrakznCam-Setup.exe` und `KrakznCam.ipa` am Release
   (Workflow „Release bauen“). Erst dann sehen die Programme das Update.

Hat sich die iPhone-App geändert, muss die neue `KrakznCam.ipa` von Hand mit Sideloadly aufgespielt werden
(Datei neu auswählen). Die automatische Erneuerung von Sideloadly signiert nur die alte Datei neu.

### Virtuelle Kamera für OBS (Windows 11)

1. Unter **Einrichtung → Virtuelle Kamera** auf **Einrichten** klicken und die Windows-Abfrage bestätigen.
   Das passiert nur einmal: KrakznCam kopiert `krakzncam_vcam.dll` nach `C:\Program Files\KrakznCam`
   und meldet sie bei Windows an. `krakzncam_vcam.dll` muss dafür neben `krakzncam.exe` liegen.
2. In OBS eine Quelle **Videoaufnahmegerät** hinzufügen und **KrakznCam (Windows virtuelle Kamera)** wählen
   (1080p oder 720p, 30 oder 60 FPS). Das klappt genauso in Discord, Teams, Zoom und der Kamera-App.
3. KrakznCam muss dafür nicht offen sein: Die Kamera ist dauerhaft bei Windows angemeldet. Benutzt
   OBS sie und KrakznCam ist geschlossen, verbindet sich die Kamera selbst mit dem iPhone (im
   Windows-Kameradienst, nur solange OBS sie benutzt). Startet man KrakznCam, übernimmt es nahtlos.
   Die KrakznCam-App auf dem iPhone muss offen sein. Ohne iPhone-Bild zeigt die Kamera einen Hinweis.

Die virtuelle Kamera kostet nur Leistung, während ein Programm sie wirklich benutzt: Dann rechnet die
Grafikkarte das Bild auf das gewünschte Format um. Liegt eine neuere `krakzncam_vcam.dll` neben dem
Programm, erscheint unter Einrichtung **Aktualisieren**.

Entfernen (mit Administratorrechten):

```powershell
Start-Process .\krakzncam.exe -ArgumentList '--uninstall-camera' -Verb RunAs
```

Ohne iPhone ausprobieren:

```powershell
.\krakzncam_fakephone.exe
```

```powershell
.\krakzncam.exe --direct 127.0.0.1:17878
```

Die virtuelle Kamera prüfen, so wie OBS sie öffnet (KrakznCam muss laufen):

```powershell
.\krakzncam_vcamtest.exe --device --out kamera.png
```

Protokolle:
- `%LOCALAPPDATA%\KrakznCam\krakzncam.log` (Programm)
- `%LOCALAPPDATA%\KrakznCam\einrichtung.log` (Einrichten der virtuellen Kamera, Aufräumen beim Deinstallieren)
- `%TEMP%\KrakznCam-Setup.log` (Setup)
- `C:\ProgramData\KrakznCam\vcam.log` (virtuelle Kamera im Windows-Kameradienst)
- `C:\ProgramData\KrakznCam\kamera-direkt.log` (Kamera empfängt selbst, wenn KrakznCam geschlossen ist)

## Verbindung testen (Python)

Unabhängig vom Windows-Programm prüft dieses Testprogramm die ganze Strecke.
Es braucht nur Python, keine Zusatzpakete:

```powershell
py tools\krakzncam_test.py --list
```

```powershell
py tools\krakzncam_test.py
```

```powershell
py tools\krakzncam_test.py --save test.h264 --seconds 10
```

- `--list` zeigt das iPhone mit `USB` an.
- Ohne Parameter erscheinen jede Sekunde FPS und Mbit/s.
- `test.h264` lässt sich mit VLC abspielen.

Die KrakznCam-App muss dabei geöffnet sein, im Hintergrund gibt iOS die Kamera nicht frei.
Mit „Bildschirm abdunkeln“ in der App sparst du Akku, die Übertragung läuft weiter.

## Nächste Schritte

1. **WLAN statt Kabel:** einmal per USB koppeln, danach verschlüsselt über WLAN (Idee, noch nicht gebaut).
