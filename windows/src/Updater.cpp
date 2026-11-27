#include "Updater.h"

#include "Http.h"
#include "Json.h"
#include "Log.h"
#include "TextUtil.h"

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace {

std::vector<int> VersionParts(const std::string& text) {
    std::vector<int> parts;
    size_t i = 0;
    while (i < text.size() && !isdigit(static_cast<unsigned char>(text[i]))) ++i;  // „v“ am Anfang
    while (i < text.size() && parts.size() < 4) {
        int value = 0;
        bool digits = false;
        while (i < text.size() && isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + (text[i] - '0');
            digits = true;
            ++i;
        }
        if (!digits) break;
        parts.push_back(value);
        if (i < text.size() && text[i] == '.') ++i;
        else break;  // Zusätze wie „-beta“ ignorieren
    }
    return parts;
}

// SHA-256 einer Datei als Kleinbuchstaben-Hex.
std::string Sha256File(const std::wstring& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return result;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            std::vector<unsigned char> buffer(256 * 1024);
            DWORD read = 0;
            while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
                BCryptHashData(hash, buffer.data(), read, 0);
            }
            CloseHandle(file);
            unsigned char digest[32];
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                char hex[3];
                for (unsigned char byte : digest) {
                    snprintf(hex, sizeof(hex), "%02x", byte);
                    result += hex;
                }
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

std::wstring DownloadDirectory() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring directory = std::wstring(temp) + L"KrakznCam-Update";
    CreateDirectoryW(directory.c_str(), nullptr);
    return directory;
}

}  // namespace

