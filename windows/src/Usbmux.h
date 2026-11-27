#pragma once
#include "Net.h"

#include <string>
#include <vector>

struct UsbmuxDevice {
    int id = 0;
    std::string connectionType;  // "USB" oder "Network" (WLAN-Sync)
    std::string serial;
};

// Client für den USB-Tunnel des Apple Mobile Device Service (127.0.0.1:27015).
namespace Usbmux {
constexpr uint16_t kServicePort = 27015;

// false = Dienst nicht erreichbar. Eine leere Liste heißt: kein Gerät angeschlossen.
bool ListDevices(std::vector<UsbmuxDevice>& devices);

// Öffnet einen TCP-Tunnel zu `port` auf dem Gerät. Bei Erfolg ist der Socket danach eine
// direkte Verbindung zur App. result: 0 = OK, 3 = abgelehnt (App lauscht nicht),
// -1 = Dienst nicht erreichbar oder Protokollfehler.
SOCKET Connect(int deviceId, uint16_t port, int& result);
}  // namespace Usbmux
