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
