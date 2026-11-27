#include "Log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}

std::wstring DefaultLogPath() {
    PWSTR folder = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        path = folder;
        path += L"\\KrakznCam";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\krakzncam.log";
    }
    CoTaskMemFree(folder);
    return path;
}

void LogInit(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    if (!path.empty()) {
        _wfopen_s(&g_file, path.c_str(), L"wb");
    }
}

void LogLine(const char* format, ...) {
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);
    char line[2200];
    snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u  %s\r\n", now.wHour, now.wMinute, now.wSecond,
             now.wMilliseconds, message);

    std::lock_guard lock(g_mutex);
    OutputDebugStringA(line);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}
