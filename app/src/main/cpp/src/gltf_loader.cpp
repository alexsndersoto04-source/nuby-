#include "nuby/gltf_loader.hpp"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nuby {
namespace {
constexpr std::uint32_t kGlbMagic = 0x46546C67U;  // ASCII "glTF" in little endian.
constexpr std::uint32_t kJsonChunkType = 0x4E4F534AU;  // ASCII "JSON".
constexpr std::uint32_t kBinaryChunkType = 0x004E4942U;  // ASCII "BIN\\0".
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kChunkHeaderBytes = 8;
constexpr std::size_t kMaximumGlbBytes = 64U * 1024U * 1024U;
constexpr std::size_t kMaximumJsonBytes = 16U * 1024U * 1024U;

std::uint32_t readU32(std::string_view bytes, std::size_t offset) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return static_cast<std::uint32_t>(data[0])
            | (static_cast<std::uint32_t>(data[1]) << 8U)
            | (static_cast<std::uint32_t>(data[2]) << 16U)
            | (static_cast<std::uint32_t>(data[3]) << 24U);
}

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object } type = Type::Null;
    bool booleanValue = false;
    std::string stringValue;
    std::vector<JsonValue> arrayValue;
    std::unordered_map<std::string, JsonValue> objectValue;
};

class JsonParser final {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& root, std::string& error) {
        error_ = &error;
        if (text_.empty() || text_.size() > kMaximumJsonBytes) {
            return setError("glTF JSON is empty or exceeds the supported size");
        }
        skipWhitespace();
        if (!parseValue(root, 0)) return false;
        skipWhitespace();
        if (position_ != text_.size()) return setError("glTF JSON has trailing data");
        return true;
    }

private:
    bool setError(const char* message) {
        *error_ = message;
        return false;
    }

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char value = text_[position_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') break;
            ++position_;
        }
    }

    bool parseValue(JsonValue& output, unsigned int depth) {
        if (depth > 64U) return setError("glTF JSON nesting exceeds the supported depth");
        skipWhitespace();
        if (position_ >= text_.size()) return setError("glTF JSON ended before a value");
        const char value = text_[position_];
        if (value == '{') return parseObject(output, depth + 1U);
        if (value == '[') return parseArray(output, depth + 1U);
        if (value == '"') {
            output.type = JsonValue::Type::String;
            return parseString(output.stringValue);
        }
        if (value == 't' && consumeLiteral("true")) {
            output.type = JsonValue::Type::Boolean;
            output.booleanValue = true;
            return true;
        }
        if (value == 'f' && consumeLiteral("false")) {
            output.type = JsonValue::Type::Boolean;
            output.booleanValue = false;
            return true;
        }
        if (value == 'n' && consumeLiteral("null")) {
            output.type = JsonValue::Type::Null;
            return true;
        }
        if (value == '-' || (value >= '0' && value <= '9')) {
            return parseNumber(output);
        }
        return setError("glTF JSON contains an invalid value");
    }

    bool consumeLiteral(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) return false;
        position_ += literal.size();
        return true;
    }

    static void appendUtf8(std::uint32_t codepoint, std::string& output) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    bool parseHexQuad(std::uint32_t& result) {
        if (text_.size() - position_ < 4U) return false;
        result = 0;
        for (unsigned int index = 0; index < 4U; ++index) {
            const char value = text_[position_++];
            result <<= 4U;
            if (value >= '0' && value <= '9') result |= static_cast<std::uint32_t>(value - '0');
            else if (value >= 'a' && value <= 'f') result |= static_cast<std::uint32_t>(value - 'a' + 10);
            else if (value >= 'A' && value <= 'F') result |= static_cast<std::uint32_t>(value - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseString(std::string& output) {
        if (position_ >= text_.size() || text_[position_] != '"') return false;
        ++position_;
        output.clear();
        while (position_ < text_.size()) {
            const unsigned char value = static_cast<unsigned char>(text_[position_++]);
            if (value == '"') return true;
            if (value < 0x20U) return setError("glTF JSON string contains a control byte");
            if (value != '\\') {
                output.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= text_.size()) return setError("glTF JSON escape is truncated");
            const char escape = text_[position_++];
            switch (escape) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = 0;
                    if (!parseHexQuad(codepoint)) return setError("glTF JSON Unicode escape is invalid");
                    if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                        if (text_.size() - position_ < 6U || text_[position_] != '\\'
                                || text_[position_ + 1U] != 'u') {
                            return setError("glTF JSON high surrogate has no low surrogate");
                        }
                        position_ += 2U;
                        std::uint32_t low = 0;
                        if (!parseHexQuad(low) || low < 0xDC00U || low > 0xDFFFU) {
                            return setError("glTF JSON low surrogate is invalid");
                        }
                        codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U)
                                + (low - 0xDC00U);
                    } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                        return setError("glTF JSON contains an unpaired low surrogate");
                    }
                    appendUtf8(codepoint, output);
                    break;
                }
                default: return setError("glTF JSON contains an unknown string escape");
            }
        }
        return setError("glTF JSON string is unterminated");
    }

    bool parseNumber(JsonValue& output) {
        const std::size_t start = position_;
        if (text_[position_] == '-') ++position_;
        if (position_ >= text_.size()) return setError("glTF JSON number is truncated");
        if (text_[position_] == '0') {
            ++position_;
            if (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                return setError("glTF JSON number has a leading zero");
            }
        } else if (text_[position_] >= '1' && text_[position_] <= '9') {
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        } else {
            return setError("glTF JSON number has no integer digits");
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            const std::size_t fractionStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (fractionStart == position_) return setError("glTF JSON fraction has no digits");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const std::size_t exponentStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (exponentStart == position_) return setError("glTF JSON exponent has no digits");
        }
        output.type = JsonValue::Type::Number;
        output.stringValue.assign(text_.substr(start, position_ - start));
        return true;
    }

    bool parseArray(JsonValue& output, unsigned int depth) {
        output.type = JsonValue::Type::Array;
        ++position_;
        skipWhitespace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parseValue(item, depth)) return false;
            output.arrayValue.push_back(std::move(item));
            skipWhitespace();
            if (position_ >= text_.size()) return setError("glTF JSON array is unterminated");
            if (text_[position_] == ']') {
                ++position_;
                return true;
            }
            if (text_[position_++] != ',') return setError("glTF JSON array requires a comma");
        }
    }

    bool parseObject(JsonValue& output, unsigned int depth) {
        output.type = JsonValue::Type::Object;
        ++position_;
        skipWhitespace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return setError("glTF JSON object key is invalid");
            skipWhitespace();
            if (position_ >= text_.size() || text_[position_++] != ':') {
                return setError("glTF JSON object key requires a colon");
            }
            JsonValue value;
            if (!parseValue(value, depth)) return false;
            if (!output.objectValue.emplace(std::move(key), std::move(value)).second) {
                return setError("glTF JSON object contains a duplicate key");
            }
            skipWhitespace();
            if (position_ >= text_.size()) return setError("glTF JSON object is unterminated");
            if (text_[position_] == '}') {
                ++position_;
                return true;
            }
            if (text_[position_++] != ',') return setError("glTF JSON object requires a comma");
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::string* error_ = nullptr;
};

