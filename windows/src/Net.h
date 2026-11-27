#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <cstdint>

// Kleine Helfer rund um Winsock.
bool NetInit();
bool SendAll(SOCKET s, const void* data, size_t size);
bool RecvAll(SOCKET s, void* data, size_t size);
// Verbindet mit Zeitlimit. INVALID_SOCKET bei Fehler.
SOCKET TcpConnect(const char* host, uint16_t port, int timeoutMs);
void SetRecvTimeout(SOCKET s, int timeoutMs);
void CloseSocket(SOCKET& s);
