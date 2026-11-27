#pragma once
#include <string>

// Einfaches Protokoll in eine Datei (und in den Debugger). Thread-sicher.
void LogInit(const std::wstring& path);
void LogLine(const char* format, ...);

// %LOCALAPPDATA%\KabelCam\kabelcam.log
std::wstring DefaultLogPath();
