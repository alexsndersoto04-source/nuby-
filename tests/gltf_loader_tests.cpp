#include "nuby/gltf_loader.hpp"
#include "nuby/image_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void appendU32(std::string& data, std::uint32_t value) {
    data.push_back(static_cast<char>(value & 0xFFU));
    data.push_back(static_cast<char>((value >> 8U) & 0xFFU));
    data.push_back(static_cast<char>((value >> 16U) & 0xFFU));
    data.push_back(static_cast<char>((value >> 24U) & 0xFFU));
}

void setU32(std::string& data, std::size_t offset, std::uint32_t value) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
        data[offset + byte] = static_cast<char>((value >> (byte * 8U)) & 0xFFU);
    }
}

void appendChunk(std::string& data, std::uint32_t type, std::string payload) {
    while ((payload.size() & 3U) != 0U) payload.push_back(type == 0x4E4F534AU ? ' ' : '\0');
    appendU32(data, static_cast<std::uint32_t>(payload.size()));
    appendU32(data, type);
    data.append(payload);
}

std::string makeGlb() {
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, "{\"asset\":{\"version\":\"2.0\"}}");
    appendChunk(data, 0x004E4942U, std::string("ABCD", 4));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeTriangleGlb(std::uint16_t thirdIndex = 2, bool includeSkin = false) {
    std::string binary;
    for (float value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(binary, bits);
    }
    for (std::uint16_t index : {std::uint16_t{0}, std::uint16_t{1}, thirdIndex}) {
        binary.push_back(static_cast<char>(index & 0xFFU));
        binary.push_back(static_cast<char>((index >> 8U) & 0xFFU));
    }

    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":";
    if (includeSkin) {
        binary.append(2, '\0');  // Align the next bufferView to four bytes.
        for (int vertex = 0; vertex < 3; ++vertex) {
            binary.append({static_cast<char>(0), static_cast<char>(0),
                           static_cast<char>(0), static_cast<char>(0)});
        }
        for (int vertex = 0; vertex < 3; ++vertex) {
            const float weights[] = {2.0f, 0.0f, 0.0f, 0.0f};
            for (float weight : weights) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &weight, sizeof(bits));
                appendU32(binary, bits);
            }
        }
        json += "104}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6},"
                "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12},"
                "{\"buffer\":0,\"byteOffset\":56,\"byteLength\":48}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
                "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
                "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
                "\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":1}]}]}";
    } else {
        json += "42}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
                "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6}],"
                "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
                "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
                "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}";
    }
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, std::move(json));
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeTriangleWithMaterialGlb(const std::string& materialJson) {
    std::string json;
    std::string binary;
    std::string error;
    if (!nuby::parseGlbContainer(makeTriangleGlb(), json, binary, error)) return {};
    const std::string primitiveEnd = "\"indices\":1}]";
    const std::size_t primitive = json.find(primitiveEnd);
    const std::size_t meshes = json.find("\"meshes\":");
    if (primitive == std::string::npos || meshes == std::string::npos) return {};
    json.replace(primitive, primitiveEnd.size(), "\"indices\":1,\"material\":0}]");
    const std::size_t updatedMeshes = json.find("\"meshes\":");
    json.insert(updatedMeshes, "\"materials\":[{" + materialJson + "}],");
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, std::move(json));
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeTexturedTriangleGlb(const std::string& materialJson,
                                    const std::string& textureDefinitions =
                                            R"JSON("images":[{"bufferView":2,"mimeType":"image/png"}],"textures":[{"source":0}],)JSON") {
    std::string json;
    std::string binary;
    std::string error;
    if (!nuby::parseGlbContainer(makeTriangleGlb(), json, binary, error)) return {};
    const std::string pngHex = "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c48900000010494441547801010500faff00ff2010ff0580022f7afc29410000000049454e44ae426082";
    std::string image;
    for (std::size_t index = 0; index < pngHex.size(); index += 2U) {
        image.push_back(static_cast<char>(std::stoul(pngHex.substr(index, 2U), nullptr, 16)));
    }
    while ((binary.size() & 3U) != 0U) binary.push_back('\0');
    const std::size_t imageOffset = binary.size();
    binary += image;
    const std::string oldLength = R"JSON("byteLength":42)JSON";
    const std::size_t bufferLength = json.find(oldLength);
    if (bufferLength == std::string::npos) return {};
    json.replace(bufferLength, oldLength.size(), R"JSON("byteLength":)JSON"
            + std::to_string(binary.size()));
    const std::string oldViews = R"JSON({"buffer":0,"byteOffset":36,"byteLength":6}])JSON";
    const std::size_t views = json.find(oldViews);
    if (views == std::string::npos) return {};
    json.replace(views, oldViews.size(), oldViews.substr(0, oldViews.size() - 1U)
            + R"JSON(,{"buffer":0,"byteOffset":)JSON"
            + std::to_string(imageOffset) + R"JSON(,"byteLength":)JSON"
            + std::to_string(image.size()) + "}]");
    const std::string primitive = R"JSON("indices":1}])JSON";
    const std::size_t primitiveOffset = json.find(primitive);
    const std::size_t meshOffset = json.find(R"JSON("meshes":)JSON");
    if (primitiveOffset == std::string::npos || meshOffset == std::string::npos) return {};
    json.replace(primitiveOffset, primitive.size(), R"JSON("indices":1,"material":0}])JSON");
    const std::size_t updatedMeshes = json.find(R"JSON("meshes":)JSON");
    json.insert(updatedMeshes, R"JSON("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)JSON"
            + textureDefinitions + R"JSON("materials":[{)JSON" + materialJson + "}],");
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, std::move(json));
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeMultiNodeStaticGlb(bool cycle = false) {
    std::string json;
    std::string binary;
    std::string error;
    if (!nuby::parseGlbContainer(makeTriangleGlb(), json, binary, error)) return {};
    const std::string singleMesh = R"JSON("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])JSON";
    const std::string twoMeshes = R"JSON("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1},{"attributes":{"POSITION":0},"indices":1}]},{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])JSON";
    const std::size_t meshOffset = json.find(singleMesh);
    if (meshOffset == std::string::npos) return {};
    json.replace(meshOffset, singleMesh.size(), twoMeshes);
    std::string scene = R"JSON("scene":0,"scenes":[{"nodes":[0,2,3]}],"nodes":[{"name":"parent","translation":[2,0,0],"children":[1]},{"name":"rotated_mesh","rotation":[0,0,0.7071067812,0.7071067812],"scale":[2,1,1],"mesh":0},{"name":"matrix_mesh","matrix":[-1,0,0,0,0,1,0,0,0,0,1,0,-4,1,0,1],"mesh":1},{"name":"mesh_instance","translation":[8,0,0],"mesh":0},{"name":"not_in_selected_scene","translation":[100,0,0],"mesh":1}],)JSON";
    if (cycle) {
        const std::size_t meshNode = scene.find(R"JSON("mesh":0)JSON");
        if (meshNode == std::string::npos) return {};
        scene.insert(meshNode + std::string(R"JSON("mesh":0)JSON").size(), R"JSON(,"children":[0])JSON");
    }
    const std::size_t updatedMeshes = json.find("\"meshes\":");
    if (updatedMeshes == std::string::npos) return {};
    json.insert(updatedMeshes, scene);
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, std::move(json));
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeSkeletalGlb() {
    std::string binary;
    for (float value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(binary, bits);
    }
    for (std::uint16_t index : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{2}}) {
        binary.push_back(static_cast<char>(index & 0xFFU));
        binary.push_back(static_cast<char>((index >> 8U) & 0xFFU));
    }
    binary.append(2, '\0');
    for (int vertex = 0; vertex < 3; ++vertex) {
        binary.append({static_cast<char>(0), static_cast<char>(0),
                       static_cast<char>(0), static_cast<char>(0)});
    }
    for (int vertex = 0; vertex < 3; ++vertex) {
        const float weights[] = {1.0f, 0.0f, 0.0f, 0.0f};
        for (float weight : weights) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &weight, sizeof(bits));
            appendU32(binary, bits);
        }
    }
    const float inverseBind[] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 1,
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0,  0, 0, 0, 1};
    for (float value : inverseBind) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(binary, bits);
    }
    for (float value : {0.0f, 1.0f}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(binary, bits);
    }
    for (float value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(binary, bits);
    }
    const std::string json =
            "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":264}],"
            "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
            "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":6},"
            "{\"buffer\":0,\"byteOffset\":44,\"byteLength\":12},"
            "{\"buffer\":0,\"byteOffset\":56,\"byteLength\":48},"
            "{\"buffer\":0,\"byteOffset\":104,\"byteLength\":128},"
            "{\"buffer\":0,\"byteOffset\":232,\"byteLength\":8},"
            "{\"buffer\":0,\"byteOffset\":240,\"byteLength\":24}],"
            "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
            "{\"bufferView\":2,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
            "{\"bufferView\":4,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"},"
            "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
            "{\"bufferView\":6,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}],"
            "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
            "\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":1}]}],"
            "\"nodes\":[{\"name\":\"child_joint\",\"translation\":[1,0,0]},"
            "{\"name\":\"root_joint\",\"children\":[0]},"
            "{\"name\":\"mesh\",\"mesh\":0,\"skin\":0}],"
            "\"skins\":[{\"joints\":[0,1],\"inverseBindMatrices\":4}],"
            "\"animations\":[{\"name\":\"walk\",\"samplers\":[{\"input\":5,\"output\":6}],"
            "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"translation\"}}]}]}";
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, json);
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeMultipleMeshSkeletalGlb(bool translateSecondMesh = false) {
    std::string json;
    std::string binary;
    std::string error;
    if (!nuby::parseGlbContainer(makeSkeletalGlb(), json, binary, error)) return {};
    const std::string singleMesh =
            R"JSON("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1}]}],)JSON";
    const std::string twoMeshes =
            R"JSON("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1}]},{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3},"indices":1}]}],)JSON";
    const std::size_t meshPosition = json.find(singleMesh);
    if (meshPosition == std::string::npos) return {};
    json.replace(meshPosition, singleMesh.size(), twoMeshes);
    const std::string meshNode = R"JSON({"name":"mesh","mesh":0,"skin":0})JSON";
    const std::string secondMeshNode = translateSecondMesh
            ? R"JSON({"name":"mesh_accessory","mesh":1,"skin":0,"translation":[1,0,0]})JSON"
            : R"JSON({"name":"mesh_accessory","mesh":1,"skin":0})JSON";
    const std::size_t nodePosition = json.find(meshNode);
    if (nodePosition == std::string::npos) return {};
    json.replace(nodePosition, meshNode.size(), meshNode + "," + secondMeshNode);

    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, json);
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