bool parseGltfJsonDocument(std::string_view json, JsonValue& root, std::string& error) {
    JsonParser parser(json);
    if (!parser.parse(root, error)) return false;
    if (root.type != JsonValue::Type::Object) {
        return fail(error, "glTF JSON root must be an object");
    }
    const auto asset = root.objectValue.find("asset");
    if (asset == root.objectValue.end() || asset->second.type != JsonValue::Type::Object) {
        return fail(error, "glTF JSON has no asset object");
    }
    const auto version = asset->second.objectValue.find("version");
    if (version == asset->second.objectValue.end()
            || version->second.type != JsonValue::Type::String) {
        return fail(error, "glTF asset.version must be a string");
    }
    if (version->second.stringValue != "2.0") {
        return fail(error, "Only glTF asset version 2.0 is supported");
    }
    return true;
}
}  // namespace

bool parseGlbContainer(std::string_view bytes, std::string& json,
                       std::string& binary, std::string& error) {
    json.clear();
    binary.clear();
    error.clear();
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaximumGlbBytes) {
        return fail(error, "GLB size is outside the supported limits");
    }
    if (readU32(bytes, 0) != kGlbMagic) return fail(error, "GLB magic is invalid");
    if (readU32(bytes, 4) != 2U) return fail(error, "Only glTF binary version 2 is supported");
    const std::uint32_t declaredLength = readU32(bytes, 8);
    if (declaredLength != bytes.size()) return fail(error, "GLB declared length does not match data");

    std::size_t offset = kHeaderBytes;
    bool foundJson = false;
    bool foundBinary = false;
    bool foundAdditionalChunk = false;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kChunkHeaderBytes) {
            return fail(error, "GLB contains a truncated chunk header");
        }
        const std::uint32_t chunkLength = readU32(bytes, offset);
        const std::uint32_t chunkType = readU32(bytes, offset + 4U);
        offset += kChunkHeaderBytes;
        if ((chunkLength & 3U) != 0U || chunkLength > bytes.size() - offset) {
            return fail(error, "GLB chunk length is invalid or truncated");
        }
        const std::string_view chunk = bytes.substr(offset, chunkLength);
        if (!foundJson) {
            if (chunkType != kJsonChunkType) {
                return fail(error, "GLB JSON must be the first chunk");
            }
            if (chunkLength == 0U || chunkLength > kMaximumJsonBytes) {
                return fail(error, "GLB JSON chunk is empty or exceeds the size limit");
            }
            std::size_t jsonLength = chunk.size();
            while (jsonLength > 0U && chunk[jsonLength - 1U] == ' ') --jsonLength;
            if (jsonLength == 0U) return fail(error, "GLB JSON chunk contains no document");
            for (std::size_t index = 0; index < jsonLength; ++index) {
                if (chunk[index] == '\0') return fail(error, "GLB JSON contains an embedded null byte");
            }
            json.assign(chunk.data(), jsonLength);
            foundJson = true;
        } else if (chunkType == kJsonChunkType) {
            return fail(error, "GLB contains more than one JSON chunk");
        } else if (chunkType == kBinaryChunkType) {
            if (foundBinary) return fail(error, "GLB contains more than one BIN chunk");
            if (foundAdditionalChunk) {
                return fail(error, "GLB BIN chunk must precede additional chunks");
            }
            binary.assign(chunk.data(), chunk.size());
            foundBinary = true;
        } else {
            foundAdditionalChunk = true;
        }
        offset += chunkLength;
    }
    if (!foundJson) return fail(error, "GLB has no JSON chunk");
    return true;
}

bool validateGltf2Json(std::string_view json, std::string& error) {
    error.clear();
    JsonValue root;
    return parseGltfJsonDocument(json, root, error);
}

