#include "nuby/image_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void appendByte(std::string& output, unsigned int value) {
    output.push_back(static_cast<char>(value & 0xFFU));
}

void append16(std::string& output, unsigned int value) {
    appendByte(output, value);
    appendByte(output, value >> 8U);
}

std::string tgaHeader(unsigned int width, unsigned int height,
                      unsigned int bits, unsigned int imageType,
                      unsigned int descriptor) {
    std::string output;
    output.reserve(18);
    appendByte(output, 0);  // ID length
    appendByte(output, 0);  // no color map
    appendByte(output, imageType);
    output.append(5, '\0'); // color-map specification
    output.append(4, '\0'); // image origin
    append16(output, width);
    append16(output, height);
    appendByte(output, bits);
    appendByte(output, descriptor);
    return output;
}

void appendBgr(std::string& output, unsigned int red, unsigned int green,
               unsigned int blue, unsigned int alpha = 255) {
    appendByte(output, blue);
    appendByte(output, green);
    appendByte(output, red);
    if (alpha != 255) appendByte(output, alpha);
}

bool pixelEquals(const nuby::ImageRgba8& image, std::size_t index,
                 unsigned int red, unsigned int green, unsigned int blue,
                 unsigned int alpha = 255) {
    if (index * 4U + 3U >= image.pixels.size()) return false;
    return image.pixels[index * 4U] == red
            && image.pixels[index * 4U + 1U] == green
            && image.pixels[index * 4U + 2U] == blue
            && image.pixels[index * 4U + 3U] == alpha;
}

void appendBigEndian(std::string& output, std::uint32_t value) {
    appendByte(output, value >> 24U);
    appendByte(output, value >> 16U);
    appendByte(output, value >> 8U);
    appendByte(output, value);
}

std::uint32_t testPngCrc(std::string_view bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char value : bytes) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void appendPngChunk(std::string& png, const char* type, const std::string& payload) {
    appendBigEndian(png, static_cast<std::uint32_t>(payload.size()));
    std::string crcInput(type, 4U);
    crcInput += payload;
    png += crcInput;
    appendBigEndian(png, testPngCrc(crcInput));
}

std::string storedZlib(const std::string& raw) {
    std::string output;
    appendByte(output, 0x78U);
    appendByte(output, 0x01U);
    std::size_t offset = 0;
    do {
        const std::size_t count = std::min<std::size_t>(65535U, raw.size() - offset);
        const bool final = offset + count == raw.size();
        appendByte(output, final ? 1U : 0U);
        append16(output, static_cast<unsigned int>(count));
        append16(output, static_cast<unsigned int>(~count));
        output.append(raw.data() + offset, count);
        offset += count;
    } while (offset < raw.size());
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (unsigned char value : raw) {
        a = (a + value) % 65521U;
        b = (b + a) % 65521U;
    }
    appendBigEndian(output, (b << 16U) | a);
    return output;
}

std::string makePngWithZlib(std::uint32_t width, std::uint32_t height,
                            unsigned int bitDepth, unsigned int colorType,
                            const std::string& zlibData,
                            const std::string& palette = {},
                            const std::string& transparency = {},
                            std::size_t idatChunkSize = 0U) {
    std::string png("\x89PNG\r\n\x1a\n", 8U);
    std::string header;
    appendBigEndian(header, width);
    appendBigEndian(header, height);
    appendByte(header, bitDepth);
    appendByte(header, colorType);
    appendByte(header, 0U);
    appendByte(header, 0U);
    appendByte(header, 0U);
    appendPngChunk(png, "IHDR", header);
    if (!palette.empty()) appendPngChunk(png, "PLTE", palette);
    if (!transparency.empty()) appendPngChunk(png, "tRNS", transparency);
    const std::size_t chunkSize = idatChunkSize == 0U ? zlibData.size() : idatChunkSize;
    for (std::size_t offset = 0; offset < zlibData.size(); offset += chunkSize) {
        appendPngChunk(png, "IDAT", zlibData.substr(offset, chunkSize));
    }
    appendPngChunk(png, "IEND", {});
    return png;
}

