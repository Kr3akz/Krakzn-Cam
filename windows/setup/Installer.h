#pragma once
// Was das KrakznCam-Setup tatsächlich tut (ohne Oberfläche).
//
// Installiert wird nur für den angemeldeten Benutzer nach %LOCALAPPDATA%\Programs\KrakznCam.
// Dafür braucht es keine Administratorrechte, auch nicht für spätere Updates. Nur die virtuelle
// Kamera (eine DLL in „Programme“, die der Windows-Kameradienst lädt) fragt einmal per UAC nach,
// und bei Updates nur dann, wenn sich die Kamera-DLL geändert hat.
//
// Die Programmdateien stecken als Ressourcen im Setup (101 krakzncam.exe, 102 krakzncam_vcam.dll,
// 103 KrakznCam.ipa, falls beim Bauen vorhanden).

#include <windows.h>

#include <functional>
#include <string>

struct InstallPaths {
    std::wstring installDir;    // %LOCALAPPDATA%\Programs\KrakznCam
    std::wstring startMenu;     // Verknüpfung im Startmenü
    std::wstring desktop;       // Verknüpfung auf dem Desktop
    std::wstring uninstallKey;  // unter HKEY_CURRENT_USER
    std::wstring dataDir;       // %LOCALAPPDATA%\KrakznCam (Protokolle, Einstellungen)
    bool test = false;          // Testordner: keine Kamera, kein Aufräumen bei Sideloadly
    std::wstring testRoot;
};

// testRoot leer = echte Orte. Sonst landet alles in diesem Ordner (zum Ausprobieren).
InstallPaths MakePaths(const std::wstring& testRoot);

struct InstallOptions {
    bool desktopShortcut = true;
    bool camera = true;   // virtuelle Kamera einrichten (nur bei Neuinstallation)
    bool update = false;  // Update aus KrakznCam heraus
    DWORD waitPid = 0;    // auf das Ende dieses Prozesses warten (das alte KrakznCam)
};

struct TaskResult {
    bool ok = false;
    std::string error;  // bei Fehler: was schiefging
    std::string note;   // bei Erfolg: Hinweis (z. B. Kamera später einrichten)
};

// fraction 0..1 und eine kurze Beschreibung des aktuellen Schritts.
using ProgressFn = std::function<void(float fraction, const std::string& step)>;

TaskResult Install(const InstallPaths& paths, const InstallOptions& options, const ProgressFn& progress);
TaskResult Uninstall(const InstallPaths& paths, const ProgressFn& progress);

std::string InstalledVersion(const InstallPaths& paths);  // leer = nicht installiert
bool CameraSupported();                                   // Windows 11
bool LaunchApp(const InstallPaths& paths);
// Liegt das Setup selbst im Programmordner (Deinstallation)? Dann muss es sich vorher kopieren.
bool RunningFromInstallDir(const InstallPaths& paths);
// Nach der Deinstallation: die eigene Datei kurz danach löschen lassen.
void DeleteSelfLater();
