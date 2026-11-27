#pragma once
#include <windows.h>

#include <string>
#include <vector>

struct SetupItem {
    enum class Status { Ok, Missing, Warning, Info };
    Status status = Status::Ok;
    std::string key;  // windows, itunes, icloud, usb, sideloadly, appledevices
    std::string title;
    std::string detail;
    std::string actionLabel;    // leer = kein Knopf
    std::wstring actionTarget;  // URL, ms-settings:-Link oder Programmpfad
};

struct SetupReport {
    std::vector<SetupItem> items;
    bool sideloadReady = false;   // iTunes, iCloud und Sideloadly passend installiert
    std::wstring sideloadlyExe;   // leer, wenn nicht gefunden
    bool sideloadlyDaemonRunning = false;    // Hintergrunddienst läuft gerade
    bool sideloadlyDaemonAutostart = false;  // Hintergrunddienst startet mit Windows
    std::wstring appPackage;      // KrakznCam-App (.ipa) neben krakzncam.exe, leer wenn nicht da
    DWORD windowsBuild = 0;
};

// Prüft, was für KrakznCam und Sideloadly installiert ist. Liest nur die Registry
// und den Dienststatus, dauert wenige Millisekunden.
SetupReport RunSetupCheck();

// Öffnet eine URL im Browser bzw. startet ein Programm.
void OpenTarget(const std::wstring& target);
// Öffnet den Explorer mit der Datei markiert.
void RevealInExplorer(const std::wstring& path);

const char* SetupStatusName(SetupItem::Status status);
