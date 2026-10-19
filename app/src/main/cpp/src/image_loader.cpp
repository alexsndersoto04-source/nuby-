#include "nuby/image_loader.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace nuby {
namespace {
constexpr std::uint32_t kMaximumDimension = 4096;
constexpr std::uint64_t kMaximumPixels = 16U * 1024U * 1024U;

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

std::uint8_t byteAt(std::string_view source, std::size_t offset) {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(source[offset]));
}

std::uint16_t littleEndian16(std::string_view source, std::size_t offset) {
    return static_cast<std::uint16_t>(byteAt(source, offset))
            | static_cast<std::uint16_t>(byteAt(source, offset + 1)) << 8U;
}

bool readPixel(std::string_view source, std::size_t& offset,
               std::size_t bytesPerPixel, std::uint8_t* rgba) {
    if (offset > source.size() || bytesPerPixel > source.size() - offset) return false;
    rgba[0] = byteAt(source, offset + 2);
    rgba[1] = byteAt(source, offset + 1);
    rgba[2] = byteAt(source, offset);
    rgba[3] = bytesPerPixel == 4 ? byteAt(source, offset + 3) : 255;
    offset += bytesPerPixel;
    return true;
}

}  // namespace

bool planRgba8UploadRows(std::uint32_t width, std::uint32_t height,
                         std::uint32_t firstRow, std::uint64_t byteBudget,
                         ImageUploadChunk& chunk) {
    chunk = {};
    const std::uint64_t rowBytes = static_cast<std::uint64_t>(width) * 4U;
    if (width == 0U || height == 0U || firstRow >= height || rowBytes > byteBudget) {
        return false;
    }
    const std::uint64_t rows = std::min<std::uint64_t>(height - firstRow,
                                                        byteBudget / rowBytes);
    if (rows == 0U || rows > std::numeric_limits<std::uint32_t>::max()) return false;
    chunk.rowCount = static_cast<std::uint32_t>(rows);
    chunk.byteCount = rows * rowBytes;
    return true;
}

bool generateMipChain(ImageRgba8 base, std::vector<ImageRgba8>& levels,
                      std::string& error) {
    error.clear();
    if (base.width == 0U || base.height == 0U
            || static_cast<std::uint64_t>(base.width) * base.height > kMaximumPixels
            || base.pixels.size() != static_cast<std::size_t>(base.width)
                    * base.height * 4U) {
        return fail(error, "mipmap source dimensions or pixel storage are invalid");
    }

    std::vector<ImageRgba8> candidate;
    candidate.push_back(std::move(base));
    while (candidate.back().width > 1U || candidate.back().height > 1U) {
        const ImageRgba8& source = candidate.back();
        ImageRgba8 destination;
        destination.width = std::max(1U, source.width / 2U);
        destination.height = std::max(1U, source.height / 2U);
        const std::uint64_t pixelCount = static_cast<std::uint64_t>(destination.width)
                * destination.height;
        if (pixelCount > std::numeric_limits<std::size_t>::max() / 4U) {
            return fail(error, "mipmap level byte size overflows addressable memory");
        }
        destination.pixels.resize(static_cast<std::size_t>(pixelCount) * 4U);
        for (std::uint32_t y = 0; y < destination.height; ++y) {
            const std::uint32_t yBegin = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(y) * source.height / destination.height);
            const std::uint32_t yEnd = static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(y + 1U) * source.height / destination.height);
            for (std::uint32_t x = 0; x < destination.width; ++x) {
                const std::uint32_t xBegin = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x) * source.width / destination.width);
                const std::uint32_t xEnd = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x + 1U) * source.width / destination.width);
                std::uint64_t channels[4]{};
                std::uint64_t samples = 0U;
                for (std::uint32_t sy = yBegin; sy < yEnd; ++sy) {
                    for (std::uint32_t sx = xBegin; sx < xEnd; ++sx) {
                        const std::size_t sourceOffset =
                                (static_cast<std::size_t>(sy) * source.width + sx) * 4U;
                        for (std::size_t channel = 0; channel < 4U; ++channel) {
                            channels[channel] += source.pixels[sourceOffset + channel];
                        }
                        ++samples;
                    }
                }
                const std::size_t destinationOffset =
                        (static_cast<std::size_t>(y) * destination.width + x) * 4U;
                for (std::size_t channel = 0; channel < 4U; ++channel) {
                    destination.pixels[destinationOffset + channel] =
                            static_cast<std::uint8_t>((channels[channel] + samples / 2U) / samples);
                }
            }
        }
        candidate.push_back(std::move(destination));
    }
    levels = std::move(candidate);
    return true;
}