std::string makeSkeletalGlbWithInterpolation(const std::string& interpolation) {
    std::string json;
    std::string binary;
    std::string error;
    if (!nuby::parseGlbContainer(makeSkeletalGlb(), json, binary, error)) return {};
    const std::string sampler = R"JSON("samplers":[{"input":5,"output":6}])JSON";
    const std::string animatedSampler = "\"samplers\":[{\"input\":5,\"output\":6,\"interpolation\":\""
            + interpolation + "\"}]";
    const std::size_t samplerPosition = json.find(sampler);
    if (samplerPosition == std::string::npos) return {};
    json.replace(samplerPosition, sampler.size(), animatedSampler);
    if (interpolation == "CUBICSPLINE") {
        const auto replaceOnce = [&json](const std::string& from, const std::string& to) {
            const std::size_t position = json.find(from);
            if (position == std::string::npos) return false;
            json.replace(position, from.size(), to);
            return true;
        };
        if (!replaceOnce(R"JSON("byteLength":264)JSON", R"JSON("byteLength":312)JSON")
                || !replaceOnce(R"JSON("byteOffset":240,"byteLength":24)JSON",
                                R"JSON("byteOffset":240,"byteLength":72)JSON")
                || !replaceOnce(R"JSON("bufferView":6,"componentType":5126,"count":2,"type":"VEC3")JSON",
                                R"JSON("bufferView":6,"componentType":5126,"count":6,"type":"VEC3")JSON")) {
            return {};
        }
        binary.resize(240U);
        for (float value : {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                            1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            appendU32(binary, bits);
        }
    }
    std::string data;
    appendU32(data, 0x46546C67U);
    appendU32(data, 2U);
    appendU32(data, 0U);
    appendChunk(data, 0x4E4F534AU, json);
    appendChunk(data, 0x004E4942U, std::move(binary));
    setU32(data, 8, static_cast<std::uint32_t>(data.size()));
    return data;
}

void testGlbChunksExtractAndTrimJsonPadding() {
    const std::string glb = makeGlb();
    std::string json;
    std::string binary;
    std::string error;
    expect(nuby::parseGlbContainer(glb, json, binary, error),
           "valid glTF binary container is accepted");
    expect(json == "{\"asset\":{\"version\":\"2.0\"}}",
           "JSON chunk is extracted without alignment padding");
    expect(binary == "ABCD", "BIN chunk bytes are preserved");
    expect(error.empty(), "successful container parse clears an old error message");

    std::string withExtensionChunk;
    appendU32(withExtensionChunk, 0x46546C67U);
    appendU32(withExtensionChunk, 2U);
    appendU32(withExtensionChunk, 0U);
    appendChunk(withExtensionChunk, 0x4E4F534AU, "{}");
    appendChunk(withExtensionChunk, 0x004E4942U, "WXYZ");
    appendChunk(withExtensionChunk, 0x12345678U, "ABCD");
    setU32(withExtensionChunk, 8, static_cast<std::uint32_t>(withExtensionChunk.size()));
    expect(nuby::parseGlbContainer(withExtensionChunk, json, binary, error),
           "optional extension chunks after BIN are tolerated");
}

void testGlbRejectsInvalidHeadersAndLengths() {
    std::string json;
    std::string binary;
    std::string error;
    std::string glb = makeGlb();
    glb[0] = 'X';
    expect(!nuby::parseGlbContainer(glb, json, binary, error), "invalid GLB magic is rejected");
    expect(json.empty() && binary.empty(), "failed parse clears partial outputs");

    glb = makeGlb();
    setU32(glb, 4, 1U);
    expect(!nuby::parseGlbContainer(glb, json, binary, error),
           "legacy GLB versions are rejected");
    glb = makeGlb();
    setU32(glb, 8, static_cast<std::uint32_t>(glb.size() - 4U));
    expect(!nuby::parseGlbContainer(glb, json, binary, error),
           "mismatched declared container length is rejected");

    glb = makeGlb();
    glb.pop_back();
    setU32(glb, 8, static_cast<std::uint32_t>(glb.size()));
    expect(!nuby::parseGlbContainer(glb, json, binary, error),
           "truncated chunks are rejected even with a matching outer length");
}

void testGlbImportsTriangleAndSkinAttributes() {
    std::vector<nuby::GltfPrimitive> primitives;
    std::string error;
    expect(nuby::parseGlbMeshes(makeTriangleGlb(), primitives, error),
           "minimal self-contained GLB triangle imports");
    expect(primitives.size() == 1 && primitives[0].vertices.size() == 3
                   && primitives[0].indices == std::vector<std::uint32_t>({0, 1, 2}),
           "triangle primitive topology and vertex count are preserved");
    if (primitives.size() == 1 && primitives[0].vertices.size() == 3) {
        expect(primitives[0].vertices[1].position[0] == 1.0f
                       && primitives[0].vertices[2].position[1] == 1.0f,
               "little-endian float positions decode correctly");
        expect(primitives[0].vertices[0].normal[2] == 1.0f,
               "missing normals are generated from triangle geometry");
    }

    expect(!nuby::parseGlbMeshes(makeTriangleGlb(3), primitives, error),
           "triangle indices outside the position accessor are rejected");
    expect(primitives.empty(), "failed mesh import does not leave partial output");
    expect(nuby::parseGlbMeshes(makeTriangleGlb(2, true), primitives, error),
           "four-influence joint and weight attributes import");
    expect(primitives.size() == 1 && primitives[0].hasSkinAttributes
                   && primitives[0].influences.size() == 3,
           "skinned vertices retain per-vertex influence data");
    if (primitives.size() == 1 && !primitives[0].influences.empty()) {
        const auto& first = primitives[0].influences[0];
        expect(first.jointIndices[0] == 0 && first.weights[0] == 1.0f
                       && first.weights[1] == 0.0f,
               "skin weights are normalized while joint indices are preserved");
    }
}

void testGltfPbrMaterialFactors() {
    const std::string material = R"JSON("pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,0.5],"metallicFactor":0.7,"roughnessFactor":0.3},"emissiveFactor":[0.1,0.2,0.3],"alphaMode":"BLEND")JSON";
    std::vector<nuby::GltfPrimitive> primitives;
    std::string error;
    expect(nuby::parseGlbMeshes(makeTriangleWithMaterialGlb(material), primitives, error),
           "glTF metallic-roughness factors import on a mesh primitive");
    expect(primitives.size() == 1 && primitives[0].hasMaterial,
           "primitive preserves its referenced glTF material");
    if (primitives.size() == 1 && primitives[0].hasMaterial) {
        const nuby::GltfMaterial& parsed = primitives[0].material;
        expect(std::fabs(parsed.baseColorFactor[0] - 0.2f) < 1.0e-6f
                       && std::fabs(parsed.baseColorFactor[2] - 0.6f) < 1.0e-6f
                       && std::fabs(parsed.baseColorFactor[3] - 0.5f) < 1.0e-6f
                       && std::fabs(parsed.metallicFactor - 0.7f) < 1.0e-6f
                       && std::fabs(parsed.roughnessFactor - 0.3f) < 1.0e-6f
                       && std::fabs(parsed.emissiveFactor[1] - 0.2f) < 1.0e-6f
                       && parsed.alphaBlend,
               "PBR color, metalness, roughness, emission and alpha mode are retained");
    }

    expect(!nuby::parseGlbMeshes(makeTriangleWithMaterialGlb(
                   R"JSON("pbrMetallicRoughness":{"baseColorFactor":[-0.1,0,0,1]})JSON"),
                   primitives, error),
           "out-of-range glTF base color factors are rejected");
    expect(nuby::parseGlbMeshes(makeTriangleWithMaterialGlb(
                   R"JSON("alphaMode":"MASK")JSON"), primitives, error)
                   && primitives.size() == 1U && primitives[0].material.alphaMask
                   && !primitives[0].material.alphaBlend
                   && std::fabs(primitives[0].material.alphaCutoff - 0.5f) < 1.0e-6f,
           "masked materials retain the default alpha cutoff as an opaque cutout mode");
    expect(nuby::parseGlbMeshes(makeTriangleWithMaterialGlb(
                   R"JSON("alphaMode":"MASK","alphaCutoff":0.25)JSON"),
                   primitives, error)
                   && primitives.size() == 1U
                   && std::fabs(primitives[0].material.alphaCutoff - 0.25f) < 1.0e-6f,
           "masked material custom alpha cutoff is retained");
    expect(!nuby::parseGlbMeshes(makeTriangleWithMaterialGlb(
                   R"JSON("alphaMode":"MASK","alphaCutoff":1.1)JSON"),
                   primitives, error),
           "out-of-range alpha cutoffs are rejected");
    expect(!nuby::parseGlbMeshes(makeTexturedTriangleGlb(
                   R"JSON("pbrMetallicRoughness":{"baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"rotation":0.5}}}})JSON"),
                   primitives, error),
           "rotated KHR_texture_transform is rejected rather than approximated");
    expect(!nuby::parseGlbMeshes(makeTexturedTriangleGlb(
                   R"JSON("pbrMetallicRoughness":{"baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"texCoord":1}}}})JSON"),
                   primitives, error),
           "KHR_texture_transform cannot silently select unsupported UV sets");
    expect(!nuby::parseGlbMeshes(makeTriangleWithMaterialGlb(
                   R"JSON("pbrMetallicRoughness":{"baseColorTexture":{"index":0}})JSON"),
                   primitives, error),
           "unsupported glTF material textures are not silently discarded");
}