namespace {
const JsonValue* member(const JsonValue& object, const char* name) {
    if (object.type != JsonValue::Type::Object) return nullptr;
    const auto found = object.objectValue.find(name);
    return found == object.objectValue.end() ? nullptr : &found->second;
}

bool unsignedInteger(const JsonValue* value, std::uint32_t& output) {
    if (value == nullptr || value->type != JsonValue::Type::Number
            || value->stringValue.empty()) return false;
    std::uint64_t parsed = 0;
    for (char digit : value->stringValue) {
        if (digit < '0' || digit > '9') return false;
        const std::uint64_t numericDigit = static_cast<unsigned int>(digit - '0');
        if (parsed > (std::numeric_limits<std::uint32_t>::max() - numericDigit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + numericDigit;
    }
    output = static_cast<std::uint32_t>(parsed);
    return true;
}

bool optionalUnsigned(const JsonValue& object, const char* name,
                      std::uint32_t defaultValue, std::uint32_t& output) {
    const JsonValue* value = member(object, name);
    if (value == nullptr) {
        output = defaultValue;
        return true;
    }
    return unsignedInteger(value, output);
}

struct BufferView {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t stride = 0;
};

struct Accessor {
    std::size_t viewIndex = 0;
    std::size_t offset = 0;
    std::size_t count = 0;
    std::uint32_t componentType = 0;
    std::string type;
    bool normalized = false;
};

bool parseViewsAndAccessors(const JsonValue& root, std::size_t binarySize,
                            std::vector<BufferView>& views,
                            std::vector<Accessor>& accessors, std::string& error) {
    const JsonValue* buffers = member(root, "buffers");
    if (buffers == nullptr || buffers->type != JsonValue::Type::Array
            || buffers->arrayValue.size() != 1U) {
        return fail(error, "GLB must contain exactly one embedded buffer");
    }
    std::uint32_t declaredBufferLength = 0;
    const JsonValue& buffer = buffers->arrayValue.front();
    if (member(buffer, "uri") != nullptr
            || !unsignedInteger(member(buffer, "byteLength"), declaredBufferLength)
            || declaredBufferLength > binarySize || binarySize - declaredBufferLength > 3U) {
        return fail(error, "GLB buffer declaration is invalid or does not match its BIN chunk");
    }

    const JsonValue* viewList = member(root, "bufferViews");
    if (viewList == nullptr || viewList->type != JsonValue::Type::Array
            || viewList->arrayValue.size() > 65536U) {
        return fail(error, "GLB bufferViews array is missing or exceeds limits");
    }
    views.clear();
    views.reserve(viewList->arrayValue.size());
    for (const JsonValue& source : viewList->arrayValue) {
        std::uint32_t bufferIndex = 0;
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
        std::uint32_t stride = 0;
        if (!optionalUnsigned(source, "buffer", 0, bufferIndex) || bufferIndex != 0
                || !optionalUnsigned(source, "byteOffset", 0, offset)
                || !unsignedInteger(member(source, "byteLength"), length)
                || !optionalUnsigned(source, "byteStride", 0, stride)
                || offset > declaredBufferLength || length > declaredBufferLength - offset
                || (offset & 3U) != 0U
                || (stride != 0U && (stride < 4U || stride > 252U || (stride & 3U) != 0U))
                || (member(source, "target") != nullptr
                    && !unsignedInteger(member(source, "target"), bufferIndex))) {
            return fail(error, "GLB bufferView contains invalid bounds or fields");
        }
        views.push_back({offset, length, stride});
    }

    const JsonValue* accessorList = member(root, "accessors");
    if (accessorList == nullptr || accessorList->type != JsonValue::Type::Array
            || accessorList->arrayValue.size() > 65536U) {
        return fail(error, "GLB accessors array is missing or exceeds limits");
    }
    accessors.clear();
    accessors.reserve(accessorList->arrayValue.size());
    for (const JsonValue& source : accessorList->arrayValue) {
        std::uint32_t viewIndex = 0;
        std::uint32_t byteOffset = 0;
        std::uint32_t count = 0;
        std::uint32_t componentType = 0;
        const JsonValue* type = member(source, "type");
        const JsonValue* normalized = member(source, "normalized");
        if (!unsignedInteger(member(source, "bufferView"), viewIndex)
                || viewIndex >= views.size()
                || !optionalUnsigned(source, "byteOffset", 0, byteOffset)
                || !unsignedInteger(member(source, "count"), count) || count == 0U
                || count > 4000000U
                || !unsignedInteger(member(source, "componentType"), componentType)
                || type == nullptr || type->type != JsonValue::Type::String
                || member(source, "sparse") != nullptr
                || (normalized != nullptr && normalized->type != JsonValue::Type::Boolean)) {
            return fail(error, "GLB accessor uses an unsupported or invalid layout");
        }
        Accessor accessor{};
        accessor.viewIndex = viewIndex;
        accessor.offset = byteOffset;
        accessor.count = count;
        accessor.componentType = componentType;
        accessor.type = type->stringValue;
        accessor.normalized = normalized != nullptr && normalized->booleanValue;
        accessors.push_back(std::move(accessor));
    }
    return true;
}

bool accessorSpan(const Accessor& accessor, const std::vector<BufferView>& views,
                  std::size_t componentBytes, std::size_t componentCount,
                  std::string_view binary, std::size_t& base, std::size_t& stride) {
    if (accessor.viewIndex >= views.size()
            || componentBytes > std::numeric_limits<std::size_t>::max() / componentCount) return false;
    const std::size_t elementBytes = componentBytes * componentCount;
    const BufferView& view = views[accessor.viewIndex];
    stride = view.stride == 0U ? elementBytes : view.stride;
    if (accessor.offset > view.length || elementBytes > view.length - accessor.offset
            || view.offset > std::numeric_limits<std::size_t>::max() - accessor.offset
            || stride < elementBytes || stride % componentBytes != 0U
            || (view.offset + accessor.offset) % componentBytes != 0U) return false;
    const std::size_t remaining = view.length - accessor.offset - elementBytes;
    if (accessor.count - 1U > remaining / stride) return false;
    base = view.offset + accessor.offset;
    return base <= binary.size() && (accessor.count - 1U) <= (binary.size() - base - elementBytes) / stride;
}

float readFloat32(std::string_view bytes, std::size_t offset) {
    const std::uint32_t bits = readU32(bytes, offset);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits), "float32 is required");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool readFloatAccessor(const Accessor& accessor, const std::vector<BufferView>& views,
                       std::string_view binary, const char* expectedType,
                       std::size_t components, std::vector<float>& output) {
    if (accessor.type != expectedType || accessor.componentType != 5126U
            || accessor.normalized || accessor.count > 4000000U
            || accessor.count > std::numeric_limits<std::size_t>::max() / components) return false;
    std::size_t base = 0;
    std::size_t stride = 0;
    if (!accessorSpan(accessor, views, sizeof(float), components, binary, base, stride)) return false;
    output.resize(accessor.count * components);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        for (std::size_t component = 0; component < components; ++component) {
            const float value = readFloat32(binary, base + index * stride + component * sizeof(float));
            if (!std::isfinite(value)) return false;
            output[index * components + component] = value;
        }
    }
    return true;
}

bool readJointAccessor(const Accessor& accessor, const std::vector<BufferView>& views,
                       std::string_view binary,
                       std::vector<std::array<std::uint16_t, 4>>& output) {
    if (accessor.type != "VEC4" || accessor.normalized
            || (accessor.componentType != 5121U && accessor.componentType != 5123U)) return false;
    const std::size_t componentBytes = accessor.componentType == 5121U ? 1U : 2U;
    std::size_t base = 0;
    std::size_t stride = 0;
    if (!accessorSpan(accessor, views, componentBytes, 4U, binary, base, stride)) return false;
    output.resize(accessor.count);
    for (std::size_t vertex = 0; vertex < accessor.count; ++vertex) {
        for (std::size_t joint = 0; joint < 4U; ++joint) {
            const std::size_t offset = base + vertex * stride + joint * componentBytes;
            const std::uint32_t value = componentBytes == 1U
                    ? static_cast<unsigned char>(binary[offset])
                    : static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset]))
                            | (static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset + 1U])) << 8U);
            output[vertex][joint] = static_cast<std::uint16_t>(value);
        }
    }
    return true;
}

bool readWeightAccessor(const Accessor& accessor, const std::vector<BufferView>& views,
                        std::string_view binary,
                        std::vector<std::array<float, 4>>& output) {
    output.resize(accessor.count);
    if (accessor.componentType == 5126U) {
        std::vector<float> values;
        if (!readFloatAccessor(accessor, views, binary, "VEC4", 4U, values)) return false;
        for (std::size_t vertex = 0; vertex < accessor.count; ++vertex) {
            for (std::size_t influence = 0; influence < 4U; ++influence) {
                output[vertex][influence] = values[vertex * 4U + influence];
            }
        }
        return true;
    }
    if (accessor.type != "VEC4" || !accessor.normalized
            || (accessor.componentType != 5121U && accessor.componentType != 5123U)) return false;
    const std::size_t componentBytes = accessor.componentType == 5121U ? 1U : 2U;
    const float maximum = accessor.componentType == 5121U ? 255.0f : 65535.0f;
    std::size_t base = 0;
    std::size_t stride = 0;
    if (!accessorSpan(accessor, views, componentBytes, 4U, binary, base, stride)) return false;
    for (std::size_t vertex = 0; vertex < accessor.count; ++vertex) {
        for (std::size_t influence = 0; influence < 4U; ++influence) {
            const std::size_t offset = base + vertex * stride + influence * componentBytes;
            const std::uint32_t value = componentBytes == 1U
                    ? static_cast<unsigned char>(binary[offset])
                    : static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset]))
                            | (static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset + 1U])) << 8U);
            output[vertex][influence] = static_cast<float>(value) / maximum;
        }
    }
    return true;
}

