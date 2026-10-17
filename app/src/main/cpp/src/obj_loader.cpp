#include "nuby/obj_loader.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace nuby {
namespace {

constexpr std::size_t kMaximumSourceBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumAttributeValues = 500000;
constexpr std::size_t kMaximumExpandedVertices = 500000;
constexpr std::size_t kMaximumFaceCorners = 256;
constexpr std::size_t kMaximumMaterialLibraries = 32;
constexpr std::size_t kMaximumSubmeshes = 1024;
constexpr std::size_t kMaximumLineLength = 4096;

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

std::string trim(std::string value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool safeProjectRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 4096 || path.front() == '/'
            || path.front() == '\\' || path.find('\\') != std::string::npos
            || path.find(':') != std::string::npos) return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        if (start == end) return false;
        const std::string segment = path.substr(start, end - start);
        if (segment == "." || segment == "..") return false;
        for (unsigned char character : segment) {
            if (character < 0x20U || character == 0x7FU) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

using TriangleCorners = std::array<std::size_t, 3>;

struct ProjectedPoint {
    double x = 0.0;
    double y = 0.0;
};

double orient2d(const ProjectedPoint& a, const ProjectedPoint& b,
                const ProjectedPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool onSegment(const ProjectedPoint& a, const ProjectedPoint& b,
               const ProjectedPoint& p, double epsilon) {
    return p.x >= std::min(a.x, b.x) - epsilon
            && p.x <= std::max(a.x, b.x) + epsilon
            && p.y >= std::min(a.y, b.y) - epsilon
            && p.y <= std::max(a.y, b.y) + epsilon;
}

bool segmentsIntersect(const ProjectedPoint& a, const ProjectedPoint& b,
                       const ProjectedPoint& c, const ProjectedPoint& d,
                       double epsilon) {
    const double o1 = orient2d(a, b, c);
    const double o2 = orient2d(a, b, d);
    const double o3 = orient2d(c, d, a);
    const double o4 = orient2d(c, d, b);
    if (((o1 > epsilon && o2 < -epsilon) || (o1 < -epsilon && o2 > epsilon))
            && ((o3 > epsilon && o4 < -epsilon)
                || (o3 < -epsilon && o4 > epsilon))) return true;
    const double coordinateEpsilon = std::sqrt(epsilon);
    return (std::fabs(o1) <= epsilon && onSegment(a, b, c, coordinateEpsilon))
            || (std::fabs(o2) <= epsilon && onSegment(a, b, d, coordinateEpsilon))
            || (std::fabs(o3) <= epsilon && onSegment(c, d, a, coordinateEpsilon))
            || (std::fabs(o4) <= epsilon && onSegment(c, d, b, coordinateEpsilon));
}

bool triangulateFace(const std::vector<FaceIndex>& face,
                     const std::vector<Vec3>& positions,
                     std::vector<TriangleCorners>& triangles) {
    triangles.clear();
    if (face.size() < 3) return false;

    std::array<double, 3> newell{};
    for (std::size_t i = 0; i < face.size(); ++i) {
        const Vec3& current = positions[face[i].position];
        const Vec3& next = positions[face[(i + 1U) % face.size()].position];
        newell[0] += (static_cast<double>(current[1]) - next[1])
                   * (static_cast<double>(current[2]) + next[2]);
        newell[1] += (static_cast<double>(current[2]) - next[2])
                   * (static_cast<double>(current[0]) + next[0]);
        newell[2] += (static_cast<double>(current[0]) - next[0])
                   * (static_cast<double>(current[1]) + next[1]);
    }
    const std::array<double, 3> absoluteNormal{
            std::fabs(newell[0]), std::fabs(newell[1]), std::fabs(newell[2])};
    const std::size_t droppedAxis = static_cast<std::size_t>(
            std::max_element(absoluteNormal.begin(), absoluteNormal.end())
                    - absoluteNormal.begin());
    if (absoluteNormal[droppedAxis] <= 1.0e-12) return false;

    std::vector<ProjectedPoint> projected;
    projected.reserve(face.size());
    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    for (const FaceIndex& corner : face) {
        const Vec3& position = positions[corner.position];
        ProjectedPoint point{};
        if (droppedAxis == 0U) {
            point = {position[1], position[2]};
        } else if (droppedAxis == 1U) {
            point = {position[0], position[2]};
        } else {
            point = {position[0], position[1]};
        }
        projected.push_back(point);
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    const double scale = std::max(maxX - minX, maxY - minY);
    const double epsilon = std::max(scale * scale * 1.0e-10, 1.0e-12);
    if (scale <= 1.0e-12) return false;

    // Reject non-planar polygons instead of producing unpredictable shading.
    const double normalLength = std::sqrt(newell[0] * newell[0]
            + newell[1] * newell[1] + newell[2] * newell[2]);
    if (normalLength <= 1.0e-12) return false;
    const Vec3& planeOrigin = positions[face.front().position];
    const double planarityTolerance = std::max(scale * 1.0e-4, 1.0e-6);
    for (const FaceIndex& corner : face) {
        const Vec3& position = positions[corner.position];
        const double distance = (newell[0] * (position[0] - planeOrigin[0])
                + newell[1] * (position[1] - planeOrigin[1])
                + newell[2] * (position[2] - planeOrigin[2])) / normalLength;
        if (std::fabs(distance) > planarityTolerance) return false;
    }

    // Check that the projected boundary is a simple polygon.
    for (std::size_t i = 0; i < face.size(); ++i) {
        const std::size_t nextI = (i + 1U) % face.size();
        for (std::size_t j = i + 1U; j < face.size(); ++j) {
            const std::size_t nextJ = (j + 1U) % face.size();
            if (i == j || nextI == j || nextJ == i) continue;
            if (segmentsIntersect(projected[i], projected[nextI],
                                  projected[j], projected[nextJ], epsilon)) return false;
        }
    }

    double signedAreaTwice = 0.0;
    for (std::size_t i = 0; i < projected.size(); ++i) {
        const ProjectedPoint& a = projected[i];
        const ProjectedPoint& b = projected[(i + 1U) % projected.size()];
        signedAreaTwice += a.x * b.y - b.x * a.y;
    }
    if (std::fabs(signedAreaTwice) <= epsilon) return false;
    const double winding = signedAreaTwice > 0.0 ? 1.0 : -1.0;

    std::vector<std::size_t> remaining(face.size());
    for (std::size_t i = 0; i < remaining.size(); ++i) remaining[i] = i;
    triangles.reserve(face.size() - 2U);
    while (remaining.size() > 3U) {
        bool clippedEar = false;
        for (std::size_t i = 0; i < remaining.size(); ++i) {
            const std::size_t previous = remaining[(i + remaining.size() - 1U)
                                                   % remaining.size()];
            const std::size_t current = remaining[i];
            const std::size_t next = remaining[(i + 1U) % remaining.size()];
            if (winding * orient2d(projected[previous], projected[current],
                                   projected[next]) <= epsilon) continue;

            bool containsCorner = false;
            for (std::size_t candidate : remaining) {
                if (candidate == previous || candidate == current || candidate == next) {
                    continue;
                }
                const double edge0 = winding * orient2d(
                        projected[previous], projected[current], projected[candidate]);
                const double edge1 = winding * orient2d(
                        projected[current], projected[next], projected[candidate]);
                const double edge2 = winding * orient2d(
                        projected[next], projected[previous], projected[candidate]);
                if (edge0 >= -epsilon && edge1 >= -epsilon && edge2 >= -epsilon) {
                    containsCorner = true;
                    break;
                }
            }
            if (containsCorner) continue;

            triangles.push_back({previous, current, next});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            clippedEar = true;
            break;
        }
        if (!clippedEar) {
            triangles.clear();
            return false;
        }
    }
    if (winding * orient2d(projected[remaining[0]], projected[remaining[1]],
                           projected[remaining[2]]) <= epsilon) {
        triangles.clear();
        return false;
    }
    triangles.push_back({remaining[0], remaining[1], remaining[2]});
    return triangles.size() == face.size() - 2U;
}

bool parseObjModelInternal(std::string_view source, ObjModel& model, std::string& error) {
    model = {};
    error.clear();
    if (source.empty() || source.size() > kMaximumSourceBytes) {
        error = "OBJ source is empty or exceeds the maximum size";
        return false;
    }

    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<std::array<float, 2>> texcoords;
    std::unordered_set<std::string> materialLibrarySet;
    std::string activeMaterial;
    std::size_t expandedVertexCount = 0;
    std::istringstream input{std::string(source)};
    std::string line;
    std::size_t lineNumber = 0;

    while (std::getline(input, line)) {
        ++lineNumber;
        if (line.size() > kMaximumLineLength) {
            error = "OBJ record exceeds the maximum line length at line "
                  + std::to_string(lineNumber);
            model = {};
            return false;
        }
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        std::istringstream fields(line);
        std::string record;
        if (!(fields >> record)) continue;

        if (record == "v") {
            Vec3 position{};
            if (!(fields >> position[0] >> position[1] >> position[2])
                    || !std::isfinite(position[0]) || !std::isfinite(position[1])
                    || !std::isfinite(position[2])
                    || positions.size() >= kMaximumAttributeValues) {
                error = "invalid vertex at line " + std::to_string(lineNumber);
                model = {};
                return false;
            }
            positions.push_back(position);
        } else if (record == "vn") {
            Vec3 normal{};
            if (!(fields >> normal[0] >> normal[1] >> normal[2])
                    || !std::isfinite(normal[0]) || !std::isfinite(normal[1])
                    || !std::isfinite(normal[2])
                    || normals.size() >= kMaximumAttributeValues) {
                error = "invalid normal at line " + std::to_string(lineNumber);
                model = {};
                return false;
            }
            normals.push_back(normalized(normal));
        } else if (record == "vt") {
            std::array<float, 2> uv{};
            if (!(fields >> uv[0] >> uv[1]) || !std::isfinite(uv[0])
                    || !std::isfinite(uv[1])
                    || texcoords.size() >= kMaximumAttributeValues) {
                error = "invalid texture coordinate at line " + std::to_string(lineNumber);
                model = {};
                return false;
            }
            texcoords.push_back(uv);
        } else if (record == "mtllib") {
            std::string library;
            while (fields >> library) {
                if (library.front() == '-') {
                    error = "OBJ mtllib options are not supported at line "
                          + std::to_string(lineNumber);
                    model = {};
                    return false;
                }
                if (!safeProjectRelativePath(library)) {
                    error = "OBJ material library path is unsafe at line "
                          + std::to_string(lineNumber);
                    model = {};
                    return false;
                }
                if (materialLibrarySet.insert(library).second) {
                    if (model.materialLibraries.size() >= kMaximumMaterialLibraries) {
                        error = "OBJ contains too many material libraries";
                        model = {};
                        return false;
                    }
                    model.materialLibraries.push_back(std::move(library));
                }
            }
        } else if (record == "usemtl") {
            std::getline(fields, activeMaterial);
            activeMaterial = trim(std::move(activeMaterial));
            if (activeMaterial.empty() || activeMaterial.size() > 128) {
                error = "usemtl name is empty or exceeds limits at line "
                      + std::to_string(lineNumber);
                model = {};
                return false;
            }
        } else if (record == "f") {
            std::vector<FaceIndex> face;
            std::string token;
            while (fields >> token) {
                FaceIndex index{};
                if (!parseFaceIndex(token, positions.size(), texcoords.size(),
                                    normals.size(), index)) {
                    error = "invalid face index at line " + std::to_string(lineNumber);
                    model = {};
                    return false;
                }
                face.push_back(index);
                if (face.size() > kMaximumFaceCorners) {
                    error = "face exceeds the maximum corner count at line "
                          + std::to_string(lineNumber);
                    model = {};
                    return false;
                }
            }
            if (face.size() < 3) {
                error = "face has fewer than three vertices at line "
                      + std::to_string(lineNumber);
                model = {};
                return false;
            }

            std::vector<TriangleCorners> triangles;
            if (!triangulateFace(face, positions, triangles)) {
                error = "face is degenerate, non-planar, self-intersecting or cannot be triangulated at line "
                      + std::to_string(lineNumber);
                model = {};
                return false;
            }
            const std::size_t verticesForFace = triangles.size() * 3U;
            if (verticesForFace > kMaximumExpandedVertices - expandedVertexCount) {
                error = "OBJ exceeds the maximum expanded vertex count";
                model = {};
                return false;
            }
            expandedVertexCount += verticesForFace;

            if (model.submeshes.empty()
                    || model.submeshes.back().materialName != activeMaterial) {
                if (model.submeshes.size() >= kMaximumSubmeshes) {
                    error = "OBJ contains too many material submeshes";
                    model = {};
                    return false;
                }
                model.submeshes.push_back(ObjSubmesh{});
                model.submeshes.back().materialName = activeMaterial;
            }
            std::vector<MeshVertex>& vertices = model.submeshes.back().vertices;

            for (const TriangleCorners& corners : triangles) {
                const std::array<FaceIndex, 3> triangle{
                        face[corners[0]], face[corners[1]], face[corners[2]]};
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

    if (model.submeshes.empty()) {
        error = "OBJ contains no valid triangle faces";
        return false;
    }
    return true;
}
}  // namespace

bool parseObjModel(std::string_view source, ObjModel& model, std::string& error) {
    return parseObjModelInternal(source, model, error);
}

bool parseObj(std::string_view source, std::vector<MeshVertex>& vertices,
              std::string& error) {
    vertices.clear();
    ObjModel model;
    if (!parseObjModel(source, model, error)) return false;
    for (const ObjSubmesh& submesh : model.submeshes) {
        vertices.insert(vertices.end(), submesh.vertices.begin(), submesh.vertices.end());
    }
    return true;
}

}  // namespace nuby
