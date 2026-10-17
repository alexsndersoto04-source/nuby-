#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nuby {

struct ImageRgba8 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Pixels are tightly packed RGBA8 with the first row at the bottom, as
    // expected by OpenGL texture coordinates.
    std::vector<std::uint8_t> pixels;
};

// Dependency-free TGA decoder for uncompressed and RLE true-color images.
// Color-mapped and grayscale TGA files are intentionally rejected.
bool decodeTga(std::string_view source, ImageRgba8& image, std::string& error);

}  // namespace nuby
