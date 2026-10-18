#include "nuby/image_loader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

namespace nuby {
namespace {

constexpr std::array<std::uint8_t, 8> kPngSignature{
        137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
constexpr std::uint32_t kMaximumDimension = 4096U;
constexpr std::uint64_t kMaximumPixels = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumCompressedBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumInflatedBytes = 68U * 1024U * 1024U;

constexpr std::array<std::uint32_t, 256> makePngCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xEDB88320U : 0U);
        }
        table[index] = value;
    }
    return table;
}

constexpr auto kPngCrcTable = makePngCrcTable();

bool failPng(std::string& error, const char* message) {
    error = message;
    return false;
}

std::uint8_t pngByte(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[offset]));
}

std::uint32_t readBigEndian32(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(pngByte(bytes, offset)) << 24U
            | static_cast<std::uint32_t>(pngByte(bytes, offset + 1U)) << 16U
            | static_cast<std::uint32_t>(pngByte(bytes, offset + 2U)) << 8U
            | static_cast<std::uint32_t>(pngByte(bytes, offset + 3U));
}

std::uint16_t readBigEndian16(std::string_view bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(pngByte(bytes, offset)) << 8U
            | static_cast<std::uint16_t>(pngByte(bytes, offset + 1U));
}

std::uint32_t pngCrc32(std::string_view bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (unsigned char value : bytes) {
        const std::uint8_t index = static_cast<std::uint8_t>(crc ^ value);
        crc = (crc >> 8U) ^ kPngCrcTable[index];
    }
    return crc ^ 0xFFFFFFFFU;
}

class DeflateBits {
public:
    explicit DeflateBits(std::string_view bytes) : bytes_(bytes) {}

    bool readBits(unsigned count, std::uint32_t& value) {
        if (count > 24U || bitOffset_ > bytes_.size() * 8U
                || count > bytes_.size() * 8U - bitOffset_) return false;
        value = 0;
        for (unsigned bit = 0; bit < count; ++bit) {
            const std::size_t offset = bitOffset_++;
            value |= static_cast<std::uint32_t>(
                    (pngByte(bytes_, offset / 8U) >> (offset & 7U)) & 1U) << bit;
        }
        return true;
    }

    void alignToByte() { bitOffset_ = (bitOffset_ + 7U) & ~std::size_t{7U}; }

    std::size_t byteOffset() const { return bitOffset_ / 8U; }

private:
    std::string_view bytes_;
    std::size_t bitOffset_ = 0;
};

struct HuffmanTable {
    std::array<std::uint16_t, 16> counts{};
    std::array<std::uint16_t, 16> firstCode{};
    std::array<std::uint16_t, 16> firstSymbol{};
    std::vector<std::uint16_t> symbols;
    unsigned maximumLength = 0;

    bool build(const std::vector<std::uint8_t>& lengths) {
        counts.fill(0U);
        maximumLength = 0;
        for (std::uint8_t length : lengths) {
            if (length > 15U) return false;
            if (length == 0U) continue;
            ++counts[length];
            maximumLength = std::max(maximumLength, static_cast<unsigned>(length));
        }
        if (maximumLength == 0U) return false;

        std::uint32_t code = 0;
        std::uint16_t symbolOffset = 0;
        for (unsigned length = 1; length <= 15U; ++length) {
            code = (code + counts[length - 1U]) << 1U;
            if (code + counts[length] > (1U << length)) return false;
            firstCode[length] = static_cast<std::uint16_t>(code);
            firstSymbol[length] = symbolOffset;
            symbolOffset = static_cast<std::uint16_t>(symbolOffset + counts[length]);
        }
        symbols.resize(symbolOffset);
        std::array<std::uint16_t, 16> nextSymbol = firstSymbol;
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const std::uint8_t length = lengths[symbol];
            if (length != 0U) symbols[nextSymbol[length]++] = static_cast<std::uint16_t>(symbol);
        }
        return true;
    }

    bool decode(DeflateBits& bits, std::uint16_t& symbol) const {
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= maximumLength; ++length) {
            std::uint32_t nextBit = 0;
            if (!bits.readBits(1U, nextBit)) return false;
            code = (code << 1U) | nextBit;
            const std::uint32_t first = firstCode[length];
            const std::uint32_t count = counts[length];
            if (code >= first && code - first < count) {
                symbol = symbols[firstSymbol[length] + code - first];
                return true;
            }
        }
        return false;
    }
};

