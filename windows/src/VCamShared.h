#pragma once
// Verbindung zwischen KrakznCam und der virtuellen Kamera.
//
// Die virtuelle Kamera ist eine Media Source (krakzncam_vcam.dll), die Windows im
// Frame-Server-Dienst lädt, sobald ein Programm wie OBS die Kamera „KrakznCam“ öffnet.
// KrakznCam schreibt das aktuelle Bild in einen gemeinsamen Speicher, die Kamera liest es.
//
// - Den Speicher legt die Kamera an (der Dienst darf globale Namen anlegen, KrakznCam nicht).
// - Die Kamera trägt das gewünschte Format und ein Lebenszeichen ein. Nur solange es frisch ist,
//   rechnet KrakznCam Bilder um. Ohne Zuschauer kostet die virtuelle Kamera also nichts.
// - Das Bild ist NV12 (BT.709, Videobereich 16–235), eng gepackt (Zeilenlänge = Breite).
// - Schutz ohne Sperre: KrakznCam zählt `sequence` vor dem Schreiben auf ungerade und danach
//   auf gerade. Die Kamera verwirft eine Kopie, wenn sich die Zahl dabei geändert hat.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace VCam {

// {BC09AC5E-011E-4A36-AB2C-88338367A148}
inline constexpr GUID kSourceClsid = {0xbc09ac5e, 0x011e, 0x4a36, {0xab, 0x2c, 0x88, 0x33, 0x83, 0x67, 0xa1, 0x48}};
inline constexpr wchar_t kSourceClsidString[] = L"{BC09AC5E-011E-4A36-AB2C-88338367A148}";
inline constexpr wchar_t kFriendlyName[] = L"KrakznCam";
inline constexpr wchar_t kDllName[] = L"krakzncam_vcam.dll";
inline constexpr wchar_t kMappingName[] = L"Global\\KrakznCamVirtualCamera";

inline constexpr uint32_t kMagic = 0x4D41434Bu;  // "KCAM"
inline constexpr uint32_t kVersion = 1;
inline constexpr uint32_t kMaxWidth = 1920;
inline constexpr uint32_t kMaxHeight = 1080;
inline constexpr size_t kMaxFrameBytes = size_t(kMaxWidth) * kMaxHeight * 3 / 2;

// Wie lange ein Lebenszeichen bzw. ein Bild als aktuell gilt.
inline constexpr ULONGLONG kConsumerTimeoutMs = 2000;
inline constexpr ULONGLONG kFrameTimeoutMs = 1500;

struct alignas(64) Header {
    uint32_t magic;
    uint32_t version;
    // Schreibt die Kamera: gewünschtes Format und Lebenszeichen.
    volatile LONG wantedWidth;
    volatile LONG wantedHeight;
    volatile LONG wantedFps;
    volatile LONG reserved;
    volatile LONG64 consumerTick;  // GetTickCount64() bei der letzten Bildausgabe
    // Schreibt KrakznCam.
    volatile LONG64 sequence;
    volatile LONG frameWidth;
    volatile LONG frameHeight;
    volatile LONG64 frameTick;  // GetTickCount64() beim Schreiben
    volatile LONG64 frameNumber;
};

inline constexpr size_t kMappingSize = sizeof(Header) + kMaxFrameBytes;

inline uint8_t* FrameData(Header* header) {
    return reinterpret_cast<uint8_t*>(header) + sizeof(Header);
}

inline size_t FrameBytes(uint32_t width, uint32_t height) {
    return size_t(width) * height * 3 / 2;
}

// Name des gemeinsamen Speichers. Für Tests ohne Dienst lässt er sich über die Umgebungsvariable
// KRAKZNCAM_VCAM_SHARED ändern (z. B. „Local\KrakznCamTest“, das darf jeder Benutzer anlegen).
inline std::wstring MappingName() {
    wchar_t value[128];
    const DWORD length = GetEnvironmentVariableW(L"KRAKZNCAM_VCAM_SHARED", value, 128);
    return length > 0 && length < 128 ? std::wstring(value, length) : std::wstring(kMappingName);
}

// Ereignis „neues Bild liegt bereit“: KrakznCam setzt es nach jedem Bild, die Kamera liefert dann
// sofort aus, statt nach eigener Uhr (zwei 60-Hz-Uhren laufen sonst auseinander und es ruckelt).
inline std::wstring FrameEventName() {
    return MappingName() + L"Frame";
}

// Markierung „KrakznCam läuft und liefert die Bilder“. Gibt es sie nicht, empfängt die
// Kamera-DLL das iPhone selbst (z. B. wenn nur OBS offen ist).
inline std::wstring AppMutexName() {
    return MappingName() + L"App";
}

// Läuft KrakznCam? Auch ein fremdes, nicht zu öffnendes Objekt zählt (anderer Benutzer/Dienst).
inline bool AppRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, AppMutexName().c_str());
    if (mutex) {
        CloseHandle(mutex);
        return true;
    }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

}  // namespace VCam