std::string makeStoredPng(std::uint32_t width, std::uint32_t height,
                          unsigned int bitDepth, unsigned int colorType,
                          const std::string& scanlines,
                          const std::string& palette = {},
                          const std::string& transparency = {},
                          std::size_t idatChunkSize = 0U) {
    return makePngWithZlib(width, height, bitDepth, colorType, storedZlib(scanlines),
                           palette, transparency, idatChunkSize);
}

std::uint8_t testPaeth(std::uint8_t left, std::uint8_t above, std::uint8_t upperLeft) {
    const int p = static_cast<int>(left) + above - upperLeft;
    const int a = std::abs(p - static_cast<int>(left));
    const int b = std::abs(p - static_cast<int>(above));
    const int c = std::abs(p - static_cast<int>(upperLeft));
    if (a <= b && a <= c) return left;
    if (b <= c) return above;
    return upperLeft;
}

void testPngReconstructsAllFiltersAndUsesBottomOrigin() {
    constexpr std::size_t width = 2U;
    constexpr std::size_t height = 5U;
    constexpr std::size_t rowBytes = width * 4U;
    std::vector<std::uint8_t> rows(height * rowBytes);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < rowBytes; ++x) {
            rows[y * rowBytes + x] = static_cast<std::uint8_t>(11U + y * 31U + x * 7U);
        }
    }
    std::string filtered;
    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t filter = static_cast<std::uint8_t>(y);
        appendByte(filtered, filter);
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const std::uint8_t value = rows[y * rowBytes + x];
            const std::uint8_t left = x < 4U ? 0U : rows[y * rowBytes + x - 4U];
            const std::uint8_t above = y == 0U ? 0U : rows[(y - 1U) * rowBytes + x];
            const std::uint8_t upperLeft = y == 0U || x < 4U
                    ? 0U : rows[(y - 1U) * rowBytes + x - 4U];
            std::uint8_t predictor = 0U;
            if (filter == 1U) predictor = left;
            else if (filter == 2U) predictor = above;
            else if (filter == 3U) predictor = static_cast<std::uint8_t>(
                    (static_cast<unsigned>(left) + above) / 2U);
            else if (filter == 4U) predictor = testPaeth(left, above, upperLeft);
            appendByte(filtered, static_cast<std::uint8_t>(value - predictor));
        }
    }
    const std::string png = makeStoredPng(width, height, 8U, 6U, filtered, {}, {}, 5U);
    nuby::ImageRgba8 image;
    std::string error;
    expect(nuby::decodePng(png, image, error), "PNG decoder supports stored DEFLATE and split IDAT chunks");
    expect(image.width == width && image.height == height && image.pixels.size() == rows.size(),
           "decoded PNG dimensions and tightly packed RGBA output are correct");
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const std::size_t output = (height - 1U - y) * rowBytes + x;
            expect(image.pixels[output] == rows[y * rowBytes + x],
                   "all five PNG filters reconstruct source pixels and flip rows");
        }
    }
}

