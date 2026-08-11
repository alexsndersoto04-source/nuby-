#pragma once

// ============================================================================
// NUBY MEDIA — Imagen decodificada en memoria (RGBA plano, 8 bits por canal).
// La producen los decodificadores propios de Nuby (PNG por ahora).
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <vector>

namespace nuby::media {

struct Image {
    int width{0};
    int height{0};
    std::vector<uint32_t> pixels; // 0xAARRGGBB, orden fila-arriba→abajo

    bool valid() const { return width > 0 && height > 0 && pixels.size() == (size_t)width * height; }

    uint32_t at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return 0x00000000;
        return pixels[(size_t)y * width + x];
    }
};

} // namespace nuby::media