int CompareVersions(const std::string& a, const std::string& b) {
    const std::vector<int> left = VersionParts(a);
    const std::vector<int> right = VersionParts(b);
    for (size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const int x = i < left.size() ? left[i] : 0;
        const int y = i < right.size() ? right[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

Updater::Updater(std::string currentVersion, std::wstring feed, bool active)
    : m_current(std::move(currentVersion)), m_feed(std::move(feed)) {
    if (!active) {
        m_status.state = State::Off;
        return;
    }
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // Reste früherer Updates wegräumen (das gerade laufende Setup lässt sich nicht löschen, das ist gewollt).
    WIN32_FIND_DATAW data{};
    const std::wstring directory = DownloadDirectory();
    HANDLE find = FindFirstFileW((directory + L"\\*.exe").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((directory + L"\\" + data.cFileName).c_str());
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    m_thread = std::thread([this] { Run(); });
}

Updater::~Updater() {
    if (m_thread.joinable()) {
        SetEvent(m_stopEvent);
        m_thread.join();
    }
    if (m_wakeEvent) CloseHandle(m_wakeEvent);
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

Updater::Status Updater::GetStatus() {
    std::lock_guard lock(m_mutex);
    return m_status;
}

void Updater::SetState(State state, const std::string& error) {
    std::lock_guard lock(m_mutex);
    m_status.state = state;
    m_status.error = error;
}

void Updater::CheckNow() {
    if (!m_thread.joinable()) return;
    m_checkRequested = true;
    SetEvent(m_wakeEvent);
}

void Updater::Download() {
    if (!m_thread.joinable()) return;
    m_downloadRequested = true;
    SetEvent(m_wakeEvent);
}

void Updater::Run() {
    // Kurz nach dem Start prüfen (nicht sofort, damit der Start flott bleibt), dann alle 6 Stunden.
    DWORD wait = 3000;
    const HANDLE events[] = {m_stopEvent, m_wakeEvent};
    for (;;) {
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, wait);
        if (result == WAIT_OBJECT_0) break;
        if (m_downloadRequested.exchange(false)) {
            DoDownload();
        } else if (result == WAIT_TIMEOUT || m_checkRequested.exchange(false)) {
            State state;
            {
                std::lock_guard lock(m_mutex);
                state = m_status.state;
            }
            // Nicht neu prüfen, während ein Update bereitliegt oder lädt.
            if (state != State::Downloading && state != State::Ready) Check();
            // Release ohne Setup: GitHub baut es vermutlich gerade, also bald nochmal schauen.
            wait = m_retrySoon.exchange(false) ? 15 * 60 * 1000 : 6 * 60 * 60 * 1000;
        }
    }
}

void Updater::Check() {
    SetState(State::Checking);
    std::string body;
    const HttpResult http = HttpGetText(m_feed, body, 4 * 1024 * 1024, L"application/vnd.github+json");
    if (!http.ok) {
        // 404: Es gibt noch kein Release. Kein Fehler, den man zeigen müsste.
        if (http.status == 404) {
            SetState(State::UpToDate);
        } else {
            LogLine("Update-Prüfung fehlgeschlagen: %s", http.error.c_str());
            SetState(State::Failed, http.error);
        }
        return;
    }
    JsonValue release;
    if (!ParseJson(body, release) || release.type != JsonValue::Type::Object) {
        SetState(State::Failed, "Antwort nicht lesbar");
        return;
    }
    const std::string tag = release.String("tag_name");
    if (release.Bool("draft") || release.Bool("prerelease") || CompareVersions(tag, m_current) <= 0) {
        std::lock_guard lock(m_mutex);
        m_status.state = State::UpToDate;
        m_status.error.clear();
        return;
    }
    const JsonValue* assets = release.Find("assets");
    const JsonValue* setup = nullptr;
    if (assets && assets->type == JsonValue::Type::Array) {
        for (const JsonValue& asset : assets->items) {
            if (asset.String("name") == kSetupAssetName) setup = &asset;
        }
    }
    if (!setup) {
        LogLine("Release %s hat noch kein %s, prüfe in 15 Minuten erneut", tag.c_str(), kSetupAssetName);
        m_retrySoon = true;
        SetState(State::UpToDate);
        return;
    }
    std::string version = tag;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) version.erase(0, 1);
    LogLine("Update gefunden: %s (installiert: %s)", version.c_str(), m_current.c_str());
    std::lock_guard lock(m_mutex);
    m_assetUrl = Wide(setup->String("browser_download_url"));
    m_assetSize = static_cast<uint64_t>(setup->Number("size"));
    m_assetDigest = setup->String("digest");
    m_status.state = State::Available;
    m_status.version = version;
    m_status.progress = 0.0f;
    m_status.error.clear();
}

void Updater::DoDownload() {
    std::wstring url;
    uint64_t expectedSize = 0;
    std::string digest;
    std::string version;
    {
        std::lock_guard lock(m_mutex);
        if (m_status.state != State::Available && m_status.state != State::Failed) return;
        if (m_assetUrl.empty()) return;
        url = m_assetUrl;
        expectedSize = m_assetSize;
        digest = m_assetDigest;
        version = m_status.version;
        m_status.state = State::Downloading;
        m_status.progress = 0.0f;
        m_status.error.clear();
    }
    const std::wstring path = DownloadDirectory() + L"\\KrakznCam-Setup-" + Wide(version) + L".exe";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetState(State::Failed, "Datei lässt sich nicht speichern");
        return;
    }
    const HttpResult http = HttpGet(
        url,
        [&](const void* data, size_t size) {
            DWORD written = 0;
            return WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size &&
                   WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0;
        },
        [&](uint64_t received, uint64_t total) {
            if (total == 0) total = expectedSize;
            std::lock_guard lock(m_mutex);
            m_status.progress = total ? static_cast<float>(static_cast<double>(received) / total) : 0.0f;
        });
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);

    std::string error;
    if (!http.ok) error = http.error;
    else if (expectedSize != 0 && static_cast<uint64_t>(size.QuadPart) != expectedSize) error = "Download unvollständig";
    else if (digest.rfind("sha256:", 0) == 0 && Sha256File(path) != digest.substr(7)) error = "Prüfsumme stimmt nicht";
    if (!error.empty()) {
        LogLine("Update-Download fehlgeschlagen: %s", error.c_str());
        DeleteFileW(path.c_str());
        SetState(State::Failed, error);
        return;
    }
    LogLine("Update %s heruntergeladen (%lld Bytes)", version.c_str(), static_cast<long long>(size.QuadPart));
    std::lock_guard lock(m_mutex);
    m_downloadedFile = path;
    m_status.state = State::Ready;
    m_status.progress = 1.0f;
}

bool Updater::LaunchInstaller() {
    std::wstring file;
    {
        std::lock_guard lock(m_mutex);
        if (m_status.state != State::Ready) return false;
        file = m_downloadedFile;
    }
    std::wstring command = L"\"" + file + L"\" --update --wait-pid " + std::to_wstring(GetCurrentProcessId());
    // Nur für Tests: zusätzliche Argumente fürs Setup, z. B. --test-root.
    wchar_t extra[512];
    const DWORD extraLength = GetEnvironmentVariableW(L"KRAKZNCAM_SETUP_ARGS", extra, 512);
    if (extraLength > 0 && extraLength < 512) command += L" " + std::wstring(extra, extraLength);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(file.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
        LogLine("Setup ließ sich nicht starten (Fehler %lu)", GetLastError());
        SetState(State::Failed, "Setup ließ sich nicht starten");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    LogLine("Update wird installiert, KrakznCam beendet sich");
    return true;
}