void testPngIndexedGrayscaleAndTransparency() {
    const std::string palette{static_cast<char>(255), 0, 0, 0, static_cast<char>(255), 0,
                              0, 0, static_cast<char>(255), static_cast<char>(255),
                              static_cast<char>(255), static_cast<char>(255)};
    const std::string alpha{static_cast<char>(255), static_cast<char>(128), 0,
                            static_cast<char>(255)};
    nuby::ImageRgba8 image;
    std::string error;
    const std::string indexed = makeStoredPng(4U, 1U, 2U, 3U,
            std::string({static_cast<char>(0), static_cast<char>(0x1B)}), palette, alpha);
    expect(nuby::decodePng(indexed, image, error), "2-bit indexed PNG with PLTE and tRNS decodes");
    expect(pixelEquals(image, 0, 255, 0, 0, 255)
                   && pixelEquals(image, 1, 0, 255, 0, 128)
                   && pixelEquals(image, 2, 0, 0, 255, 0)
                   && pixelEquals(image, 3, 255, 255, 255, 255),
           "indexed PNG palette entries and per-entry transparency expand to RGBA");

    const std::string gray = makeStoredPng(4U, 1U, 2U, 0U,
            std::string({static_cast<char>(0), static_cast<char>(0x1B)}), {},
            std::string({static_cast<char>(0), static_cast<char>(2)}));
    expect(nuby::decodePng(gray, image, error), "packed 2-bit grayscale PNG decodes");
    expect(pixelEquals(image, 0, 0, 0, 0)
                   && pixelEquals(image, 1, 85, 85, 85)
                   && pixelEquals(image, 2, 170, 170, 170, 0)
                   && pixelEquals(image, 3, 255, 255, 255),
           "low-bit grayscale samples scale to RGBA8 and transparent key is honored");

    const std::string grayAlpha = makeStoredPng(1U, 1U, 8U, 4U,
            std::string({static_cast<char>(0), 64, 33}));
    expect(nuby::decodePng(grayAlpha, image, error)
                   && pixelEquals(image, 0, 64, 64, 64, 33),
           "grayscale-alpha PNG pixels expand to RGBA");
    const std::string rgb = makeStoredPng(1U, 1U, 8U, 2U,
            std::string({static_cast<char>(0), 9, 8, 7}), {},
            std::string({static_cast<char>(0), 9, static_cast<char>(0), 8,
                         static_cast<char>(0), 7}));
    expect(nuby::decodePng(rgb, image, error) && pixelEquals(image, 0, 9, 8, 7, 0),
           "true-color PNG transparency keys are honored");
}

void testPngFixedAndDynamicHuffmanInflation() {
    const std::string fixedHex = "78da63f8cfc0f01f081b40140397889c86918d5b40544a5e050069000789";
    std::string fixedCompressed;
    for (std::size_t index = 0; index < fixedHex.size(); index += 2U) {
        fixedCompressed.push_back(static_cast<char>(std::stoul(fixedHex.substr(index, 2U), nullptr, 16)));
    }
    const std::string fixedPng = makePngWithZlib(3U, 2U, 8U, 6U, fixedCompressed);
    nuby::ImageRgba8 image;
    std::string error;
    expect(nuby::decodePng(fixedPng, image, error), "PNG fixed-Huffman DEFLATE stream inflates");
    expect(image.width == 3U && image.height == 2U
                   && pixelEquals(image, 0, 10, 20, 30, 40)
                   && pixelEquals(image, 2, 90, 100, 110, 120)
                   && pixelEquals(image, 3, 255, 0, 0, 255)
                   && pixelEquals(image, 5, 0, 0, 255, 0),
           "fixed-Huffman PNG decodes and vertically flips RGBA rows");

    const std::string compressedHex =
            "78da0dca554803000005c019985860610c2c4c2c4cec44dd6c4c2c4c2cb0b0c0623ac5c219d889858905161658600c7582c52c2c4c6660e2bbef231004a494f5ac5c02a2532915cd7d93cb3bccfb4f2e51394d1307afd084ace2dace91b98d832b169a8abeb52b5a3eada51fedf4e18b1bcdd4d13b0cadae6b741eed954d501acd2d30260d6d606a65178d474c5e0b2d3c31bb046d61f3f01a4d46d5c0062dbda0b2156defecf11b4ddbccc907adb4be7b0cede68d5d08cdd63d2816ad6d707a15ed87575c01cd372229076d7c71eb084d5856cd102d2e835a85b6c6387f4253d43127a1e59635f4a01ddfbe73a019d97904a355b70fcda03dfff249a091fd2293d17a2796b6d1384588ea6821f1998568b3ebfb1768924aba16682979e58d68f493bb0f340d637b4fb4a29a8e61b4cb973f7e344b67ff28b4a67fe4b7a83e";
    std::string compressed;
    for (std::size_t index = 0; index < compressedHex.size(); index += 2U) {
        compressed.push_back(static_cast<char>(std::stoul(compressedHex.substr(index, 2U), nullptr, 16)));
    }
    const std::string png = makePngWithZlib(344U, 1U, 8U, 0U, compressed);
    expect(nuby::decodePng(png, image, error), "PNG dynamic-Huffman DEFLATE stream inflates");
    expect(image.width == 344U && image.height == 1U && image.pixels.size() == 344U * 4U,
           "dynamic-Huffman PNG produces the requested output size");
    for (std::size_t x = 0; x < 344U; ++x) {
        const std::uint8_t expected = static_cast<std::uint8_t>((x * 13U + x / 7U) & 0xFFU);
        expect(pixelEquals(image, x, expected, expected, expected),
               "dynamic-Huffman PNG literals and back-references decode correctly");
    }
}