void testGlbAlphaMaskMaterials() {
    const std::string material = R"JSON("pbrMetallicRoughness":{"baseColorFactor":[0.7,0.8,0.9,0.6],"baseColorTexture":{"index":0}},"alphaMode":"MASK","alphaCutoff":0.35)JSON";
    const std::string glb = makeTexturedTriangleGlb(material);
    std::vector<nuby::GltfPrimitive> primitives;
    std::string error;
    expect(nuby::parseGlbMeshes(glb, primitives, error)
                   && primitives.size() == 1U && primitives[0].hasMaterial
                   && primitives[0].material.alphaMask
                   && !primitives[0].material.alphaBlend
                   && primitives[0].material.baseColorTexture.imageIndex == 0
                   && std::fabs(primitives[0].material.baseColorFactor[3] - 0.6f) < 1.0e-6f
                   && std::fabs(primitives[0].material.alphaCutoff - 0.35f) < 1.0e-6f,
           "textured GLB MASK keeps its alpha map, factor, and cutoff for camera and shadow rendering");
    nuby::GltfStaticAsset asset;
    expect(nuby::parseGlbStaticAsset(glb, asset, error)
                   && asset.primitives.size() == 1U
                   && asset.primitives[0].material.alphaMask
                   && std::fabs(asset.primitives[0].material.alphaCutoff - 0.35f) < 1.0e-6f,
           "static scene import preserves alpha-mask settings for rendering");
}

