#pragma once
#include <cstdint>
#include <vector>

// Bild, das die virtuelle Kamera zeigt, solange von KrakznCam nichts kommt: dunkler Grund,
// das Objektiv aus dem Logo und ein kurzer Hinweis. NV12, BT.709, Videobereich.
class Placeholder {
public:
    // Liefert das Bild in der gewünschten Größe (wird einmal je Größe gezeichnet).
    const std::vector<uint8_t>& Get(uint32_t width, uint32_t height);

private:
    std::vector<uint8_t> m_data;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
