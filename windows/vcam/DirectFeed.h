#pragma once
#include <windows.h>

#include <thread>

// Empfängt das iPhone-Bild in der Kamera selbst, wenn KrakznCam nicht läuft (z. B. ist nur OBS
// offen): USB-Tunnel, H.264-Dekodierung und Umrechnung wie in KrakznCam, Ausgabe in den
// gemeinsamen Speicher, aus dem die Kamera liest.
//
// Läuft KrakznCam oder wird es gestartet, trennt sich die Kamera vom iPhone und KrakznCam
// übernimmt (das iPhone bedient immer nur einen Empfänger). Wird KrakznCam geschlossen, verbindet
// sich die Kamera wieder selbst. Das alles läuft nur, solange ein Programm die Kamera benutzt.
class DirectFeed {
public:
    DirectFeed();
    ~DirectFeed();
    DirectFeed(const DirectFeed&) = delete;
    DirectFeed& operator=(const DirectFeed&) = delete;

    void Start();
    void Stop();

private:
    void Run();

    HANDLE m_stopEvent = nullptr;
    std::thread m_thread;
};
