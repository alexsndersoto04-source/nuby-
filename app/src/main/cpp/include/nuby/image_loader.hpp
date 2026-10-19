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

struct ImageUploadChunk {
    std::uint32_t rowCount = 0U;
    std::uint64_t byteCount = 0U;
};

// Plans a whole-row RGBA8 transfer that fits within byteBudget.
bool planRgba8UploadRows(std::uint32_t width, std::uint32_t height,
                         std::uint32_t firstRow, std::uint64_t byteBudget,
                         ImageUploadChunk& chunk);

// Builds a complete box-filtered mip chain on a CPU worker. The input base
// level is moved into levels; dimensions follow OpenGL's floor-halving rule.
bool generateMipChain(ImageRgba8 base, std::vector<ImageRgba8>& levels,
                      std::string& error);

// Computes RGBA8 storage across all mip levels without overflowing.
bool imageMipChainByteSize(const std::vector<ImageRgba8>& levels,
                           std::uint64_t& byteSize);

// Dependency-free TGA decoder for uncompressed and RLE true-color images.
// Color-mapped and grayscale TGA files are intentionally rejected.
bool decodeTga(std::string_view source, ImageRgba8& image, std::string& error);

// Dependency-free PNG decoder for non-interlaced 8-bit true-color, grayscale,
// grayscale-alpha and RGBA images, plus indexed and 1/2/4-bit grayscale PNGs.
// 16-bit samples, Adam7 interlace and APNG animation are rejected. Output rows
// use the same bottom-origin convention.
bool decodePng(std::string_view source, ImageRgba8& image, std::string& error);

// Dispatches by file signature, not extension.
bool decodeImage(std::string_view source, ImageRgba8& image, std::string& error);

}  // namespace nuby