bool readIndexAccessor(const Accessor& accessor, const std::vector<BufferView>& views,
                       std::string_view binary, std::vector<std::uint32_t>& output) {
    if (accessor.type != "SCALAR" || accessor.normalized
            || (accessor.componentType != 5121U && accessor.componentType != 5123U
                && accessor.componentType != 5125U)) return false;
    const std::size_t componentBytes = accessor.componentType == 5121U ? 1U
            : accessor.componentType == 5123U ? 2U : 4U;
    std::size_t base = 0;
    std::size_t stride = 0;
    if (!accessorSpan(accessor, views, componentBytes, 1U, binary, base, stride)) return false;
    output.resize(accessor.count);
    for (std::size_t index = 0; index < accessor.count; ++index) {
        const std::size_t offset = base + index * stride;
        output[index] = componentBytes == 1U
                ? static_cast<unsigned char>(binary[offset])
                : componentBytes == 2U
                        ? static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset]))
                                | (static_cast<std::uint32_t>(static_cast<unsigned char>(binary[offset + 1U])) << 8U)
                        : readU32(binary, offset);
    }
    return true;
}

bool jsonFloat(const JsonValue* value, float& output) {
    if (value == nullptr || value->type != JsonValue::Type::Number) return false;
    char* end = nullptr;
    errno = 0;
    const float parsed = std::strtof(value->stringValue.c_str(), &end);
    if (errno == ERANGE || end == value->stringValue.c_str()
            || end != value->stringValue.c_str() + value->stringValue.size()
            || !std::isfinite(parsed)) return false;
    output = parsed;
    return true;
}

template <std::size_t N>
bool jsonFloatArray(const JsonValue* value, std::array<float, N>& output) {
    if (value == nullptr || value->type != JsonValue::Type::Array
            || value->arrayValue.size() != N) return false;
    for (std::size_t index = 0; index < N; ++index) {
        if (!jsonFloat(&value->arrayValue[index], output[index])) return false;
    }
    return true;
}

bool parseGltfMaterials(const JsonValue& root, std::vector<GltfMaterial>& output,
                        std::string& error) {
    output.clear();
    const JsonValue* materials = member(root, "materials");
    if (materials == nullptr) return true;
    if (materials->type != JsonValue::Type::Array || materials->arrayValue.size() > 4096U) {
        return fail(error, "GLB materials array is invalid or exceeds limits");
    }
    output.reserve(materials->arrayValue.size());
    for (const JsonValue& source : materials->arrayValue) {
        if (source.type != JsonValue::Type::Object) {
            return fail(error, "GLB material must be an object");
        }
        for (const char* textureSlot : {"normalTexture", "occlusionTexture", "emissiveTexture"}) {
            if (member(source, textureSlot) != nullptr) {
                return fail(error, "Textured glTF materials are not supported by this renderer");
            }
        }
        GltfMaterial material;
        const JsonValue* pbr = member(source, "pbrMetallicRoughness");
        if (pbr != nullptr && pbr->type != JsonValue::Type::Object) {
            return fail(error, "GLB pbrMetallicRoughness must be an object");
        }
        if (pbr != nullptr) {
            if (member(*pbr, "baseColorTexture") != nullptr
                    || member(*pbr, "metallicRoughnessTexture") != nullptr) {
                return fail(error, "Textured glTF materials are not supported by this renderer");
            }
            const JsonValue* baseColor = member(*pbr, "baseColorFactor");
            if (baseColor != nullptr && !jsonFloatArray(baseColor, material.baseColorFactor)) {
                return fail(error, "GLB baseColorFactor must contain four finite values");
            }
            const JsonValue* metallic = member(*pbr, "metallicFactor");
            const JsonValue* roughness = member(*pbr, "roughnessFactor");
            if ((metallic != nullptr && !jsonFloat(metallic, material.metallicFactor))
                    || (roughness != nullptr
                        && !jsonFloat(roughness, material.roughnessFactor))) {
                return fail(error, "GLB metallic or roughness factor is invalid");
            }
        }
        const JsonValue* emissive = member(source, "emissiveFactor");
        if (emissive != nullptr && !jsonFloatArray(emissive, material.emissiveFactor)) {
            return fail(error, "GLB emissiveFactor must contain three finite values");
        }
        for (float value : material.baseColorFactor) {
            if (value < 0.0f || value > 1.0f) {
                return fail(error, "GLB baseColorFactor components must be in [0, 1]");
            }
        }
        if (material.metallicFactor < 0.0f || material.metallicFactor > 1.0f
                || material.roughnessFactor < 0.0f || material.roughnessFactor > 1.0f) {
            return fail(error, "GLB metallic and roughness factors must be in [0, 1]");
        }
        for (float value : material.emissiveFactor) {
            if (value < 0.0f) return fail(error, "GLB emissiveFactor cannot be negative");
        }
        const JsonValue* alphaMode = member(source, "alphaMode");
        if (alphaMode != nullptr) {
            if (alphaMode->type != JsonValue::Type::String) {
                return fail(error, "GLB alphaMode must be a string");
            }
            if (alphaMode->stringValue == "BLEND") material.alphaBlend = true;
            else if (alphaMode->stringValue == "MASK") {
                return fail(error, "GLB MASK alpha mode is unsupported without alpha-cutout rendering");
            } else if (alphaMode->stringValue != "OPAQUE") {
                return fail(error, "GLB alphaMode is unknown");
            }
        }
        output.push_back(material);
    }
    return true;
}

struct NodeData {
    std::string name;
    std::int32_t parentIndex = -1;
    bool matrixAuthored = false;
    std::array<float, 3> translation{0.0f, 0.0f, 0.0f};
    SkeletalQuaternion rotation;
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
};

