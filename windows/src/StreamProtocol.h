#pragma once
#include "Net.h"

#include <cstdint>
#include <string>
#include <vector>

// KrakznCam-Protokoll, siehe docs/protokoll.md
namespace KrakznCam {

constexpr uint16_t kPort = 7878;
constexpr uint8_t kMsgHello = 0x01;
constexpr uint8_t kMsgVideoFrame = 0x02;
constexpr uint8_t kMsgCommand = 0x10;
constexpr uint32_t kMaxMessage = 64u * 1024 * 1024;

inline uint32_t ReadBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline uint64_t ReadBE64(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

inline void AppendBE32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

inline void AppendBE64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

inline std::vector<uint8_t> BuildMessage(uint8_t type, const void* payload, size_t size) {
    std::vector<uint8_t> message;
    message.reserve(5 + size);
    AppendBE32(message, static_cast<uint32_t>(1 + size));
    message.push_back(type);
    const uint8_t* p = static_cast<const uint8_t*>(payload);
    message.insert(message.end(), p, p + size);
    return message;
}

// Liest eine ganze Nachricht. false bei Fehler, Zeitüberschreitung oder ungültiger Länge.
inline bool ReadMessage(SOCKET s, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t header[4];
    if (!RecvAll(s, header, sizeof(header))) return false;
    uint32_t length = ReadBE32(header);
    if (length < 1 || length > kMaxMessage) return false;
    if (!RecvAll(s, &type, 1)) return false;
    payload.resize(length - 1);
    return payload.empty() || RecvAll(s, payload.data(), payload.size());
}

inline bool SendCommand(SOCKET s, const std::string& json) {
    std::vector<uint8_t> message = BuildMessage(kMsgCommand, json.data(), json.size());
    return SendAll(s, message.data(), message.size());
}

}  // namespace KrakznCam