void refreshFirstPngIdatCrc(std::string& png) {
    const std::size_t typeOffset = png.find("IDAT");
    if (typeOffset == std::string::npos || typeOffset < 4U) return;
    const std::uint32_t length = static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(png[typeOffset - 4U])) << 24U
            | static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(png[typeOffset - 3U])) << 16U
            | static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(png[typeOffset - 2U])) << 8U
            | static_cast<std::uint32_t>(static_cast<std::uint8_t>(png[typeOffset - 1U]));
    const std::size_t crcOffset = typeOffset + 4U + length;
    if (crcOffset + 4U > png.size()) return;
    const std::uint32_t crc = testPngCrc(std::string_view(png).substr(typeOffset, 4U + length));
    png[crcOffset] = static_cast<char>(crc >> 24U);
    png[crcOffset + 1U] = static_cast<char>(crc >> 16U);
    png[crcOffset + 2U] = static_cast<char>(crc >> 8U);
    png[crcOffset + 3U] = static_cast<char>(crc);
}

void testMalformedPngIsRejectedWithoutReplacingPreviousImage() {
    const std::string valid = makeStoredPng(1U, 1U, 8U, 6U,
            std::string({static_cast<char>(0), 1, 2, 3, 4}));
    nuby::ImageRgba8 image{1U, 1U, {7U, 8U, 9U, 10U}};
    std::string error;
    std::string badCrc = valid;
    badCrc[badCrc.size() - 5U] ^= 1;
    expect(!nuby::decodePng(badCrc, image, error) && !error.empty(),
           "PNG chunk CRC corruption is rejected");
    expect(image.pixels == std::vector<std::uint8_t>({7U, 8U, 9U, 10U}),
           "failed PNG decode leaves caller image unchanged");
    std::string badAdler = valid;
    const std::size_t idatOffset = badAdler.find("IDAT");
    const std::uint32_t idatLength = static_cast<std::uint32_t>(
            static_cast<std::uint8_t>(badAdler[idatOffset - 4U])) << 24U
            | static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(badAdler[idatOffset - 3U])) << 16U
            | static_cast<std::uint32_t>(
                    static_cast<std::uint8_t>(badAdler[idatOffset - 2U])) << 8U
            | static_cast<std::uint32_t>(static_cast<std::uint8_t>(badAdler[idatOffset - 1U]));
    badAdler[idatOffset + 4U + idatLength - 1U] ^= 1;
    refreshFirstPngIdatCrc(badAdler);
    expect(!nuby::decodePng(badAdler, image, error),
           "PNG zlib Adler checksum is validated independently of chunk CRC");
    std::string interlaced = valid;
    // IHDR interlace method is byte 28 in the minimal signature/length/type/header layout.
    interlaced[28] = 1;
    const std::uint32_t crc = testPngCrc(std::string_view(interlaced).substr(12U, 17U));
    interlaced[29] = static_cast<char>(crc >> 24U);
    interlaced[30] = static_cast<char>(crc >> 16U);
    interlaced[31] = static_cast<char>(crc >> 8U);
    interlaced[32] = static_cast<char>(crc);
    expect(!nuby::decodePng(interlaced, image, error), "Adam7 interlaced PNG is rejected explicitly");
    expect(!nuby::decodePng(valid.substr(0, 20U), image, error), "truncated PNG chunks are rejected");
    const std::string oversized = makeStoredPng(5000U, 1U, 8U, 6U, {});
    expect(!nuby::decodePng(oversized, image, error), "PNG dimensions above decoder limits are rejected");
    std::string invalidSignature = valid;
    invalidSignature[0] = 'X';
    expect(!nuby::decodeImage(invalidSignature, image, error),
           "image signature dispatch does not misclassify invalid PNG as a valid image");
    expect(nuby::decodeImage(valid, image, error) && pixelEquals(image, 0, 1, 2, 3, 4),
           "generic image loader dispatches PNG by signature");
}

