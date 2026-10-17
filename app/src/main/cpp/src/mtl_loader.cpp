#include "nuby/mtl_loader.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <unordered_set>
#include <vector>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumSourceBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumLineLength = 1024;
constexpr std::size_t kMaximumMaterials = 256;
constexpr std::size_t kMaximumPathLength = 4096;

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

std::string trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool safeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > kMaximumPathLength || path.front() == '/'
            || path.front() == '\\' || path.find(':') != std::string::npos
            || path.find('\\') != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        if (end == start) return false;
        const std::string segment = path.substr(start, end - start);
        if (segment == "." || segment == "..") return false;
        for (unsigned char c : segment) {
            if (c < 0x20U || c == 0x7FU) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool readColor(std::istringstream& fields, std::array<float, 3>& color,
               float maximum = 1.0f) {
    if (!(fields >> color[0] >> color[1] >> color[2])) return false;
    for (float value : color) {
        if (!std::isfinite(value) || value < 0.0f || value > maximum) return false;
    }
    fields >> std::ws;
    return fields.peek() == std::char_traits<char>::eof();
}

bool readScalar(std::istringstream& fields, float& value,
                float minimum, float maximum) {
    if (!(fields >> value) || !std::isfinite(value)
            || value < minimum || value > maximum) return false;
    fields >> std::ws;
    return fields.peek() == std::char_traits<char>::eof();
}

std::string resolveMapPath(const std::string& directory, const std::string& path) {
    if (!safeRelativePath(path)) return {};
    if (directory.empty()) return path;
    return directory + "/" + path;
}

bool parseFiniteFloat(const std::string& token, float& value) {
    std::istringstream number(token);
    number.imbue(std::locale::classic());
    if (!(number >> value)) return false;
    number >> std::ws;
    return number.peek() == std::char_traits<char>::eof() && std::isfinite(value);
}

bool parseMapSpecification(std::istringstream& fields,
                           std::string& path,
                           TextureMapTransform& transform,
                           TextureMapValueRange& valueRange,
                           bool allowScalarChannel = false,
                           ScalarTextureChannel* scalarChannel = nullptr) {
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token) tokens.push_back(std::move(token));
    std::size_t index = 0;
    while (index < tokens.size() && !tokens[index].empty()
            && tokens[index].front() == '-') {
        const std::string option = tokens[index++];
        if (option == "-clamp") {
            if (index >= tokens.size()) return false;
            if (tokens[index] == "on") transform.clampToEdge = true;
            else if (tokens[index] == "off") transform.clampToEdge = false;
            else return false;
            ++index;
            continue;
        }
        if (option == "-imfchan") {
            if (!allowScalarChannel || scalarChannel == nullptr || index >= tokens.size()) {
                return false;
            }
            if (tokens[index] == "r") *scalarChannel = ScalarTextureChannel::Red;
            else if (tokens[index] == "g") *scalarChannel = ScalarTextureChannel::Green;
            else if (tokens[index] == "b") *scalarChannel = ScalarTextureChannel::Blue;
            else if (tokens[index] == "m") *scalarChannel = ScalarTextureChannel::Matte;
            else if (tokens[index] == "l") *scalarChannel = ScalarTextureChannel::Luminance;
            else return false;
            ++index;
            continue;
        }
        if (option == "-mm") {
            if (index + 1 >= tokens.size()
                    || !parseFiniteFloat(tokens[index], valueRange.base)
                    || !parseFiniteFloat(tokens[index + 1], valueRange.gain)
                    || std::fabs(valueRange.base) > 16.0f
                    || std::fabs(valueRange.gain) > 16.0f) return false;
            index += 2;
            continue;
        }
        if (option != "-s" && option != "-o") return false;
        if (index + 1 >= tokens.size()) return false;
        float first = 0.0f;
        float second = 0.0f;
        if (!parseFiniteFloat(tokens[index++], first)
                || !parseFiniteFloat(tokens[index++], second)) return false;
        // Wavefront permits an optional third, depth-axis component. It is
        // irrelevant for this 2D renderer, but must still be a valid number.
        if (index < tokens.size()) {
            float third = 0.0f;
            if (parseFiniteFloat(tokens[index], third)) ++index;
        }
        if (option == "-s") {
            transform.uvScale = {first, second};
        } else {
            transform.uvOffset = {first, second};
        }
    }
    if (index >= tokens.size()) return false;
    path = tokens[index++];
    while (index < tokens.size()) {
        path += ' ';
        path += tokens[index++];
    }
    path = trim(std::move(path));
    for (float scale : transform.uvScale) {
        if (std::fabs(scale) > 1000.0f) return false;
    }
    for (float offset : transform.uvOffset) {
        if (std::fabs(offset) > 10000.0f) return false;
    }
    return !path.empty();
}

}  // namespace

