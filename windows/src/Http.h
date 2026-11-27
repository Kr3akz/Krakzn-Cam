#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Einfacher HTTP(S)-Abruf mit WinHTTP (folgt Weiterleitungen, z. B. von GitHub zum Download-Server).
struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string error;
};

// Ruft url ab und gibt den Inhalt stückweise an sink weiter (false bricht ab).
// progress(empfangen, gesamt) mit gesamt = 0, wenn unbekannt.
HttpResult HttpGet(const std::wstring& url, const std::function<bool(const void*, size_t)>& sink,
                   const std::function<void(uint64_t, uint64_t)>& progress = {},
                   const wchar_t* acceptHeader = nullptr);

// Ruft url ab und liefert den ganzen Inhalt (höchstens maxBytes).
HttpResult HttpGetText(const std::wstring& url, std::string& body, size_t maxBytes = 4 * 1024 * 1024,
                       const wchar_t* acceptHeader = nullptr);
