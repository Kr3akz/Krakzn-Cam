#pragma once
#include <d3d11.h>

#include <string>

// Speichert eine BGRA-Textur als PNG (für Selbsttests und Fehlersuche).
bool SaveTexturePng(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                    const std::wstring& path);
