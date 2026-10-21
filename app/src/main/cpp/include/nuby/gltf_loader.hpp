#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nuby/obj_loader.hpp"
#include "nuby/skeletal_animation.hpp"

namespace nuby {

struct GltfTextureBinding {
    std::int32_t imageIndex = -1;
    std::array<float, 2> uvScale{1.0f, 1.0f};
    std::array<float, 2> uvOffset{0.0f, 0.0f};
    float uvRotation = 0.0f;
    std::uint8_t uvSet = 0U;
    bool clampToEdge = false;
};

struct GltfMaterial {
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> emissiveFactor{0.0f, 0.0f, 0.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float occlusionStrength = 1.0f;
    float normalScale = 1.0f;
    float emissiveStrength = 1.0f;
    float indexOfRefraction = 1.5f;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughness = 0.0f;
    float clearcoatNormalScale = 1.0f;
    bool unlit = false;
    GltfTextureBinding clearcoatTexture;
    GltfTextureBinding clearcoatRoughnessTexture;
    GltfTextureBinding clearcoatNormalTexture;
    GltfTextureBinding baseColorTexture;
    GltfTextureBinding metallicRoughnessTexture;
    GltfTextureBinding normalTexture;
    GltfTextureBinding occlusionTexture;
    GltfTextureBinding emissiveTexture;
    float alphaCutoff = 0.5f;
    bool alphaBlend = false;
    bool alphaMask = false;
};

struct GltfEmbeddedImage {
    std::string mimeType;
    std::string bytes;
};

struct GltfPrimitive {
    std::uint32_t meshIndex = 0;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SkeletalVertexInfluence> influences;
    std::vector<std::array<float, 4>> tangents;
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
    std::vector<GltfEmbeddedImage> images;
};

struct GltfSkeletalAsset {
    std::vector<GltfPrimitive> primitives;
    std::vector<GltfEmbeddedImage> images;
    // Parent-first pose palette; may include unweighted TRS ancestor nodes.
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
// buffer. Supports float POSITION/NORMAL/TANGENT/TEXCOORD_0/1, unsigned scalar
// indices, optional JOINTS_0/WEIGHTS_0 with four influences, glTF PBR factors and the
// KHR_materials_unlit, KHR_materials_emissive_strength, KHR_materials_ior and
// KHR_materials_clearcoat extensions. Standard
// texture slots select UV set 0 or 1; KHR_texture_transform offset/scale/rotation
// and its texCoord override are retained. Higher-level APIs return encoded image
// bytes. External images, non-PNG formats, custom filters and UV sets above 1
// are rejected.
bool parseGlbMeshes(std::string_view bytes, std::vector<GltfPrimitive>& output,
                    std::string& error);

// Imports static meshes in the selected scene, traversing arbitrary acyclic
// node hierarchies and baking each mesh-node transform into a distinct part.
// Matrix-authored and TRS nodes, instancing, and multiple mesh primitives are
// supported; skinned, animated, and morph-target assets are explicitly rejected.
bool parseGlbStaticAsset(std::string_view bytes, GltfStaticAsset& output,
                         std::string& error);

// Imports a constrained single-skin GLB subset with multiple mesh nodes,
// parent-first skin joints, TRS intermediary ancestors, inverse-bind matrices,
// per-vertex weights, embedded PNG PBR textures and LINEAR, STEP or CUBICSPLINE
// animation. Intermediary ancestors are unweighted palette entries; affine
// matrix-authored ancestors are preserved as static transforms. Mesh-node
// transforms follow the glTF skin-space convention and are not baked into the
// vertices. The pose palette is limited to 64 entries; matrix-node animation
// targets and multiple skins are rejected.
bool parseGlbSkeletalAsset(std::string_view bytes, GltfSkeletalAsset& output,
                           std::string& error);

}  // namespace nuby