void testUncompressedTopOriginConvertsToBottomLeftRgba() {
    std::string source = tgaHeader(2, 2, 24, 2, 0x20);
    appendBgr(source, 255, 0, 0);   // top left: red
    appendBgr(source, 0, 255, 0);   // top right: green
    appendBgr(source, 0, 0, 255);   // bottom left: blue
    appendBgr(source, 255, 255, 255); // bottom right: white

    nuby::ImageRgba8 image;
    std::string error;
    expect(nuby::decodeTga(source, image, error), "uncompressed TGA decodes");
    expect(image.width == 2 && image.height == 2 && image.pixels.size() == 16,
           "decoded texture dimensions and RGBA storage are correct");
    expect(pixelEquals(image, 0, 0, 0, 255)
                   && pixelEquals(image, 1, 255, 255, 255)
                   && pixelEquals(image, 2, 255, 0, 0)
                   && pixelEquals(image, 3, 0, 255, 0),
           "top-origin BGR data is flipped and converted to OpenGL RGBA order");
    nuby::ImageRgba8 autoDetected;
    expect(nuby::decodeImage(source, autoDetected, error)
                   && autoDetected.pixels == image.pixels,
           "generic image loader retains TGA support while detecting by signature");
}

void testRleAndRightOriginImagesDecode() {
    std::string source = tgaHeader(2, 1, 24, 10, 0x10);
    appendByte(source, 0x81); // two identical pixels
    appendBgr(source, 40, 80, 120);

    nuby::ImageRgba8 image;
    std::string error;
    expect(nuby::decodeTga(source, image, error), "RLE true-color TGA decodes");
    expect(pixelEquals(image, 0, 40, 80, 120)
                   && pixelEquals(image, 1, 40, 80, 120),
           "RLE packet expansion preserves color values and right-origin placement");

    std::string raw = tgaHeader(2, 1, 32, 2, 0x10);
    appendBgr(raw, 10, 20, 30, 40); // source right pixel
    appendBgr(raw, 50, 60, 70, 80); // source left pixel
    expect(nuby::decodeTga(raw, image, error), "32-bit TGA with alpha decodes");
    expect(pixelEquals(image, 0, 50, 60, 70, 80)
                   && pixelEquals(image, 1, 10, 20, 30, 40),
           "right-origin pixels are mirrored and alpha is preserved");
}