bool imageMipChainByteSize(const std::vector<ImageRgba8>& levels,
                           std::uint64_t& byteSize) {
    byteSize = 0U;
    if (levels.empty()) return false;
    for (const ImageRgba8& level : levels) {
        const std::uint64_t pixelCount = static_cast<std::uint64_t>(level.width)
                * level.height;
        if (level.width == 0U || level.height == 0U
                || pixelCount > std::numeric_limits<std::uint64_t>::max() / 4U) {
            byteSize = 0U;
            return false;
        }
        const std::uint64_t expected = pixelCount * 4U;
        if (expected != level.pixels.size()
                || expected > std::numeric_limits<std::uint64_t>::max() - byteSize) {
            byteSize = 0U;
            return false;
        }
        byteSize += expected;
    }
    return true;
}

bool decodeTga(std::string_view source, ImageRgba8& image, std::string& error) {
    error.clear();
    if (source.size() < 18) return fail(error, "TGA header is truncated");

    const std::uint8_t idLength = byteAt(source, 0);
    const std::uint8_t colorMapType = byteAt(source, 1);
    const std::uint8_t imageType = byteAt(source, 2);
    const std::uint32_t width = littleEndian16(source, 12);
    const std::uint32_t height = littleEndian16(source, 14);
    const std::uint8_t bitsPerPixel = byteAt(source, 16);
    const std::uint8_t descriptor = byteAt(source, 17);
    if (colorMapType != 0 || (imageType != 2 && imageType != 10)) {
        return fail(error, "TGA must be true-color, uncompressed or RLE");
    }
    if ((bitsPerPixel != 24 && bitsPerPixel != 32)
            || (descriptor & 0xC0U) != 0U) {
        return fail(error, "TGA pixel format or interleaving mode is unsupported");
    }
    if (width == 0 || height == 0 || width > kMaximumDimension
            || height > kMaximumDimension
            || static_cast<std::uint64_t>(width) * height > kMaximumPixels) {
        return fail(error, "TGA dimensions are invalid or exceed the decoder limits");
    }

    const std::size_t bytesPerPixel = bitsPerPixel / 8U;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    const std::size_t headerSize = 18U + idLength;
    if (headerSize > source.size()) return fail(error, "TGA image ID is truncated");

    ImageRgba8 candidate;
    candidate.width = width;
    candidate.height = height;
    candidate.pixels.resize(pixelCount * 4U);
    std::size_t inputOffset = headerSize;
    std::size_t decodedPixels = 0;
    const bool topOrigin = (descriptor & 0x20U) != 0U;
    const bool rightOrigin = (descriptor & 0x10U) != 0U;

    const auto storePixel = [&](std::size_t sourceIndex, const std::uint8_t* rgba) {
        const std::size_t sourceY = sourceIndex / width;
        const std::size_t sourceX = sourceIndex % width;
        const std::size_t destinationY = topOrigin ? height - 1U - sourceY : sourceY;
        const std::size_t destinationX = rightOrigin ? width - 1U - sourceX : sourceX;
        const std::size_t destinationOffset = (destinationY * width + destinationX) * 4U;
        candidate.pixels[destinationOffset] = rgba[0];
        candidate.pixels[destinationOffset + 1U] = rgba[1];
        candidate.pixels[destinationOffset + 2U] = rgba[2];
        candidate.pixels[destinationOffset + 3U] = rgba[3];
    };

    if (imageType == 2) {
        if (pixelCount > (source.size() - inputOffset) / bytesPerPixel) {
            return fail(error, "TGA pixel data is truncated");
        }
        std::uint8_t rgba[4]{};
        while (decodedPixels < pixelCount) {
            if (!readPixel(source, inputOffset, bytesPerPixel, rgba)) {
                return fail(error, "TGA pixel data is truncated");
            }
            storePixel(decodedPixels++, rgba);
        }
    } else {
        std::uint8_t rgba[4]{};
        while (decodedPixels < pixelCount) {
            if (inputOffset >= source.size()) return fail(error, "TGA RLE packet is truncated");
            const std::uint8_t packet = byteAt(source, inputOffset++);
            const std::size_t packetLength = (packet & 0x7FU) + 1U;
            if (packetLength > pixelCount - decodedPixels) {
                return fail(error, "TGA RLE packet exceeds image dimensions");
            }
            if ((packet & 0x80U) != 0U) {
                if (!readPixel(source, inputOffset, bytesPerPixel, rgba)) {
                    return fail(error, "TGA RLE pixel is truncated");
                }
                for (std::size_t count = 0; count < packetLength; ++count) {
                    storePixel(decodedPixels++, rgba);
                }
            } else {
                for (std::size_t count = 0; count < packetLength; ++count) {
                    if (!readPixel(source, inputOffset, bytesPerPixel, rgba)) {
                        return fail(error, "TGA raw packet is truncated");
                    }
                    storePixel(decodedPixels++, rgba);
                }
            }
        }
    }

    image = std::move(candidate);
    return true;
}

}  // namespace nuby
