#include "nuby/gltf_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
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
                   && asset.primitives.size() == 1,
           "Wayfarer asset contains its rig, four action clips, and skinned mesh");
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
    testSkeletalGlbImportsJointsBindMatricesAndClips();
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