bool parseMtl(std::string_view source, const std::string& libraryAssetPath,
              std::vector<WavefrontMaterial>& materials, std::string& error) {
    error.clear();
    if (source.empty() || source.size() > kMaximumSourceBytes
            || !safeRelativePath(libraryAssetPath)) {
        return fail(error, "MTL source or library asset path is invalid");
    }
    const std::size_t directoryEnd = libraryAssetPath.find_last_of('/');
    const std::string directory = directoryEnd == std::string::npos
            ? std::string{} : libraryAssetPath.substr(0, directoryEnd);
    std::istringstream input{std::string(source)};
    input.imbue(std::locale::classic());
    std::string line;
    std::vector<WavefrontMaterial> candidate;
    std::unordered_set<std::string> names;
    WavefrontMaterial* current = nullptr;
    bool currentHasExplicitRoughness = false;

    while (std::getline(input, line)) {
        if (line.size() > kMaximumLineLength) {
            return fail(error, "MTL record exceeds the maximum line length");
        }
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        line = trim(std::move(line));
        if (line.empty()) continue;
        std::istringstream fields(line);
        fields.imbue(std::locale::classic());
        std::string record;
        fields >> record;
        if (record == "newmtl") {
            std::string name;
            std::getline(fields, name);
            name = trim(std::move(name));
            if (name.empty() || name.size() > 128 || !names.insert(name).second
                    || candidate.size() >= kMaximumMaterials) {
                return fail(error, "MTL material name is empty, duplicated or exceeds limits");
            }
            candidate.push_back(WavefrontMaterial{});
            current = &candidate.back();
            current->name = std::move(name);
            currentHasExplicitRoughness = false;
            continue;
        }
        if (current == nullptr) {
            if (record == "Kd" || record == "Ke" || record == "Ns" || record == "Pm"
                    || record == "Pr" || record == "d" || record == "Tr"
                    || record == "map_d" || record == "map_Kd" || record == "map_Kn"
                    || record == "norm" || record == "map_Pr" || record == "map_Pm"
                    || record == "map_AO" || record == "map_Ke") {
                return fail(error, "MTL material property appears before newmtl");
            }
            continue;
        }

        if (record == "Kd") {
            if (!readColor(fields, current->material.color)) {
                return fail(error, "MTL diffuse color is invalid");
            }
        } else if (record == "Ke") {
            if (!readColor(fields, current->material.emission, 32.0f)) {
                return fail(error, "MTL emissive color is invalid");
            }
        } else if (record == "Ns") {
            float exponent = 0.0f;
            if (!readScalar(fields, exponent, 0.0f, 100000.0f)) {
                return fail(error, "MTL specular exponent is invalid");
            }
            if (!currentHasExplicitRoughness) {
                current->material.roughness = std::clamp(
                        std::sqrt(2.0f / (exponent + 2.0f)), 0.06f, 1.0f);
            }
        } else if (record == "Pm") {
            if (!readScalar(fields, current->material.metallic, 0.0f, 1.0f)) {
                return fail(error, "MTL metallic factor is invalid");
            }
        } else if (record == "Pr") {
            if (!readScalar(fields, current->material.roughness, 0.0f, 1.0f)) {
                return fail(error, "MTL roughness factor is invalid");
            }
            currentHasExplicitRoughness = true;
        } else if (record == "d" || record == "Tr") {
            float transparency = 0.0f;
            if (!readScalar(fields, transparency, 0.0f, 1.0f)) {
                return fail(error, "MTL dissolve or transparency factor is invalid");
            }
            current->material.opacity = record == "d" ? transparency : 1.0f - transparency;
        } else if (record == "map_d" || record == "map_Kd" || record == "map_Kn"
                || record == "norm" || record == "map_Pr" || record == "map_Pm"
                || record == "map_AO" || record == "map_Ke") {
            std::string rawPath;
            TextureMapTransform transform;
            TextureMapValueRange valueRange;
            ScalarTextureChannel opacityChannel = ScalarTextureChannel::Luminance;
            const bool isOpacityMap = record == "map_d";
            if (!parseMapSpecification(fields, rawPath, transform, valueRange,
                                       isOpacityMap, &opacityChannel)) {
                return fail(error, "MTL texture map path or options are invalid or unsupported");
            }
            const std::string resolved = resolveMapPath(directory, rawPath);
            if (resolved.empty()) return fail(error, "MTL texture map escapes its asset directory");
            if (isOpacityMap) {
                current->material.opacityTexture = resolved;
                current->material.opacityMapTransform = transform;
                current->material.opacityMapValueRange = valueRange;
                current->material.opacityMapChannel = opacityChannel;
            } else if (record == "map_Kd") {
                current->material.albedoTexture = resolved;
                current->material.albedoMapTransform = transform;
                current->material.albedoMapValueRange = valueRange;
            } else if (record == "map_Kn" || record == "norm") {
                current->material.normalTexture = resolved;
                current->material.normalMapTransform = transform;
                current->material.normalMapValueRange = valueRange;
            } else if (record == "map_Pr") {
                current->material.roughnessTexture = resolved;
                current->material.roughnessMapTransform = transform;
                current->material.roughnessMapValueRange = valueRange;
            } else if (record == "map_Pm") {
                current->material.metallicTexture = resolved;
                current->material.metallicMapTransform = transform;
                current->material.metallicMapValueRange = valueRange;
            } else if (record == "map_AO") {
                current->material.occlusionTexture = resolved;
                current->material.occlusionMapTransform = transform;
                current->material.occlusionMapValueRange = valueRange;
            } else {
                current->material.emissiveTexture = resolved;
                current->material.emissiveMapTransform = transform;
                current->material.emissiveMapValueRange = valueRange;
            }
        }
    }

    if (candidate.empty()) return fail(error, "MTL file contains no materials");
    materials = std::move(candidate);
    return true;
}

}  // namespace nuby
