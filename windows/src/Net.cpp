#include "Net.h"

#include <cstdio>

bool NetInit() {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

bool SendAll(SOCKET s, const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
        int chunk = size > 1 << 20 ? 1 << 20 : static_cast<int>(size);
        int sent = send(s, p, chunk, 0);
        if (sent <= 0) return false;
        p += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

bool RecvAll(SOCKET s, void* data, size_t size) {
    char* p = static_cast<char*>(data);
    while (size > 0) {
        int chunk = size > 1 << 20 ? 1 << 20 : static_cast<int>(size);
        int received = recv(s, p, chunk, 0);
        if (received <= 0) return false;
        p += received;
        size -= static_cast<size_t>(received);
    }
    return true;
}

SOCKET TcpConnect(const char* host, uint16_t port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char portText[8];
    snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));
    addrinfo* result = nullptr;
    if (getaddrinfo(host, portText, &hints, &result) != 0 || !result) return INVALID_SOCKET;

    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        return INVALID_SOCKET;
    }

    // Nicht blockierend verbinden, damit ein geschlossener Port nicht sekundenlang hängt.
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    int rc = connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (rc == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        fd_set writeSet;
        fd_set errorSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(s, &writeSet);
        FD_SET(s, &errorSet);
        timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        rc = select(0, nullptr, &writeSet, &errorSet, &timeout);
        if (rc <= 0 || FD_ISSET(s, &errorSet) || !FD_ISSET(s, &writeSet)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    nonBlocking = 0;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    int receiveBuffer = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBuffer), sizeof(receiveBuffer));
    return s;
}

void SetRecvTimeout(SOCKET s, int timeoutMs) {
    DWORD value = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
}

void CloseSocket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        closesocket(s);
        s = INVALID_SOCKET;
    }
}