bool parseNodeData(const JsonValue& root, std::vector<NodeData>& nodes,
                   std::string& error) {
    const JsonValue* sourceNodes = member(root, "nodes");
    if (sourceNodes == nullptr || sourceNodes->type != JsonValue::Type::Array
            || sourceNodes->arrayValue.empty() || sourceNodes->arrayValue.size() > 65536U) {
        return fail(error, "GLB nodes array is missing or exceeds limits");
    }
    nodes.assign(sourceNodes->arrayValue.size(), NodeData{});
    for (std::size_t index = 0; index < sourceNodes->arrayValue.size(); ++index) {
        const JsonValue& source = sourceNodes->arrayValue[index];
        NodeData& node = nodes[index];
        node.matrixAuthored = member(source, "matrix") != nullptr;
        if (const JsonValue* name = member(source, "name"); name != nullptr) {
            if (name->type != JsonValue::Type::String || name->stringValue.size() > 1024U) {
                return fail(error, "GLB node name is invalid");
            }
            node.name = name->stringValue;
        }
        if (member(source, "translation") != nullptr
                && !jsonFloatArray(member(source, "translation"), node.translation)) {
            return fail(error, "GLB node translation is invalid");
        }
        std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
        if (member(source, "rotation") != nullptr
                && !jsonFloatArray(member(source, "rotation"), rotation)) {
            return fail(error, "GLB node rotation is invalid");
        }
        node.rotation = {rotation[0], rotation[1], rotation[2], rotation[3]};
        if (member(source, "scale") != nullptr
                && !jsonFloatArray(member(source, "scale"), node.scale)) {
            return fail(error, "GLB node scale is invalid");
        }
        const JsonValue* children = member(source, "children");
        if (children == nullptr) continue;
        if (children->type != JsonValue::Type::Array) {
            return fail(error, "GLB node children must be an array");
        }
        for (const JsonValue& childValue : children->arrayValue) {
            std::uint32_t child = 0;
            if (!unsignedInteger(&childValue, child) || child >= nodes.size()
                    || child == index || nodes[child].parentIndex != -1) {
                return fail(error, "GLB node hierarchy contains an invalid or repeated parent");
            }
            nodes[child].parentIndex = static_cast<std::int32_t>(index);
        }
    }
    return true;
}

bool nodeHasIdentityTransform(const NodeData& node) {
    constexpr float epsilon = 1.0e-5f;
    return std::fabs(node.translation[0]) <= epsilon
            && std::fabs(node.translation[1]) <= epsilon
            && std::fabs(node.translation[2]) <= epsilon
            && std::fabs(node.rotation.x) <= epsilon
            && std::fabs(node.rotation.y) <= epsilon
            && std::fabs(node.rotation.z) <= epsilon
            && std::fabs(node.rotation.w - 1.0f) <= epsilon
            && std::fabs(node.scale[0] - 1.0f) <= epsilon
            && std::fabs(node.scale[1] - 1.0f) <= epsilon
            && std::fabs(node.scale[2] - 1.0f) <= epsilon;
}
}  // namespace

