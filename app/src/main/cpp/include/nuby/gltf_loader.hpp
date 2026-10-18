#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nuby/obj_loader.hpp"
#include "nuby/skeletal_animation.hpp"

namespace nuby {

struct GltfPrimitive {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SkeletalVertexInfluence> influences;
    bool hasSkinAttributes = false;
};

struct GltfAnimation {
    std::string name;
    SkeletalAnimationClip clip;
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
// and JOINTS_0/WEIGHTS_0 with four influences. Skeletons, materials and clips
// are not decoded yet.
bool parseGlbMeshes(std::string_view bytes, std::vector<GltfPrimitive>& output,
                    std::string& error);

// Imports a constrained single-mesh, single-skin GLB subset, including
// parent-first joints, inverse-bind matrices, per-vertex weights and LINEAR
// translation/rotation/scale channels. Unsupported hierarchy and interpolation
// forms are rejected rather than silently discarded.
bool parseGlbSkeletalAsset(std::string_view bytes, GltfSkeletalAsset& output,
                           std::string& error);

}  // namespace nuby