void testGlbMaterialsBindEmbeddedPngTextures() {
    const std::string material = R"JSON("pbrMetallicRoughness":{"baseColorFactor":[0.8,0.7,0.6,0.5],"baseColorTexture":{"index":0,"extensions":{"KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3]}}},"metallicRoughnessTexture":{"index":0},"metallicFactor":0.3,"roughnessFactor":0.4},"normalTexture":{"index":0},"occlusionTexture":{"index":0,"strength":0.45},"emissiveTexture":{"index":0},"emissiveFactor":[0.1,0.2,0.3],"alphaMode":"BLEND")JSON";
    const std::string glb = makeTexturedTriangleGlb(material);
    std::vector<nuby::GltfPrimitive> primitives;
    std::string error;
    expect(nuby::parseGlbMeshes(glb, primitives, error),
           "GLB material texture slots resolve to embedded PNG images");
    expect(primitives.size() == 1 && primitives[0].hasMaterial
                   && primitives[0].material.baseColorTexture.imageIndex == 0
                   && primitives[0].material.metallicRoughnessTexture.imageIndex == 0
                   && primitives[0].material.normalTexture.imageIndex == 0
                   && primitives[0].material.occlusionTexture.imageIndex == 0
                   && primitives[0].material.emissiveTexture.imageIndex == 0
                   && primitives[0].material.alphaBlend
                   && std::fabs(primitives[0].material.occlusionStrength - 0.45f) < 1.0e-6f
                   && std::fabs(primitives[0].material.baseColorTexture.uvOffset[0] - 0.25f) < 1.0e-6f
                   && std::fabs(primitives[0].material.baseColorTexture.uvOffset[1] - 0.5f) < 1.0e-6f
                   && std::fabs(primitives[0].material.baseColorTexture.uvScale[0] - 2.0f) < 1.0e-6f
                   && std::fabs(primitives[0].material.baseColorTexture.uvScale[1] - 3.0f) < 1.0e-6f,
           "all supported glTF PBR texture references and blend factors are retained");

    nuby::GltfStaticAsset asset;
    expect(nuby::parseGlbStaticAsset(glb, asset, error)
                   && asset.images.size() == 1U && asset.images[0].mimeType == "image/png"
                   && asset.images[0].bytes.size() == 73U
                   && asset.primitives.size() == 1U
                   && std::fabs(asset.primitives[0].material.baseColorTexture.uvScale[0] - 2.0f) < 1.0e-6f
                   && std::fabs(asset.primitives[0].material.baseColorTexture.uvOffset[1] - 0.5f) < 1.0e-6f,
           "static GLB asset exposes embedded PNG bytes and resolved texture transforms");
    if (asset.images.size() == 1U) {
        nuby::ImageRgba8 decoded;
        expect(nuby::decodePng(asset.images[0].bytes, decoded, error)
                       && decoded.width == 1U && decoded.height == 1U
                       && decoded.pixels == std::vector<std::uint8_t>({255U, 32U, 16U, 255U}),
               "embedded GLB PNG bytes decode through the shared engine image subsystem");
    }

    const std::string external = R"JSON("images":[{"uri":"albedo.png"}],"textures":[{"source":0}],)JSON";
    expect(!nuby::parseGlbMeshes(makeTexturedTriangleGlb(material, external), primitives, error),
           "external glTF image URIs are rejected rather than silently dropped");
    const std::string jpeg = R"JSON("images":[{"bufferView":2,"mimeType":"image/jpeg"}],"textures":[{"source":0}],)JSON";
    expect(!nuby::parseGlbMeshes(makeTexturedTriangleGlb(material, jpeg), primitives, error),
           "unsupported JPEG glTF images are explicitly rejected");
    const std::string clampSampler = R"JSON("images":[{"bufferView":2,"mimeType":"image/png"}],"textures":[{"source":0,"sampler":0}],"samplers":[{"wrapS":33071,"wrapT":33071}],)JSON";
    expect(nuby::parseGlbMeshes(makeTexturedTriangleGlb(material, clampSampler), primitives, error)
                   && primitives.size() == 1U
                   && primitives[0].material.baseColorTexture.clampToEdge
                   && primitives[0].material.normalTexture.clampToEdge,
           "glTF CLAMP_TO_EDGE addressing is retained on all material texture slots");
    const std::string unsupportedFilter = R"JSON("images":[{"bufferView":2,"mimeType":"image/png"}],"textures":[{"source":0,"sampler":0}],"samplers":[{"magFilter":9728}],)JSON";
    expect(!nuby::parseGlbMeshes(makeTexturedTriangleGlb(material, unsupportedFilter), primitives, error),
           "unsupported glTF sampler filtering is explicitly rejected");
    expect(!nuby::parseGlbMeshes(makeTexturedTriangleGlb(
                   R"JSON("pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1}})JSON"),
                   primitives, error),
           "glTF material UV sets other than TEXCOORD_0 are rejected");
}

