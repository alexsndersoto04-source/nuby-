#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nuby/obj_loader.hpp"
#include "nuby/skeletal_animation.hpp"

namespace nuby {

struct GltfMaterial {
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> emissiveFactor{0.0f, 0.0f, 0.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    bool alphaBlend = false;
};

struct GltfPrimitive {
    std::uint32_t meshIndex = 0;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SkeletalVertexInfluence> influences;
    GltfMaterial material;
    bool hasMaterial = false;
    bool hasSkinAttributes = false;
};

struct GltfAnimation {
    std::string name;
    SkeletalAnimationClip clip;
};

struct GltfStaticAsset {
    // One transformed primitive per visible mesh-node/primitive instance in the
    // selected glTF scene. Node transforms are baked into positions and normals.
    std::vector<GltfPrimitive> primitives;
};

struct GltfSkeletalAsset {
    std::vector<GltfPrimitive> primitives;
    std::vector<SkeletalJoint> skeleton;
    std::vector<GltfAnimation> animations;
};

// Extracts the JSON and optional BIN chunks from a glTF 2.0 binary container
// (GLB). This is deliberately only the container layer; JSON accessors, skins,
// and animations are decoded by the higher-level importer.
bool parseGlbContainer(std::string_view bytes, std::string& json,
                       std::string& binary, std::string& error);

// Validates JSON syntax and the required glTF 2.0 asset.version field.
bool validateGltf2Json(std::string_view json, std::string& error);

// Imports triangle primitives from a self-contained GLB with one embedded
// buffer. Supports float POSITION/NORMAL/TEXCOORD_0, unsigned scalar indices,
// optional JOINTS_0/WEIGHTS_0 with four influences, and per-primitive glTF
// metallic-roughness material factors. Image textures and MASK alpha are
// rejected until their renderer paths are implemented.
bool parseGlbMeshes(std::string_view bytes, std::vector<GltfPrimitive>& output,
                    std::string& error);

// Imports static meshes in the selected scene, traversing arbitrary acyclic
// node hierarchies and baking each mesh-node transform into a distinct part.
// Matrix-authored and TRS nodes, instancing, and multiple mesh primitives are
// supported; skinned, animated, and morph-target assets are explicitly rejected.
bool parseGlbStaticAsset(std::string_view bytes, GltfStaticAsset& output,
                         std::string& error);

// Imports a constrained single-mesh-node, single-skin GLB subset, including
// multiple triangle primitives, parent-first joints, inverse-bind matrices,
// per-vertex weights, PBR material factors and LINEAR TRS animation channels.
// Unsupported hierarchy, image textures and interpolation forms are rejected
// rather than silently discarded.
bool parseGlbSkeletalAsset(std::string_view bytes, GltfSkeletalAsset& output,
                           std::string& error);

}  // namespace nuby
