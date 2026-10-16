#include "nuby/obj_loader.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <sstream>
#include <vector>

namespace nuby {
namespace {

struct FaceIndex {
    std::size_t position = 0;
    std::size_t texcoord = 0;
    std::size_t normal = 0;
    bool hasTexcoord = false;
    bool hasNormal = false;
};

using Vec3 = std::array<float, 3>;

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

Vec3 normalized(const Vec3& value) {
    const float lengthSquared = value[0] * value[0]
                              + value[1] * value[1]
                              + value[2] * value[2];
    if (lengthSquared < 1.0e-12f) return {0.0f, 1.0f, 0.0f};
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    return {value[0] * inverseLength, value[1] * inverseLength, value[2] * inverseLength};
}

bool parseInteger(std::string_view text, int& value) {
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool resolveIndex(std::string_view text, std::size_t count, std::size_t& index) {
    int raw = 0;
    if (!parseInteger(text, raw) || raw == 0) return false;
    const long long resolved = raw > 0
            ? static_cast<long long>(raw) - 1
            : static_cast<long long>(count) + static_cast<long long>(raw);
    if (resolved < 0 || resolved >= static_cast<long long>(count)) return false;
    index = static_cast<std::size_t>(resolved);
    return true;
}

bool parseFaceIndex(std::string_view token, std::size_t positionCount,
                    std::size_t texcoordCount, std::size_t normalCount,
                    FaceIndex& result) {
    const std::size_t firstSlash = token.find('/');
    const std::string_view positionPart = token.substr(0, firstSlash);
    if (!resolveIndex(positionPart, positionCount, result.position)) return false;
    if (firstSlash == std::string_view::npos) return true;

    const std::size_t secondSlash = token.find('/', firstSlash + 1);
    const std::size_t texcoordEnd = secondSlash == std::string_view::npos
            ? token.size() : secondSlash;
    const std::string_view texcoordPart = token.substr(firstSlash + 1,
                                                       texcoordEnd - firstSlash - 1);
    if (!texcoordPart.empty()) {
        if (!resolveIndex(texcoordPart, texcoordCount, result.texcoord)) return false;
        result.hasTexcoord = true;
    }

    if (secondSlash != std::string_view::npos) {
        const std::string_view normalPart = token.substr(secondSlash + 1);
        if (!normalPart.empty()) {
            if (!resolveIndex(normalPart, normalCount, result.normal)) return false;
            result.hasNormal = true;
        }
    }
    return true;
}

}  // namespace

bool parseObj(std::string_view source, std::vector<MeshVertex>& vertices,
              std::string& error) {
    vertices.clear();
    error.clear();

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<std::array<float, 2>> texcoords;
    std::istringstream input{std::string(source)};
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        std::istringstream fields(line);
        std::string record;
        if (!(fields >> record) || record[0] == '#') continue;

        if (record == "v") {
            Vec3 position{};
            if (!(fields >> position[0] >> position[1] >> position[2])) {
                error = "invalid vertex at line " + std::to_string(lineNumber);
                return false;
            }
            positions.push_back(position);
        } else if (record == "vn") {
            Vec3 normal{};
            if (!(fields >> normal[0] >> normal[1] >> normal[2])) {
                error = "invalid normal at line " + std::to_string(lineNumber);
                return false;
            }
            normals.push_back(normalized(normal));
        } else if (record == "vt") {
            std::array<float, 2> uv{};
            if (!(fields >> uv[0] >> uv[1])) {
                error = "invalid texture coordinate at line " + std::to_string(lineNumber);
                return false;
            }
            texcoords.push_back(uv);
        } else if (record == "f") {
            std::vector<FaceIndex> face;
            std::string token;
            while (fields >> token) {
                if (!token.empty() && token[0] == '#') break;
                FaceIndex index{};
                if (!parseFaceIndex(token, positions.size(), texcoords.size(),
                                    normals.size(), index)) {
                    error = "invalid face index at line " + std::to_string(lineNumber);
                    vertices.clear();
                    return false;
                }
                face.push_back(index);
            }
            if (face.size() < 3) {
                error = "face has fewer than three vertices at line "
                      + std::to_string(lineNumber);
                vertices.clear();
                return false;
            }

            // Fan triangulation handles triangles, quads and convex n-gons.
            for (std::size_t corner = 1; corner + 1 < face.size(); ++corner) {
                const std::array<FaceIndex, 3> triangle{face[0], face[corner], face[corner + 1]};
                const Vec3 edgeA = subtract(positions[triangle[1].position],
                                            positions[triangle[0].position]);
                const Vec3 edgeB = subtract(positions[triangle[2].position],
                                            positions[triangle[0].position]);
                const Vec3 generatedNormal = normalized(cross(edgeA, edgeB));

                for (const FaceIndex& index : triangle) {
                    MeshVertex vertex{};
                    const Vec3& position = positions[index.position];
                    const Vec3& normal = index.hasNormal ? normals[index.normal] : generatedNormal;
                    vertex.position[0] = position[0];
                    vertex.position[1] = position[1];
                    vertex.position[2] = position[2];
                    vertex.normal[0] = normal[0];
                    vertex.normal[1] = normal[1];
                    vertex.normal[2] = normal[2];
                    if (index.hasTexcoord) {
                        vertex.uv[0] = texcoords[index.texcoord][0];
                        vertex.uv[1] = texcoords[index.texcoord][1];
                    }
                    vertices.push_back(vertex);
                }
            }
        }
    }

    if (vertices.empty()) {
        error = "OBJ contains no valid triangle faces";
        return false;
    }
    return true;
}

}  // namespace nuby
