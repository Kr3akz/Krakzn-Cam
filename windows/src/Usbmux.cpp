#include "Usbmux.h"

#include "Plist.h"

#include <atomic>

namespace {

std::atomic<uint32_t> g_tag{1};

SOCKET OpenService() {
    SOCKET s = TcpConnect("127.0.0.1", Usbmux::kServicePort, 1000);
    if (s != INVALID_SOCKET) SetRecvTimeout(s, 5000);
    return s;
}

PlistDictWriter BaseRequest(const char* messageType) {
    PlistDictWriter writer;
    writer.AddString("MessageType", messageType);
    writer.AddString("ClientVersionString", "kabelcam");
    writer.AddString("ProgName", "kabelcam");
    return writer;
}

// Kopf: Länge, Version (1 = plist), Typ (8 = plist), Tag. Little Endian.
bool Exchange(SOCKET s, const std::string& xml, PlistValue& reply) {
    uint32_t header[4] = {static_cast<uint32_t>(16 + xml.size()), 1, 8, g_tag++};
    if (!SendAll(s, header, sizeof(header)) || !SendAll(s, xml.data(), xml.size())) return false;

    uint32_t replyHeader[4];
    if (!RecvAll(s, replyHeader, sizeof(replyHeader))) return false;
    uint32_t length = replyHeader[0];
    if (length < 16 || length > 4 * 1024 * 1024) return false;
    std::string body(length - 16, '\0');
    if (!RecvAll(s, body.data(), body.size())) return false;
    return ParsePlistXml(body, reply);
}

}  // namespace

bool Usbmux::ListDevices(std::vector<UsbmuxDevice>& devices) {
    devices.clear();
    SOCKET s = OpenService();
    if (s == INVALID_SOCKET) return false;
    PlistValue reply;
    bool ok = Exchange(s, BaseRequest("ListDevices").Finish(), reply);
    CloseSocket(s);
    if (!ok) return false;

    if (const PlistValue* list = reply.Find("DeviceList")) {
        for (const PlistValue& entry : list->values) {
            UsbmuxDevice device;
            const PlistValue* props = entry.Find("Properties");
            int64_t fallbackId = props ? props->IntegerOr("DeviceID", 0) : 0;
            device.id = static_cast<int>(entry.IntegerOr("DeviceID", fallbackId));
            if (props) {
                device.connectionType = props->StringOr("ConnectionType", "");
                device.serial = props->StringOr("SerialNumber", "");
            }
            if (device.id != 0) devices.push_back(device);
        }
    }
    return true;
}

SOCKET Usbmux::Connect(int deviceId, uint16_t port, int& result) {
    result = -1;
    SOCKET s = OpenService();
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    PlistDictWriter writer = BaseRequest("Connect");
    writer.AddInteger("DeviceID", deviceId);
    writer.AddInteger("PortNumber", htons(port));  // usbmux erwartet Netzwerk-Byte-Reihenfolge
    PlistValue reply;
    if (!Exchange(s, writer.Finish(), reply)) {
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    result = static_cast<int>(reply.IntegerOr("Number", -1));
    if (result != 0) {
        CloseSocket(s);
        return INVALID_SOCKET;
    }
    return s;
}