bool parseGlbMeshes(std::string_view bytes, std::vector<GltfPrimitive>& output,
                    std::string& error) {
    output.clear();
    std::string json;
    std::string binary;
    if (!parseGlbContainer(bytes, json, binary, error)) return false;
    JsonValue root;
    if (!parseGltfJsonDocument(json, root, error)) return false;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;
    if (!parseViewsAndAccessors(root, binary.size(), views, accessors, error)) return false;
    std::vector<GltfMaterial> materials;
    if (!parseGltfMaterials(root, materials, error)) return false;

    const JsonValue* meshes = member(root, "meshes");
    if (meshes == nullptr || meshes->type != JsonValue::Type::Array
            || meshes->arrayValue.empty() || meshes->arrayValue.size() > 4096U) {
        return fail(error, "GLB meshes array is missing or exceeds limits");
    }
    std::vector<GltfPrimitive> candidate;
    std::size_t totalVertices = 0;
    std::size_t totalIndices = 0;
    for (const JsonValue& mesh : meshes->arrayValue) {
        const JsonValue* primitives = member(mesh, "primitives");
        if (primitives == nullptr || primitives->type != JsonValue::Type::Array
                || primitives->arrayValue.empty()) {
            return fail(error, "GLB mesh has no primitives");
        }
        for (const JsonValue& primitive : primitives->arrayValue) {
            std::uint32_t mode = 4U;
            if (!optionalUnsigned(primitive, "mode", 4U, mode) || mode != 4U) {
                return fail(error, "Only glTF triangle primitives are supported");
            }
            const JsonValue* attributes = member(primitive, "attributes");
            if (attributes == nullptr || attributes->type != JsonValue::Type::Object) {
                return fail(error, "GLB primitive has no attributes object");
            }
            const JsonValue* positionIndex = member(*attributes, "POSITION");
            std::uint32_t positionAccessorIndex = 0;
            if (!unsignedInteger(positionIndex, positionAccessorIndex)
                    || positionAccessorIndex >= accessors.size()) {
                return fail(error, "GLB primitive has no valid POSITION accessor");
            }
            const Accessor& positionAccessor = accessors[positionAccessorIndex];
            GltfPrimitive parsed{};
            const JsonValue* materialIndexValue = member(primitive, "material");
            if (materialIndexValue != nullptr) {
                std::uint32_t materialIndex = 0;
                if (!unsignedInteger(materialIndexValue, materialIndex)
                        || materialIndex >= materials.size()) {
                    return fail(error, "GLB primitive references an invalid material");
                }
                parsed.material = materials[materialIndex];
                parsed.hasMaterial = true;
            }
            std::vector<float> positions;
            if (!readFloatAccessor(positionAccessor, views, binary, "VEC3", 3U, positions)) {
                return fail(error, "GLB POSITION accessor has an unsupported layout");
            }
            const std::size_t vertexCount = positionAccessor.count;
            if (vertexCount > 4000000U - totalVertices) {
                return fail(error, "GLB mesh vertex total exceeds limits");
            }
            parsed.vertices.resize(vertexCount);
            for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    parsed.vertices[vertex].position[axis] = positions[vertex * 3U + axis];
                }
            }
            const auto readOptionalAttribute = [&](const char* name, const char* type,
                                                   std::size_t components,
                                                   bool normals) -> bool {
                const JsonValue* value = member(*attributes, name);
                if (value == nullptr) return true;
                std::uint32_t accessorIndex = 0;
                if (!unsignedInteger(value, accessorIndex) || accessorIndex >= accessors.size()
                        || accessors[accessorIndex].count != vertexCount) return false;
                std::vector<float> values;
                if (!readFloatAccessor(accessors[accessorIndex], views, binary,
                                       type, components, values)) return false;
                for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                    if (normals) {
                        for (std::size_t axis = 0; axis < 3; ++axis) {
                            parsed.vertices[vertex].normal[axis] = values[vertex * components + axis];
                        }
                    } else {
                        parsed.vertices[vertex].uv[0] = values[vertex * components];
                        parsed.vertices[vertex].uv[1] = values[vertex * components + 1U];
                    }
                }
                return true;
            };
            const bool hasNormals = member(*attributes, "NORMAL") != nullptr;
            if (!readOptionalAttribute("NORMAL", "VEC3", 3U, true)
                    || !readOptionalAttribute("TEXCOORD_0", "VEC2", 2U, false)) {
                return fail(error, "GLB NORMAL or TEXCOORD_0 accessor is invalid");
            }
            const JsonValue* jointAttribute = member(*attributes, "JOINTS_0");
            const JsonValue* weightAttribute = member(*attributes, "WEIGHTS_0");
            bool unsupportedSkinSet = false;
            for (const auto& attribute : attributes->objectValue) {
                unsupportedSkinSet = unsupportedSkinSet
                        || (attribute.first.rfind("JOINTS_", 0) == 0
                            && attribute.first != "JOINTS_0")
                        || (attribute.first.rfind("WEIGHTS_", 0) == 0
                            && attribute.first != "WEIGHTS_0");
            }
            if (unsupportedSkinSet || ((jointAttribute == nullptr) != (weightAttribute == nullptr))) {
                return fail(error, "GLB skin attributes must provide one JOINTS_0/WEIGHTS_0 pair");
            }
            if (jointAttribute != nullptr) {
                std::uint32_t jointAccessorIndex = 0;
                std::uint32_t weightAccessorIndex = 0;
                if (!unsignedInteger(jointAttribute, jointAccessorIndex)
                        || jointAccessorIndex >= accessors.size()
                        || !unsignedInteger(weightAttribute, weightAccessorIndex)
                        || weightAccessorIndex >= accessors.size()
                        || accessors[jointAccessorIndex].count != vertexCount
                        || accessors[weightAccessorIndex].count != vertexCount) {
                    return fail(error, "GLB skin accessors do not match the vertex count");
                }
                std::vector<std::array<std::uint16_t, 4>> joints;
                std::vector<std::array<float, 4>> weights;
                if (!readJointAccessor(accessors[jointAccessorIndex], views, binary, joints)
                        || !readWeightAccessor(accessors[weightAccessorIndex], views, binary, weights)) {
                    return fail(error, "GLB skin accessors use unsupported component types");
                }
                parsed.influences.resize(vertexCount);
                parsed.hasSkinAttributes = true;
                for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
                    float sum = 0.0f;
                    for (std::size_t influence = 0; influence < 4U; ++influence) {
                        const float weight = weights[vertex][influence];
                        if (!std::isfinite(weight) || weight < 0.0f || weight > 1000000.0f) {
                            return fail(error, "GLB vertex skin weight is invalid");
                        }
                        sum += weight;
                        parsed.influences[vertex].jointIndices[influence] = joints[vertex][influence];
                        parsed.influences[vertex].weights[influence] = weight;
                    }
                    if (!std::isfinite(sum) || sum <= 1.0e-8f) {
                        return fail(error, "GLB vertex skin weights have zero total");
                    }
                    for (float& weight : parsed.influences[vertex].weights) weight /= sum;
                }
            }
            const JsonValue* indices = member(primitive, "indices");
            if (indices != nullptr) {
                std::uint32_t accessorIndex = 0;
                if (!unsignedInteger(indices, accessorIndex) || accessorIndex >= accessors.size()
                        || !readIndexAccessor(accessors[accessorIndex], views, binary,
                                              parsed.indices)) {
                    return fail(error, "GLB primitive indices use an unsupported layout");
                }
            } else {
                parsed.indices.resize(vertexCount);
                for (std::size_t index = 0; index < vertexCount; ++index) {
                    parsed.indices[index] = static_cast<std::uint32_t>(index);
                }
            }
            if (parsed.indices.empty() || parsed.indices.size() % 3U != 0U) {
                return fail(error, "GLB triangle primitive has an incomplete index triplet");
            }
            if (candidate.size() >= 8192U || parsed.indices.size() > 12000000U - totalIndices) {
                return fail(error, "GLB primitive or index total exceeds limits");
            }
            for (std::uint32_t index : parsed.indices) {
                if (index >= vertexCount) return fail(error, "GLB primitive index exceeds its vertex count");
            }
            if (!hasNormals) {
                for (std::size_t triangle = 0; triangle < parsed.indices.size(); triangle += 3U) {
                    const std::uint32_t ia = parsed.indices[triangle];
                    const std::uint32_t ib = parsed.indices[triangle + 1U];
                    const std::uint32_t ic = parsed.indices[triangle + 2U];
                    const MeshVertex& a = parsed.vertices[ia];
                    const MeshVertex& b = parsed.vertices[ib];
                    const MeshVertex& c = parsed.vertices[ic];
                    const float abx = b.position[0] - a.position[0];
                    const float aby = b.position[1] - a.position[1];
                    const float abz = b.position[2] - a.position[2];
                    const float acx = c.position[0] - a.position[0];
                    const float acy = c.position[1] - a.position[1];
                    const float acz = c.position[2] - a.position[2];
                    const std::array<float, 3> faceNormal{
                        aby * acz - abz * acy,
                        abz * acx - abx * acz,
                        abx * acy - aby * acx};
                    for (std::uint32_t vertex : {ia, ib, ic}) {
                        for (std::size_t axis = 0; axis < 3U; ++axis) {
                            parsed.vertices[vertex].normal[axis] += faceNormal[axis];
                        }
                    }
                }
                for (MeshVertex& vertex : parsed.vertices) {
                    const float lengthSquared = vertex.normal[0] * vertex.normal[0]
                            + vertex.normal[1] * vertex.normal[1]
                            + vertex.normal[2] * vertex.normal[2];
                    if (std::isfinite(lengthSquared) && lengthSquared > 1.0e-20f) {
                        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
                        for (float& component : vertex.normal) component *= inverseLength;
                    } else {
                        vertex.normal[0] = 0.0f;
                        vertex.normal[1] = 1.0f;
                        vertex.normal[2] = 0.0f;
                    }
                }
            }
            totalVertices += vertexCount;
            totalIndices += parsed.indices.size();
            candidate.push_back(std::move(parsed));
        }
    }
    if (candidate.empty()) return fail(error, "GLB contains no supported mesh primitives");
    output = std::move(candidate);
    error.clear();
    return true;
}