bool buildFixedTables(HuffmanTable& literalLength, HuffmanTable& distance) {
    std::vector<std::uint8_t> literalLengths(288U);
    for (std::size_t symbol = 0; symbol <= 143U; ++symbol) literalLengths[symbol] = 8U;
    for (std::size_t symbol = 144U; symbol <= 255U; ++symbol) literalLengths[symbol] = 9U;
    for (std::size_t symbol = 256U; symbol <= 279U; ++symbol) literalLengths[symbol] = 7U;
    for (std::size_t symbol = 280U; symbol <= 287U; ++symbol) literalLengths[symbol] = 8U;
    return literalLength.build(literalLengths)
            && distance.build(std::vector<std::uint8_t>(32U, 5U));
}

bool buildDynamicTables(DeflateBits& bits, HuffmanTable& literalLength,
                        HuffmanTable& distance) {
    std::uint32_t hlitValue = 0;
    std::uint32_t hdistValue = 0;
    std::uint32_t hclenValue = 0;
    if (!bits.readBits(5U, hlitValue) || !bits.readBits(5U, hdistValue)
            || !bits.readBits(4U, hclenValue)) return false;
    const std::size_t literalCount = hlitValue + 257U;
    const std::size_t distanceCount = hdistValue + 1U;
    const std::size_t codeLengthCount = hclenValue + 4U;
    if (literalCount > 286U || distanceCount > 32U) return false;

    constexpr std::array<std::uint8_t, 19> order{
            16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U,
            11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U};
    std::vector<std::uint8_t> codeLengths(19U, 0U);
    for (std::size_t index = 0; index < codeLengthCount; ++index) {
        std::uint32_t length = 0;
        if (!bits.readBits(3U, length)) return false;
        codeLengths[order[index]] = static_cast<std::uint8_t>(length);
    }
    HuffmanTable codeLengthTable;
    if (!codeLengthTable.build(codeLengths)) return false;

    std::vector<std::uint8_t> lengths;
    lengths.reserve(literalCount + distanceCount);
    while (lengths.size() < literalCount + distanceCount) {
        std::uint16_t symbol = 0;
        if (!codeLengthTable.decode(bits, symbol)) return false;
        if (symbol <= 15U) {
            lengths.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        std::uint32_t repeat = 0;
        std::uint8_t repeatedValue = 0;
        if (symbol == 16U) {
            if (lengths.empty() || !bits.readBits(2U, repeat)) return false;
            repeat += 3U;
            repeatedValue = lengths.back();
        } else if (symbol == 17U) {
            if (!bits.readBits(3U, repeat)) return false;
            repeat += 3U;
        } else if (symbol == 18U) {
            if (!bits.readBits(7U, repeat)) return false;
            repeat += 11U;
        } else {
            return false;
        }
        if (repeat > literalCount + distanceCount - lengths.size()) return false;
        lengths.insert(lengths.end(), repeat, repeatedValue);
    }

    std::vector<std::uint8_t> literalLengths(
            lengths.begin(), lengths.begin() + static_cast<std::ptrdiff_t>(literalCount));
    std::vector<std::uint8_t> distanceLengths(
            lengths.begin() + static_cast<std::ptrdiff_t>(literalCount), lengths.end());
    return literalLengths.size() > 256U && literalLengths[256U] != 0U
            && literalLength.build(literalLengths) && distance.build(distanceLengths);
}

bool inflateZlib(std::string_view stream, std::size_t expectedSize,
                 std::vector<std::uint8_t>& output) {
    if (stream.size() < 6U || expectedSize > kMaximumInflatedBytes) return false;
    const std::uint8_t cmf = pngByte(stream, 0U);
    const std::uint8_t flags = pngByte(stream, 1U);
    if ((cmf & 0x0FU) != 8U || (cmf >> 4U) > 7U
            || ((static_cast<unsigned>(cmf) << 8U) | flags) % 31U != 0U
            || (flags & 0x20U) != 0U) return false;

    DeflateBits bits(stream.substr(2U, stream.size() - 6U));
    std::vector<std::uint8_t> decoded;
    decoded.reserve(expectedSize);
    constexpr std::array<std::uint16_t, 29> lengthBase{
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    constexpr std::array<std::uint8_t, 29> lengthExtra{
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    constexpr std::array<std::uint16_t, 30> distanceBase{
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
            193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
            6145, 8193, 12289, 16385, 24577};
    constexpr std::array<std::uint8_t, 30> distanceExtra{
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    bool finalBlock = false;
    while (!finalBlock) {
        std::uint32_t finalFlag = 0;
        std::uint32_t blockType = 0;
        if (!bits.readBits(1U, finalFlag) || !bits.readBits(2U, blockType)
                || blockType == 3U) return false;
        finalBlock = finalFlag != 0U;
        if (blockType == 0U) {
            bits.alignToByte();
            std::uint32_t length = 0;
            std::uint32_t complement = 0;
            if (!bits.readBits(16U, length) || !bits.readBits(16U, complement)
                    || (length ^ 0xFFFFU) != complement
                    || length > expectedSize - decoded.size()) return false;
            for (std::uint32_t index = 0; index < length; ++index) {
                std::uint32_t value = 0;
                if (!bits.readBits(8U, value)) return false;
                decoded.push_back(static_cast<std::uint8_t>(value));
            }
            continue;
        }

        HuffmanTable literalLength;
        HuffmanTable distance;
        const bool tablesReady = blockType == 1U
                ? buildFixedTables(literalLength, distance)
                : buildDynamicTables(bits, literalLength, distance);
        if (!tablesReady) return false;
        while (true) {
            std::uint16_t symbol = 0;
            if (!literalLength.decode(bits, symbol)) return false;
            if (symbol < 256U) {
                if (decoded.size() >= expectedSize) return false;
                decoded.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol == 256U) break;
            if (symbol < 257U || symbol > 285U) return false;
            const std::size_t lengthIndex = symbol - 257U;
            std::uint32_t extraLength = 0;
            if (!bits.readBits(lengthExtra[lengthIndex], extraLength)) return false;
            const std::size_t copyLength = lengthBase[lengthIndex] + extraLength;
            std::uint16_t distanceSymbol = 0;
            if (!distance.decode(bits, distanceSymbol) || distanceSymbol >= 30U) return false;
            std::uint32_t extraDistance = 0;
            if (!bits.readBits(distanceExtra[distanceSymbol], extraDistance)) return false;
            const std::size_t copyDistance = distanceBase[distanceSymbol] + extraDistance;
            if (copyDistance == 0U || copyDistance > decoded.size()
                    || copyLength > expectedSize - decoded.size()) return false;
            for (std::size_t index = 0; index < copyLength; ++index) {
                decoded.push_back(decoded[decoded.size() - copyDistance]);
            }
        }
    }

    bits.alignToByte();
    const std::size_t trailerOffset = 2U + bits.byteOffset();
    if (decoded.size() != expectedSize || trailerOffset > stream.size()
            || stream.size() - trailerOffset != 4U) return false;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (std::uint8_t value : decoded) {
        a = (a + value) % 65521U;
        b = (b + a) % 65521U;
    }
    const std::uint32_t adler = (b << 16U) | a;
    if (readBigEndian32(stream, trailerOffset) != adler) return false;
    output = std::move(decoded);
    return true;
}

std::uint8_t paethPredictor(std::uint8_t left, std::uint8_t above,
                            std::uint8_t upperLeft) {
    const int p = static_cast<int>(left) + above - upperLeft;
    const int leftDistance = std::abs(p - static_cast<int>(left));
    const int aboveDistance = std::abs(p - static_cast<int>(above));
    const int diagonalDistance = std::abs(p - static_cast<int>(upperLeft));
    if (leftDistance <= aboveDistance && leftDistance <= diagonalDistance) return left;
    if (aboveDistance <= diagonalDistance) return above;
    return upperLeft;
}

}  // namespace

bool decodePng(std::string_view source, ImageRgba8& image, std::string& error) {
    error.clear();
    if (source.size() > kMaximumCompressedBytes) {
        return failPng(error, "PNG file exceeds decoder size limits");
    }
    if (source.size() < kPngSignature.size()) return failPng(error, "PNG signature is truncated");
    for (std::size_t index = 0; index < kPngSignature.size(); ++index) {
        if (pngByte(source, index) != kPngSignature[index]) {
            return failPng(error, "PNG signature is invalid");
        }
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t bitDepth = 0;
    std::uint8_t colorType = 0;
    bool haveHeader = false;
    bool haveEnd = false;
    bool sawIdat = false;
    bool idatEnded = false;
    std::string compressed;
    std::vector<std::uint8_t> palette;
    std::vector<std::uint8_t> transparency;
    std::size_t offset = kPngSignature.size();
    std::size_t chunkCount = 0U;
    while (offset < source.size()) {
        if (source.size() - offset < 12U) return failPng(error, "PNG chunk header is truncated");
        const std::uint32_t chunkLength = readBigEndian32(source, offset);
        offset += 4U;
        const std::size_t chunkStart = offset;
        if (chunkLength > source.size() - offset - 8U) {
            return failPng(error, "PNG chunk length exceeds the input");
        }
        const std::string_view type = source.substr(offset, 4U);
        offset += 4U;
        if (chunkCount++ == 0U && type != "IHDR") {
            return failPng(error, "PNG must begin with an IHDR chunk");
        }
        if (!haveHeader && type != "IHDR") {
            return failPng(error, "PNG contains a chunk before IHDR");
        }
        const std::string_view data = source.substr(offset, chunkLength);
        offset += chunkLength;
        const std::uint32_t expectedCrc = readBigEndian32(source, offset);
        const std::string_view crcInput = source.substr(chunkStart, 4U + chunkLength);
        if (pngCrc32(crcInput) != expectedCrc) return failPng(error, "PNG chunk CRC is invalid");
        offset += 4U;

        if (type == "acTL" || type == "fcTL" || type == "fdAT") {
            return failPng(error, "Animated PNG is unsupported by the static image loader");
        }
        const bool isIdat = type == "IDAT";
        if (sawIdat && !isIdat) idatEnded = true;
        if (isIdat) {
            if (idatEnded || !haveHeader || chunkLength > kMaximumCompressedBytes - compressed.size()) {
                return failPng(error, "PNG IDAT chunks are invalid or exceed limits");
            }
            compressed.append(data.data(), data.size());
            sawIdat = true;
            continue;
        }
        if (type == "IHDR") {
            if (haveHeader || chunkLength != 13U || sawIdat) {
                return failPng(error, "PNG IHDR chunk is duplicated or malformed");
            }
            width = readBigEndian32(data, 0U);
            height = readBigEndian32(data, 4U);
            bitDepth = pngByte(data, 8U);
            colorType = pngByte(data, 9U);
            const std::uint8_t compression = pngByte(data, 10U);
            const std::uint8_t filter = pngByte(data, 11U);
            const std::uint8_t interlace = pngByte(data, 12U);
            const bool validDepth = colorType == 0U
                    ? bitDepth == 1U || bitDepth == 2U || bitDepth == 4U || bitDepth == 8U
                    : colorType == 2U || colorType == 4U || colorType == 6U
                            ? bitDepth == 8U
                            : colorType == 3U
                                    ? bitDepth == 1U || bitDepth == 2U
                                            || bitDepth == 4U || bitDepth == 8U
                                    : false;
            if (width == 0U || height == 0U || width > kMaximumDimension
                    || height > kMaximumDimension
                    || static_cast<std::uint64_t>(width) * height > kMaximumPixels
                    || !validDepth || compression != 0U || filter != 0U || interlace != 0U) {
                return failPng(error, "PNG dimensions, sample format or interlace mode is unsupported");
            }
            haveHeader = true;
        } else if (type == "PLTE") {
            if (!haveHeader || sawIdat || !palette.empty() || chunkLength == 0U || chunkLength > 768U
                    || chunkLength % 3U != 0U || colorType == 0U || colorType == 4U) {
                return failPng(error, "PNG palette chunk is invalid");
            }
            palette.assign(data.begin(), data.end());
        } else if (type == "tRNS") {
            if (!haveHeader || sawIdat || !transparency.empty() || chunkLength == 0U
                    || (colorType == 3U && palette.empty()) || colorType == 4U || colorType == 6U) {
                return failPng(error, "PNG transparency chunk is invalid");
            }
            transparency.assign(data.begin(), data.end());
        } else if (type == "IEND") {
            if (!haveHeader || !sawIdat || chunkLength != 0U) {
                return failPng(error, "PNG IEND chunk is invalid");
            }
            haveEnd = true;
            if (offset != source.size()) return failPng(error, "PNG contains data after IEND");
            break;
        } else if ((static_cast<unsigned char>(type[0]) & 0x20U) == 0U) {
            return failPng(error, "PNG contains an unsupported critical chunk");
        }
    }
    if (!haveHeader || !haveEnd || !sawIdat || compressed.empty()) {
        return failPng(error, "PNG is missing required chunks");
    }
    if (colorType == 3U) {
        const std::size_t paletteEntries = palette.size() / 3U;
        if (paletteEntries == 0U || paletteEntries > (1U << bitDepth)
                || transparency.size() > paletteEntries) {
            return failPng(error, "PNG indexed palette or transparency table is invalid");
        }
    } else if ((colorType == 0U && !transparency.empty() && transparency.size() != 2U)
            || (colorType == 2U && !transparency.empty() && transparency.size() != 6U)) {
        return failPng(error, "PNG transparency key has an invalid size");
    }
    if (colorType == 0U && !transparency.empty()
            && readBigEndian16(std::string_view(
                    reinterpret_cast<const char*>(transparency.data()), transparency.size()), 0U)
                    >= (1U << bitDepth)) {
        return failPng(error, "PNG grayscale transparency key exceeds the sample depth");
    }
    if (colorType == 2U && !transparency.empty()) {
        for (std::size_t component = 0; component < 3U; ++component) {
            if (readBigEndian16(std::string_view(
                    reinterpret_cast<const char*>(transparency.data()), transparency.size()),
                    component * 2U) > 255U) {
                return failPng(error, "PNG true-color transparency key exceeds 8-bit samples");
            }
        }
    }

    const std::size_t channels = colorType == 0U || colorType == 3U ? 1U
            : colorType == 2U ? 3U : colorType == 4U ? 2U : 4U;
    const std::size_t bitsPerPixel = channels * bitDepth;
    const std::size_t rowBytes = (static_cast<std::size_t>(width) * bitsPerPixel + 7U) / 8U;
    if (rowBytes == 0U || height > (kMaximumInflatedBytes / (rowBytes + 1U))) {
        return failPng(error, "PNG inflated image exceeds decoder limits");
    }
    const std::size_t inflatedSize = static_cast<std::size_t>(height) * (rowBytes + 1U);
    std::vector<std::uint8_t> inflated;
    if (!inflateZlib(compressed, inflatedSize, inflated)) {
        return failPng(error, "PNG DEFLATE stream is invalid or unsupported");
    }

    std::vector<std::uint8_t> scanlines(static_cast<std::size_t>(height) * rowBytes);
    const std::size_t filterBytesPerPixel = std::max<std::size_t>(1U, (bitsPerPixel + 7U) / 8U);
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t inputRow = y * (rowBytes + 1U);
        const std::uint8_t filter = inflated[inputRow];
        if (filter > 4U) return failPng(error, "PNG scanline uses an invalid filter");
        std::uint8_t* row = scanlines.data() + y * rowBytes;
        const std::uint8_t* previous = y == 0U ? nullptr : row - rowBytes;
        for (std::size_t x = 0; x < rowBytes; ++x) {
            const std::uint8_t raw = inflated[inputRow + 1U + x];
            const std::uint8_t left = x < filterBytesPerPixel ? 0U : row[x - filterBytesPerPixel];
            const std::uint8_t above = previous == nullptr ? 0U : previous[x];
            const std::uint8_t upperLeft = previous == nullptr || x < filterBytesPerPixel
                    ? 0U : previous[x - filterBytesPerPixel];
            std::uint8_t predictor = 0U;
            if (filter == 1U) predictor = left;
            else if (filter == 2U) predictor = above;
            else if (filter == 3U) predictor = static_cast<std::uint8_t>(
                    (static_cast<unsigned>(left) + above) / 2U);
            else if (filter == 4U) predictor = paethPredictor(left, above, upperLeft);
            row[x] = static_cast<std::uint8_t>(raw + predictor);
        }
    }

    std::vector<std::uint8_t>().swap(inflated);
    ImageRgba8 candidate;
    candidate.width = width;
    candidate.height = height;
    candidate.pixels.resize(static_cast<std::size_t>(width) * height * 4U);
    const auto sample = [&](const std::uint8_t* row, std::size_t sampleIndex) -> std::uint8_t {
        if (bitDepth == 8U) return row[sampleIndex];
        const std::size_t bit = sampleIndex * bitDepth;
        const unsigned shift = 8U - bitDepth - static_cast<unsigned>(bit & 7U);
        const unsigned value = (row[bit / 8U] >> shift) & ((1U << bitDepth) - 1U);
        return static_cast<std::uint8_t>(value * 255U / ((1U << bitDepth) - 1U));
    };
    const std::size_t paletteEntries = palette.size() / 3U;
    for (std::size_t y = 0; y < height; ++y) {
        const std::uint8_t* row = scanlines.data() + y * rowBytes;
        const std::size_t destinationY = height - 1U - y;
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t destination = (destinationY * width + x) * 4U;
            std::uint8_t red = 0U;
            std::uint8_t green = 0U;
            std::uint8_t blue = 0U;
            std::uint8_t alpha = 255U;
            if (colorType == 0U) {
                const std::uint8_t gray = sample(row, x);
                red = green = blue = gray;
                if (!transparency.empty() && bitDepth == 8U
                        && gray == readBigEndian16(std::string_view(
                                reinterpret_cast<const char*>(transparency.data()), transparency.size()), 0U)) {
                    alpha = 0U;
                } else if (!transparency.empty() && bitDepth < 8U) {
                    const std::size_t bit = x * bitDepth;
                    const unsigned shift = 8U - bitDepth - static_cast<unsigned>(bit & 7U);
                    const std::uint16_t rawGray = static_cast<std::uint16_t>(
                            (row[bit / 8U] >> shift) & ((1U << bitDepth) - 1U));
                    const std::uint16_t key = readBigEndian16(std::string_view(
                            reinterpret_cast<const char*>(transparency.data()), transparency.size()), 0U);
                    if (rawGray == key) alpha = 0U;
                }
            } else if (colorType == 2U) {
                red = row[x * 3U];
                green = row[x * 3U + 1U];
                blue = row[x * 3U + 2U];
                if (!transparency.empty() && red == readBigEndian16(std::string_view(
                            reinterpret_cast<const char*>(transparency.data()), transparency.size()), 0U)
                        && green == readBigEndian16(std::string_view(
                            reinterpret_cast<const char*>(transparency.data()), transparency.size()), 2U)
                        && blue == readBigEndian16(std::string_view(
                            reinterpret_cast<const char*>(transparency.data()), transparency.size()), 4U)) {
                    alpha = 0U;
                }
            } else if (colorType == 3U) {
                const std::size_t bit = x * bitDepth;
                const unsigned shift = 8U - bitDepth - static_cast<unsigned>(bit & 7U);
                const std::size_t paletteIndex = (row[bit / 8U] >> shift) & ((1U << bitDepth) - 1U);
                if (paletteIndex >= paletteEntries) return failPng(error, "PNG pixel references a missing palette entry");
                red = palette[paletteIndex * 3U];
                green = palette[paletteIndex * 3U + 1U];
                blue = palette[paletteIndex * 3U + 2U];
                if (paletteIndex < transparency.size()) alpha = transparency[paletteIndex];
            } else if (colorType == 4U) {
                red = green = blue = row[x * 2U];
                alpha = row[x * 2U + 1U];
            } else {
                red = row[x * 4U];
                green = row[x * 4U + 1U];
                blue = row[x * 4U + 2U];
                alpha = row[x * 4U + 3U];
            }
            candidate.pixels[destination] = red;
            candidate.pixels[destination + 1U] = green;
            candidate.pixels[destination + 2U] = blue;
            candidate.pixels[destination + 3U] = alpha;
        }
    }
    image = std::move(candidate);
    return true;
}

bool decodeImage(std::string_view source, ImageRgba8& image, std::string& error) {
    if (source.size() >= kPngSignature.size()) {
        bool isPng = true;
        for (std::size_t index = 0; index < kPngSignature.size(); ++index) {
            if (pngByte(source, index) != kPngSignature[index]) {
                isPng = false;
                break;
            }
        }
        if (isPng) return decodePng(source, image, error);
    }
    return decodeTga(source, image, error);
}

}  // namespace nuby
