#include "Http.h"

#include <windows.h>
#include <winhttp.h>

#include "TextUtil.h"

#include <vector>

namespace {

struct Handle {
    HINTERNET value = nullptr;
    ~Handle() {
        if (value) WinHttpCloseHandle(value);
    }
};

std::string WinHttpError(const char* step) {
    return std::string(step) + " fehlgeschlagen (Fehler " + std::to_string(GetLastError()) + ")";
}

}  // namespace

HttpResult HttpGet(const std::wstring& url, const std::function<bool(const void*, size_t)>& sink,
                   const std::function<void(uint64_t, uint64_t)>& progress, const wchar_t* acceptHeader) {
    HttpResult result;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    std::vector<wchar_t> path(url.size() + 1);
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path.data();
    parts.dwUrlPathLength = static_cast<DWORD>(path.size());
    wchar_t extra[2048] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        result.error = "Ungültige Adresse";
        return result;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    // Allgemeiner Programmname, keine persönlichen Daten.
    Handle session{WinHttpOpen(L"KrakznCam-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) {
        result.error = WinHttpError("WinHttpOpen");
        return result;
    }
    WinHttpSetTimeouts(session.value, 10000, 10000, 15000, 30000);
    Handle connection{WinHttpConnect(session.value, host, parts.nPort, 0)};
    if (!connection.value) {
        result.error = WinHttpError("Verbinden");
        return result;
    }
    const std::wstring object = std::wstring(path.data()) + extra;
    Handle request{WinHttpOpenRequest(connection.value, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request.value) {
        result.error = WinHttpError("Anfrage");
        return result;
    }
    std::wstring headers;
    if (acceptHeader) headers = std::wstring(L"Accept: ") + acceptHeader + L"\r\n";
    if (!WinHttpSendRequest(request.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        result.error = "Keine Verbindung zu " + Utf8(host) + " (Fehler " +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);
    if (status != 200) {
        result.error = "Antwort " + std::to_string(status);
        return result;
    }
    uint64_t total = 0;
    wchar_t lengthText[32] = {};
    DWORD lengthSize = sizeof(lengthText);
    if (WinHttpQueryHeaders(request.value, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText,
                            &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
        total = _wcstoui64(lengthText, nullptr, 10);
    }

    std::vector<char> buffer(64 * 1024);
    uint64_t received = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            result.error = WinHttpError("Herunterladen");
            return result;
        }
        if (read == 0) break;
        received += read;
        if (!sink(buffer.data(), read)) {
            result.error = "Abgebrochen";
            return result;
        }
        if (progress) progress(received, total);
    }
    if (total != 0 && received != total) {
        result.error = "Download unvollständig";
        return result;
    }
    result.ok = true;
    return result;
}

HttpResult HttpGetText(const std::wstring& url, std::string& body, size_t maxBytes, const wchar_t* acceptHeader) {
    body.clear();
    return HttpGet(
        url,
        [&](const void* data, size_t size) {
            if (body.size() + size > maxBytes) return false;
            body.append(static_cast<const char*>(data), size);
            return true;
        },
        {}, acceptHeader);
}