void testStaticGlbTraversesMultipleMeshNodesAndBakesTransforms() {
    nuby::GltfStaticAsset asset;
    std::string error;
    expect(nuby::parseGlbStaticAsset(makeMultiNodeStaticGlb(), asset, error),
           "static GLB traverses selected scene roots and nested mesh nodes");
    expect(asset.primitives.size() == 5
                   && asset.primitives[0].meshIndex == 0
                   && asset.primitives[1].meshIndex == 0
                   && asset.primitives[2].meshIndex == 1
                   && asset.primitives[3].meshIndex == 0,
           "multiple primitives, distinct meshes and repeated mesh instances are preserved");
    if (asset.primitives.size() == 5
            && asset.primitives[0].vertices.size() == 3
            && asset.primitives[2].vertices.size() == 3
            && asset.primitives[3].vertices.size() == 3) {
        const auto& nested = asset.primitives[0].vertices;
        const auto& matrixNode = asset.primitives[2].vertices;
        const auto& instance = asset.primitives[3].vertices;
        expect(std::fabs(nested[0].position[0] - 2.0f) < 1.0e-5f
                       && std::fabs(nested[0].position[1]) < 1.0e-5f
                       && std::fabs(nested[1].position[0] - 2.0f) < 1.0e-4f
                       && std::fabs(nested[1].position[1] - 2.0f) < 1.0e-4f,
               "parent translation, child quaternion rotation and non-uniform scale compose");
        expect(std::fabs(matrixNode[0].position[0] + 4.0f) < 1.0e-5f
                       && std::fabs(matrixNode[0].position[1] - 1.0f) < 1.0e-5f
                       && std::fabs(matrixNode[1].position[0] + 5.0f) < 1.0e-5f
                       && asset.primitives[2].indices == std::vector<std::uint32_t>({0, 2, 1}),
               "matrix-authored reflection is baked with corrected triangle winding");
        expect(std::fabs(instance[0].position[0] - 8.0f) < 1.0e-5f
                       && std::fabs(instance[1].position[0] - 9.0f) < 1.0e-5f,
               "a source mesh can be instanced by multiple nodes independently");
    }
    nuby::GltfStaticAsset cyclic;
    expect(!nuby::parseGlbStaticAsset(makeMultiNodeStaticGlb(true), cyclic, error)
                   && cyclic.primitives.empty(),
           "static importer rejects cyclic node hierarchies without partial output");

    nuby::GltfStaticAsset preserved;
    preserved.primitives.push_back({});
    expect(!nuby::parseGlbStaticAsset(makeSkeletalGlb(), preserved, error)
                   && preserved.primitives.empty(),
           "static importer rejects skinned assets and clears partial output");
}