void testPackagedAdventureMaterialMapsDecode() {
    const std::string paths[] = {
        "app/src/main/assets/textures/sun_relic_albedo.tga",
        "app/src/main/assets/textures/sun_relic_normal.tga",
        "app/src/main/assets/textures/sun_relic_metal_rough.tga"
    };
    for (const std::string& path : paths) {
        std::ifstream file(path, std::ios::binary);
        expect(file.good(), "packaged adventure material texture exists");
        if (!file.good()) continue;
        const std::string source((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
        nuby::ImageRgba8 image;
        std::string error;
        expect(nuby::decodeTga(source, image, error),
               "packaged adventure material texture decodes");
        expect(image.width == 128 && image.height == 128,
               "packaged material texture has its intended mobile-friendly size");
    }
}

void testPackagedWayfarerFaceTextureDecodes() {
    std::ifstream file("app/src/main/assets/textures/wayfarer_face.tga", std::ios::binary);
    expect(file.good(), "packaged wayfarer face texture exists");
    if (!file.good()) return;
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    nuby::ImageRgba8 image;
    std::string error;
    expect(nuby::decodeTga(source, image, error),
           "wayfarer face texture decodes through the engine TGA loader");
    expect(image.width == 256 && image.height == 256,
           "facial details use a compact 256-pixel texture for readability");
}

void testPackagedOpacityMaskContainsVariableMatteAlpha() {
    std::ifstream file("app/src/main/assets/meshes/orbital_shard_opacity.tga",
                       std::ios::binary);
    expect(file.good(), "packaged orbital shard opacity mask exists");
    if (!file.good()) return;
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    nuby::ImageRgba8 image;
    std::string error;
    expect(nuby::decodeTga(source, image, error),
           "packaged orbital shard opacity mask decodes");
    expect(image.width == 64 && image.height == 64,
           "opacity mask uses a compact texture dimension");
    if (image.pixels.empty()) return;
    std::uint8_t minimumAlpha = 255;
    std::uint8_t maximumAlpha = 0;
    for (std::size_t pixel = 3; pixel < image.pixels.size(); pixel += 4) {
        minimumAlpha = std::min(minimumAlpha, image.pixels[pixel]);
        maximumAlpha = std::max(maximumAlpha, image.pixels[pixel]);
    }
    const std::size_t centerAlpha = (32U * 64U + 32U) * 4U + 3U;
    expect(minimumAlpha < 16 && maximumAlpha > 240
                   && image.pixels[centerAlpha] > 240,
           "opacity mask provides transparent edges and an opaque center in its matte channel");
}

void testMalformedAndUnsupportedImagesAreRejectedTransactionally() {
    nuby::ImageRgba8 image;
    image.width = 1;
    image.height = 1;
    image.pixels = {1, 2, 3, 4};
    const std::string invalidFiles[] = {
        std::string("short"),
        tgaHeader(1, 1, 8, 2, 0),
        tgaHeader(1, 1, 24, 3, 0),
        tgaHeader(5000, 1, 24, 2, 0),
        tgaHeader(1, 1, 24, 2, 0)
    };
    for (const std::string& source : invalidFiles) {
        std::string error;
        expect(!nuby::decodeTga(source, image, error),
               "unsupported, oversized or truncated TGA is rejected");
        expect(!error.empty(), "image decode failure reports a useful error");
        expect(image.width == 1 && image.height == 1
                       && image.pixels == std::vector<std::uint8_t>({1, 2, 3, 4}),
               "failed image decode leaves the previous texture untouched");
    }

    std::string oversizedRle = tgaHeader(1, 1, 24, 10, 0);
    appendByte(oversizedRle, 0x81); // two pixels cannot fit into a 1x1 image
    appendBgr(oversizedRle, 255, 255, 255);
    std::string error;
    expect(!nuby::decodeTga(oversizedRle, image, error),
           "RLE packets cannot write beyond the declared image dimensions");
}
}  // namespace

int main() {
    testPngReconstructsAllFiltersAndUsesBottomOrigin();
    testPngIndexedGrayscaleAndTransparency();
    testPngFixedAndDynamicHuffmanInflation();
    testMalformedPngIsRejectedWithoutReplacingPreviousImage();
    testUncompressedTopOriginConvertsToBottomLeftRgba();
    testRleAndRightOriginImagesDecode();
    testPackagedAdventureMaterialMapsDecode();
    testPackagedWayfarerFaceTextureDecodes();
    testPackagedOpacityMaskContainsVariableMatteAlpha();
    testMalformedAndUnsupportedImagesAreRejectedTransactionally();
    if (failures != 0) {
        std::cerr << failures << " image loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All image loader tests passed\n";
    return EXIT_SUCCESS;
}