bool parseGlbSkeletalAsset(std::string_view bytes, GltfSkeletalAsset& output,
                           std::string& error) {
    output = {};
    std::string json;
    std::string binary;
    if (!parseGlbContainer(bytes, json, binary, error)) return false;
    JsonValue root;
    if (!parseGltfJsonDocument(json, root, error)) return false;
    std::vector<BufferView> views;
    std::vector<Accessor> accessors;
    if (!parseViewsAndAccessors(root, binary.size(), views, accessors, error)) return false;
    std::vector<NodeData> nodes;
    if (!parseNodeData(root, nodes, error)) return false;

    const JsonValue* skins = member(root, "skins");
    const JsonValue* meshes = member(root, "meshes");
    if (skins == nullptr || skins->type != JsonValue::Type::Array || skins->arrayValue.size() != 1U
            || meshes == nullptr || meshes->type != JsonValue::Type::Array
            || meshes->arrayValue.size() != 1U) {
        return fail(error, "Skeletal GLB requires exactly one skin and one mesh");
    }
    const JsonValue* skinJoints = member(skins->arrayValue[0], "joints");
    if (skinJoints == nullptr || skinJoints->type != JsonValue::Type::Array
            || skinJoints->arrayValue.empty() || skinJoints->arrayValue.size() > 64U) {
        return fail(error, "GLB skin joint list is empty or exceeds the 64-joint limit");
    }

    std::vector<std::uint32_t> skinNodes;
    skinNodes.reserve(skinJoints->arrayValue.size());
    for (const JsonValue& value : skinJoints->arrayValue) {
        std::uint32_t nodeIndex = 0;
        if (!unsignedInteger(&value, nodeIndex) || nodeIndex >= nodes.size()
                || std::find(skinNodes.begin(), skinNodes.end(), nodeIndex) != skinNodes.end()) {
            return fail(error, "GLB skin contains an invalid or duplicate joint node");
        }
        skinNodes.push_back(nodeIndex);
    }

    std::uint32_t meshNodeIndex = 0;
    std::size_t matchingMeshNodes = 0;
    const JsonValue* nodeValues = member(root, "nodes");
    for (std::size_t index = 0; index < nodeValues->arrayValue.size(); ++index) {
        const JsonValue& source = nodeValues->arrayValue[index];
        const JsonValue* meshIndexValue = member(source, "mesh");
        const JsonValue* skinIndexValue = member(source, "skin");
        if (meshIndexValue == nullptr && skinIndexValue == nullptr) continue;
        std::uint32_t meshIndex = 0;
        std::uint32_t skinIndex = 0;
        if (meshIndexValue == nullptr || skinIndexValue == nullptr
                || !unsignedInteger(meshIndexValue, meshIndex) || meshIndex != 0
                || !unsignedInteger(skinIndexValue, skinIndex) || skinIndex != 0) {
            return fail(error, "Skeletal GLB only supports one mesh node using skin zero");
        }
        ++matchingMeshNodes;
        meshNodeIndex = static_cast<std::uint32_t>(index);
    }
    if (matchingMeshNodes != 1U || nodes[meshNodeIndex].parentIndex != -1
            || nodes[meshNodeIndex].matrixAuthored
            || !nodeHasIdentityTransform(nodes[meshNodeIndex])) {
        return fail(error, "Skeletal mesh node must be a single identity root node");
    }

    std::vector<std::int32_t> skinIndexByNodeIndex(nodes.size(), -1);
    for (std::size_t skinIndex = 0; skinIndex < skinNodes.size(); ++skinIndex) {
        skinIndexByNodeIndex[skinNodes[skinIndex]] = static_cast<std::int32_t>(skinIndex);
        if (nodes[skinNodes[skinIndex]].matrixAuthored) {
            return fail(error, "Matrix-authored skin joints are not supported");
        }
    }
    // Reject intermediary nodes after constructing the complete node-to-skin map.
    for (std::uint32_t nodeIndex : skinNodes) {
        const std::int32_t parent = nodes[nodeIndex].parentIndex;
        if (parent >= 0 && skinIndexByNodeIndex[static_cast<std::size_t>(parent)] == -1) {
            return fail(error, "Non-joint intermediary nodes in a skin hierarchy are unsupported");
        }
    }

    std::vector<std::int32_t> parentFirstNodes;
    std::vector<std::uint8_t> visitState(nodes.size(), 0U);
    std::function<bool(std::uint32_t)> visitJoint = [&](std::uint32_t nodeIndex) {
        std::uint8_t& state = visitState[nodeIndex];
        if (state == 1U) return false;
        if (state == 2U) return true;
        state = 1U;
        const std::int32_t parent = nodes[nodeIndex].parentIndex;
        if (parent >= 0 && !visitJoint(static_cast<std::uint32_t>(parent))) return false;
        state = 2U;
        parentFirstNodes.push_back(nodeIndex);
        return true;
    };
    for (std::uint32_t nodeIndex : skinNodes) {
        if (!visitJoint(nodeIndex)) return fail(error, "GLB skin hierarchy contains a cycle");
    }

    GltfSkeletalAsset candidate;
    candidate.skeleton.reserve(parentFirstNodes.size());
    std::vector<std::int32_t> jointIndexByNode(nodes.size(), -1);
    for (std::uint32_t nodeIndex : parentFirstNodes) {
        const NodeData& node = nodes[nodeIndex];
        SkeletalJoint joint{};
        joint.name = node.name.empty() ? "joint_" + std::to_string(nodeIndex) : node.name;
        joint.bindTranslation = node.translation;
        joint.bindRotation = node.rotation;
        joint.bindScale = node.scale;
        if (node.parentIndex >= 0) {
            joint.parentIndex = jointIndexByNode[static_cast<std::size_t>(node.parentIndex)];
            if (joint.parentIndex < 0) return fail(error, "GLB joint order is not parent-first");
        }
        jointIndexByNode[nodeIndex] = static_cast<std::int32_t>(candidate.skeleton.size());
        candidate.skeleton.push_back(std::move(joint));
    }

    const JsonValue* inverseBindValue = member(skins->arrayValue[0], "inverseBindMatrices");
    if (inverseBindValue != nullptr) {
        std::uint32_t accessorIndex = 0;
        if (!unsignedInteger(inverseBindValue, accessorIndex) || accessorIndex >= accessors.size()
                || accessors[accessorIndex].count != skinNodes.size()) {
            return fail(error, "GLB inverseBindMatrices accessor is invalid");
        }
        std::vector<float> matrices;
        if (!readFloatAccessor(accessors[accessorIndex], views, binary, "MAT4", 16U, matrices)) {
            return fail(error, "GLB inverseBindMatrices must be a float MAT4 accessor");
        }
        for (std::size_t skinIndex = 0; skinIndex < skinNodes.size(); ++skinIndex) {
            const std::size_t jointIndex = static_cast<std::size_t>(
                    jointIndexByNode[skinNodes[skinIndex]]);
            std::copy_n(matrices.begin() + static_cast<std::ptrdiff_t>(skinIndex * 16U), 16U,
                        candidate.skeleton[jointIndex].inverseBindMatrix.begin());
        }
    }

    std::string skeletonError;
    SkeletalAnimationClip bindPoseValidation{};
    bindPoseValidation.joints.resize(candidate.skeleton.size());
    if (!validateSkeletalAnimation(candidate.skeleton, bindPoseValidation, skeletonError)) {
        return fail(error, skeletonError.c_str());
    }

    if (!parseGlbMeshes(bytes, candidate.primitives, error)) return false;
    if (candidate.primitives.empty()) return fail(error, "Skeletal GLB contains no primitives");
    for (GltfPrimitive& primitive : candidate.primitives) {
        if (!primitive.hasSkinAttributes || primitive.influences.size() != primitive.vertices.size()) {
            return fail(error, "Every skeletal GLB primitive must provide JOINTS_0 and WEIGHTS_0");
        }
        for (SkeletalVertexInfluence& influence : primitive.influences) {
            for (std::size_t slot = 0; slot < influence.jointIndices.size(); ++slot) {
                const std::uint16_t skinIndex = influence.jointIndices[slot];
                if (skinIndex >= skinNodes.size()) {
                    return fail(error, "GLB vertex references a joint outside the skin");
                }
                influence.jointIndices[slot] = static_cast<std::uint16_t>(
                        jointIndexByNode[skinNodes[skinIndex]]);
            }
        }
    }

    const JsonValue* animations = member(root, "animations");
    if (animations != nullptr) {
        if (animations->type != JsonValue::Type::Array || animations->arrayValue.size() > 4096U) {
            return fail(error, "GLB animations array exceeds limits");
        }
        std::unordered_map<std::string, std::size_t> animationNameCounts;
        for (std::size_t animationNumber = 0; animationNumber < animations->arrayValue.size();
             ++animationNumber) {
            const JsonValue& sourceAnimation = animations->arrayValue[animationNumber];
            const JsonValue* samplers = member(sourceAnimation, "samplers");
            const JsonValue* channels = member(sourceAnimation, "channels");
            if (samplers == nullptr || samplers->type != JsonValue::Type::Array
                    || channels == nullptr || channels->type != JsonValue::Type::Array
                    || samplers->arrayValue.empty() || channels->arrayValue.size() > 65536U) {
                return fail(error, "GLB animation samplers or channels are invalid");
            }
            GltfAnimation animation{};
            const JsonValue* name = member(sourceAnimation, "name");
            if (name != nullptr && name->type != JsonValue::Type::String) {
                return fail(error, "GLB animation name is invalid");
            }
            animation.name = name == nullptr || name->stringValue.empty()
                    ? "Animation_" + std::to_string(animationNumber) : name->stringValue;
            if (animation.name.size() > 256U) {
                return fail(error, "GLB animation name exceeds the scene clip-name limit");
            }
            if (++animationNameCounts[animation.name] > 1U) {
                return fail(error, "GLB animation names must be unique for clip selection");
            }
            animation.clip.joints.resize(candidate.skeleton.size());
            bool hasJointChannel = false;
            for (const JsonValue& channel : channels->arrayValue) {
                const JsonValue* samplerValue = member(channel, "sampler");
                const JsonValue* target = member(channel, "target");
                std::uint32_t samplerIndex = 0;
                std::uint32_t targetNode = 0;
                if (target == nullptr || target->type != JsonValue::Type::Object) {
                    return fail(error, "GLB animation channel has no target object");
                }
                const JsonValue* targetNodeValue = member(*target, "node");
                const JsonValue* path = member(*target, "path");
                if (!unsignedInteger(samplerValue, samplerIndex)
                        || samplerIndex >= samplers->arrayValue.size()
                        || !unsignedInteger(targetNodeValue, targetNode)
                        || targetNode >= nodes.size()
                        || path == nullptr || path->type != JsonValue::Type::String) {
                    return fail(error, "GLB animation channel target or sampler is invalid");
                }
                const std::int32_t jointIndex = jointIndexByNode[targetNode];
                if (jointIndex < 0) continue;
                const std::string& pathName = path->stringValue;
                SkeletalAnimationTrack& track = animation.clip.joints[
                        static_cast<std::size_t>(jointIndex)];
                SkeletalVectorTrack* vectorTrack = nullptr;
                SkeletalQuaternionTrack* quaternionTrack = nullptr;
                const char* outputType = nullptr;
                std::size_t outputComponents = 0;
                if (pathName == "translation") {
                    vectorTrack = &track.translation;
                    outputType = "VEC3";
                    outputComponents = 3U;
                } else if (pathName == "scale") {
                    vectorTrack = &track.scale;
                    outputType = "VEC3";
                    outputComponents = 3U;
                } else if (pathName == "rotation") {
                    quaternionTrack = &track.rotation;
                    outputType = "VEC4";
                    outputComponents = 4U;
                } else {
                    return fail(error, "GLB skeletal animation path is unsupported");
                }
                if ((vectorTrack != nullptr && !vectorTrack->keyframes.empty())
                        || (quaternionTrack != nullptr && !quaternionTrack->keyframes.empty())) {
                    return fail(error, "GLB animation has duplicate channels for one joint path");
                }
                const JsonValue& sampler = samplers->arrayValue[samplerIndex];
                const JsonValue* interpolation = member(sampler, "interpolation");
                if (interpolation != nullptr
                        && (interpolation->type != JsonValue::Type::String
                            || interpolation->stringValue != "LINEAR")) {
                    return fail(error, "Only LINEAR glTF skeletal animation is supported");
                }
                std::uint32_t inputAccessorIndex = 0;
                std::uint32_t outputAccessorIndex = 0;
                if (!unsignedInteger(member(sampler, "input"), inputAccessorIndex)
                        || inputAccessorIndex >= accessors.size()
                        || !unsignedInteger(member(sampler, "output"), outputAccessorIndex)
                        || outputAccessorIndex >= accessors.size()) {
                    return fail(error, "GLB animation sampler accessor is invalid");
                }
                const Accessor& inputAccessor = accessors[inputAccessorIndex];
                const Accessor& outputAccessor = accessors[outputAccessorIndex];
                std::vector<float> times;
                std::vector<float> values;
                if (!readFloatAccessor(inputAccessor, views, binary, "SCALAR", 1U, times)
                        || !readFloatAccessor(outputAccessor, views, binary,
                                              outputType, outputComponents, values)
                        || inputAccessor.count != outputAccessor.count) {
                    return fail(error, "GLB animation sampler data has an unsupported layout");
                }
                for (std::size_t key = 0; key < times.size(); ++key) {
                    if (key > 0U && times[key] <= times[key - 1U]) {
                        return fail(error, "GLB animation key times must increase strictly");
                    }
                    animation.clip.durationSeconds = std::max(animation.clip.durationSeconds, times[key]);
                    if (vectorTrack != nullptr) {
                        const std::array<float, 3> value{{values[key * 3U], values[key * 3U + 1U],
                                                         values[key * 3U + 2U]}};
                        if (!vectorTrack->addKeyframe(times[key], value)) {
                            return fail(error, "GLB animation translation or scale key is invalid");
                        }
                    } else {
                        const SkeletalQuaternion value{values[key * 4U], values[key * 4U + 1U],
                                values[key * 4U + 2U], values[key * 4U + 3U]};
                        if (!quaternionTrack->addKeyframe(times[key], value)) {
                            return fail(error, "GLB animation quaternion key is invalid");
                        }
                    }
                }
                hasJointChannel = hasJointChannel || !times.empty();
            }
            if (!hasJointChannel) continue;
            if (!validateSkeletalAnimation(candidate.skeleton, animation.clip, error)) {
                return fail(error, "GLB skeletal animation clip failed validation");
            }
            candidate.animations.push_back(std::move(animation));
        }
    }

    output = std::move(candidate);
    error.clear();
    return true;
}

}  // namespace nuby