void testSkeletalGlbImportsJointsBindMatricesAndClips() {
    nuby::GltfSkeletalAsset asset;
    std::string error;
    expect(nuby::parseGlbSkeletalAsset(makeSkeletalGlb(), asset, error),
           "minimal skinned GLB and linear animation import");
    expect(asset.primitives.size() == 1 && asset.skeleton.size() == 2
                   && asset.animations.size() == 1,
           "skeletal GLB returns mesh, hierarchy, and named clip");
    if (asset.skeleton.size() == 2) {
        expect(asset.skeleton[0].name == "root_joint" && asset.skeleton[0].parentIndex == -1
                       && asset.skeleton[1].name == "child_joint"
                       && asset.skeleton[1].parentIndex == 0,
               "unordered skin joints are reordered parent-first");
        expect(asset.skeleton[1].inverseBindMatrix[12] == -1.0f
                       && asset.skeleton[0].inverseBindMatrix[15] == 1.0f,
               "inverse bind matrices follow the skin-to-palette remap");
    }
    if (asset.animations.size() == 1 && asset.skeleton.size() == 2) {
        expect(asset.animations[0].name == "walk"
                       && asset.animations[0].clip.durationSeconds == 1.0f,
               "animation name and duration are imported");
        nuby::SkeletalPose pose;
        expect(nuby::evaluateSkeletalPose(asset.skeleton, asset.animations[0].clip,
                                          0.5f, pose, error),
               "imported glTF animation evaluates using the engine pose core");
        expect(pose.jointMatrices.size() == 2
                       && std::fabs(pose.jointMatrices[0][12] - 0.5f) < 1.0e-5f
                       && std::fabs(pose.jointMatrices[1][12] - 0.5f) < 1.0e-5f,
               "imported animation and inverse bind produce the expected child pose");
    }
    expect(asset.primitives.size() == 1 && asset.primitives[0].hasSkinAttributes
                   && asset.primitives[0].influences[0].jointIndices[0] == 1,
           "mesh skin indices map into the imported skeleton palette");
}

void testSkeletalGlbImportsMultipleIdentityMeshNodes() {
    nuby::GltfSkeletalAsset asset;
    std::string error;
    expect(nuby::parseGlbSkeletalAsset(makeMultipleMeshSkeletalGlb(), asset, error),
           "one skin can drive several identity-root mesh nodes");
    expect(asset.skeleton.size() == 2U && asset.primitives.size() == 2U
                   && asset.primitives[0].meshIndex == 0U
                   && asset.primitives[1].meshIndex == 1U
                   && asset.primitives[0].hasSkinAttributes
                   && asset.primitives[1].hasSkinAttributes,
           "each referenced skeletal mesh contributes independently skinned draw parts");

    nuby::GltfSkeletalAsset rejected;
    rejected.primitives.push_back({});
    expect(!nuby::parseGlbSkeletalAsset(
                   makeMultipleMeshSkeletalGlb(true), rejected, error)
                   && rejected.primitives.empty(),
           "skinned mesh-node transforms remain rejected transactionally");
}

void testSkeletalGlbStepAndCubicSplineInterpolation() {
    for (const auto& testCase : {
            std::pair<std::string, nuby::SkeletalInterpolation>{
                    "STEP", nuby::SkeletalInterpolation::Step},
            std::pair<std::string, nuby::SkeletalInterpolation>{
                    "CUBICSPLINE", nuby::SkeletalInterpolation::CubicSpline}}) {
        nuby::GltfSkeletalAsset asset;
        std::string error;
        expect(nuby::parseGlbSkeletalAsset(
                       makeSkeletalGlbWithInterpolation(testCase.first), asset, error),
               "skeletal GLB accepts STEP and CUBICSPLINE animation channels");
        if (asset.animations.empty() || asset.skeleton.empty()) continue;
        const nuby::SkeletalAnimationTrack& rootTrack = asset.animations[0].clip.joints[0];
        expect(rootTrack.translation.interpolation == testCase.second,
               "glTF sampler interpolation mode is retained per track");
        nuby::SkeletalPose pose;
        expect(nuby::evaluateSkeletalPose(asset.skeleton, asset.animations[0].clip,
                                          0.5f, pose, error),
               "non-linear imported clips evaluate through the shared pose core");
        const float expectedRootX = testCase.first == "STEP" ? 0.0f : 0.625f;
        expect(pose.jointMatrices.size() == 2U
                       && std::fabs(pose.jointMatrices[0][12] - expectedRootX) < 1.0e-5f,
               "STEP holds the left key and cubic Hermite uses authored tangents");
    }
}

void testPackagedAlphaMaskFixture() {
    std::ifstream input("app/src/main/assets/engine-tests/alpha_mask_card.glb", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    expect(input.good() || input.eof(), "packaged alpha-mask validation GLB is readable");
    nuby::GltfStaticAsset asset;
    std::string error;
    expect(nuby::parseGlbStaticAsset(bytes, asset, error),
           "alpha-mask validation GLB parses through the static scene importer");
    expect(asset.primitives.size() == 1U && asset.images.size() == 1U
                   && asset.primitives[0].hasMaterial
                   && asset.primitives[0].material.alphaMask
                   && asset.primitives[0].material.baseColorTexture.imageIndex == 0
                   && asset.primitives[0].material.alphaCutoff == 0.5f,
           "validation scene exposes its embedded texture and default cutout material");
    if (asset.images.size() == 1U) {
        nuby::ImageRgba8 decoded;
        expect(nuby::decodePng(asset.images[0].bytes, decoded, error)
                       && decoded.width == 64U && decoded.height == 96U,
               "validation material image decodes through the engine PNG path");
        if (!decoded.pixels.empty()) {
            bool hasTransparent = false;
            bool hasOpaque = false;
            for (std::size_t pixel = 3U; pixel < decoded.pixels.size(); pixel += 4U) {
                hasTransparent = hasTransparent || decoded.pixels[pixel] == 0U;
                hasOpaque = hasOpaque || decoded.pixels[pixel] == 255U;
            }
            expect(hasTransparent && hasOpaque,
                   "validation PNG contains both cutout and solid fragments");
        }
    }
}

void testPackagedWayfarerAsset() {
    std::ifstream input("app/src/main/assets/characters/wayfarer_skeletal.glb", std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    expect(input.good() || input.eof(), "packaged Wayfarer GLB asset is readable");
    nuby::GltfSkeletalAsset asset;
    std::string error;
    expect(nuby::parseGlbSkeletalAsset(bytes, asset, error),
           "packaged Wayfarer GLB passes the skeletal importer");
    expect(asset.skeleton.size() == 7 && asset.animations.size() == 4
                   && asset.primitives.size() == 6,
           "Wayfarer asset contains its rig, four clips, and six material-separated skinned parts");
    expect(std::all_of(asset.primitives.begin(), asset.primitives.end(),
                       [](const nuby::GltfPrimitive& primitive) {
                           return primitive.hasSkinAttributes && primitive.hasMaterial;
                       }),
           "every Wayfarer outfit part carries a skin and a parsed PBR material");
    const auto clipNamed = [&asset](const char* name) {
        return std::find_if(asset.animations.begin(), asset.animations.end(),
                [name](const nuby::GltfAnimation& animation) {
                    return animation.name == name;
                });
    };
    const auto idle = clipNamed("idle");
    const auto walk = clipNamed("walk");
    const auto run = clipNamed("run");
    const auto jump = clipNamed("jump");
    expect(idle != asset.animations.end() && walk != asset.animations.end()
                   && run != asset.animations.end() && jump != asset.animations.end(),
           "Wayfarer exposes named idle, walk, run, and jump clips");
    if (idle != asset.animations.end() && walk != asset.animations.end()
            && run != asset.animations.end() && jump != asset.animations.end()) {
        nuby::SkeletalPose idlePose;
        nuby::SkeletalPose walkPose;
        nuby::SkeletalPose runPose;
        nuby::SkeletalPose jumpPose;
        expect(nuby::evaluateSkeletalPose(asset.skeleton, idle->clip, 0.25f, idlePose, error)
                       && nuby::evaluateSkeletalPose(asset.skeleton, walk->clip, 0.25f, walkPose, error)
                       && nuby::evaluateSkeletalPose(asset.skeleton, run->clip, 0.25f, runPose, error)
                       && nuby::evaluateSkeletalPose(asset.skeleton, jump->clip, 0.25f, jumpPose, error)
                       && idlePose.jointMatrices.size() == 7
                       && walkPose.jointMatrices.size() == 7
                       && runPose.jointMatrices.size() == 7
                       && jumpPose.jointMatrices.size() == 7,
               "all packaged Wayfarer clips evaluate through the CPU pose core");
        expect(idlePose.jointMatrices[3] != runPose.jointMatrices[3]
                       && walkPose.jointMatrices[5] != runPose.jointMatrices[5]
                       && idlePose.jointMatrices[3] != jumpPose.jointMatrices[3],
               "action clips produce distinct articulated poses");
    }
}

void testGltfJsonSyntaxAndVersionValidation() {
    std::string error;
    expect(nuby::validateGltf2Json("{\"asset\":{\"version\":\"2.0\"}}", error),
           "valid glTF 2.0 JSON metadata is accepted");
    expect(error.empty(), "valid glTF JSON clears previous parse errors");
    expect(!nuby::validateGltf2Json("{\"asset\":{\"version\":\"1.0\"}}", error),
           "unsupported glTF version is rejected");
    expect(!nuby::validateGltf2Json("[]", error), "non-object JSON root is rejected");
    expect(!nuby::validateGltf2Json("{\"asset\":{\"version\":2}}", error),
           "non-string asset version is rejected");
    expect(!nuby::validateGltf2Json("{\"asset\":{\"version\":\"2.0\"},}", error),
           "trailing object comma is rejected");
    expect(!nuby::validateGltf2Json("{\"asset\":{\"version\":\"2.0\"}} false", error),
           "trailing JSON data is rejected");
    expect(!nuby::validateGltf2Json("{\"asset\":{\"version\":\"2.0\",\"version\":\"2.0\"}}", error),
           "duplicate JSON object keys are rejected");
    expect(!nuby::validateGltf2Json("{\"asset\":{\"version\":\"2.0\"},\"bad\":01}", error),
           "JSON numbers with leading zeroes are rejected");
    expect(nuby::validateGltf2Json(
                   "{\"asset\":{\"version\":\"2.0\"},\"name\":\"\\uD83D\\uDE80\"}", error),
           "valid UTF-16 surrogate pairs in JSON escapes are accepted");
    expect(!nuby::validateGltf2Json(
                   "{\"asset\":{\"version\":\"2.0\"},\"name\":\"\\uD83D\"}", error),
           "unpaired JSON surrogates are rejected");
}

void testGlbRequiresOneLeadingJsonChunk() {
    std::string json;
    std::string binary;
    std::string error;
    std::string noJson;
    appendU32(noJson, 0x46546C67U);
    appendU32(noJson, 2U);
    appendU32(noJson, 0U);
    appendChunk(noJson, 0x004E4942U, "ABCD");
    setU32(noJson, 8, static_cast<std::uint32_t>(noJson.size()));
    expect(!nuby::parseGlbContainer(noJson, json, binary, error),
           "BIN chunk cannot precede the required JSON chunk");

    std::string duplicateJson;
    appendU32(duplicateJson, 0x46546C67U);
    appendU32(duplicateJson, 2U);
    appendU32(duplicateJson, 0U);
    appendChunk(duplicateJson, 0x4E4F534AU, "{}");
    appendChunk(duplicateJson, 0x4E4F534AU, "{}");
    setU32(duplicateJson, 8, static_cast<std::uint32_t>(duplicateJson.size()));
    expect(!nuby::parseGlbContainer(duplicateJson, json, binary, error),
           "duplicate JSON chunks are rejected");

    std::string duplicateBin;
    appendU32(duplicateBin, 0x46546C67U);
    appendU32(duplicateBin, 2U);
    appendU32(duplicateBin, 0U);
    appendChunk(duplicateBin, 0x4E4F534AU, "{}");
    appendChunk(duplicateBin, 0x004E4942U, "ABCD");
    appendChunk(duplicateBin, 0x004E4942U, "EFGH");
    setU32(duplicateBin, 8, static_cast<std::uint32_t>(duplicateBin.size()));
    expect(!nuby::parseGlbContainer(duplicateBin, json, binary, error),
           "duplicate BIN chunks are rejected");

    std::string binaryOutOfOrder;
    appendU32(binaryOutOfOrder, 0x46546C67U);
    appendU32(binaryOutOfOrder, 2U);
    appendU32(binaryOutOfOrder, 0U);
    appendChunk(binaryOutOfOrder, 0x4E4F534AU, "{}");
    appendChunk(binaryOutOfOrder, 0x12345678U, "ABCD");
    appendChunk(binaryOutOfOrder, 0x004E4942U, "EFGH");
    setU32(binaryOutOfOrder, 8, static_cast<std::uint32_t>(binaryOutOfOrder.size()));
    expect(!nuby::parseGlbContainer(binaryOutOfOrder, json, binary, error),
           "BIN must precede any optional extension chunks");
}
}  // namespace

int main() {
    testGlbChunksExtractAndTrimJsonPadding();
    testGlbRejectsInvalidHeadersAndLengths();
    testGlbImportsTriangleAndSkinAttributes();
    testGltfPbrMaterialFactors();
    testGlbAlphaMaskMaterials();
    testGlbMaterialsBindEmbeddedPngTextures();
    testStaticGlbTraversesMultipleMeshNodesAndBakesTransforms();
    testSkeletalGlbImportsJointsBindMatricesAndClips();
    testSkeletalGlbImportsMultipleIdentityMeshNodes();
    testSkeletalGlbStepAndCubicSplineInterpolation();
    testPackagedAlphaMaskFixture();
    testPackagedWayfarerAsset();
    testGltfJsonSyntaxAndVersionValidation();
    testGlbRequiresOneLeadingJsonChunk();
    if (failures != 0) {
        std::cerr << failures << " glTF loader test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All glTF loader tests passed\n";
    return EXIT_SUCCESS;
}
