#include "nuby/renderer.hpp"
#include "nuby/scene_io.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#include <android/log.h>

namespace nuby {
using Vertex = MeshVertex;

namespace {

constexpr const char* kLogTag = "NubyEngine";
constexpr float kPi = 3.14159265358979323846f;
constexpr std::uint64_t kMaxGpuAssetUploadBytesPerTurn = 512U * 1024U;

using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

Mat4 identity() {
    Mat4 value{};
    value[0] = value[5] = value[10] = value[15] = 1.0f;
    return value;
}

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 4; ++k) {
                result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
            }
        }
    }
    return result;
}

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

float dot(const Vec3& a, const Vec3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 normalize(const Vec3& value) {
    const float length = std::sqrt(std::max(dot(value, value), 0.000001f));
    return {value[0] / length, value[1] / length, value[2] / length};
}

Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 forward = normalize(subtract(center, eye));
    const Vec3 side = normalize(cross(forward, up));
    const Vec3 correctedUp = cross(side, forward);

    Mat4 result = identity();
    result[0] = side[0];
    result[4] = side[1];
    result[8] = side[2];
    result[1] = correctedUp[0];
    result[5] = correctedUp[1];
    result[9] = correctedUp[2];
    result[2] = -forward[0];
    result[6] = -forward[1];
    result[10] = -forward[2];
    result[12] = -dot(side, eye);
    result[13] = -dot(correctedUp, eye);
    result[14] = dot(forward, eye);
    return result;
}

Mat4 perspective(float verticalFov, float aspect, float nearPlane, float farPlane) {
    Mat4 result{};
    const float f = 1.0f / std::tan(verticalFov * 0.5f);
    result[0] = f / aspect;
    result[5] = f;
    result[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
    result[11] = -1.0f;
    result[14] = (2.0f * farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

void appendTriangle(std::vector<MeshVertex>& vertices,
                    const MeshVertex& a,
                    const MeshVertex& b,
                    const MeshVertex& c) {
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
}

bool safeRelativeAssetPath(const std::string& path) {
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

bool resolveAssetReference(const std::string& baseAssetPath,
                           const std::string& relativePath,
                           std::string& resolvedPath) {
    if (!safeRelativeAssetPath(baseAssetPath) || !safeRelativeAssetPath(relativePath)) {
        return false;
    }
    const std::size_t slash = baseAssetPath.find_last_of('/');
    if (slash == std::string::npos) resolvedPath = relativePath;
    else resolvedPath = baseAssetPath.substr(0, slash + 1) + relativePath;
    return safeRelativeAssetPath(resolvedPath);
}

void combineHash(std::size_t& seed, std::size_t value) {
    seed ^= value + static_cast<std::size_t>(0x9e3779b9U) + (seed << 6U) + (seed >> 2U);
}

template <typename T>
void hashValue(std::size_t& seed, const T& value) {
    combineHash(seed, std::hash<T>{}(value));
}

std::array<float, 2> textureRotationPair(float radians) {
    return {std::cos(radians), std::sin(radians)};
}

std::size_t materialBatchHash(const Material& material) {
    std::size_t hash = 0;
    for (float value : material.color) hashValue(hash, value);
    for (float value : material.emission) hashValue(hash, value);
    hashValue(hash, material.metallic);
    hashValue(hash, material.roughness);
    hashValue(hash, material.opacity);
    hashValue(hash, material.ambientOcclusion);
    hashValue(hash, material.normalMapScale);
    hashValue(hash, material.emissiveStrength);
    hashValue(hash, material.indexOfRefraction);
    hashValue(hash, material.clearcoatFactor);
    hashValue(hash, material.clearcoatRoughness);
    hashValue(hash, material.clearcoatNormalScale);
    hashValue(hash, material.specularFactor);
    for (float component : material.specularColorFactor) hashValue(hash, component);
    for (float component : material.sheenColorFactor) hashValue(hash, component);
    hashValue(hash, material.sheenRoughness);
    hashValue(hash, material.unlit);
    hashValue(hash, material.proceduralGrid);
    const std::array<const std::string*, 15> texturePaths{{
            &material.albedoTexture, &material.normalTexture,
            &material.metallicRoughnessTexture, &material.metallicTexture,
            &material.roughnessTexture, &material.occlusionTexture,
            &material.opacityTexture, &material.emissiveTexture,
            &material.clearcoatTexture, &material.clearcoatRoughnessTexture,
            &material.clearcoatNormalTexture, &material.specularTexture,
            &material.specularColorTexture, &material.sheenColorTexture,
            &material.sheenRoughnessTexture}};
    for (const std::string* path : texturePaths) hashValue(hash, *path);
    const std::array<const TextureMapTransform*, 15> transforms{{
            &material.albedoMapTransform, &material.normalMapTransform,
            &material.metallicRoughnessMapTransform, &material.metallicMapTransform,
            &material.roughnessMapTransform, &material.occlusionMapTransform,
            &material.opacityMapTransform, &material.emissiveMapTransform,
            &material.clearcoatMapTransform, &material.clearcoatRoughnessMapTransform,
            &material.clearcoatNormalMapTransform, &material.specularMapTransform,
            &material.specularColorMapTransform, &material.sheenColorMapTransform,
            &material.sheenRoughnessMapTransform}};
    for (const TextureMapTransform* transform : transforms) {
        for (float value : transform->uvScale) hashValue(hash, value);
        for (float value : transform->uvOffset) hashValue(hash, value);
        hashValue(hash, transform->uvRotation);
        hashValue(hash, transform->clampToEdge);
        hashValue(hash, transform->uvSet);
    }
    const std::array<const TextureMapValueRange*, 8> ranges{{
            &material.albedoMapValueRange, &material.normalMapValueRange,
            &material.metallicRoughnessMapValueRange, &material.metallicMapValueRange,
            &material.roughnessMapValueRange, &material.occlusionMapValueRange,
            &material.opacityMapValueRange, &material.emissiveMapValueRange}};
    for (const TextureMapValueRange* range : ranges) {
        hashValue(hash, range->base);
        hashValue(hash, range->gain);
    }
    hashValue(hash, static_cast<unsigned int>(material.opacityMapChannel));
    return hash;
}

bool sameMaterial(const Material& left, const Material& right) {
    const auto sameTransform = [](const TextureMapTransform& a,
                                  const TextureMapTransform& b) {
        return a.uvScale == b.uvScale && a.uvOffset == b.uvOffset
                && a.uvRotation == b.uvRotation && a.clampToEdge == b.clampToEdge
                && a.uvSet == b.uvSet;
    };
    const auto sameRange = [](const TextureMapValueRange& a,
                              const TextureMapValueRange& b) {
        return a.base == b.base && a.gain == b.gain;
    };
    return left.color == right.color && left.emission == right.emission
            && left.metallic == right.metallic && left.roughness == right.roughness
            && left.opacity == right.opacity
            && left.ambientOcclusion == right.ambientOcclusion
            && left.normalMapScale == right.normalMapScale
            && left.emissiveStrength == right.emissiveStrength
            && left.indexOfRefraction == right.indexOfRefraction
            && left.clearcoatFactor == right.clearcoatFactor
            && left.clearcoatRoughness == right.clearcoatRoughness
            && left.clearcoatNormalScale == right.clearcoatNormalScale
            && left.specularFactor == right.specularFactor
            && left.specularColorFactor == right.specularColorFactor
            && left.sheenColorFactor == right.sheenColorFactor
            && left.sheenRoughness == right.sheenRoughness
            && left.unlit == right.unlit
            && left.proceduralGrid == right.proceduralGrid
            && left.albedoTexture == right.albedoTexture
            && left.normalTexture == right.normalTexture
            && left.metallicRoughnessTexture == right.metallicRoughnessTexture
            && left.metallicTexture == right.metallicTexture
            && left.roughnessTexture == right.roughnessTexture
            && left.occlusionTexture == right.occlusionTexture
            && left.opacityTexture == right.opacityTexture
            && left.emissiveTexture == right.emissiveTexture
            && left.clearcoatTexture == right.clearcoatTexture
            && left.clearcoatRoughnessTexture == right.clearcoatRoughnessTexture
            && left.clearcoatNormalTexture == right.clearcoatNormalTexture
            && left.specularTexture == right.specularTexture
            && left.specularColorTexture == right.specularColorTexture
            && left.sheenColorTexture == right.sheenColorTexture
            && left.sheenRoughnessTexture == right.sheenRoughnessTexture
            && sameTransform(left.albedoMapTransform, right.albedoMapTransform)
            && sameTransform(left.normalMapTransform, right.normalMapTransform)
            && sameTransform(left.metallicRoughnessMapTransform,
                             right.metallicRoughnessMapTransform)
            && sameTransform(left.metallicMapTransform, right.metallicMapTransform)
            && sameTransform(left.roughnessMapTransform, right.roughnessMapTransform)
            && sameTransform(left.occlusionMapTransform, right.occlusionMapTransform)
            && sameTransform(left.opacityMapTransform, right.opacityMapTransform)
            && sameTransform(left.emissiveMapTransform, right.emissiveMapTransform)
            && sameTransform(left.clearcoatMapTransform, right.clearcoatMapTransform)
            && sameTransform(left.clearcoatRoughnessMapTransform,
                             right.clearcoatRoughnessMapTransform)
            && sameTransform(left.clearcoatNormalMapTransform,
                             right.clearcoatNormalMapTransform)
            && sameTransform(left.specularMapTransform, right.specularMapTransform)
            && sameTransform(left.specularColorMapTransform,
                             right.specularColorMapTransform)
            && sameTransform(left.sheenColorMapTransform, right.sheenColorMapTransform)
            && sameTransform(left.sheenRoughnessMapTransform,
                             right.sheenRoughnessMapTransform)
            && sameRange(left.albedoMapValueRange, right.albedoMapValueRange)
            && sameRange(left.normalMapValueRange, right.normalMapValueRange)
            && sameRange(left.metallicRoughnessMapValueRange,
                         right.metallicRoughnessMapValueRange)
            && sameRange(left.metallicMapValueRange, right.metallicMapValueRange)
            && sameRange(left.roughnessMapValueRange, right.roughnessMapValueRange)
            && sameRange(left.occlusionMapValueRange, right.occlusionMapValueRange)
            && sameRange(left.opacityMapValueRange, right.opacityMapValueRange)
            && sameRange(left.emissiveMapValueRange, right.emissiveMapValueRange)
            && left.opacityMapChannel == right.opacityMapChannel;
}

void bindInstanceAttributes(GLuint buffer,
                            const std::vector<std::array<float, 16>>& models) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(models.size() * sizeof(models.front())),
            models.data(), GL_STREAM_DRAW);
    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(std::array<float, 16>));
    for (GLuint column = 0; column < 4; ++column) {
        const GLuint location = 3U + column;
        const std::uintptr_t offset = static_cast<std::uintptr_t>(column)
                * 4U * sizeof(float);
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, stride,
                reinterpret_cast<const void*>(offset));
        glVertexAttribDivisor(location, 1);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void unbindInstanceAttributes() {
    for (GLuint location = 3; location < 7; ++location) {
        glVertexAttribDivisor(location, 0);
        glDisableVertexAttribArray(location);
    }
}

bool readTextAsset(AAssetManager* manager, const std::string& assetPath,
                   std::size_t maximumBytes, std::string& output) {
    output.clear();
    if (manager == nullptr || !safeRelativeAssetPath(assetPath)) return false;
    AAsset* asset = AAssetManager_open(manager, assetPath.c_str(), AASSET_MODE_BUFFER);
    if (asset == nullptr) return false;
    const off_t length = AAsset_getLength(asset);
    if (length <= 0 || static_cast<std::uint64_t>(length) > maximumBytes) {
        AAsset_close(asset);
        return false;
    }
    output.resize(static_cast<std::size_t>(length));
    std::size_t bytesRead = 0;
    while (bytesRead < output.size()) {
        const int count = AAsset_read(asset, output.data() + bytesRead,
                                      output.size() - bytesRead);
        if (count <= 0) break;
        bytesRead += static_cast<std::size_t>(count);
    }
    AAsset_close(asset);
    if (bytesRead != output.size()) {
        output.clear();
        return false;
    }
    return true;
}

constexpr const char* kVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 9) in vec2 aUv1;
layout(location = 10) in vec4 aTangent;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in uvec4 aJointIndices;
layout(location = 8) in vec4 aJointWeights;
layout(std140) uniform SkinPalette { mat4 uJointMatrices[64]; };
uniform mat4 uViewProjection;
uniform mat4 uView;
uniform mat4 uModel;
uniform int uUseInstancing;
uniform int uSkinningEnabled;
// Match the fragment stage; GLES integer precision defaults differ by stage.
uniform highp int uHasTangents;
out vec3 vWorldPosition;
out vec3 vWorldNormal;
out vec2 vUv;
out vec2 vUv1;
out vec4 vWorldTangent;
out float vViewDepth;
void main() {
    mat4 model = uUseInstancing != 0 ? aInstanceModel : uModel;
    vec4 localPosition = vec4(aPosition, 1.0);
    vec3 localNormal = aNormal;
    vec3 localTangent = vec3(0.0);
    float tangentHandedness = aTangent.w;
    if (uSkinningEnabled != 0) {
        mat4 skin = mat4(0.0);
        skin += uJointMatrices[int(aJointIndices.x)] * aJointWeights.x;
        skin += uJointMatrices[int(aJointIndices.y)] * aJointWeights.y;
        skin += uJointMatrices[int(aJointIndices.z)] * aJointWeights.z;
        skin += uJointMatrices[int(aJointIndices.w)] * aJointWeights.w;
        localPosition = skin * localPosition;
        localNormal = mat3(skin) * localNormal;
        if (uHasTangents != 0) {
            localTangent = mat3(skin) * aTangent.xyz;
            if (determinant(mat3(skin)) < 0.0) tangentHandedness = -tangentHandedness;
        }
    } else if (uHasTangents != 0) {
        localTangent = aTangent.xyz;
    }
    vec4 world = model * localPosition;
    vWorldPosition = world.xyz;
    vWorldNormal = mat3(model) * localNormal;
    vWorldTangent = vec4(0.0, 0.0, 0.0, 1.0);
    if (uHasTangents != 0) {
        float modelHandedness = determinant(mat3(model)) < 0.0 ? -1.0 : 1.0;
        vWorldTangent = vec4(mat3(model) * localTangent,
                tangentHandedness * modelHandedness);
    }
    vUv = aUv;
    vUv1 = aUv1;
    vViewDepth = -(uView * world).z;
    gl_Position = uViewProjection * world;
}
)";

constexpr const char* kFragmentShader = R"(
#version 300 es
precision highp float;
in vec3 vWorldPosition;
in vec3 vWorldNormal;
in vec4 vWorldTangent;
in vec2 vUv;
in vec2 vUv1;
in float vViewDepth;
uniform vec3 uColor;
uniform vec3 uEmission;
uniform float uMetallic;
uniform float uRoughness;
uniform float uOpacity;
uniform int uAlphaMaskEnabled;
uniform float uAlphaCutoff;
uniform float uNormalMapScale;
uniform float uEmissiveStrength;
uniform float uIndexOfRefraction;
uniform float uClearcoatFactor;
uniform float uClearcoatRoughness;
uniform float uClearcoatNormalScale;
uniform float uSpecularFactor;
uniform vec3 uSpecularColorFactor;
uniform float uSheenRoughnessFactor;
uniform vec3 uSheenColorFactor;
uniform int uUnlit;
uniform highp int uHasTangents;
uniform float uAmbientOcclusion;
uniform sampler2D uAlbedoTexture;
uniform sampler2D uNormalTexture;
uniform sampler2D uMetallicRoughnessTexture;
uniform sampler2D uMetallicTexture;
uniform sampler2D uRoughnessTexture;
uniform sampler2D uOcclusionTexture;
uniform sampler2D uOpacityTexture;
uniform sampler2D uEmissiveTexture;
uniform sampler2D uClearcoatTexture;
uniform sampler2D uClearcoatRoughnessTexture;
uniform sampler2D uClearcoatNormalTexture;
uniform sampler2D uSpecularTexture;
uniform sampler2D uSpecularColorTexture;
uniform sampler2D uSheenColorTexture;
uniform sampler2D uSheenRoughnessTexture;
uniform vec2 uAlbedoUvScale;
uniform vec2 uAlbedoUvOffset;
uniform vec2 uAlbedoUvRotation;
uniform vec2 uNormalUvScale;
uniform vec2 uNormalUvOffset;
uniform vec2 uNormalUvRotation;
uniform vec2 uMetallicRoughnessUvScale;
uniform vec2 uMetallicRoughnessUvOffset;
uniform vec2 uMetallicRoughnessUvRotation;
uniform vec2 uMetallicUvScale;
uniform vec2 uMetallicUvOffset;
uniform vec2 uMetallicUvRotation;
uniform vec2 uRoughnessUvScale;
uniform vec2 uRoughnessUvOffset;
uniform vec2 uRoughnessUvRotation;
uniform vec2 uOcclusionUvScale;
uniform vec2 uOcclusionUvOffset;
uniform vec2 uOcclusionUvRotation;
uniform vec2 uOpacityUvScale;
uniform vec2 uOpacityUvOffset;
uniform vec2 uOpacityUvRotation;
uniform vec2 uEmissiveUvScale;
uniform vec2 uEmissiveUvOffset;
uniform vec2 uEmissiveUvRotation;
uniform vec2 uClearcoatUvScale;
uniform vec2 uClearcoatUvOffset;
uniform vec2 uClearcoatUvRotation;
uniform vec2 uClearcoatRoughnessUvScale;
uniform vec2 uClearcoatRoughnessUvOffset;
uniform vec2 uClearcoatRoughnessUvRotation;
uniform vec2 uClearcoatNormalUvScale;
uniform vec2 uClearcoatNormalUvOffset;
uniform vec2 uClearcoatNormalUvRotation;
uniform vec2 uSpecularUvScale;
uniform vec2 uSpecularUvOffset;
uniform vec2 uSpecularUvRotation;
uniform vec2 uSpecularColorUvScale;
uniform vec2 uSpecularColorUvOffset;
uniform vec2 uSpecularColorUvRotation;
uniform vec2 uSheenColorUvScale;
uniform vec2 uSheenColorUvOffset;
uniform vec2 uSheenColorUvRotation;
uniform vec2 uSheenRoughnessUvScale;
uniform vec2 uSheenRoughnessUvOffset;
uniform vec2 uSheenRoughnessUvRotation;
uniform int uTextureUvSet[15];
uniform int uOpacityMapChannel;
uniform vec2 uAlbedoMapBaseGain;
uniform vec2 uNormalMapBaseGain;
uniform vec2 uMetallicRoughnessMapBaseGain;
uniform vec2 uMetallicMapBaseGain;
uniform vec2 uRoughnessMapBaseGain;
uniform vec2 uOcclusionMapBaseGain;
uniform vec2 uOpacityMapBaseGain;
uniform vec2 uEmissiveMapBaseGain;
uniform int uHasAlbedoTexture;
uniform int uHasNormalTexture;
uniform int uHasMetallicRoughnessTexture;
uniform int uHasMetallicTexture;
uniform int uHasRoughnessTexture;
uniform int uHasOcclusionTexture;
uniform int uHasOpacityTexture;
uniform int uHasEmissiveTexture;
uniform int uHasClearcoatTexture;
uniform int uHasClearcoatRoughnessTexture;
uniform int uHasClearcoatNormalTexture;
uniform int uHasSpecularTexture;
uniform int uHasSpecularColorTexture;
uniform int uHasSheenColorTexture;
uniform int uHasSheenRoughnessTexture;
uniform vec3 uCameraPosition;
uniform vec3 uLightDirection;
uniform int uFogEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uSkyAmbientStrength;
uniform vec3 uSkyZenithColor;
uniform vec3 uSkyHorizonColor;
uniform vec3 uSkyGroundColor;
uniform vec3 uDirectionalLightColor;
uniform float uDirectionalLightIntensity;
uniform float uShadowStrength;
#ifndef NUBY_DISABLE_CSM
uniform sampler2D uShadowMap;
uniform mat4 uLightViewProjection0;
uniform mat4 uLightViewProjection1;
uniform mat4 uLightViewProjection2;
uniform vec3 uCascadeFarSplits;
#endif
uniform int uShadowsEnabled;
uniform int uGrid;
uniform int uPointLightCount;
uniform vec4 uPointLightPositionRange[4];
uniform vec4 uPointLightColorIntensity[4];
uniform vec3 uAmbientSH[9];
out vec4 fragmentColor;

vec3 srgbToLinear(vec3 color) {
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(low, high, step(vec3(0.04045), color));
}

vec3 adjustTextureValues(vec3 sampleValue, vec2 baseGain) {
    return clamp(vec3(baseGain.x) + sampleValue * baseGain.y, 0.0, 1.0);
}

float adjustTextureValue(float sampleValue, vec2 baseGain) {
    return clamp(baseGain.x + sampleValue * baseGain.y, 0.0, 1.0);
}

#ifdef NUBY_DISABLE_CSM
float shadowAmount(vec3 normal, vec3 lightDirection) {
    return 0.0;
}
#else
float shadowAmount(vec3 normal, vec3 lightDirection) {
    if (uShadowsEnabled == 0 || vViewDepth > uCascadeFarSplits.z) return 0.0;
    int cascade = vViewDepth <= uCascadeFarSplits.x ? 0
            : (vViewDepth <= uCascadeFarSplits.y ? 1 : 2);
    vec4 worldPosition = vec4(vWorldPosition, 1.0);
    vec4 lightPosition;
    if (cascade == 0) {
        lightPosition = uLightViewProjection0 * worldPosition;
    } else if (cascade == 1) {
        lightPosition = uLightViewProjection1 * worldPosition;
    } else {
        lightPosition = uLightViewProjection2 * worldPosition;
    }
    vec3 projected = lightPosition.xyz / max(lightPosition.w, 0.0001);
    projected = projected * 0.5 + 0.5;
    if (projected.x < 0.0 || projected.x > 1.0 ||
        projected.y < 0.0 || projected.y > 1.0 ||
        projected.z < 0.0 || projected.z > 1.0) return 0.0;

    float bias = max(0.0015 * (1.0 - dot(normal, lightDirection)), 0.00045);
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float cascadeWidth = 1.0 / 3.0;
    vec2 atlasUv = vec2((float(cascade) + projected.x) * cascadeWidth,
                        projected.y);
    vec2 atlasMinimum = vec2(float(cascade) * cascadeWidth + texel.x * 0.5,
                             texel.y * 0.5);
    vec2 atlasMaximum = vec2(float(cascade + 1) * cascadeWidth - texel.x * 0.5,
                             1.0 - texel.y * 0.5);
    float occlusion = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUv = clamp(atlasUv + vec2(float(x), float(y)) * texel,
                                  atlasMinimum, atlasMaximum);
            float closestDepth = texture(uShadowMap, sampleUv).r;
            occlusion += projected.z - bias > closestDepth ? 1.0 : 0.0;
        }
    }
    return occlusion / 9.0;
}
#endif

float distributionGGX(float normalHalf, float roughness) {
    const float pi = 3.14159265;
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = normalHalf * normalHalf * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(pi * denominator * denominator, 0.001);
}

float geometrySchlickGGX(float normalDirection, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) * 0.125;
    return normalDirection / max(normalDirection * (1.0 - k) + k, 0.001);
}

vec3 fresnelSchlick(float viewHalf, vec3 baseReflectance) {
    return baseReflectance + (1.0 - baseReflectance) * pow(1.0 - viewHalf, 5.0);
}

vec3 evaluateAmbientSH(vec3 normal) {
    return uAmbientSH[0] * 0.282095
         + uAmbientSH[1] * (0.488603 * normal.y)
         + uAmbientSH[2] * (0.488603 * normal.z)
         + uAmbientSH[3] * (0.488603 * normal.x)
         + uAmbientSH[4] * (1.092548 * normal.x * normal.y)
         + uAmbientSH[5] * (1.092548 * normal.y * normal.z)
         + uAmbientSH[6] * (0.315392 * (3.0 * normal.z * normal.z - 1.0))
         + uAmbientSH[7] * (1.092548 * normal.x * normal.z)
         + uAmbientSH[8] * (0.546274 * (normal.x * normal.x - normal.y * normal.y));
}

vec3 evaluateDirectLight(vec3 normal, vec3 viewDirection, vec3 lightDirection,
                         vec3 baseColor, float metallic, float roughness,
                         vec3 dielectricReflectance, vec3 radiance) {
    float normalLight = max(dot(normal, lightDirection), 0.0);
    if (normalLight <= 0.0) return vec3(0.0);

    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float normalView = max(dot(normal, viewDirection), 0.001);
    float normalHalf = max(dot(normal, halfDirection), 0.0);
    float viewHalf = max(dot(viewDirection, halfDirection), 0.0);
    vec3 baseReflectance = mix(dielectricReflectance, baseColor, metallic);
    vec3 fresnel = fresnelSchlick(viewHalf, baseReflectance);
    float distribution = distributionGGX(normalHalf, roughness);
    float geometryTerm = geometrySchlickGGX(normalView, roughness)
                       * geometrySchlickGGX(max(normalLight, 0.001), roughness);
    vec3 specular = (distribution * geometryTerm * fresnel)
                  / max(4.0 * normalView * max(normalLight, 0.001), 0.001);
    vec3 diffuseWeight = (vec3(1.0) - fresnel) * (1.0 - metallic);
    return (diffuseWeight * baseColor / 3.14159265 + specular)
            * radiance * normalLight;
}

vec3 evaluateClearcoatDirectLight(vec3 normal, vec3 viewDirection,
                                  vec3 lightDirection, float roughness,
                                  vec3 radiance) {
    float normalLight = max(dot(normal, lightDirection), 0.0);
    if (normalLight <= 0.0) return vec3(0.0);
    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float normalView = max(dot(normal, viewDirection), 0.001);
    float normalHalf = max(dot(normal, halfDirection), 0.0);
    float viewHalf = max(dot(viewDirection, halfDirection), 0.0);
    float fresnel = 0.04 + 0.96 * pow(1.0 - viewHalf, 5.0);
    float distribution = distributionGGX(normalHalf, roughness);
    float geometry = geometrySchlickGGX(normalView, roughness)
            * geometrySchlickGGX(max(normalLight, 0.001), roughness);
    float specular = distribution * geometry * fresnel
            / max(4.0 * normalView * max(normalLight, 0.001), 0.001);
    return radiance * normalLight * specular;
}

vec3 evaluateSheenDirectLight(vec3 normal, vec3 viewDirection,
                              vec3 lightDirection, float roughness,
                              vec3 sheenColor, vec3 radiance) {
    float normalLight = max(dot(normal, lightDirection), 0.0);
    float normalView = max(dot(normal, viewDirection), 0.001);
    if (normalLight <= 0.0) return vec3(0.0);
    vec3 halfVector = lightDirection + viewDirection;
    float halfLengthSquared = dot(halfVector, halfVector);
    if (halfLengthSquared <= 0.000001) return vec3(0.0);
    vec3 halfDirection = halfVector * inversesqrt(halfLengthSquared);
    float normalHalf = max(dot(normal, halfDirection), 0.0);
    float alpha = max(roughness * roughness, 0.0001);
    float inverseAlpha = 1.0 / alpha;
    float sineSquaredHalf = max(1.0 - normalHalf * normalHalf, 0.0);
    float distribution = (2.0 + inverseAlpha)
            * pow(sineSquaredHalf, 0.5 * inverseAlpha) / (2.0 * 3.14159265);
    float visibility = 1.0 / max(4.0 * (normalLight + normalView
            - normalLight * normalView), 0.001);
    return sheenColor * radiance * normalLight * distribution * visibility;
}

vec3 applyTangentNormal(vec3 geometricNormal, vec3 tangentNormal, vec2 uv,
                        bool useAuthoredTangent) {
    if (useAuthoredTangent) {
        vec3 tangent = vWorldTangent.xyz
                - geometricNormal * dot(geometricNormal, vWorldTangent.xyz);
        float tangentLengthSquared = dot(tangent, tangent);
        if (tangentLengthSquared > 0.000001) {
            tangent *= inversesqrt(tangentLengthSquared);
            vec3 bitangent = normalize(cross(geometricNormal, tangent))
                    * vWorldTangent.w;
            return normalize(mat3(tangent, bitangent, geometricNormal) * tangentNormal);
        }
    }
    vec3 positionDx = dFdx(vWorldPosition);
    vec3 positionDy = dFdy(vWorldPosition);
    vec2 uvDx = dFdx(uv);
    vec2 uvDy = dFdy(uv);
    float uvDeterminant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
    if (abs(uvDeterminant) > 0.00001) {
        vec3 tangent = normalize((positionDx * uvDy.y - positionDy * uvDx.y)
                / uvDeterminant);
        vec3 bitangent = normalize((-positionDx * uvDy.x + positionDy * uvDx.x)
                / uvDeterminant);
        return normalize(mat3(tangent, bitangent, geometricNormal) * tangentNormal);
    }
    return geometricNormal;
}

vec2 selectTextureUv(int uvSet) {
    return uvSet == 1 ? vUv1 : vUv;
}

vec2 transformUv(vec2 uv, vec2 scale, vec2 offset, vec2 rotation) {
    vec2 scaled = uv * scale;
    return vec2(rotation.x * scaled.x - rotation.y * scaled.y,
                rotation.y * scaled.x + rotation.x * scaled.y) + offset;
}

void main() {
    vec3 normal = normalize(vWorldNormal);
    vec3 geometricNormal = normal;
    vec3 lightDirection = normalize(-uLightDirection);
    vec2 opacityUv = transformUv(selectTextureUv(uTextureUvSet[6]), uOpacityUvScale, uOpacityUvOffset, uOpacityUvRotation);
    float opacity = clamp(uOpacity, 0.0, 1.0);
    if (uHasOpacityTexture != 0) {
        vec4 opacitySample = texture(uOpacityTexture, opacityUv);
        float mapOpacity = opacitySample.r;
        if (uOpacityMapChannel == 1) mapOpacity = opacitySample.g;
        else if (uOpacityMapChannel == 2) mapOpacity = opacitySample.b;
        else if (uOpacityMapChannel == 3) mapOpacity = opacitySample.a;
        else if (uOpacityMapChannel == 4) {
            mapOpacity = dot(opacitySample.rgb, vec3(0.2126, 0.7152, 0.0722));
        }
        opacity *= adjustTextureValue(mapOpacity, uOpacityMapBaseGain);
    }

    vec2 albedoUv = transformUv(selectTextureUv(uTextureUvSet[0]), uAlbedoUvScale, uAlbedoUvOffset, uAlbedoUvRotation);
    vec4 albedoSample = vec4(1.0);
    if (uHasAlbedoTexture != 0) albedoSample = texture(uAlbedoTexture, albedoUv);
    if (uAlphaMaskEnabled != 0) {
        if (opacity * albedoSample.a < uAlphaCutoff) discard;
        opacity = 1.0;
    }

    if (uGrid == 1) {
        float gridVisibility = 1.0 - shadowAmount(normal, lightDirection) * 0.82;
        vec2 coordinate = vWorldPosition.xz * 0.5;
        vec2 derivative = max(fwidth(coordinate), vec2(0.0001));
        vec2 gridDistance = abs(fract(coordinate - 0.5) - 0.5) / derivative;
        float line = 1.0 - min(min(gridDistance.x, gridDistance.y), 1.0);
        float fade = 1.0 - smoothstep(2.0, 15.0, length(vWorldPosition.xz));
        float centerGlow = exp(-length(vWorldPosition.xz) * 0.48);
        vec3 base = vec3(0.012, 0.019, 0.043) * (0.48 + gridVisibility * 0.52);
        vec3 gridColor = vec3(0.12, 0.67, 0.72)
                * (line * fade * 0.48 * (0.38 + gridVisibility * 0.62) + centerGlow * 0.12);
        fragmentColor = vec4(base + gridColor, opacity);
        return;
    }

    vec3 baseColor = uColor;
    vec2 normalUv = transformUv(selectTextureUv(uTextureUvSet[1]), uNormalUvScale, uNormalUvOffset, uNormalUvRotation);
    vec2 metallicRoughnessUv = transformUv(selectTextureUv(uTextureUvSet[2]), uMetallicRoughnessUvScale,
                                       uMetallicRoughnessUvOffset, uMetallicRoughnessUvRotation);
    vec2 metallicUv = transformUv(selectTextureUv(uTextureUvSet[3]), uMetallicUvScale, uMetallicUvOffset, uMetallicUvRotation);
    vec2 roughnessUv = transformUv(selectTextureUv(uTextureUvSet[4]), uRoughnessUvScale, uRoughnessUvOffset, uRoughnessUvRotation);
    vec2 occlusionUv = transformUv(selectTextureUv(uTextureUvSet[5]), uOcclusionUvScale, uOcclusionUvOffset, uOcclusionUvRotation);
    vec2 emissiveUv = transformUv(selectTextureUv(uTextureUvSet[7]), uEmissiveUvScale, uEmissiveUvOffset, uEmissiveUvRotation);
    vec2 clearcoatUv = transformUv(selectTextureUv(uTextureUvSet[8]), uClearcoatUvScale,
            uClearcoatUvOffset, uClearcoatUvRotation);
    vec2 clearcoatRoughnessUv = transformUv(
            selectTextureUv(uTextureUvSet[9]), uClearcoatRoughnessUvScale,
            uClearcoatRoughnessUvOffset, uClearcoatRoughnessUvRotation);
    vec2 clearcoatNormalUv = transformUv(
            selectTextureUv(uTextureUvSet[10]), uClearcoatNormalUvScale,
            uClearcoatNormalUvOffset, uClearcoatNormalUvRotation);
    vec2 specularUv = transformUv(selectTextureUv(uTextureUvSet[11]),
            uSpecularUvScale, uSpecularUvOffset, uSpecularUvRotation);
    vec2 specularColorUv = transformUv(selectTextureUv(uTextureUvSet[12]),
            uSpecularColorUvScale, uSpecularColorUvOffset, uSpecularColorUvRotation);
    vec2 sheenColorUv = transformUv(selectTextureUv(uTextureUvSet[13]),
            uSheenColorUvScale, uSheenColorUvOffset, uSheenColorUvRotation);
    vec2 sheenRoughnessUv = transformUv(selectTextureUv(uTextureUvSet[14]),
            uSheenRoughnessUvScale, uSheenRoughnessUvOffset,
            uSheenRoughnessUvRotation);
    if (uHasAlbedoTexture != 0) {
        baseColor *= srgbToLinear(adjustTextureValues(
                albedoSample.rgb, uAlbedoMapBaseGain));
    }
    if (uHasNormalTexture != 0) {
        vec3 normalSample = texture(uNormalTexture, normalUv).rgb;
        vec3 tangentNormal = adjustTextureValues(normalSample, uNormalMapBaseGain)
                           * 2.0 - 1.0;
        tangentNormal.xy *= uNormalMapScale;
        float tangentLengthSquared = dot(tangentNormal, tangentNormal);
        tangentNormal = tangentLengthSquared > 0.000001
                ? tangentNormal * inversesqrt(tangentLengthSquared)
                : vec3(0.0, 0.0, 1.0);
        bool defaultNormalUv = uTextureUvSet[1] == 0
                && all(lessThan(abs(uNormalUvScale - vec2(1.0)), vec2(0.00001)))
                && all(lessThan(abs(uNormalUvRotation - vec2(1.0, 0.0)), vec2(0.00001)));
        normal = applyTangentNormal(normal, tangentNormal, normalUv,
                uHasTangents != 0 && defaultNormalUv);
    }

    float roughness = clamp(uRoughness, 0.06, 1.0);
    float metallic = clamp(uMetallic, 0.0, 1.0);
    if (uHasMetallicRoughnessTexture != 0) {
        vec4 materialSample = texture(uMetallicRoughnessTexture, metallicRoughnessUv);
        materialSample.rgb = adjustTextureValues(materialSample.rgb,
                                                  uMetallicRoughnessMapBaseGain);
        roughness *= materialSample.g;
        metallic *= materialSample.b;
    }
    if (uHasRoughnessTexture != 0) {
        roughness = adjustTextureValue(
                texture(uRoughnessTexture, roughnessUv).r, uRoughnessMapBaseGain);
    }
    if (uHasMetallicTexture != 0) {
        metallic = adjustTextureValue(
                texture(uMetallicTexture, metallicUv).r, uMetallicMapBaseGain);
    }
    roughness = clamp(roughness, 0.06, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    float occlusion = uAmbientOcclusion;
    if (uHasOcclusionTexture != 0) {
        occlusion *= adjustTextureValue(
                texture(uOcclusionTexture, occlusionUv).r, uOcclusionMapBaseGain);
    }

    float clearcoatFactor = clamp(uClearcoatFactor, 0.0, 1.0);
    float clearcoatRoughness = clamp(uClearcoatRoughness, 0.06, 1.0);
    if (clearcoatFactor > 0.0 && uHasClearcoatTexture != 0) {
        clearcoatFactor *= texture(uClearcoatTexture, clearcoatUv).r;
    }
    if (clearcoatFactor > 0.0 && uHasClearcoatRoughnessTexture != 0) {
        clearcoatRoughness *= texture(uClearcoatRoughnessTexture,
                                      clearcoatRoughnessUv).g;
    }
    clearcoatFactor = clamp(clearcoatFactor, 0.0, 1.0);
    clearcoatRoughness = clamp(clearcoatRoughness, 0.06, 1.0);
    vec3 clearcoatNormal = geometricNormal;
    if (clearcoatFactor > 0.0 && uHasClearcoatNormalTexture != 0) {
        vec3 clearcoatTangentNormal = texture(uClearcoatNormalTexture,
                                              clearcoatNormalUv).rgb * 2.0 - 1.0;
        clearcoatTangentNormal.xy *= uClearcoatNormalScale;
        float clearcoatNormalLengthSquared = dot(clearcoatTangentNormal,
                                                  clearcoatTangentNormal);
        clearcoatTangentNormal = clearcoatNormalLengthSquared > 0.000001
                ? clearcoatTangentNormal * inversesqrt(clearcoatNormalLengthSquared)
                : vec3(0.0, 0.0, 1.0);
        bool defaultClearcoatNormalUv = uTextureUvSet[10] == 0
                && all(lessThan(abs(uClearcoatNormalUvScale - vec2(1.0)), vec2(0.00001)))
                && all(lessThan(abs(uClearcoatNormalUvRotation - vec2(1.0, 0.0)), vec2(0.00001)));
        clearcoatNormal = applyTangentNormal(geometricNormal,
                clearcoatTangentNormal, clearcoatNormalUv,
                uHasTangents != 0 && defaultClearcoatNormalUv);
    }

    vec3 sheenColor = clamp(uSheenColorFactor, 0.0, 1.0);
    float sheenColorStrength = max(max(sheenColor.r, sheenColor.g), sheenColor.b);
    if (sheenColorStrength > 0.0 && uHasSheenColorTexture != 0) {
        sheenColor *= srgbToLinear(texture(uSheenColorTexture, sheenColorUv).rgb);
    }
    float sheenRoughness = clamp(uSheenRoughnessFactor, 0.0, 1.0);
    if (sheenColorStrength > 0.0 && uHasSheenRoughnessTexture != 0) {
        sheenRoughness *= texture(uSheenRoughnessTexture, sheenRoughnessUv).a;
    }
    sheenColorStrength = max(max(sheenColor.r, sheenColor.g), sheenColor.b);
    sheenRoughness = clamp(sheenRoughness, 0.0, 1.0);

    float visibility = 1.0 - shadowAmount(normal, lightDirection)
            * clamp(uShadowStrength, 0.0, 1.0);
    vec3 viewDirection = normalize(uCameraPosition - vWorldPosition);
    float specularFactor = clamp(uSpecularFactor, 0.0, 1.0);
    vec3 specularColorFactor = clamp(uSpecularColorFactor, 0.0, 1.0);
    if (uHasSpecularTexture != 0) {
        specularFactor *= texture(uSpecularTexture, specularUv).a;
    }
    if (uHasSpecularColorTexture != 0) {
        specularColorFactor *= srgbToLinear(
                texture(uSpecularColorTexture, specularColorUv).rgb);
    }
    float iorRatio = (uIndexOfRefraction - 1.0) / (uIndexOfRefraction + 1.0);
    vec3 dielectricReflectance = iorRatio * iorRatio
            * specularFactor * specularColorFactor;
    vec3 directionalRadiance = vec3(2.3, 2.15, 1.88)
            * uDirectionalLightColor * max(uDirectionalLightIntensity, 0.0);
    vec3 directLight = evaluateDirectLight(normal, viewDirection, lightDirection,
            baseColor, metallic, roughness, dielectricReflectance,
            directionalRadiance) * visibility;
    vec3 clearcoatDirectLight = vec3(0.0);
    if (clearcoatFactor > 0.0) {
        clearcoatDirectLight = evaluateClearcoatDirectLight(clearcoatNormal,
                viewDirection, lightDirection, clearcoatRoughness,
                directionalRadiance) * visibility;
    }
    vec3 sheenDirectLight = vec3(0.0);
    if (sheenColorStrength > 0.0) {
        sheenDirectLight = evaluateSheenDirectLight(normal, viewDirection,
                lightDirection, max(sheenRoughness, 0.07), sheenColor,
                directionalRadiance) * visibility;
    }

    for (int index = 0; index < 4; ++index) {
        if (index >= uPointLightCount) break;
        vec4 positionRange = uPointLightPositionRange[index];
        vec3 toLight = positionRange.xyz - vWorldPosition;
        float distanceSquared = max(dot(toLight, toLight), 0.04);
        float distanceToLight = sqrt(distanceSquared);
        float rangeFraction = distanceToLight / max(positionRange.w, 0.01);
        float rangeFade = clamp(1.0 - rangeFraction * rangeFraction
                                      * rangeFraction * rangeFraction, 0.0, 1.0);
        float attenuation = rangeFade * rangeFade / distanceSquared;
        vec4 colorIntensity = uPointLightColorIntensity[index];
        vec3 pointRadiance = colorIntensity.rgb * colorIntensity.a * attenuation;
        vec3 pointDirection = toLight / max(distanceToLight, 0.001);
        directLight += evaluateDirectLight(normal, viewDirection,
                pointDirection, baseColor, metallic, roughness,
                dielectricReflectance, pointRadiance);
        if (clearcoatFactor > 0.0) {
            clearcoatDirectLight += evaluateClearcoatDirectLight(clearcoatNormal,
                    viewDirection, pointDirection, clearcoatRoughness, pointRadiance);
        }
        if (sheenColorStrength > 0.0) {
            sheenDirectLight += evaluateSheenDirectLight(normal, viewDirection,
                    pointDirection, max(sheenRoughness, 0.07), sheenColor,
                    pointRadiance);
        }
    }

    vec3 ambientLight = max(evaluateAmbientSH(normal), vec3(0.0));
    float upwardWeight = max(normal.y, 0.0);
    float downwardWeight = max(-normal.y, 0.0);
    float horizonWeight = max(1.0 - abs(normal.y), 0.0);
    vec3 skyAmbient = uSkyZenithColor * upwardWeight
            + uSkyGroundColor * downwardWeight
            + uSkyHorizonColor * horizonWeight;
    ambientLight += skyAmbient * max(uSkyAmbientStrength, 0.0);
    vec3 ambient = baseColor * ambientLight * occlusion * (1.0 - metallic * 0.55);
    vec3 emission = uEmission * uEmissiveStrength;
    if (uHasEmissiveTexture != 0) {
        vec3 emissiveSample = texture(uEmissiveTexture, emissiveUv).rgb;
        emission *= srgbToLinear(adjustTextureValues(
                emissiveSample, uEmissiveMapBaseGain));
    }
    vec3 baseLayerLighting = ambient + directLight;
    if (sheenColorStrength > 0.0) {
        float sheenView = max(dot(normal, viewDirection), 0.0);
        float sheenEnergyCompensation = clamp(sheenColorStrength * 0.5
                * pow(1.0 - sheenView, 5.0), 0.0, 0.8);
        baseLayerLighting *= 1.0 - sheenEnergyCompensation;
        baseLayerLighting += sheenDirectLight;
    }
    vec3 layeredLighting = baseLayerLighting;
    if (clearcoatFactor > 0.0) {
        float coatView = max(dot(clearcoatNormal, viewDirection), 0.0);
        float coatFresnel = 0.04 + 0.96 * pow(1.0 - coatView, 5.0);
        layeredLighting = layeredLighting * (1.0 - clearcoatFactor * coatFresnel)
                + clearcoatDirectLight * clearcoatFactor;
    }
    vec3 color = uUnlit != 0 ? baseColor + emission : layeredLighting + emission;
    if (uFogEnabled != 0) {
        float viewDistance = vViewDepth;
        float fogAmount = clamp((viewDistance - uFogStart)
                / max(uFogEnd - uFogStart, 0.001), 0.0, 1.0);
        color = mix(color, uFogColor, fogAmount);
    }
    color = (color * (2.51 * color + 0.03))
          / max(color * (2.43 * color + 0.59) + 0.14, vec3(0.001));
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    fragmentColor = vec4(color, opacity);
}
)";

// Keep the scene visible if a device driver rejects the advanced PBR/CSM shader.
constexpr const char* kFallbackVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 9) in vec2 aUv1;
layout(location = 7) in uvec4 aJointIndices;
layout(location = 8) in vec4 aJointWeights;
layout(std140) uniform SkinPalette { mat4 uJointMatrices[64]; };
uniform mat4 uViewProjection;
uniform mat4 uModel;
uniform int uSkinningEnabled;
out vec3 vFallbackNormal;
out vec2 vFallbackUv;
out vec2 vFallbackUv1;
out vec3 vFallbackWorldPosition;
void main() {
    vec4 localPosition = vec4(aPosition, 1.0);
    vec3 localNormal = aNormal;
    if (uSkinningEnabled != 0) {
        mat4 skin = mat4(0.0);
        skin += uJointMatrices[int(aJointIndices.x)] * aJointWeights.x;
        skin += uJointMatrices[int(aJointIndices.y)] * aJointWeights.y;
        skin += uJointMatrices[int(aJointIndices.z)] * aJointWeights.z;
        skin += uJointMatrices[int(aJointIndices.w)] * aJointWeights.w;
        localPosition = skin * localPosition;
        localNormal = mat3(skin) * localNormal;
    }
    vec4 worldPosition = uModel * localPosition;
    vFallbackNormal = normalize(mat3(uModel) * localNormal);
    vFallbackUv = aUv;
    vFallbackUv1 = aUv1;
    vFallbackWorldPosition = worldPosition.xyz;
    gl_Position = uViewProjection * worldPosition;
}
)";

constexpr const char* kFallbackFragmentShader = R"(
#version 300 es
precision highp float;
in vec3 vFallbackNormal;
in vec2 vFallbackUv;
in vec2 vFallbackUv1;
in vec3 vFallbackWorldPosition;
uniform vec3 uCameraPosition;
uniform int uFogEnabled;
uniform vec3 uFogColor;
uniform float uFogStart;
uniform float uFogEnd;
uniform float uSkyAmbientStrength;
uniform vec3 uSkyZenithColor;
uniform vec3 uSkyHorizonColor;
uniform vec3 uSkyGroundColor;
uniform vec3 uColor;
uniform vec3 uEmission;
uniform vec3 uLightDirection;
uniform vec3 uDirectionalLightColor;
uniform float uDirectionalLightIntensity;
uniform float uOpacity;
uniform int uAlphaMaskEnabled;
uniform float uAlphaCutoff;
uniform float uEmissiveStrength;
uniform int uUnlit;
uniform sampler2D uAlbedoTexture;
uniform int uHasAlbedoTexture;
uniform int uAlbedoUvSet;
uniform vec2 uAlbedoUvScale;
uniform vec2 uAlbedoUvOffset;
uniform vec2 uAlbedoUvRotation;
out vec4 fragmentColor;
vec2 selectFallbackUv() {
    return uAlbedoUvSet == 1 ? vFallbackUv1 : vFallbackUv;
}
vec2 transformUv(vec2 uv, vec2 scale, vec2 offset, vec2 rotation) {
    vec2 scaled = uv * scale;
    return vec2(rotation.x * scaled.x - rotation.y * scaled.y,
                rotation.y * scaled.x + rotation.x * scaled.y) + offset;
}

void main() {
    float opacity = clamp(uOpacity, 0.0, 1.0);
    vec4 albedoSample = vec4(1.0);
    if (uHasAlbedoTexture != 0) {
        albedoSample = texture(uAlbedoTexture,
                transformUv(selectFallbackUv(), uAlbedoUvScale, uAlbedoUvOffset, uAlbedoUvRotation));
    }
    if (uAlphaMaskEnabled != 0) {
        if (opacity * albedoSample.a < uAlphaCutoff) discard;
        opacity = 1.0;
    }
    vec3 normal = normalize(vFallbackNormal);
    vec3 lightDirection = normalize(-uLightDirection);
    float diffuse = max(dot(normal, lightDirection), 0.0);
    float upwardWeight = max(normal.y, 0.0);
    float downwardWeight = max(-normal.y, 0.0);
    float horizonWeight = max(1.0 - abs(normal.y), 0.0);
    vec3 skyAmbient = uSkyZenithColor * upwardWeight
            + uSkyGroundColor * downwardWeight
            + uSkyHorizonColor * horizonWeight;
    vec3 lighting = vec3(0.28) + 0.72 * diffuse * uDirectionalLightColor
            * max(uDirectionalLightIntensity, 0.0)
            + skyAmbient * max(uSkyAmbientStrength, 0.0);
    vec3 baseColor = uColor * albedoSample.rgb;
    vec3 color = uUnlit != 0 ? baseColor + uEmission * uEmissiveStrength
                             : baseColor * lighting + uEmission * uEmissiveStrength;
    if (uFogEnabled != 0) {
        float viewDistance = length(uCameraPosition - vFallbackWorldPosition);
        float fogAmount = clamp((viewDistance - uFogStart)
                / max(uFogEnd - uFogStart, 0.001), 0.0, 1.0);
        color = mix(color, uFogColor, fogAmount);
    }
    fragmentColor = vec4(color, opacity);
}
)";

constexpr const char* kShadowVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec3 aPosition;
layout(location = 2) in vec2 aUv;
layout(location = 9) in vec2 aUv1;
layout(location = 3) in mat4 aInstanceModel;
layout(location = 7) in uvec4 aJointIndices;
layout(location = 8) in vec4 aJointWeights;
layout(std140) uniform SkinPalette { mat4 uJointMatrices[64]; };
uniform mat4 uLightViewProjection;
uniform mat4 uModel;
uniform int uUseInstancing;
uniform int uSkinningEnabled;
out vec2 vShadowUv;
out vec2 vShadowUv1;
void main() {
    mat4 model = uUseInstancing != 0 ? aInstanceModel : uModel;
    vec4 localPosition = vec4(aPosition, 1.0);
    if (uSkinningEnabled != 0) {
        mat4 skin = mat4(0.0);
        skin += uJointMatrices[int(aJointIndices.x)] * aJointWeights.x;
        skin += uJointMatrices[int(aJointIndices.y)] * aJointWeights.y;
        skin += uJointMatrices[int(aJointIndices.z)] * aJointWeights.z;
        skin += uJointMatrices[int(aJointIndices.w)] * aJointWeights.w;
        localPosition = skin * localPosition;
    }
    vShadowUv = aUv;
    vShadowUv1 = aUv1;
    gl_Position = uLightViewProjection * model * localPosition;
}
)";

constexpr const char* kShadowFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vShadowUv;
in vec2 vShadowUv1;
uniform sampler2D uAlbedoTexture;
uniform int uHasAlbedoTexture;
uniform int uAlphaMaskEnabled;
uniform float uAlphaCutoff;
uniform float uBaseColorAlpha;
uniform int uAlbedoUvSet;
uniform vec2 uAlbedoUvScale;
uniform vec2 uAlbedoUvOffset;
uniform vec2 uAlbedoUvRotation;
vec2 selectShadowUv() {
    return uAlbedoUvSet == 1 ? vShadowUv1 : vShadowUv;
}
vec2 transformUv(vec2 uv, vec2 scale, vec2 offset, vec2 rotation) {
    vec2 scaled = uv * scale;
    return vec2(rotation.x * scaled.x - rotation.y * scaled.y,
                rotation.y * scaled.x + rotation.x * scaled.y) + offset;
}

void main() {
    if (uAlphaMaskEnabled != 0) {
        float alpha = uBaseColorAlpha;
        if (uHasAlbedoTexture != 0) {
            alpha *= texture(uAlbedoTexture,
                    transformUv(selectShadowUv(), uAlbedoUvScale, uAlbedoUvOffset, uAlbedoUvRotation)).a;
        }
        if (alpha < uAlphaCutoff) discard;
    }
}
)";

constexpr const char* kFullscreenVertexShader = R"(
#version 300 es
precision highp float;
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

constexpr const char* kSkyFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform vec3 uViewForward;
uniform vec3 uViewRight;
uniform vec3 uViewUp;
uniform float uTanHalfFov;
uniform float uAspectRatio;
uniform vec3 uSunDirection;
uniform vec3 uSunColor;
uniform float uSunAngularRadius;
uniform int uSunDiskEnabled;
uniform vec3 uZenithColor;
uniform vec3 uHorizonColor;
uniform vec3 uGroundColor;
out vec4 fragmentColor;
void main() {
    vec2 screen = vUv * 2.0 - 1.0;
    vec3 ray = normalize(uViewForward
            + uViewRight * screen.x * uTanHalfFov * uAspectRatio
            + uViewUp * screen.y * uTanHalfFov);
    float elevation = clamp(ray.y, -1.0, 1.0);
    float skyBlend = pow(clamp(elevation, 0.0, 1.0), 0.62);
    float groundBlend = pow(clamp(-elevation, 0.0, 1.0), 0.55);
    vec3 color = elevation >= 0.0
            ? mix(uHorizonColor, uZenithColor, skyBlend)
            : mix(uHorizonColor, uGroundColor, groundBlend);
    if (uSunDiskEnabled != 0) {
        float alignment = dot(ray, normalize(uSunDirection));
        float radius = clamp(uSunAngularRadius, 0.001, 0.5);
        float disk = smoothstep(cos(radius), cos(radius * 0.78), alignment);
        float glow = pow(max(alignment, 0.0), 96.0) * 0.16;
        color += uSunColor * (disk * 2.4 + glow);
    }
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / 2.2));
    fragmentColor = vec4(color, 1.0);
}
)";

constexpr const char* kBloomExtractFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uScene;
out vec4 fragmentColor;
void main() {
    vec3 color = texture(uScene, vUv).rgb;
    float brightness = max(max(color.r, color.g), color.b);
    float contribution = max(brightness - 0.72, 0.0) / max(brightness, 0.001);
    fragmentColor = vec4(color * contribution, 1.0);
}
)";

constexpr const char* kBlurFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uSource;
uniform vec2 uDirection;
uniform vec2 uTexelSize;
out vec4 fragmentColor;
void main() {
    vec3 result = texture(uSource, vUv).rgb * 0.227027;
    result += texture(uSource, vUv + uDirection * uTexelSize * 1.384615).rgb * 0.316216;
    result += texture(uSource, vUv - uDirection * uTexelSize * 1.384615).rgb * 0.316216;
    result += texture(uSource, vUv + uDirection * uTexelSize * 3.230769).rgb * 0.070270;
    result += texture(uSource, vUv - uDirection * uTexelSize * 3.230769).rgb * 0.070270;
    fragmentColor = vec4(result, 1.0);
}
)";

constexpr const char* kCompositeFragmentShader = R"(
#version 300 es
precision highp float;
in vec2 vUv;
uniform sampler2D uScene;
uniform sampler2D uBloom;
out vec4 fragmentColor;
void main() {
    vec3 sceneColor = texture(uScene, vUv).rgb;
    vec3 bloomColor = texture(uBloom, vUv).rgb;
    fragmentColor = vec4(clamp(sceneColor + bloomColor * 0.32, 0.0, 1.0), 1.0);
}
)";

}  // namespace

GLuint Renderer::compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        const std::size_t nullTerminator = log.find('\0');
        if (nullTerminator != std::string::npos) log.resize(nullTerminator);
        const char* stage = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
        lastProgramBuildError_ = std::string("Shader compile failed (") + stage
                + "): " + log;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Shader compile failed: %s", log.c_str());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Renderer::createProgram(const char* vertexSource, const char* fragmentSource) {
    lastProgramBuildError_.clear();
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked == GL_FALSE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        const std::size_t nullTerminator = log.find('\0');
        if (nullTerminator != std::string::npos) log.resize(nullTerminator);
        lastProgramBuildError_ = std::string("Program link failed: ") + log;
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Program link failed: %s", log.c_str());
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

Renderer::Mesh Renderer::uploadMesh(const std::vector<Vertex>& vertices) {
    Mesh mesh{};
    if (vertices.empty()) return mesh;

    bool finitePositions = true;
    for (const Vertex& vertex : vertices) {
        for (float component : vertex.position) {
            if (!std::isfinite(component)) finitePositions = false;
        }
    }
    if (finitePositions) {
        Vec3 minimum{vertices.front().position[0], vertices.front().position[1],
                vertices.front().position[2]};
        Vec3 maximum = minimum;
        for (const Vertex& vertex : vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
                maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
            }
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            mesh.boundsCenter[axis] = (minimum[axis] + maximum[axis]) * 0.5f;
        }
        float radiusSquared = 0.0f;
        for (const Vertex& vertex : vertices) {
            const float dx = vertex.position[0] - mesh.boundsCenter[0];
            const float dy = vertex.position[1] - mesh.boundsCenter[1];
            const float dz = vertex.position[2] - mesh.boundsCenter[2];
            radiusSquared = std::max(radiusSquared, dx * dx + dy * dy + dz * dz);
        }
        if (std::isfinite(radiusSquared)) {
            mesh.boundsRadius = std::sqrt(radiusSquared);
            mesh.boundsValid = std::isfinite(mesh.boundsRadius);
        }
    }

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
                 vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, uv)));
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(9, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<const void*>(offsetof(Vertex, uv1)));
    glBindVertexArray(0);
    mesh.vertexCount = static_cast<GLsizei>(vertices.size());
    mesh.gpuBytes = static_cast<std::uint64_t>(vertices.size()) * sizeof(Vertex);
    return mesh;
}

bool Renderer::prepareCpuGpuPrimitive(std::vector<MeshVertex> vertices,
                                      std::vector<std::uint32_t> indices,
                                      std::vector<std::array<float, 4>> tangents,
                                      std::vector<SkeletalVertexInfluence> influences,
                                      bool skinned, CpuGpuPrimitive& output,
                                      std::string& error) {
    error.clear();
    if (vertices.empty() || vertices.size() > static_cast<std::size_t>(
                    std::numeric_limits<GLsizei>::max())
            || indices.size() > static_cast<std::size_t>(
                    std::numeric_limits<GLsizei>::max())
            || (!tangents.empty() && tangents.size() != vertices.size())
            || (skinned && (indices.empty() || influences.size() != vertices.size()))) {
        error = "mesh primitive has invalid vertex, index, or skin counts";
        return false;
    }

    CpuGpuPrimitive candidate;
    candidate.skinned = skinned;
    for (std::array<float, 4>& tangent : tangents) {
        const float lengthSquared = tangent[0] * tangent[0] + tangent[1] * tangent[1]
                + tangent[2] * tangent[2];
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f
                || (tangent[3] != 1.0f && tangent[3] != -1.0f)) {
            error = "mesh primitive contains invalid tangent vectors";
            return false;
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        tangent[0] *= inverseLength;
        tangent[1] *= inverseLength;
        tangent[2] *= inverseLength;
    }
    candidate.tangents = std::move(tangents);
    Vec3 minimum{vertices.front().position[0], vertices.front().position[1],
            vertices.front().position[2]};
    Vec3 maximum = minimum;
    if (skinned) candidate.skinnedVertices.resize(vertices.size());
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        const MeshVertex& source = vertices[index];
        for (std::size_t axis = 0; axis < 3U; ++axis) {
            if (!std::isfinite(source.position[axis]) || !std::isfinite(source.normal[axis])) {
                error = "mesh primitive contains non-finite positions or normals";
                return false;
            }
            minimum[axis] = std::min(minimum[axis], source.position[axis]);
            maximum[axis] = std::max(maximum[axis], source.position[axis]);
        }
        for (float uv : source.uv) {
            if (!std::isfinite(uv)) {
                error = "mesh primitive contains non-finite texture coordinates";
                return false;
            }
        }
        if (!skinned) continue;

        const SkeletalVertexInfluence& influence = influences[index];
        CpuSkinnedVertex& destination = candidate.skinnedVertices[index];
        std::copy(std::begin(source.position), std::end(source.position), destination.position);
        std::copy(std::begin(source.normal), std::end(source.normal), destination.normal);
        std::copy(std::begin(source.uv), std::end(source.uv), destination.uv);
        std::copy(std::begin(source.uv1), std::end(source.uv1), destination.uv1);
        for (std::size_t slot = 0; slot < 4U; ++slot) {
            if (!std::isfinite(influence.weights[slot]) || influence.weights[slot] < 0.0f
                    || influence.jointIndices[slot] >= kMaximumGpuJoints) {
                error = "mesh primitive contains invalid skin weights or joints";
                return false;
            }
            destination.jointIndices[slot] = influence.jointIndices[slot];
            destination.weights[slot] = influence.weights[slot];
        }
    }
    for (std::uint32_t index : indices) {
        if (index >= vertices.size()) {
            error = "mesh primitive index lies outside its vertex range";
            return false;
        }
    }

    candidate.boundsCenter = {(minimum[0] + maximum[0]) * 0.5f,
                              (minimum[1] + maximum[1]) * 0.5f,
                              (minimum[2] + maximum[2]) * 0.5f};
    float radiusSquared = 0.0f;
    for (const MeshVertex& vertex : vertices) {
        const float dx = vertex.position[0] - candidate.boundsCenter[0];
        const float dy = vertex.position[1] - candidate.boundsCenter[1];
        const float dz = vertex.position[2] - candidate.boundsCenter[2];
        radiusSquared = std::max(radiusSquared, dx * dx + dy * dy + dz * dz);
    }
    if (!std::isfinite(radiusSquared)) {
        error = "mesh primitive bounds are not finite";
        return false;
    }
    candidate.boundsRadius = std::sqrt(radiusSquared);
    candidate.boundsValid = std::isfinite(candidate.boundsRadius);
    if (!skinned) candidate.vertices = std::move(vertices);
    candidate.indices = std::move(indices);
    output = std::move(candidate);
    return true;
}

bool Renderer::prepareGlbAsset(AAssetManager* manager, const std::string& assetPath,
                               CpuGlbModel& model, std::string& error) {
    std::string bytes;
    constexpr std::size_t kMaximumGlbAssetBytes = 64U * 1024U * 1024U;
    if (!readTextAsset(manager, assetPath, kMaximumGlbAssetBytes, bytes)) {
        error = "GLB asset is unavailable or exceeds limits: " + assetPath;
        return false;
    }

    CpuGlbModel candidate{};
    GltfSkeletalAsset skeletal;
    if (parseGlbSkeletalAsset(bytes, skeletal, error)) {
        candidate.skeletal = true;
        candidate.primitives = std::move(skeletal.primitives);
        candidate.images = std::move(skeletal.images);
        candidate.skeleton = std::move(skeletal.skeleton);
        candidate.animations = std::move(skeletal.animations);
    } else {
        const std::string skeletalError = error;
        GltfStaticAsset staticAsset;
        if (!parseGlbStaticAsset(bytes, staticAsset, error)) {
            error = "GLB import failed for " + assetPath + " (skeletal: "
                    + skeletalError + "; static: " + error + ")";
            return false;
        }
        candidate.primitives = std::move(staticAsset.primitives);
        candidate.images = std::move(staticAsset.images);
    }

    candidate.gpuPrimitives.reserve(candidate.primitives.size());
    for (GltfPrimitive& primitive : candidate.primitives) {
        CpuGpuPrimitive geometry;
        if (!prepareCpuGpuPrimitive(std::move(primitive.vertices),
                std::move(primitive.indices), std::move(primitive.tangents),
                std::move(primitive.influences),
                candidate.skeletal, geometry, error)) {
            error = "GLB GPU preparation failed for " + assetPath + ": " + error;
            return false;
        }
        candidate.gpuPrimitives.push_back(std::move(geometry));
    }
    model = std::move(candidate);
    error.clear();
    return true;
}

bool Renderer::prepareObjAsset(AAssetManager* manager, const std::string& assetPath,
                               CpuObjModel& model, std::string& error) {
    std::string source;
    constexpr std::size_t kMaximumObjBytes = 16U * 1024U * 1024U;
    if (!readTextAsset(manager, assetPath, kMaximumObjBytes, source)) {
        error = "OBJ asset is unavailable or exceeds limits: " + assetPath;
        return false;
    }

    ObjModel parsedObj;
    if (!parseObjModel(source, parsedObj, error)) {
        error = "OBJ parse failed for " + assetPath + ": " + error;
        return false;
    }

    std::unordered_map<std::string, Material> materialsByName;
    for (const std::string& relativeLibrary : parsedObj.materialLibraries) {
        std::string libraryPath;
        if (!resolveAssetReference(assetPath, relativeLibrary, libraryPath)) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "Ignoring unsafe MTL reference %s from %s",
                    relativeLibrary.c_str(), assetPath.c_str());
            continue;
        }
        std::string librarySource;
        if (!readTextAsset(manager, libraryPath, 4U * 1024U * 1024U, librarySource)) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "MTL asset unavailable: %s", libraryPath.c_str());
            continue;
        }
        std::vector<WavefrontMaterial> parsedMaterials;
        std::string materialError;
        if (!parseMtl(librarySource, libraryPath, parsedMaterials, materialError)) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "MTL parse failed for %s: %s", libraryPath.c_str(), materialError.c_str());
            continue;
        }
        for (WavefrontMaterial& material : parsedMaterials) {
            materialsByName[material.name] = std::move(material.material);
        }
    }

    CpuObjModel candidate;
    candidate.parts.reserve(parsedObj.submeshes.size());
    for (ObjSubmesh& submesh : parsedObj.submeshes) {
        CpuObjPart part{};
        if (!prepareCpuGpuPrimitive(std::move(submesh.vertices), {}, {}, {}, false,
                                    part.geometry, error)) {
            error = "OBJ GPU preparation failed for " + assetPath + ": " + error;
            return false;
        }
        part.objectName = submesh.objectName;
        part.groupName = submesh.groupName;
        const auto material = materialsByName.find(submesh.materialName);
        if (!submesh.materialName.empty() && material != materialsByName.end()) {
            part.material = material->second;
            part.hasMaterial = true;
        }
        candidate.parts.push_back(std::move(part));
    }
    if (candidate.parts.empty()) {
        error = "OBJ contains no renderable submeshes: " + assetPath;
        return false;
    }
    model = std::move(candidate);
    error.clear();
    return true;
}

void Renderer::uploadPreparedObjAsset(const std::string& assetPath, CpuObjModel cpuModel) {
    PendingGpuAsset pending{};
    pending.assetPath = assetPath;
    pending.objModel = std::move(cpuModel);
    for (const CpuObjPart& part : pending.objModel.parts) {
        pending.totalVertexCount += part.geometry.vertices.size();
    }
    pending.importedModel.parts.reserve(pending.objModel.parts.size());
    pendingGpuUploads_.push_back(std::move(pending));
}

void Renderer::uploadPreparedGlbAsset(const std::string& assetPath, CpuGlbModel cpuModel) {
    PendingGpuAsset pending{};
    pending.assetPath = assetPath;
    pending.isGlb = true;
    pending.importedModel.skeleton = std::move(cpuModel.skeleton);
    pending.importedModel.animations = std::move(cpuModel.animations);
    pending.importedModel.parts.reserve(cpuModel.primitives.size());
    pending.glbModel = std::move(cpuModel);
    pendingGpuUploads_.push_back(std::move(pending));
}

bool Renderer::initializePendingGpuPrimitive(CpuGpuPrimitive& geometry, Mesh& mesh) {
    const std::size_t vertexCount = geometry.skinned
            ? geometry.skinnedVertices.size() : geometry.vertices.size();
    const std::size_t vertexStride = geometry.skinned
            ? sizeof(CpuSkinnedVertex) : sizeof(MeshVertex);
    const std::size_t maximumBufferBytes = static_cast<std::size_t>(
            std::numeric_limits<GLsizeiptr>::max());
    if (vertexCount == 0U || vertexCount > static_cast<std::size_t>(
                    std::numeric_limits<GLsizei>::max())
            || geometry.indices.size() > static_cast<std::size_t>(
                    std::numeric_limits<GLsizei>::max())
            || vertexCount > maximumBufferBytes / vertexStride
            || geometry.indices.size() > maximumBufferBytes / sizeof(std::uint32_t)
            || geometry.tangents.size() > maximumBufferBytes
                    / sizeof(std::array<float, 4>)
            || (!geometry.tangents.empty() && geometry.tangents.size() != vertexCount)) {
        return false;
    }
    const std::size_t vertexBytes = vertexCount * vertexStride;
    const std::size_t indexBytes = geometry.indices.size() * sizeof(std::uint32_t);
    const std::size_t tangentBytes = geometry.tangents.size()
            * sizeof(std::array<float, 4>);

    glGenVertexArrays(1, &mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    if (!geometry.indices.empty()) glGenBuffers(1, &mesh.ebo);
    if (!geometry.tangents.empty()) glGenBuffers(1, &mesh.tangentVbo);
    glBindVertexArray(mesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexBytes), nullptr, GL_STATIC_DRAW);
    if (!geometry.indices.empty()) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexBytes),
                     nullptr, GL_STATIC_DRAW);
    }
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(vertexStride),
            reinterpret_cast<const void*>(geometry.skinned
                    ? offsetof(CpuSkinnedVertex, position) : offsetof(MeshVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(vertexStride),
            reinterpret_cast<const void*>(geometry.skinned
                    ? offsetof(CpuSkinnedVertex, normal) : offsetof(MeshVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(vertexStride),
            reinterpret_cast<const void*>(geometry.skinned
                    ? offsetof(CpuSkinnedVertex, uv) : offsetof(MeshVertex, uv)));
    glEnableVertexAttribArray(9);
    glVertexAttribPointer(9, 2, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(vertexStride),
            reinterpret_cast<const void*>(geometry.skinned
                    ? offsetof(CpuSkinnedVertex, uv1) : offsetof(MeshVertex, uv1)));
    if (!geometry.tangents.empty()) {
        glBindBuffer(GL_ARRAY_BUFFER, mesh.tangentVbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(tangentBytes),
                     nullptr, GL_STATIC_DRAW);
        glEnableVertexAttribArray(10);
        glVertexAttribPointer(10, 4, GL_FLOAT, GL_FALSE,
                static_cast<GLsizei>(sizeof(std::array<float, 4>)), nullptr);
        mesh.hasTangents = true;
    }
    if (geometry.skinned) {
        glEnableVertexAttribArray(7);
        glVertexAttribIPointer(7, 4, GL_UNSIGNED_SHORT,
                static_cast<GLsizei>(vertexStride),
                reinterpret_cast<const void*>(offsetof(CpuSkinnedVertex, jointIndices)));
        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE,
                static_cast<GLsizei>(vertexStride),
                reinterpret_cast<const void*>(offsetof(CpuSkinnedVertex, weights)));
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    mesh.vertexCount = static_cast<GLsizei>(vertexCount);
    mesh.indexCount = static_cast<GLsizei>(geometry.indices.size());
    mesh.boundsCenter = geometry.boundsCenter;
    mesh.boundsRadius = geometry.boundsRadius;
    mesh.boundsValid = geometry.boundsValid;
    mesh.gpuBytes = static_cast<std::uint64_t>(vertexBytes) + indexBytes + tangentBytes;
    if (glGetError() == GL_NO_ERROR && mesh.vao != 0U && mesh.vbo != 0U
            && (geometry.indices.empty() || mesh.ebo != 0U)
            && (geometry.tangents.empty() || mesh.tangentVbo != 0U)) {
        return true;
    }
    if (mesh.vao != 0U) glDeleteVertexArrays(1, &mesh.vao);
    if (mesh.vbo != 0U) glDeleteBuffers(1, &mesh.vbo);
    if (mesh.ebo != 0U) glDeleteBuffers(1, &mesh.ebo);
    if (mesh.tangentVbo != 0U) glDeleteBuffers(1, &mesh.tangentVbo);
    mesh = {};
    return false;
}

bool Renderer::beginTextureUpload(const std::string& assetPath,
                                  std::vector<ImageRgba8> levels) {
    std::uint64_t byteSize = 0U;
    if (!imageMipChainByteSize(levels, byteSize) || levels.size() > 32U) return false;
    GLint maximumTextureSize = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximumTextureSize);
    if (maximumTextureSize <= 0 || levels.front().width
                    > static_cast<std::uint32_t>(maximumTextureSize)
            || levels.front().height > static_cast<std::uint32_t>(maximumTextureSize)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                "Texture %s exceeds device texture limits", assetPath.c_str());
        return false;
    }
    for (std::size_t index = 1U; index < levels.size(); ++index) {
        if (levels[index].width != std::max(1U, levels[index - 1U].width / 2U)
                || levels[index].height != std::max(1U, levels[index - 1U].height / 2U)) {
            return false;
        }
    }

    PendingTextureUpload upload;
    upload.assetPath = assetPath;
    upload.levels = std::move(levels);
    upload.byteSize = byteSize;
    glGenTextures(1, &upload.texture);
    if (upload.texture == 0U) return false;
    glBindTexture(GL_TEXTURE_2D, upload.texture);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(upload.levels.size()), GL_RGBA8,
            static_cast<GLsizei>(upload.levels.front().width),
            static_cast<GLsizei>(upload.levels.front().height));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &upload.texture);
        return false;
    }
    pendingTextureUploads_.push_back(std::move(upload));
    return true;
}

void Renderer::requestTextureDecode(const std::string& assetPath) {
    if (assetPath.empty() || pendingTextureAssets_.find(assetPath) != pendingTextureAssets_.end()
            || failedTextureAssets_.find(assetPath) != failedTextureAssets_.end()
            || textures_.find(assetPath) != textures_.end()) return;

    std::string embeddedBytes;
    const auto embedded = embeddedImageData_.find(assetPath);
    if (embedded != embeddedImageData_.end()) embeddedBytes = embedded->second;
    AAssetManager* manager = assetManager_;
    if (!pendingTextureAssets_.insert(assetPath).second) return;
    const std::string key = "image:" + assetPath;
    const bool queued = asyncAssetTasks_.submit(key,
            [this, manager, assetPath, encoded = std::move(embeddedBytes)]() mutable {
        ImageRgba8 image;
        std::vector<ImageRgba8> mipLevels;
        std::string error;
        bool success = false;
        std::string bytes = std::move(encoded);
        try {
            if (bytes.empty()) {
                if (manager == nullptr || !safeRelativeAssetPath(assetPath)) {
                    error = "invalid texture asset path or manager";
                } else {
                    AAsset* asset = AAssetManager_open(manager, assetPath.c_str(),
                                                       AASSET_MODE_BUFFER);
                    if (asset == nullptr) {
                        error = "texture asset not found";
                    } else {
                        constexpr off_t kMaximumAssetBytes = 64 * 1024 * 1024;
                        const off_t length = AAsset_getLength(asset);
                        if (length <= 0 || length > kMaximumAssetBytes) {
                            error = "texture asset has invalid size";
                        } else {
                            bytes.resize(static_cast<std::size_t>(length));
                            std::size_t bytesRead = 0;
                            while (bytesRead < bytes.size()) {
                                const int count = AAsset_read(asset, bytes.data() + bytesRead,
                                                              bytes.size() - bytesRead);
                                if (count <= 0) break;
                                bytesRead += static_cast<std::size_t>(count);
                            }
                            if (bytesRead != bytes.size()) error = "incomplete texture read";
                        }
                        AAsset_close(asset);
                    }
                }
            }
            if (error.empty()) success = decodeImage(bytes, image, error);
            std::string().swap(bytes);
            if (success) {
                success = generateMipChain(std::move(image), mipLevels, error);
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        } catch (...) {
            error = "unknown exception while decoding image";
        }
        return [this, assetPath, success, error = std::move(error),
                mipLevels = std::move(mipLevels)]() mutable {
            if (!success) {
                pendingTextureAssets_.erase(assetPath);
                failedTextureAssets_.insert(assetPath);
                __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "Image decode failed for %s: %s", assetPath.c_str(), error.c_str());
                loadSceneAssets();
                return;
            }
            if (!beginTextureUpload(assetPath, std::move(mipLevels))) {
                pendingTextureAssets_.erase(assetPath);
                failedTextureAssets_.insert(assetPath);
                __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "Could not allocate GPU mip levels for texture %s", assetPath.c_str());
                loadSceneAssets();
            }
        };
    });
    if (!queued) {
        pendingTextureAssets_.erase(assetPath);
    }
}

void Renderer::processAssetTasks() {
    assetCompletionsScratch_.clear();
    asyncAssetTasks_.poll(assetCompletionsScratch_, 1U);
    for (AsyncAssetTaskQueue::Completion& completion : assetCompletionsScratch_) {
        if (completion) completion();
    }
    assetCompletionsScratch_.clear();

    if (pendingTextureUploads_.empty()) {
        processPendingGpuUpload();
    } else if (pendingGpuUploads_.empty()) {
        processPendingTextureUploads();
    } else if (textureUploadTurn_) {
        processPendingTextureUploads();
        textureUploadTurn_ = false;
    } else {
        processPendingGpuUpload();
        textureUploadTurn_ = true;
    }
}

void Renderer::processPendingTextureUploads() {
    std::uint64_t remainingBytes = kMaxGpuAssetUploadBytesPerTurn;
    while (!pendingTextureUploads_.empty() && remainingBytes > 0U) {
        PendingTextureUpload& upload = pendingTextureUploads_.front();
        if (upload.levelIndex >= upload.levels.size()) {
            const std::string assetPath = upload.assetPath;
            const GLuint texture = upload.texture;
            const std::uint64_t byteSize = upload.byteSize;
            const std::size_t levelCount = upload.levels.size();
            pendingTextureUploads_.pop_front();
            textures_[assetPath] = texture;
            textureGpuBytes_[assetPath] = byteSize;
            pendingTextureAssets_.erase(assetPath);
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                    "Loaded image %s with %zu mip levels (%llu bytes)", assetPath.c_str(),
                    levelCount, static_cast<unsigned long long>(byteSize));
            loadSceneAssets();
            continue;
        }

        const ImageRgba8& level = upload.levels[upload.levelIndex];
        ImageUploadChunk chunk;
        if (!planRgba8UploadRows(level.width, level.height, upload.rowOffset,
                                 remainingBytes, chunk)) {
            glDeleteTextures(1, &upload.texture);
            const std::string assetPath = upload.assetPath;
            pendingTextureUploads_.pop_front();
            pendingTextureAssets_.erase(assetPath);
            failedTextureAssets_.insert(assetPath);
            loadSceneAssets();
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, upload.texture);
        glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(upload.levelIndex), 0,
                static_cast<GLint>(upload.rowOffset), static_cast<GLsizei>(level.width),
                static_cast<GLsizei>(chunk.rowCount), GL_RGBA, GL_UNSIGNED_BYTE,
                level.pixels.data() + static_cast<std::size_t>(upload.rowOffset)
                        * level.width * 4U);
        glBindTexture(GL_TEXTURE_2D, 0);
        const GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            const std::string assetPath = upload.assetPath;
            glDeleteTextures(1, &upload.texture);
            pendingTextureUploads_.pop_front();
            pendingTextureAssets_.erase(assetPath);
            failedTextureAssets_.insert(assetPath);
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                    "GPU mip upload failed for %s (0x%x)", assetPath.c_str(), error);
            loadSceneAssets();
            continue;
        }
        remainingBytes = chunk.byteCount >= remainingBytes
                ? 0U : remainingBytes - chunk.byteCount;
        upload.rowOffset += chunk.rowCount;
        if (upload.rowOffset == level.height) {
            ++upload.levelIndex;
            upload.rowOffset = 0U;
        }
    }
}

void Renderer::processPendingGpuUpload() {
    if (pendingGpuUploads_.empty()) return;
    PendingGpuAsset& pending = pendingGpuUploads_.front();
    const std::size_t primitiveCount = pending.isGlb
            ? pending.glbModel.primitives.size() : pending.objModel.parts.size();
    const auto deleteMesh = [](Mesh& mesh) {
        if (mesh.vao != 0U) glDeleteVertexArrays(1, &mesh.vao);
        if (mesh.vbo != 0U) glDeleteBuffers(1, &mesh.vbo);
        if (mesh.ebo != 0U) glDeleteBuffers(1, &mesh.ebo);
        if (mesh.tangentVbo != 0U) glDeleteBuffers(1, &mesh.tangentVbo);
        mesh = {};
    };
    const auto deleteImportedParts = [&deleteMesh](ImportedModel& model) {
        for (ImportedPart& part : model.parts) deleteMesh(part.mesh);
        model.parts.clear();
    };
    if (pending.nextPrimitive >= primitiveCount) {
        const std::string assetPath = pending.assetPath;
        deleteMesh(pending.activePart.mesh);
        deleteImportedParts(pending.importedModel);
        releaseEmbeddedImagesForModel(assetPath);
        pendingGpuUploads_.pop_front();
        pendingMeshAssets_.erase(assetPath);
        failedMeshAssets_.insert(assetPath);
        loadSceneAssets();
        return;
    }

    if (!pending.activePrimitiveInitialized) {
        ImportedPart part{};
        CpuGpuPrimitive* geometry = nullptr;
        if (!pending.isGlb) {
            CpuObjPart& cpuPart = pending.objModel.parts[pending.nextPrimitive];
            part.objectName = cpuPart.objectName;
            part.groupName = cpuPart.groupName;
            part.material = cpuPart.material;
            part.hasMaterial = cpuPart.hasMaterial;
            geometry = &cpuPart.geometry;
        } else {
            GltfPrimitive& primitive = pending.glbModel.primitives[pending.nextPrimitive];
            part.skinned = pending.glbModel.skeletal;
            part.hasMaterial = primitive.hasMaterial;
            geometry = &pending.glbModel.gpuPrimitives[pending.nextPrimitive];
            if (primitive.hasMaterial) {
                part.material.color = {primitive.material.baseColorFactor[0],
                                       primitive.material.baseColorFactor[1],
                                       primitive.material.baseColorFactor[2]};
                part.material.opacity = primitive.material.alphaBlend || primitive.material.alphaMask
                        ? primitive.material.baseColorFactor[3] : 1.0f;
                part.alphaMask = primitive.material.alphaMask;
                part.alphaCutoff = primitive.material.alphaCutoff;
                part.material.emission = primitive.material.emissiveFactor;
                part.material.emissiveStrength = primitive.material.emissiveStrength;
                part.material.normalMapScale = primitive.material.normalScale;
                part.material.indexOfRefraction = primitive.material.indexOfRefraction;
                part.material.clearcoatFactor = primitive.material.clearcoatFactor;
                part.material.clearcoatRoughness = primitive.material.clearcoatRoughness;
                part.material.clearcoatNormalScale = primitive.material.clearcoatNormalScale;
                part.material.specularFactor = primitive.material.specularFactor;
                part.material.specularColorFactor = primitive.material.specularColorFactor;
                part.material.sheenColorFactor = primitive.material.sheenColorFactor;
                part.material.sheenRoughness = primitive.material.sheenRoughness;
                part.material.unlit = primitive.material.unlit;
                part.material.metallic = primitive.material.metallicFactor;
                part.material.roughness = primitive.material.roughnessFactor;
                part.material.ambientOcclusion = primitive.material.occlusionStrength;
                const auto texturePath = [this, &pending](std::int32_t imageIndex) {
                    if (imageIndex < 0 || static_cast<std::size_t>(imageIndex)
                            >= pending.glbModel.images.size()) return std::string{};
                    const std::string relative = "__nuby_internal/gltf/" + pending.assetPath
                            + "/image_" + std::to_string(imageIndex) + ".png";
                    const std::string resolved = projectAssetPath(relative);
                    if (resolved.empty()) return std::string{};
                    embeddedImagesByModel_[pending.assetPath].insert(resolved);
                    const std::string& encodedImage =
                            pending.glbModel.images[static_cast<std::size_t>(imageIndex)].bytes;
                    if (embeddedImageData_.emplace(resolved, encodedImage).second) {
                        pending.importedModel.retainedCpuBytes += encodedImage.size();
                    }
                    return relative;
                };
                const auto applyTextureBinding = [&](const GltfTextureBinding& binding,
                                                     TextureMapTransform& transform) {
                    transform.uvScale = binding.uvScale;
                    transform.uvOffset = binding.uvOffset;
                    transform.uvRotation = binding.uvRotation;
                    transform.uvSet = binding.uvSet;
                    transform.clampToEdge = binding.clampToEdge;
                    return texturePath(binding.imageIndex);
                };
                part.material.albedoTexture = applyTextureBinding(
                        primitive.material.baseColorTexture, part.material.albedoMapTransform);
                part.material.metallicRoughnessTexture = applyTextureBinding(
                        primitive.material.metallicRoughnessTexture,
                        part.material.metallicRoughnessMapTransform);
                part.material.normalTexture = applyTextureBinding(
                        primitive.material.normalTexture, part.material.normalMapTransform);
                part.material.occlusionTexture = applyTextureBinding(
                        primitive.material.occlusionTexture, part.material.occlusionMapTransform);
                part.material.emissiveTexture = applyTextureBinding(
                        primitive.material.emissiveTexture, part.material.emissiveMapTransform);
                if (primitive.material.clearcoatFactor > 0.0f) {
                    part.material.clearcoatTexture = applyTextureBinding(
                            primitive.material.clearcoatTexture,
                            part.material.clearcoatMapTransform);
                    part.material.clearcoatRoughnessTexture = applyTextureBinding(
                            primitive.material.clearcoatRoughnessTexture,
                            part.material.clearcoatRoughnessMapTransform);
                    part.material.clearcoatNormalTexture = applyTextureBinding(
                            primitive.material.clearcoatNormalTexture,
                            part.material.clearcoatNormalMapTransform);
                }
                if (primitive.material.sheenColorFactor[0] > 0.0f
                        || primitive.material.sheenColorFactor[1] > 0.0f
                        || primitive.material.sheenColorFactor[2] > 0.0f) {
                    part.material.sheenColorTexture = applyTextureBinding(
                            primitive.material.sheenColorTexture,
                            part.material.sheenColorMapTransform);
                    part.material.sheenRoughnessTexture = applyTextureBinding(
                            primitive.material.sheenRoughnessTexture,
                            part.material.sheenRoughnessMapTransform);
                }
                if (primitive.material.specularFactor > 0.0f) {
                    part.material.specularTexture = applyTextureBinding(
                            primitive.material.specularTexture,
                            part.material.specularMapTransform);
                    part.material.specularColorTexture = applyTextureBinding(
                            primitive.material.specularColorTexture,
                            part.material.specularColorMapTransform);
                }
                if (primitive.material.alphaBlend && !part.material.albedoTexture.empty()) {
                    part.material.opacityTexture = part.material.albedoTexture;
                    part.material.opacityMapTransform = part.material.albedoMapTransform;
                    part.material.opacityMapChannel = ScalarTextureChannel::Matte;
                }
            }
        }
        if (geometry == nullptr || !initializePendingGpuPrimitive(*geometry, part.mesh)) {
            const std::string assetPath = pending.assetPath;
            deleteMesh(part.mesh);
            deleteMesh(pending.activePart.mesh);
            deleteImportedParts(pending.importedModel);
            releaseEmbeddedImagesForModel(assetPath);
            pendingGpuUploads_.pop_front();
            pendingMeshAssets_.erase(assetPath);
            failedMeshAssets_.insert(assetPath);
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                    "GPU buffer allocation failed for model %s", assetPath.c_str());
            loadSceneAssets();
            return;
        }
        pending.activePart = std::move(part);
        pending.activePrimitiveInitialized = true;
        pending.vertexBytesUploaded = 0U;
        pending.indexBytesUploaded = 0U;
        pending.tangentBytesUploaded = 0U;
    }

    CpuGpuPrimitive& geometry = pending.isGlb
            ? pending.glbModel.gpuPrimitives[pending.nextPrimitive]
            : pending.objModel.parts[pending.nextPrimitive].geometry;
    const std::uint8_t* vertexData = geometry.skinned
            ? reinterpret_cast<const std::uint8_t*>(geometry.skinnedVertices.data())
            : reinterpret_cast<const std::uint8_t*>(geometry.vertices.data());
    const std::uint64_t vertexBytes = geometry.skinned
            ? static_cast<std::uint64_t>(geometry.skinnedVertices.size())
                    * sizeof(CpuSkinnedVertex)
            : static_cast<std::uint64_t>(geometry.vertices.size()) * sizeof(MeshVertex);
    const std::uint64_t indexBytes = static_cast<std::uint64_t>(geometry.indices.size())
            * sizeof(std::uint32_t);
    const std::uint64_t tangentBytes = static_cast<std::uint64_t>(geometry.tangents.size())
            * sizeof(std::array<float, 4>);
    std::uint64_t remainingBytes = kMaxGpuAssetUploadBytesPerTurn;
    BufferUploadChunk chunk;
    while (planBufferUploadChunk(vertexBytes, pending.vertexBytesUploaded,
                                 remainingBytes, chunk)) {
        glBindBuffer(GL_ARRAY_BUFFER, pending.activePart.mesh.vbo);
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(chunk.offset),
                static_cast<GLsizeiptr>(chunk.byteCount), vertexData + chunk.offset);
        pending.vertexBytesUploaded += chunk.byteCount;
        remainingBytes -= chunk.byteCount;
    }
    while (pending.vertexBytesUploaded == vertexBytes
            && planBufferUploadChunk(indexBytes, pending.indexBytesUploaded,
                                     remainingBytes, chunk)) {
        glBindBuffer(GL_ARRAY_BUFFER, pending.activePart.mesh.ebo);
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(chunk.offset),
                static_cast<GLsizeiptr>(chunk.byteCount),
                reinterpret_cast<const std::uint8_t*>(geometry.indices.data())
                        + chunk.offset);
        pending.indexBytesUploaded += chunk.byteCount;
        remainingBytes -= chunk.byteCount;
    }
    while (pending.vertexBytesUploaded == vertexBytes
            && pending.indexBytesUploaded == indexBytes
            && planBufferUploadChunk(tangentBytes, pending.tangentBytesUploaded,
                                     remainingBytes, chunk)) {
        glBindBuffer(GL_ARRAY_BUFFER, pending.activePart.mesh.tangentVbo);
        glBufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(chunk.offset),
                static_cast<GLsizeiptr>(chunk.byteCount),
                reinterpret_cast<const std::uint8_t*>(geometry.tangents.data())
                        + chunk.offset);
        pending.tangentBytesUploaded += chunk.byteCount;
        remainingBytes -= chunk.byteCount;
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        const std::string assetPath = pending.assetPath;
        deleteMesh(pending.activePart.mesh);
        deleteImportedParts(pending.importedModel);
        releaseEmbeddedImagesForModel(assetPath);
        pendingGpuUploads_.pop_front();
        pendingMeshAssets_.erase(assetPath);
        failedMeshAssets_.insert(assetPath);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                "Budgeted GPU upload failed for model %s", assetPath.c_str());
        loadSceneAssets();
        return;
    }
    if (pending.vertexBytesUploaded != vertexBytes || pending.indexBytesUploaded != indexBytes
            || pending.tangentBytesUploaded != tangentBytes) {
        return;
    }

    pending.importedModel.gpuBytes += pending.activePart.mesh.gpuBytes;
    std::vector<MeshVertex>().swap(geometry.vertices);
    std::vector<CpuSkinnedVertex>().swap(geometry.skinnedVertices);
    std::vector<std::uint32_t>().swap(geometry.indices);
    std::vector<std::array<float, 4>>().swap(geometry.tangents);
    pending.importedModel.parts.push_back(std::move(pending.activePart));
    pending.activePart = {};
    pending.activePrimitiveInitialized = false;
    pending.vertexBytesUploaded = 0U;
    pending.indexBytesUploaded = 0U;
    pending.tangentBytesUploaded = 0U;
    ++pending.nextPrimitive;
    if (pending.nextPrimitive != primitiveCount) return;

    const std::string assetPath = pending.assetPath;
    const bool isGlb = pending.isGlb;
    const bool skeletal = pending.glbModel.skeletal;
    const std::size_t jointCount = pending.importedModel.skeleton.size();
    const std::size_t clipCount = pending.importedModel.animations.size();
    const std::size_t partCount = pending.importedModel.parts.size();
    const std::size_t vertexCount = isGlb ? 0U : pending.totalVertexCount;
    importedMeshes_[assetPath] = std::move(pending.importedModel);
    pendingGpuUploads_.pop_front();
    pendingMeshAssets_.erase(assetPath);
    if (!isGlb) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                "Loaded OBJ %s (%zu triangle vertices, %zu material submeshes)",
                assetPath.c_str(), vertexCount, partCount);
    } else if (skeletal) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                "Loaded skeletal GLB %s (%zu joints, %zu clips, %zu primitives)",
                assetPath.c_str(), jointCount, clipCount, partCount);
    } else {
        __android_log_print(ANDROID_LOG_INFO, kLogTag,
                "Loaded static GLB %s (%zu mesh instances)", assetPath.c_str(), partCount);
    }
    loadSceneAssets();
}

void Renderer::releaseEmbeddedImagesForModel(const std::string& meshPath) {
    const auto owned = embeddedImagesByModel_.find(meshPath);
    if (owned == embeddedImagesByModel_.end()) return;
    for (const std::string& imagePath : owned->second) {
        bool referencedByAnotherModel = false;
        for (const auto& modelImages : embeddedImagesByModel_) {
            if (modelImages.first != meshPath
                    && modelImages.second.find(imagePath) != modelImages.second.end()) {
                referencedByAnotherModel = true;
                break;
            }
        }
        if (!referencedByAnotherModel) embeddedImageData_.erase(imagePath);
    }
    embeddedImagesByModel_.erase(owned);
}

Renderer::Mesh Renderer::makeSphere(int latitudeSegments, int longitudeSegments) {
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(latitudeSegments * longitudeSegments * 6));

    const auto vertexAt = [](float latitude, float longitude, float u, float v) {
        const float sinLatitude = std::sin(latitude);
        const float x = sinLatitude * std::cos(longitude);
        const float y = std::cos(latitude);
        const float z = sinLatitude * std::sin(longitude);
        return Vertex{{x, y, z}, {x, y, z}, {u, v}};
    };

    for (int lat = 0; lat < latitudeSegments; ++lat) {
        const float lat0 = kPi * static_cast<float>(lat) / static_cast<float>(latitudeSegments);
        const float lat1 = kPi * static_cast<float>(lat + 1) / static_cast<float>(latitudeSegments);
        const float v0 = 1.0f - static_cast<float>(lat) / static_cast<float>(latitudeSegments);
        const float v1 = 1.0f - static_cast<float>(lat + 1) / static_cast<float>(latitudeSegments);
        for (int lon = 0; lon < longitudeSegments; ++lon) {
            const float lon0 = 2.0f * kPi * static_cast<float>(lon) / static_cast<float>(longitudeSegments);
            const float lon1 = 2.0f * kPi * static_cast<float>(lon + 1) / static_cast<float>(longitudeSegments);
            const float u0 = static_cast<float>(lon) / static_cast<float>(longitudeSegments);
            const float u1 = static_cast<float>(lon + 1) / static_cast<float>(longitudeSegments);
            const Vertex a = vertexAt(lat0, lon0, u0, v0);
            const Vertex b = vertexAt(lat1, lon0, u0, v1);
            const Vertex c = vertexAt(lat1, lon1, u1, v1);
            const Vertex d = vertexAt(lat0, lon1, u1, v0);
            appendTriangle(vertices, a, b, c);
            appendTriangle(vertices, a, c, d);
        }
    }
    return uploadMesh(vertices);
}

Renderer::Mesh Renderer::makeTorus(int ringSegments, int tubeSegments,
                                    float majorRadius, float minorRadius) {
    std::vector<Vertex> vertices;
    vertices.reserve(static_cast<std::size_t>(ringSegments * tubeSegments * 6));

    const auto vertexAt = [majorRadius, minorRadius](float ring, float tube,
                                                       float u, float v) {
        const float cosTube = std::cos(tube);
        const float sinTube = std::sin(tube);
        const float cosRing = std::cos(ring);
        const float sinRing = std::sin(ring);
        const float radial = majorRadius + minorRadius * cosTube;
        return Vertex{{radial * cosRing, minorRadius * sinTube, radial * sinRing},
                      {cosTube * cosRing, sinTube, cosTube * sinRing}, {u, v}};
    };

    for (int ring = 0; ring < ringSegments; ++ring) {
        const float ring0 = 2.0f * kPi * static_cast<float>(ring) / static_cast<float>(ringSegments);
        const float ring1 = 2.0f * kPi * static_cast<float>(ring + 1) / static_cast<float>(ringSegments);
        const float u0 = static_cast<float>(ring) / static_cast<float>(ringSegments);
        const float u1 = static_cast<float>(ring + 1) / static_cast<float>(ringSegments);
        for (int tube = 0; tube < tubeSegments; ++tube) {
            const float tube0 = 2.0f * kPi * static_cast<float>(tube) / static_cast<float>(tubeSegments);
            const float tube1 = 2.0f * kPi * static_cast<float>(tube + 1) / static_cast<float>(tubeSegments);
            const float v0 = static_cast<float>(tube) / static_cast<float>(tubeSegments);
            const float v1 = static_cast<float>(tube + 1) / static_cast<float>(tubeSegments);
            const Vertex a = vertexAt(ring0, tube0, u0, v0);
            const Vertex b = vertexAt(ring1, tube0, u1, v0);
            const Vertex c = vertexAt(ring1, tube1, u1, v1);
            const Vertex d = vertexAt(ring0, tube1, u0, v1);
            appendTriangle(vertices, a, b, c);
            appendTriangle(vertices, a, c, d);
        }
    }
    return uploadMesh(vertices);
}

Renderer::Mesh Renderer::makeGround() {
    const float extent = 32.0f;
    const Vertex a{{-extent, -1.22f, -extent}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}};
    const Vertex b{{ extent, -1.22f, -extent}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}};
    const Vertex c{{ extent, -1.22f,  extent}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}};
    const Vertex d{{-extent, -1.22f,  extent}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}};
    std::vector<Vertex> vertices;
    vertices.reserve(6);
    appendTriangle(vertices, a, c, b);
    appendTriangle(vertices, a, d, c);
    return uploadMesh(vertices);
}

void Renderer::buildScene() {
    scene_.clear();
    scene_.physicsSettings().floorEnabled = true;
    scene_.physicsSettings().floorHeight = -1.22f;
    scene_.physicsSettings().floorRestitution = 0.38f;
    scene_.physicsSettings().floorFriction = 0.48f;
    if (!baseMeshesInitialized_) {
        sphere_ = makeSphere(28, 40);
        torus_ = makeTorus(64, 14, 1.0f, 0.025f);
        ground_ = makeGround();
        baseMeshesInitialized_ = true;
    }

    Entity& camera = scene_.create("Main Camera", Primitive::None);
    camera.visible = false;
    camera.camera.enabled = true;
    camera.orbitCamera.enabled = true;
    scene_.setActiveCamera(camera.id);

    Entity& ground = scene_.create("Horizon Grid", Primitive::Ground);
    ground.material.color = {0.02f, 0.04f, 0.08f};
    ground.material.proceduralGrid = true;

    Entity& firstRing = scene_.create("Aether Ring", Primitive::Torus);
    firstRing.transform.position = {0.0f, 0.05f, 0.0f};
    firstRing.transform.rotation[0] = 1.02f;
    firstRing.spin.radiansPerSecond[1] = 0.21f;
    firstRing.material.color = {0.10f, 0.75f, 0.78f};
    firstRing.material.emission = {0.02f, 0.34f, 0.38f};
    firstRing.animation.enabled = true;
    firstRing.animation.durationSeconds = 4.0f;
    firstRing.animation.position.addKeyframe(0.0f, {0.0f, 0.05f, 0.0f});
    firstRing.animation.position.addKeyframe(2.0f, {0.0f, 0.12f, 0.0f});
    firstRing.animation.position.addKeyframe(4.0f, {0.0f, 0.05f, 0.0f});
    firstRing.material.metallic = 0.58f;
    firstRing.material.roughness = 0.24f;

    Entity& secondRing = scene_.create("Halo Ring", Primitive::Torus);
    secondRing.transform.position = {0.0f, 0.02f, 0.0f};
    secondRing.transform.rotation[0] = -0.72f;
    secondRing.transform.scale = {1.28f, 1.28f, 1.28f};
    secondRing.spin.radiansPerSecond[1] = -0.15f;
    secondRing.material.color = {0.92f, 0.48f, 0.22f};
    secondRing.material.emission = {0.26f, 0.08f, 0.015f};
    secondRing.material.metallic = 0.72f;
    secondRing.material.roughness = 0.27f;

    Entity& contactHalo = scene_.create("Ground Halo", Primitive::Torus);
    contactHalo.transform.position = {0.0f, -1.16f, 0.0f};
    contactHalo.transform.scale = {1.52f, 1.52f, 1.52f};
    contactHalo.material.color = {0.10f, 0.42f, 0.50f};
    contactHalo.material.emission = {0.015f, 0.09f, 0.12f};
    contactHalo.material.metallic = 0.35f;
    contactHalo.material.roughness = 0.38f;

    Entity& physicsOrb = scene_.create("Physics Sample", Primitive::Sphere);
    physicsOrb.transform.position = {1.0f, 0.48f, 0.7f};
    physicsOrb.transform.scale = {0.19f, 0.19f, 0.19f};
    physicsOrb.rigidbody.enabled = true;
    physicsOrb.rigidbody.mass = 0.8f;
    physicsOrb.rigidbody.linearDamping = 0.06f;
    physicsOrb.sphereCollider.enabled = true;
    physicsOrb.sphereCollider.radius = 1.0f;
    physicsOrb.sphereCollider.restitution = 0.52f;
    physicsOrb.sphereCollider.friction = 0.42f;
    physicsOrb.material.color = {0.96f, 0.55f, 0.20f};
    physicsOrb.material.emission = {0.32f, 0.09f, 0.018f};
    physicsOrb.material.metallic = 0.26f;
    physicsOrb.material.roughness = 0.24f;

    Entity& core = scene_.create("Aether Core", Primitive::Sphere);
    core.transform.scale = {0.70f, 0.70f, 0.70f};
    core.spin.radiansPerSecond[1] = 0.18f;
    core.orbit.enabled = true;
    core.orbit.verticalAmplitude = 0.07f;
    core.orbit.angularSpeed = 1.15f;
    core.material.color = {0.29f, 0.25f, 0.78f};
    core.material.emission = {0.035f, 0.025f, 0.16f};
    core.material.metallic = 0.18f;
    core.material.roughness = 0.31f;
    core.material.opacity = 0.82f;
    const std::uint32_t coreId = core.id;

    Entity& innerCore = scene_.create("Core Light", Primitive::Sphere);
    innerCore.transform.scale = {0.36f, 0.36f, 0.36f};
    scene_.setParent(innerCore.id, coreId);
    innerCore.pulse.enabled = true;
    innerCore.pulse.baseScale = innerCore.transform.scale;
    innerCore.pulse.amplitude = 0.10f;
    innerCore.pulse.frequency = 0.42f;
    innerCore.material.color = {0.16f, 0.78f, 0.88f};
    innerCore.material.emission = {0.20f, 0.74f, 0.88f};
    innerCore.material.metallic = 0.08f;
    innerCore.material.roughness = 0.16f;

    // Engine-lab fixture: its punched PNG must cut out identically in the camera and CSM depth map.
    Entity& alphaMaskCard = scene_.create("GLB Alpha Mask and Shadow Sample",
            Primitive::ImportedMesh);
    alphaMaskCard.meshAsset = "engine-tests/alpha_mask_card.glb";
    alphaMaskCard.transform.position = {1.18f, -1.22f, 0.38f};
    alphaMaskCard.transform.scale = {0.42f, 0.42f, 0.42f};

    // Engine-lab fixture: verify embedded sheen maps and clearcoat-over-sheen on a real GLB.
    Entity& sheenLayerSample = scene_.create("GLB Sheen and Clearcoat Layer Sample",
            Primitive::ImportedMesh);
    sheenLayerSample.meshAsset = "engine-tests/sheen_clearcoat_sphere.glb";
    sheenLayerSample.transform.position = {-1.55f, 0.62f, 0.22f};

    for (int index = 0; index < 3; ++index) {
        const bool useImportedMesh = index == 1;
        Entity& satellite = scene_.create("Orbiting Shard " + std::to_string(index + 1),
                useImportedMesh ? Primitive::ImportedMesh : Primitive::Sphere);
        if (useImportedMesh) satellite.meshAsset = "meshes/orbital_shard.obj";
        const float size = useImportedMesh ? 0.13f : 0.075f + 0.018f * static_cast<float>(index);
        const float orbit = 1.46f + 0.08f * static_cast<float>(index);
        satellite.transform.scale = {size, size, size};
        satellite.orbit.enabled = true;
        satellite.orbit.radius = orbit;
        satellite.orbit.angularSpeed = 0.48f + 0.08f * static_cast<float>(index);
        satellite.orbit.phase = 2.0f * kPi * static_cast<float>(index) / 3.0f;
        satellite.orbit.verticalAmplitude = 0.44f;
        satellite.orbit.verticalFrequency = 1.7f;
        if (!useImportedMesh) {
            if (index == 1) {
                satellite.material.color = {0.95f, 0.43f, 0.20f};
            } else {
                satellite.material.color = {0.18f, 0.79f, 0.78f};
            }
            satellite.material.emission = {satellite.material.color[0] * 0.28f,
                                           satellite.material.color[1] * 0.20f,
                                           satellite.material.color[2] * 0.16f};
            satellite.material.metallic = 0.24f;
            satellite.material.roughness = 0.29f;
        }
    }

    Entity& cyanLight = scene_.create("Aether Point Light", Primitive::None);
    cyanLight.visible = false;
    cyanLight.pointLight.enabled = true;
    cyanLight.pointLight.color = {0.10f, 0.62f, 1.0f};
    cyanLight.pointLight.intensity = 6.5f;
    cyanLight.pointLight.range = 5.0f;
    cyanLight.orbit.enabled = true;
    cyanLight.orbit.radius = 1.55f;
    cyanLight.orbit.angularSpeed = 0.34f;
    cyanLight.orbit.verticalAmplitude = 0.62f;
    cyanLight.orbit.verticalFrequency = 0.72f;

    Entity& emberLight = scene_.create("Ember Point Light", Primitive::None);
    emberLight.visible = false;
    emberLight.transform.position = {-1.35f, 0.48f, -0.82f};
    emberLight.pointLight.enabled = true;
    emberLight.pointLight.color = {1.0f, 0.23f, 0.075f};
    emberLight.pointLight.intensity = 4.5f;
    emberLight.pointLight.range = 4.2f;

    loadSceneAssets();
}

Scene& Renderer::activeScene() {
    Scene* const loadedScene = projectRuntime_.scene();
    if (loadedScene != nullptr) return *loadedScene;
    Scene* const editorScene = editorSession_.scene();
    return editorScene != nullptr ? *editorScene : scene_;
}

const Scene& Renderer::activeScene() const {
    const Scene* const loadedScene = projectRuntime_.scene();
    if (loadedScene != nullptr) return *loadedScene;
    const Scene* const editorScene = editorSession_.scene();
    return editorScene != nullptr ? *editorScene : scene_;
}

std::string Renderer::projectAssetPath(const std::string& relativePath) const {
    if (!safeRelativeAssetPath(relativePath)) return {};
    if (projectAssetPrefix_.empty()
            || relativePath.compare(0, projectAssetPrefix_.size(), projectAssetPrefix_) == 0) {
        return relativePath;
    }
    const std::string resolved = projectAssetPrefix_ + relativePath;
    return safeRelativeAssetPath(resolved) ? resolved : std::string{};
}

bool Renderer::loadProject(const std::string& descriptorAssetPath) {
    constexpr std::size_t kMaximumProjectDescriptorBytes = 64U * 1024U;
    std::string descriptorText;
    if (!safeRelativeAssetPath(descriptorAssetPath)
            || !readTextAsset(assetManager_, descriptorAssetPath,
                              kMaximumProjectDescriptorBytes, descriptorText)) {
        projectLoadError_ = "Project descriptor is unsafe, missing, or too large";
        return false;
    }

    const std::size_t directoryEnd = descriptorAssetPath.find_last_of('/');
    const std::string candidatePrefix = directoryEnd == std::string::npos
            ? std::string{} : descriptorAssetPath.substr(0, directoryEnd + 1);
    std::istringstream descriptor(descriptorText);
    const ProjectAssetReader readProjectAsset = [this, &candidatePrefix](
            const std::string& relativePath, std::string& contents, std::string& error) {
        if (!safeRelativeAssetPath(relativePath)) {
            error = "Project asset path is unsafe";
            return false;
        }
        const std::string assetPath = candidatePrefix + relativePath;
        if (!safeRelativeAssetPath(assetPath)
                || !readTextAsset(assetManager_, assetPath, 16U * 1024U * 1024U, contents)) {
            error = "Project asset is missing or exceeds the size limit";
            return false;
        }
        return true;
    };
    std::string error;
    if (!projectRuntime_.load(descriptor, readProjectAsset, error)) {
        projectLoadError_ = error.empty() ? "Project could not be loaded" : std::move(error);
        return false;
    }

    clearSceneAudioSources();
    editorSession_.close();
    projectAssetPrefix_ = candidatePrefix;
    projectLoadError_.clear();
    demoMode_ = 0;
    gameMode_ = false;
    adventureMode_ = false;
    gameSwipeDistance_ = 0.0f;
    releaseSceneAssets();
    loadSceneAssets();
    return true;
}

void Renderer::unloadProject() {
    if (!projectRuntime_.loaded()) {
        projectLoadError_.clear();
        return;
    }
    clearSceneAudioSources();
    projectRuntime_.unload();
    projectAssetPrefix_.clear();
    projectLoadError_.clear();
    releaseSceneAssets();
    demoMode_ = 0;
    gameMode_ = false;
    adventureMode_ = false;
    buildScene();
}

std::string Renderer::projectStatusText() const {
    if (!projectLoadError_.empty()) return "Project load failed: " + projectLoadError_;
    const GameProject* const project = projectRuntime_.project();
    return project == nullptr ? "No project loaded" : project->displayName;
}

bool Renderer::saveEditorProject(std::string& descriptorText, std::string& sceneText,
                                 std::string& error) const {
    if (demoMode_ != 3 || !editorSession_.active()) {
        error = "Open the visual editor before saving a project";
        return false;
    }
    return editorSession_.saveProjectText(descriptorText, sceneText, error);
}

bool Renderer::loadEditorProject(const std::string& descriptorText,
                                 const std::string& sceneText, std::string& error) {
    if (demoMode_ != 3) {
        error = "Open the visual editor before loading a project";
        return false;
    }
    if (!editorSession_.loadProjectText(descriptorText, sceneText, error)) return false;
    clearSceneAudioSources();
    projectRuntime_.unload();
    projectAssetPrefix_.clear();
    projectLoadError_.clear();
    gameMode_ = false;
    adventureMode_ = false;
    releaseSceneAssets();
    loadSceneAssets();
    return true;
}

bool Renderer::editorAddPrimitive(int type) {
    if (demoMode_ != 3 || !editorSession_.active()) return false;
    const Primitive primitive = type == 0 ? Primitive::Sphere
            : type == 1 ? Primitive::Torus : Primitive::None;
    std::string error;
    const bool added = editorSession_.addPrimitive(primitive, error);
    if (!added && !error.empty()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "Scene editor add object failed: %s", error.c_str());
    }
    return added;
}

bool Renderer::editorSelectEntity(std::uint32_t entityId) {
    return demoMode_ == 3 && editorSession_.active()
            && editorSession_.selectEntity(entityId);
}

bool Renderer::editorSelectNext(int direction) {
    return demoMode_ == 3 && editorSession_.active()
            && editorSession_.selectNext(direction);
}

bool Renderer::editorTranslate(int axis, float amount) {
    if (demoMode_ != 3 || axis < 0 || axis > 2) return false;
    const float x = axis == 0 ? amount : 0.0f;
    const float y = axis == 1 ? amount : 0.0f;
    const float z = axis == 2 ? amount : 0.0f;
    return editorSession_.moveSelected(x, y, z);
}

bool Renderer::editorRotate(int axis, float amount) {
    if (demoMode_ != 3 || axis < 0 || axis > 2) return false;
    const float x = axis == 0 ? amount : 0.0f;
    const float y = axis == 1 ? amount : 0.0f;
    const float z = axis == 2 ? amount : 0.0f;
    return editorSession_.rotateSelected(x, y, z);
}

bool Renderer::editorScale(float factor) {
    return demoMode_ == 3 && editorSession_.scaleSelected(factor);
}

bool Renderer::editorCycleColor() {
    return demoMode_ == 3 && editorSession_.cycleSelectedColor();
}

bool Renderer::editorDuplicate() {
    if (demoMode_ != 3) return false;
    std::string error;
    return editorSession_.duplicateSelected(error);
}

bool Renderer::editorDelete() {
    return demoMode_ == 3 && editorSession_.deleteSelected();
}

std::string Renderer::editorSelectionStatus() const {
    return demoMode_ == 3 ? editorSession_.selectionStatus() : std::string{};
}

void Renderer::setGameMode(bool enabled) {
    setDemoMode(enabled ? 1 : 0);
}

void Renderer::setDemoMode(int mode) {
    const int safeMode = std::clamp(mode, 0, 3);
    if (safeMode == 3 && demoMode_ == 3 && editorSession_.active()
            && !projectRuntime_.loaded()) return;

    const bool leavingProject = projectRuntime_.loaded();
    if (leavingProject) {
        projectRuntime_.unload();
        projectAssetPrefix_.clear();
        projectLoadError_.clear();
    }
    if (safeMode != 3) editorSession_.close();
    if (demoMode_ == safeMode && !leavingProject && safeMode != 3) return;

    clearSceneAudioSources();
    releaseSceneAssets();
    gameSwipeDistance_ = 0.0f;
    if (safeMode == 3) {
        std::string error;
        projectAssetPrefix_.clear();
        if (!editorSession_.createDefaultProject(error)) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                    "Could not create editor starter project: %s", error.c_str());
            editorSession_.close();
            demoMode_ = 0;
            gameMode_ = false;
            adventureMode_ = false;
            buildScene();
            loadSceneAssets();
            return;
        }
        demoMode_ = 3;
        gameMode_ = false;
        adventureMode_ = false;
        loadSceneAssets();
        return;
    }

    demoMode_ = safeMode;
    gameMode_ = demoMode_ == 1;
    adventureMode_ = demoMode_ == 2;
    if (gameMode_) {
        sampleGame_.start(scene_);
    } else if (adventureMode_) {
        adventureGame_.start(scene_);
    } else {
        buildScene();
    }
    loadSceneAssets();
}

void Renderer::setAdventureMoveInput(float horizontal, float forward) {
    if (adventureMode_) adventureGame_.setMoveInput(scene_, horizontal, forward);
}

void Renderer::jumpAdventureCharacter() {
    if (adventureMode_ && adventureGame_.jump(scene_)) {
        playAudioClip(jumpAudioClip_, 0.78f, 96U);
    }
}

void Renderer::restartGame() {
    gameSwipeDistance_ = 0.0f;
    audioEngine_.stopAll(0.045f);
    if (gameMode_) sampleGame_.start(scene_);
    else if (adventureMode_) adventureGame_.start(scene_);
}

void Renderer::pauseAudio() {
    audioEngine_.pauseAll(true);
    androidAudioOutput_.pause();
}

void Renderer::resumeAudio() {
    audioEngine_.pauseAll(false);
    androidAudioOutput_.resume();
}

bool Renderer::saveAdventureProgress(std::string& serialized, std::string& error) const {
    if (!adventureMode_) {
        error = "adventure mode is not active";
        return false;
    }
    AdventureSaveData data;
    return adventureGame_.saveData(scene_, data, error)
            && serializeAdventureSave(data, serialized, error);
}

bool Renderer::loadAdventureProgress(const std::string& serialized, std::string& error) {
    if (!adventureMode_) {
        error = "adventure mode is not active";
        return false;
    }
    AdventureSaveData data;
    if (!parseAdventureSave(serialized, data, error)) return false;
    return adventureGame_.loadData(scene_, data, error);
}

std::string Renderer::gameStatusText() const {
    if (gameMode_) return sampleGame_.statusText();
    if (adventureMode_) return adventureGame_.statusText();
    return {};
}

void Renderer::releaseSceneAssets() {
    asyncAssetTasks_.cancelAll();
    pendingMeshAssets_.clear();
    failedMeshAssets_.clear();
    pendingTextureAssets_.clear();
    failedTextureAssets_.clear();
    embeddedImageData_.clear();
    embeddedImagesByModel_.clear();
    textureGpuBytes_.clear();
    assetMemoryCache_.clear();
    for (PendingGpuAsset& pending : pendingGpuUploads_) {
        if (pending.activePart.mesh.vao != 0U) {
            glDeleteVertexArrays(1, &pending.activePart.mesh.vao);
        }
        if (pending.activePart.mesh.vbo != 0U) glDeleteBuffers(1, &pending.activePart.mesh.vbo);
        if (pending.activePart.mesh.ebo != 0U) glDeleteBuffers(1, &pending.activePart.mesh.ebo);
        if (pending.activePart.mesh.tangentVbo != 0U) glDeleteBuffers(1, &pending.activePart.mesh.tangentVbo);
        for (ImportedPart& part : pending.importedModel.parts) {
            if (part.mesh.vao != 0) glDeleteVertexArrays(1, &part.mesh.vao);
            if (part.mesh.vbo != 0) glDeleteBuffers(1, &part.mesh.vbo);
            if (part.mesh.ebo != 0) glDeleteBuffers(1, &part.mesh.ebo);
            if (part.mesh.tangentVbo != 0) glDeleteBuffers(1, &part.mesh.tangentVbo);
        }
    }
    pendingGpuUploads_.clear();
    for (PendingTextureUpload& upload : pendingTextureUploads_) {
        if (upload.texture != 0U) glDeleteTextures(1, &upload.texture);
    }
    pendingTextureUploads_.clear();
    for (auto& entry : importedMeshes_) {
        for (ImportedPart& part : entry.second.parts) {
            if (part.mesh.vao != 0) glDeleteVertexArrays(1, &part.mesh.vao);
            if (part.mesh.vbo != 0) glDeleteBuffers(1, &part.mesh.vbo);
            if (part.mesh.ebo != 0) glDeleteBuffers(1, &part.mesh.ebo);
            if (part.mesh.tangentVbo != 0) glDeleteBuffers(1, &part.mesh.tangentVbo);
        }
    }
    importedMeshes_.clear();
    evaluatedSkeletalPoses_.clear();
    for (const auto& entry : textures_) {
        if (entry.second != 0) glDeleteTextures(1, &entry.second);
    }
    textures_.clear();
    opaqueDrawItems_.clear();
    transparentDrawItems_.clear();
}

void Renderer::synchronizeSkeletalClipDurations(Scene& frameScene) {
    for (Entity& entity : frameScene.entities()) {
        if (!entity.skeletalAnimation.enabled || entity.meshAsset.empty()) continue;
        const auto model = importedMeshes_.find(projectAssetPath(entity.meshAsset));
        if (model == importedMeshes_.end()) continue;
        const auto clip = std::find_if(model->second.animations.begin(),
                model->second.animations.end(), [&entity](const GltfAnimation& animation) {
                    return animation.name == entity.skeletalAnimation.clipName;
                });
        if (clip == model->second.animations.end()
                || !std::isfinite(clip->clip.durationSeconds)
                || clip->clip.durationSeconds <= 0.0f) {
            continue;
        }
        SkeletalAnimationComponent& playback = entity.skeletalAnimation;
        playback.durationSeconds = clip->clip.durationSeconds;
        if (playback.timeSeconds > playback.durationSeconds) {
            playback.timeSeconds = playback.looping
                    ? std::fmod(playback.timeSeconds, playback.durationSeconds)
                    : playback.durationSeconds;
        }
    }
}

void Renderer::loadSceneAssets() {
    std::unordered_set<std::string> requiredMeshes;
    std::unordered_set<std::string> requiredTextures;
    const auto addMaterialTextures = [this, &requiredTextures](const Material& material) {
        const std::array<const std::string*, 15> paths{{
                &material.albedoTexture,
                &material.normalTexture,
                &material.metallicRoughnessTexture,
                &material.occlusionTexture,
                &material.metallicTexture,
                &material.roughnessTexture,
                &material.opacityTexture,
                &material.emissiveTexture,
                &material.clearcoatTexture,
                &material.clearcoatRoughnessTexture,
                &material.clearcoatNormalTexture,
                &material.specularTexture,
                &material.specularColorTexture,
                &material.sheenColorTexture,
                &material.sheenRoughnessTexture
        }};
        for (const std::string* path : paths) {
            if (path->empty()) continue;
            const std::string assetPath = projectAssetPath(*path);
            if (!assetPath.empty()) requiredTextures.insert(assetPath);
        }
    };

    for (const Entity& entity : activeScene().entities()) {
        addMaterialTextures(entity.material);
        if (entity.primitive != Primitive::ImportedMesh || entity.meshAsset.empty()) continue;
        const std::string meshPath = projectAssetPath(entity.meshAsset);
        if (!meshPath.empty()) requiredMeshes.insert(meshPath);
    }

    for (auto upload = pendingGpuUploads_.begin(); upload != pendingGpuUploads_.end();) {
        if (requiredMeshes.find(upload->assetPath) != requiredMeshes.end()) {
            ++upload;
            continue;
        }
        if (upload->activePart.mesh.vao != 0U) {
            glDeleteVertexArrays(1, &upload->activePart.mesh.vao);
        }
        if (upload->activePart.mesh.vbo != 0U) {
            glDeleteBuffers(1, &upload->activePart.mesh.vbo);
        }
        if (upload->activePart.mesh.ebo != 0U) {
            glDeleteBuffers(1, &upload->activePart.mesh.ebo);
        }
        if (upload->activePart.mesh.tangentVbo != 0U) {
            glDeleteBuffers(1, &upload->activePart.mesh.tangentVbo);
        }
        for (ImportedPart& part : upload->importedModel.parts) {
            if (part.mesh.vao != 0) glDeleteVertexArrays(1, &part.mesh.vao);
            if (part.mesh.vbo != 0) glDeleteBuffers(1, &part.mesh.vbo);
            if (part.mesh.ebo != 0) glDeleteBuffers(1, &part.mesh.ebo);
            if (part.mesh.tangentVbo != 0) glDeleteBuffers(1, &part.mesh.tangentVbo);
        }
        releaseEmbeddedImagesForModel(upload->assetPath);
        pendingMeshAssets_.erase(upload->assetPath);
        upload = pendingGpuUploads_.erase(upload);
    }

    for (auto pending = pendingMeshAssets_.begin(); pending != pendingMeshAssets_.end();) {
        if (requiredMeshes.find(*pending) != requiredMeshes.end()) {
            ++pending;
            continue;
        }
        asyncAssetTasks_.cancel("model:" + *pending);
        pending = pendingMeshAssets_.erase(pending);
    }
    for (auto failed = failedMeshAssets_.begin(); failed != failedMeshAssets_.end();) {
        if (requiredMeshes.find(*failed) != requiredMeshes.end()) ++failed;
        else failed = failedMeshAssets_.erase(failed);
    }
    for (const std::string& meshPath : requiredMeshes) {
        if (importedMeshes_.find(meshPath) != importedMeshes_.end()
                || pendingMeshAssets_.find(meshPath) != pendingMeshAssets_.end()
                || failedMeshAssets_.find(meshPath) != failedMeshAssets_.end()) continue;
        const bool isGlb = meshPath.size() >= 4U
                && meshPath.compare(meshPath.size() - 4U, 4U, ".glb") == 0;
        if (!pendingMeshAssets_.insert(meshPath).second) continue;
        AAssetManager* manager = assetManager_;
        const bool queued = asyncAssetTasks_.submit("model:" + meshPath,
                [this, manager, meshPath, isGlb]() {
            CpuObjModel objModel;
            CpuGlbModel glbModel;
            std::string error;
            bool success = false;
            try {
                success = isGlb ? prepareGlbAsset(manager, meshPath, glbModel, error)
                                : prepareObjAsset(manager, meshPath, objModel, error);
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "unknown exception while preparing model";
            }
            return [this, meshPath, isGlb, success, error = std::move(error),
                    objModel = std::move(objModel), glbModel = std::move(glbModel)]() mutable {
                if (!success) {
                    pendingMeshAssets_.erase(meshPath);
                    failedMeshAssets_.insert(meshPath);
                    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "Could not prepare model %s: %s", meshPath.c_str(), error.c_str());
                    loadSceneAssets();
                    return;
                }
                if (isGlb) uploadPreparedGlbAsset(meshPath, std::move(glbModel));
                else uploadPreparedObjAsset(meshPath, std::move(objModel));
            };
        });
        if (!queued) pendingMeshAssets_.erase(meshPath);
    }

    for (Entity& entity : activeScene().entities()) {
        if (entity.primitive != Primitive::ImportedMesh || !entity.skeletalAnimation.enabled) continue;
        const auto model = importedMeshes_.find(projectAssetPath(entity.meshAsset));
        if (model == importedMeshes_.end()) continue;
        const auto clip = std::find_if(model->second.animations.begin(), model->second.animations.end(),
                [&entity](const GltfAnimation& animation) {
                    return animation.name == entity.skeletalAnimation.clipName;
                });
        if (clip == model->second.animations.end()) continue;
        entity.skeletalAnimation.durationSeconds = clip->clip.durationSeconds;
        entity.skeletalAnimation.timeSeconds = std::clamp(
                entity.skeletalAnimation.timeSeconds, 0.0f, clip->clip.durationSeconds);
    }

    for (const std::string& meshPath : requiredMeshes) {
        const auto model = importedMeshes_.find(meshPath);
        if (model == importedMeshes_.end()) continue;
        for (const ImportedPart& part : model->second.parts) {
            if (part.hasMaterial) addMaterialTextures(part.material);
        }
    }

    std::unordered_set<std::string> requiredCacheKeys;
    requiredCacheKeys.reserve(requiredMeshes.size() + requiredTextures.size());
    const std::uint64_t lastUse = ++assetUseSequence_;
    for (const std::string& meshPath : requiredMeshes) {
        const auto model = importedMeshes_.find(meshPath);
        if (model == importedMeshes_.end()) continue;
        const std::string cacheKey = "mesh:" + meshPath;
        requiredCacheKeys.insert(cacheKey);
        const std::uint64_t cpuBytes = model->second.retainedCpuBytes;
        const std::uint64_t gpuBytes = model->second.gpuBytes;
        const std::uint64_t estimatedBytes = cpuBytes > UINT64_MAX - gpuBytes
                ? UINT64_MAX : cpuBytes + gpuBytes;
        assetMemoryCache_.update(cacheKey, estimatedBytes, lastUse);
    }
    for (const std::string& texturePath : requiredTextures) {
        const auto texture = textures_.find(texturePath);
        if (texture == textures_.end()) continue;
        const std::string cacheKey = "texture:" + texturePath;
        requiredCacheKeys.insert(cacheKey);
        const auto bytes = textureGpuBytes_.find(texturePath);
        assetMemoryCache_.update(cacheKey,
                bytes == textureGpuBytes_.end() ? 0U : bytes->second, lastUse);
    }

    constexpr AssetMemoryCache::ByteCount kUnusedAssetMemoryBudgetBytes = 16U * 1024U * 1024U;
    const std::vector<std::string> evicted = assetMemoryCache_.evictUnused(
            requiredCacheKeys, kUnusedAssetMemoryBudgetBytes);
    for (const std::string& cacheKey : evicted) {
        if (cacheKey.compare(0U, 5U, "mesh:") == 0) {
            const std::string meshPath = cacheKey.substr(5U);
            const auto model = importedMeshes_.find(meshPath);
            if (model != importedMeshes_.end()) {
                for (ImportedPart& part : model->second.parts) {
                    if (part.mesh.vao != 0) glDeleteVertexArrays(1, &part.mesh.vao);
                    if (part.mesh.vbo != 0) glDeleteBuffers(1, &part.mesh.vbo);
                    if (part.mesh.ebo != 0) glDeleteBuffers(1, &part.mesh.ebo);
                    if (part.mesh.tangentVbo != 0) glDeleteBuffers(1, &part.mesh.tangentVbo);
                }
                importedMeshes_.erase(model);
            }
            releaseEmbeddedImagesForModel(meshPath);
        } else if (cacheKey.compare(0U, 8U, "texture:") == 0) {
            const std::string texturePath = cacheKey.substr(8U);
            const auto texture = textures_.find(texturePath);
            if (texture != textures_.end()) {
                if (texture->second != 0) glDeleteTextures(1, &texture->second);
                textures_.erase(texture);
            }
            textureGpuBytes_.erase(texturePath);
        }
    }

    for (auto upload = pendingTextureUploads_.begin(); upload != pendingTextureUploads_.end();) {
        if (requiredTextures.find(upload->assetPath) != requiredTextures.end()) {
            ++upload;
            continue;
        }
        if (upload->texture != 0U) glDeleteTextures(1, &upload->texture);
        pendingTextureAssets_.erase(upload->assetPath);
        upload = pendingTextureUploads_.erase(upload);
    }
    for (auto pending = pendingTextureAssets_.begin(); pending != pendingTextureAssets_.end();) {
        if (requiredTextures.find(*pending) != requiredTextures.end()) {
            ++pending;
            continue;
        }
        asyncAssetTasks_.cancel("image:" + *pending);
        pending = pendingTextureAssets_.erase(pending);
    }
    for (auto failed = failedTextureAssets_.begin(); failed != failedTextureAssets_.end();) {
        if (requiredTextures.find(*failed) != requiredTextures.end()) ++failed;
        else failed = failedTextureAssets_.erase(failed);
    }
    for (const std::string& assetPath : requiredTextures) requestTextureDecode(assetPath);
}

std::string Renderer::assetMemoryStatusText() const {
    constexpr std::uint64_t kBytesPerMiB = 1024U * 1024U;
    const std::uint64_t bytes = assetMemoryCache_.totalBytes();
    std::uint64_t mebibytes = bytes / kBytesPerMiB;
    if (bytes % kBytesPerMiB >= kBytesPerMiB / 2U) ++mebibytes;
    return "CACHE ~" + std::to_string(mebibytes) + "MB";
}

std::string Renderer::frameTimingStatusText() const {
    const FrameTimingStats stats = frameTimingWindow_.snapshot();
    if (stats.sampleCount == 0U) return "CPU P95 --";
    std::ostringstream status;
    status << std::fixed << std::setprecision(1)
           << "CPU P95 " << stats.p95Milliseconds << "ms · MAX "
           << stats.maximumMilliseconds << "ms";
    return status.str();
}

int Renderer::sceneAssetStatus() const {
    bool hasImportedAssets = false;
    bool missingMesh = false;
    bool missingTexture = false;
    bool failed = false;
    const auto inspectMaterial = [this, &missingTexture, &failed](const Material& material) {
        const std::array<const std::string*, 15> paths{{
                &material.albedoTexture,
                &material.normalTexture,
                &material.metallicRoughnessTexture,
                &material.occlusionTexture,
                &material.metallicTexture,
                &material.roughnessTexture,
                &material.opacityTexture,
                &material.emissiveTexture,
                &material.clearcoatTexture,
                &material.clearcoatRoughnessTexture,
                &material.clearcoatNormalTexture,
                &material.specularTexture,
                &material.specularColorTexture,
                &material.sheenColorTexture,
                &material.sheenRoughnessTexture
        }};
        for (const std::string* path : paths) {
            if (path->empty()) continue;
            const std::string assetPath = projectAssetPath(*path);
            if (assetPath.empty() || textures_.find(assetPath) != textures_.end()) continue;
            missingTexture = true;
            if (failedTextureAssets_.find(assetPath) != failedTextureAssets_.end()) failed = true;
        }
    };

    for (const Entity& entity : activeScene().entities()) {
        inspectMaterial(entity.material);
        if (entity.primitive != Primitive::ImportedMesh || entity.meshAsset.empty()) continue;
        hasImportedAssets = true;
        const std::string meshPath = projectAssetPath(entity.meshAsset);
        const auto imported = importedMeshes_.find(meshPath);
        if (imported == importedMeshes_.end() || imported->second.parts.empty()) {
            missingMesh = true;
            if (failedMeshAssets_.find(meshPath) != failedMeshAssets_.end()) failed = true;
            continue;
        }
        for (const ImportedPart& part : imported->second.parts) {
            if (part.hasMaterial) inspectMaterial(part.material);
        }
    }
    if (failed) return 4;
    if (!hasImportedAssets) return 0;
    if (missingMesh) return 1;
    if (missingTexture) return 2;
    return 3;
}

const Renderer::Mesh* Renderer::meshFor(const Entity& entity) const {
    switch (entity.primitive) {
        case Primitive::Sphere: return &sphere_;
        case Primitive::Torus: return &torus_;
        case Primitive::Ground: return &ground_;
        case Primitive::ImportedMesh: {
            const std::string meshPath = projectAssetPath(entity.meshAsset);
            const auto found = importedMeshes_.find(meshPath);
            if (found == importedMeshes_.end() || found->second.parts.empty()) return &sphere_;
            return &found->second.parts.front().mesh;
        }
        case Primitive::None: return nullptr;
    }
    return nullptr;
}

void Renderer::initializeShadowMap() {
    if (fallbackRenderer_ || !csmShaderSupported_) return;
    shadowProgram_ = createProgram(kShadowVertexShader, kShadowFragmentShader);
    if (shadowProgram_ == 0) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Shadow shaders unavailable; continuing without dynamic shadows");
        return;
    }
    shadowProgramLightViewProjection_ = glGetUniformLocation(shadowProgram_, "uLightViewProjection");
    shadowProgramModel_ = glGetUniformLocation(shadowProgram_, "uModel");
    shadowProgramUseInstancing_ = glGetUniformLocation(shadowProgram_, "uUseInstancing");
    shadowProgramSkinningEnabled_ = glGetUniformLocation(shadowProgram_, "uSkinningEnabled");
    shadowProgramAlphaMaskEnabled_ = glGetUniformLocation(shadowProgram_, "uAlphaMaskEnabled");
    shadowProgramAlphaCutoff_ = glGetUniformLocation(shadowProgram_, "uAlphaCutoff");
    shadowProgramBaseColorAlpha_ = glGetUniformLocation(shadowProgram_, "uBaseColorAlpha");
    shadowProgramAlbedoTexture_ = glGetUniformLocation(shadowProgram_, "uAlbedoTexture");
    shadowProgramHasAlbedoTexture_ = glGetUniformLocation(shadowProgram_, "uHasAlbedoTexture");
    shadowProgramAlbedoUvScale_ = glGetUniformLocation(shadowProgram_, "uAlbedoUvScale");
    shadowProgramAlbedoUvOffset_ = glGetUniformLocation(shadowProgram_, "uAlbedoUvOffset");
    shadowProgramAlbedoUvRotation_ = glGetUniformLocation(shadowProgram_, "uAlbedoUvRotation");
    shadowProgramAlbedoUvSet_ = glGetUniformLocation(shadowProgram_, "uAlbedoUvSet");
    const GLuint paletteBlock = glGetUniformBlockIndex(shadowProgram_, "SkinPalette");
    if (paletteBlock != GL_INVALID_INDEX) {
        glUniformBlockBinding(shadowProgram_, paletteBlock, kSkinPaletteBindingPoint);
    }

    constexpr GLsizei atlasWidth = kShadowMapSize
            * static_cast<GLsizei>(kDirectionalShadowCascadeCount);
    glGenTextures(1, &shadowDepthTexture_);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16,
                 atlasWidth, kShadowMapSize, 0,
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &shadowFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                           GL_TEXTURE_2D, shadowDepthTexture_, 0);
    const GLenum noColorBuffer = GL_NONE;
    glDrawBuffers(1, &noColorBuffer);
    glReadBuffer(GL_NONE);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    shadowsAvailable_ = status == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!shadowsAvailable_) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "Shadow framebuffer unavailable (0x%x); using unshadowed lighting", status);
        if (shadowFramebuffer_ != 0) glDeleteFramebuffers(1, &shadowFramebuffer_);
        if (shadowDepthTexture_ != 0) glDeleteTextures(1, &shadowDepthTexture_);
        shadowFramebuffer_ = 0;
        shadowDepthTexture_ = 0;
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "Cascaded directional shadows enabled: %zu x %d atlas (%dx%d per cascade)",
            kDirectionalShadowCascadeCount,
            kShadowMapSize * static_cast<int>(kDirectionalShadowCascadeCount),
            kShadowMapSize, kShadowMapSize);
}

bool Renderer::meshIntersectsFrustum(const Mesh& mesh, const Mat4& model,
                                     const ViewFrustum& frustum) const {
    if (!mesh.boundsValid || !frustum.valid) return true;
    Vec3 worldCenter{
            model[0] * mesh.boundsCenter[0] + model[4] * mesh.boundsCenter[1]
                    + model[8] * mesh.boundsCenter[2] + model[12],
            model[1] * mesh.boundsCenter[0] + model[5] * mesh.boundsCenter[1]
                    + model[9] * mesh.boundsCenter[2] + model[13],
            model[2] * mesh.boundsCenter[0] + model[6] * mesh.boundsCenter[1]
                    + model[10] * mesh.boundsCenter[2] + model[14]};
    if (!std::isfinite(worldCenter[0]) || !std::isfinite(worldCenter[1])
            || !std::isfinite(worldCenter[2])) return true;

    const float rowNorm = std::max({
            std::fabs(model[0]) + std::fabs(model[4]) + std::fabs(model[8]),
            std::fabs(model[1]) + std::fabs(model[5]) + std::fabs(model[9]),
            std::fabs(model[2]) + std::fabs(model[6]) + std::fabs(model[10])});
    const float columnNorm = std::max({
            std::fabs(model[0]) + std::fabs(model[1]) + std::fabs(model[2]),
            std::fabs(model[4]) + std::fabs(model[5]) + std::fabs(model[6]),
            std::fabs(model[8]) + std::fabs(model[9]) + std::fabs(model[10])});
    const float scaleBound = std::sqrt(rowNorm * columnNorm);
    const float worldRadius = mesh.boundsRadius * scaleBound;
    if (!std::isfinite(worldRadius)) return true;
    return sphereIntersectsFrustum(frustum, worldCenter, worldRadius);
}

void Renderer::renderShadowMaps() {
    if (!shadowsAvailable_ || !shadowCascadesValid_ || shadowProgram_ == 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.5f, 2.5f);
    glUseProgram(shadowProgram_);

    for (std::size_t cascade = 0; cascade < kDirectionalShadowCascadeCount; ++cascade) {
        const GLint tileX = static_cast<GLint>(cascade)
                * static_cast<GLint>(kShadowMapSize);
        glViewport(tileX, 0, kShadowMapSize, kShadowMapSize);
        glEnable(GL_SCISSOR_TEST);
        glScissor(tileX, 0, kShadowMapSize, kShadowMapSize);
        glClear(GL_DEPTH_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glUniformMatrix4fv(shadowProgramLightViewProjection_, 1, GL_FALSE,
                           shadowViewProjections_[cascade].data());
        ViewFrustum cascadeFrustum;
        extractViewFrustum(shadowViewProjections_[cascade], cascadeFrustum);

        const Scene& shadowScene = activeScene();
        shadowDrawItems_.clear();
        for (const Entity& entity : shadowScene.entities()) {
            if (!entity.visible || entity.primitive == Primitive::Ground) continue;
            const Mat4 model = shadowScene.worldMatrix(entity.id);
            if (entity.primitive == Primitive::ImportedMesh) {
                const std::string meshPath = projectAssetPath(entity.meshAsset);
                const auto imported = importedMeshes_.find(meshPath);
                if (imported != importedMeshes_.end()) {
                    const auto pose = evaluatedSkeletalPoses_.find(entity.id);
                    const SkeletalPose* poseData = pose == evaluatedSkeletalPoses_.end()
                            ? nullptr : &pose->second;
                    for (const ImportedPart& part : imported->second.parts) {
                        const bool useImportedMaterial = part.hasMaterial
                                && !entity.overrideMtlMaterials;
                        const Material& material = useImportedMaterial
                                ? part.material : entity.material;
                        const bool alphaMask = useImportedMaterial && part.alphaMask;
                        const SkeletalPose* partPose = part.skinned ? poseData : nullptr;
                        if (part.mesh.vao == 0 || (!alphaMask
                                && (material.opacity < 1.0f
                                    || !material.opacityTexture.empty()))
                                || (partPose == nullptr
                                    && !meshIntersectsFrustum(part.mesh, model, cascadeFrustum))) continue;
                        shadowDrawItems_.push_back({&part.mesh, model, partPose,
                                alphaMask ? &material : nullptr, alphaMask, part.alphaCutoff});
                    }
                    continue;
                }
            }
            if (entity.material.opacity < 1.0f
                    || !entity.material.opacityTexture.empty()) continue;
            const Mesh* mesh = meshFor(entity);
            if (mesh == nullptr || mesh->vao == 0
                    || !meshIntersectsFrustum(*mesh, model, cascadeFrustum)) continue;
            shadowDrawItems_.push_back({mesh, model});
        }
        std::sort(shadowDrawItems_.begin(), shadowDrawItems_.end(),
                [](const ShadowDrawItem& left, const ShadowDrawItem& right) {
                    if (left.mesh != right.mesh) {
                        return std::less<const Mesh*>{}(left.mesh, right.mesh);
                    }
                    if (left.pose != right.pose) {
                        return std::less<const SkeletalPose*>{}(left.pose, right.pose);
                    }
                    if (left.material != right.material) {
                        return std::less<const Material*>{}(left.material, right.material);
                    }
                    if (left.alphaMask != right.alphaMask) return left.alphaMask < right.alphaMask;
                    return left.alphaCutoff < right.alphaCutoff;
                });
        for (std::size_t begin = 0; begin < shadowDrawItems_.size();) {
            const ShadowDrawItem& first = shadowDrawItems_[begin];
            std::size_t end = begin + 1;
            while (end < shadowDrawItems_.size()
                    && shadowDrawItems_[end].mesh == first.mesh
                    && shadowDrawItems_[end].pose == first.pose
                    && shadowDrawItems_[end].material == first.material
                    && shadowDrawItems_[end].alphaMask == first.alphaMask
                    && shadowDrawItems_[end].alphaCutoff == first.alphaCutoff) ++end;
            const bool useInstances = !first.alphaMask && first.pose == nullptr
                    && end - begin > 1 && instanceBuffer_ != 0
                    && shadowProgramUseInstancing_ >= 0
                    && end - begin <= static_cast<std::size_t>(
                            std::numeric_limits<GLsizei>::max());
            glBindVertexArray(first.mesh->vao);
            glUniformMatrix4fv(shadowProgramModel_, 1, GL_FALSE, first.model.data());
            glUniform1i(shadowProgramUseInstancing_, useInstances ? 1 : 0);
            const bool useSkinning = skinPaletteBuffer_ != 0 && first.pose != nullptr;
            glUniform1i(shadowProgramSkinningEnabled_, useSkinning ? 1 : 0);
            glUniform1i(shadowProgramAlphaMaskEnabled_, first.alphaMask ? 1 : 0);
            glUniform1f(shadowProgramAlphaCutoff_, first.alphaCutoff);
            const Material* shadowMaterial = first.material;
            GLuint shadowAlbedoTexture = 0U;
            if (first.alphaMask && shadowMaterial != nullptr
                    && !shadowMaterial->albedoTexture.empty()) {
                const std::string texturePath = projectAssetPath(shadowMaterial->albedoTexture);
                const auto texture = textures_.find(texturePath);
                if (texture != textures_.end()) shadowAlbedoTexture = texture->second;
            }
            glUniform1i(shadowProgramAlbedoTexture_, 0);
            glUniform1i(shadowProgramHasAlbedoTexture_,
                        shadowAlbedoTexture != 0U ? 1 : 0);
            glUniform1f(shadowProgramBaseColorAlpha_,
                        shadowMaterial != nullptr ? shadowMaterial->opacity : 1.0f);
            const TextureMapTransform defaultTransform{};
            const TextureMapTransform& albedoTransform = shadowMaterial != nullptr
                    ? shadowMaterial->albedoMapTransform : defaultTransform;
            glUniform2fv(shadowProgramAlbedoUvScale_, 1, albedoTransform.uvScale.data());
            glUniform2fv(shadowProgramAlbedoUvOffset_, 1, albedoTransform.uvOffset.data());
            const std::array<float, 2> shadowUvRotation =
                    textureRotationPair(albedoTransform.uvRotation);
            glUniform2fv(shadowProgramAlbedoUvRotation_, 1, shadowUvRotation.data());
            glUniform1i(shadowProgramAlbedoUvSet_,
                        static_cast<GLint>(albedoTransform.uvSet));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, shadowAlbedoTexture);
            const GLuint shadowSampler = shadowMaterial != nullptr
                    && shadowMaterial->albedoMapTransform.clampToEdge
                    ? clampSampler_ : repeatSampler_;
            glBindSampler(0, first.alphaMask ? shadowSampler : 0U);
            if (useSkinning) uploadSkinPalette(first.pose);
            if (useInstances) {
                instanceModelsScratch_.clear();
                instanceModelsScratch_.reserve(end - begin);
                for (std::size_t index = begin; index < end; ++index) {
                    instanceModelsScratch_.push_back(shadowDrawItems_[index].model);
                }
                bindInstanceAttributes(instanceBuffer_, instanceModelsScratch_);
                if (first.mesh->ebo != 0) {
                    glDrawElementsInstanced(GL_TRIANGLES, first.mesh->indexCount, GL_UNSIGNED_INT,
                            nullptr, static_cast<GLsizei>(instanceModelsScratch_.size()));
                } else {
                    glDrawArraysInstanced(GL_TRIANGLES, 0, first.mesh->vertexCount,
                            static_cast<GLsizei>(instanceModelsScratch_.size()));
                }
                unbindInstanceAttributes();
            } else {
                for (std::size_t index = begin; index < end; ++index) {
                    const ShadowDrawItem& item = shadowDrawItems_[index];
                    glUniformMatrix4fv(shadowProgramModel_, 1, GL_FALSE, item.model.data());
                    if (item.pose != nullptr && skinPaletteBuffer_ != 0) {
                        glUniform1i(shadowProgramSkinningEnabled_, 1);
                        uploadSkinPalette(item.pose);
                    }
                    if (item.mesh->ebo != 0) {
                        glDrawElements(GL_TRIANGLES, item.mesh->indexCount, GL_UNSIGNED_INT, nullptr);
                    } else {
                        glDrawArrays(GL_TRIANGLES, 0, item.mesh->vertexCount);
                    }
                }
            }
            begin = end;
        }
    }

    glBindVertexArray(0);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
}

void Renderer::initializePostProcessing() {
    if (fallbackRenderer_) return;
    bloomExtractProgram_ = createProgram(kFullscreenVertexShader, kBloomExtractFragmentShader);
    blurProgram_ = createProgram(kFullscreenVertexShader, kBlurFragmentShader);
    compositeProgram_ = createProgram(kFullscreenVertexShader, kCompositeFragmentShader);
    if (bloomExtractProgram_ == 0 || blurProgram_ == 0 || compositeProgram_ == 0) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Post-processing shaders unavailable; rendering directly to screen");
        return;
    }

    postUniforms_.extractScene = glGetUniformLocation(bloomExtractProgram_, "uScene");
    postUniforms_.blurSource = glGetUniformLocation(blurProgram_, "uSource");
    postUniforms_.blurDirection = glGetUniformLocation(blurProgram_, "uDirection");
    postUniforms_.blurTexelSize = glGetUniformLocation(blurProgram_, "uTexelSize");
    postUniforms_.compositeScene = glGetUniformLocation(compositeProgram_, "uScene");
    postUniforms_.compositeBloom = glGetUniformLocation(compositeProgram_, "uBloom");

    constexpr float quadVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f
    };
    glGenVertexArrays(1, &fullscreenVao_);
    glGenBuffers(1, &fullscreenVbo_);
    glBindVertexArray(fullscreenVao_);
    glBindBuffer(GL_ARRAY_BUFFER, fullscreenVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<const void*>(2 * sizeof(float)));
    glBindVertexArray(0);

    skyProgram_ = createProgram(kFullscreenVertexShader, kSkyFragmentShader);
    if (skyProgram_ != 0) {
        skyUniforms_.viewForward = glGetUniformLocation(skyProgram_, "uViewForward");
        skyUniforms_.viewRight = glGetUniformLocation(skyProgram_, "uViewRight");
        skyUniforms_.viewUp = glGetUniformLocation(skyProgram_, "uViewUp");
        skyUniforms_.tanHalfFov = glGetUniformLocation(skyProgram_, "uTanHalfFov");
        skyUniforms_.aspectRatio = glGetUniformLocation(skyProgram_, "uAspectRatio");
        skyUniforms_.sunDirection = glGetUniformLocation(skyProgram_, "uSunDirection");
        skyUniforms_.sunColor = glGetUniformLocation(skyProgram_, "uSunColor");
        skyUniforms_.sunAngularRadius = glGetUniformLocation(skyProgram_, "uSunAngularRadius");
        skyUniforms_.sunDiskEnabled = glGetUniformLocation(skyProgram_, "uSunDiskEnabled");
        skyUniforms_.zenithColor = glGetUniformLocation(skyProgram_, "uZenithColor");
        skyUniforms_.horizonColor = glGetUniformLocation(skyProgram_, "uHorizonColor");
        skyUniforms_.groundColor = glGetUniformLocation(skyProgram_, "uGroundColor");
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "Procedural sky shader unavailable; keeping the scene clear color");
    }
}

void Renderer::releasePostProcessing() {
    if (sceneFramebuffer_ != 0) glDeleteFramebuffers(1, &sceneFramebuffer_);
    if (sceneColorTexture_ != 0) glDeleteTextures(1, &sceneColorTexture_);
    if (sceneDepthBuffer_ != 0) glDeleteRenderbuffers(1, &sceneDepthBuffer_);
    glDeleteFramebuffers(static_cast<GLsizei>(bloomFramebuffers_.size()), bloomFramebuffers_.data());
    glDeleteTextures(static_cast<GLsizei>(bloomTextures_.size()), bloomTextures_.data());
    sceneFramebuffer_ = 0;
    sceneColorTexture_ = 0;
    sceneDepthBuffer_ = 0;
    bloomFramebuffers_ = {0, 0};
    bloomTextures_ = {0, 0};
    postWidth_ = 0;
    postHeight_ = 0;
    postProcessingAvailable_ = false;
}

void Renderer::resizePostProcessing() {
    if (width_ <= 0 || height_ <= 0) return;
    if (postProcessingAvailable_ && sceneColorTexture_ != 0
            && postWidth_ == width_ && postHeight_ == height_) return;
    releasePostProcessing();
    if (bloomExtractProgram_ == 0 || blurProgram_ == 0 || compositeProgram_ == 0
            || fullscreenVao_ == 0) return;

    bloomWidth_ = std::max(width_ / 4, 1);
    bloomHeight_ = std::max(height_ / 4, 1);
    glGenTextures(1, &sceneColorTexture_);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &sceneDepthBuffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, sceneDepthBuffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width_, height_);

    glGenFramebuffers(1, &sceneFramebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, sceneColorTexture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, sceneDepthBuffer_);
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glGenFramebuffers(static_cast<GLsizei>(bloomFramebuffers_.size()), bloomFramebuffers_.data());
    glGenTextures(static_cast<GLsizei>(bloomTextures_.size()), bloomTextures_.data());
    for (std::size_t index = 0; index < bloomFramebuffers_.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, bloomTextures_[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bloomWidth_, bloomHeight_, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[index]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, bloomTextures_[index], 0);
        complete = complete
                && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    postProcessingAvailable_ = complete;
    if (!postProcessingAvailable_) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Bloom framebuffers unavailable; rendering directly to screen");
        releasePostProcessing();
        return;
    }
    postWidth_ = width_;
    postHeight_ = height_;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
            "Quarter-resolution bloom enabled (%dx%d)", bloomWidth_, bloomHeight_);
}

void Renderer::drawFullscreen() {
    glBindVertexArray(fullscreenVao_);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void Renderer::drawSky(const Mat4& view, float verticalFov, float aspectRatio) {
    if (!frameSky_.enabled || skyProgram_ == 0 || fullscreenVao_ == 0) return;
    const Vec3 forward{-view[2], -view[6], -view[10]};
    const Vec3 right{view[0], view[4], view[8]};
    const Vec3 up{view[1], view[5], view[9]};
    const std::array<float, 3> sunDirection{
        -frameDirectionalLighting_.direction[0],
        -frameDirectionalLighting_.direction[1],
        -frameDirectionalLighting_.direction[2]};
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glUseProgram(skyProgram_);
    glUniform3fv(skyUniforms_.viewForward, 1, forward.data());
    glUniform3fv(skyUniforms_.viewRight, 1, right.data());
    glUniform3fv(skyUniforms_.viewUp, 1, up.data());
    glUniform1f(skyUniforms_.tanHalfFov, std::tan(verticalFov * 0.5f));
    glUniform1f(skyUniforms_.aspectRatio, aspectRatio);
    glUniform3fv(skyUniforms_.sunDirection, 1, sunDirection.data());
    glUniform3fv(skyUniforms_.sunColor, 1, frameSky_.sunColor.data());
    glUniform1f(skyUniforms_.sunAngularRadius, frameSky_.sunAngularRadius);
    glUniform1i(skyUniforms_.sunDiskEnabled, frameSky_.sunDiskEnabled ? 1 : 0);
    glUniform3fv(skyUniforms_.zenithColor, 1, frameSky_.zenithColor.data());
    glUniform3fv(skyUniforms_.horizonColor, 1, frameSky_.horizonColor.data());
    glUniform3fv(skyUniforms_.groundColor, 1, frameSky_.groundColor.data());
    drawFullscreen();
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

void Renderer::renderBloom() {
    if (!postProcessingAvailable_) return;
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, bloomWidth_, bloomHeight_);

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[0]);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(bloomExtractProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glUniform1i(postUniforms_.extractScene, 0);
    drawFullscreen();

    glUseProgram(blurProgram_);
    glUniform1i(postUniforms_.blurSource, 0);
    glUniform2f(postUniforms_.blurTexelSize,
                1.0f / static_cast<float>(bloomWidth_),
                1.0f / static_cast<float>(bloomHeight_));
    glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[1]);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[0]);
    glUniform2f(postUniforms_.blurDirection, 1.0f, 0.0f);
    drawFullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, bloomFramebuffers_[0]);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[1]);
    glUniform2f(postUniforms_.blurDirection, 0.0f, 1.0f);
    drawFullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width_, height_);
    glUseProgram(compositeProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
    glUniform1i(postUniforms_.compositeScene, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bloomTextures_[0]);
    glUniform1i(postUniforms_.compositeBloom, 1);
    drawFullscreen();
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
}

Renderer::~Renderer() {
    androidAudioOutput_.stop();
    adventureGame_.setZoneSceneReader({});
    asyncAssetTasks_.cancelAll();
}

std::shared_ptr<const AudioClip> Renderer::loadAudioClip(const std::string& relativePath) {
    const std::string assetPath = projectAssetPath(relativePath);
    if (assetPath.empty()) return nullptr;
    const auto cached = audioClipCache_.find(assetPath);
    if (cached != audioClipCache_.end()) return cached->second;
    if (failedAudioClipAssets_.find(assetPath) != failedAudioClipAssets_.end()) return nullptr;

    std::string encodedText;
    if (!readTextAsset(assetManager_, assetPath, 2U * 1024U * 1024U, encodedText)) {
        failedAudioClipAssets_.insert(assetPath);
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "Audio clip unavailable: %s", assetPath.c_str());
        return nullptr;
    }
    const std::vector<std::uint8_t> encoded(encodedText.begin(), encodedText.end());
    std::shared_ptr<AudioClip> decoded;
    std::string error;
    if (!AudioEngine::decodeWav(encoded, decoded, error)) {
        failedAudioClipAssets_.insert(assetPath);
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "Audio clip rejected (%s): %s", assetPath.c_str(), error.c_str());
        return nullptr;
    }
    audioClipCache_.emplace(assetPath, decoded);
    return decoded;
}

void Renderer::loadAudioAssets() {
    relicAudioClip_ = loadAudioClip("audio/relic.wav");
    jumpAudioClip_ = loadAudioClip("audio/jump.wav");
    gateAudioClip_ = loadAudioClip("audio/gate.wav");
}

void Renderer::clearSceneAudioSources() {
    for (const auto& source : activeAudioSourceVoices_) {
        audioEngine_.stop(source.second.handle, 0.04f);
    }
    activeAudioSourceVoices_.clear();
    startedAudioSources_.clear();
    seenAudioSourcesScratch_.clear();
    audioSourceScene_ = nullptr;
}

void Renderer::syncSceneAudioSources(Scene& scene, float deltaSeconds) {
    if (audioSourceScene_ != nullptr && audioSourceScene_ != &scene) {
        clearSceneAudioSources();
    }
    audioSourceScene_ = &scene;
    seenAudioSourcesScratch_.clear();
    for (const Entity& entity : scene.entities()) {
        const AudioSourceComponent& source = entity.audioSource;
        if (!source.enabled || !source.playOnAwake || source.assetPath.empty()) continue;
        seenAudioSourcesScratch_.insert(entity.id);
        const auto active = activeAudioSourceVoices_.find(entity.id);
        if (active != activeAudioSourceVoices_.end()
                && active->second.assetPath != source.assetPath) {
            audioEngine_.stop(active->second.handle, 0.03f);
            activeAudioSourceVoices_.erase(active);
            startedAudioSources_.erase(entity.id);
        }

        auto voice = activeAudioSourceVoices_.find(entity.id);
        if (voice != activeAudioSourceVoices_.end()
                && audioEngine_.isVoiceActive(voice->second.handle)) {
            if (source.positional) {
                const auto world = scene.worldMatrix(entity.id);
                audioEngine_.setVoicePosition(voice->second.handle,
                        {world[12], world[13], world[14]});
            }
            continue;
        }
        if (!source.looping && startedAudioSources_.find(entity.id)
                != startedAudioSources_.end()) continue;

        const std::shared_ptr<const AudioClip> clip = loadAudioClip(source.assetPath);
        if (!clip) continue;
        const auto world = scene.worldMatrix(entity.id);
        AudioPlayOptions options;
        options.bus = source.bus;
        options.gain = source.gain;
        options.priority = source.priority;
        options.looping = source.looping;
        options.positional = source.positional;
        options.position = {world[12], world[13], world[14]};
        options.referenceDistance = source.referenceDistance;
        options.maximumDistance = source.maximumDistance;
        options.rolloff = source.rolloff;
        const AudioVoiceHandle handle = audioEngine_.play(clip, options);
        if (!handle.valid()) continue;
        activeAudioSourceVoices_[entity.id] = {handle, source.assetPath};
        if (!source.looping) startedAudioSources_.insert(entity.id);
    }

    for (auto voice = activeAudioSourceVoices_.begin();
            voice != activeAudioSourceVoices_.end();) {
        if (seenAudioSourcesScratch_.find(voice->first) == seenAudioSourcesScratch_.end()) {
            audioEngine_.stop(voice->second.handle, 0.04f);
            voice = activeAudioSourceVoices_.erase(voice);
        } else {
            ++voice;
        }
    }
    for (auto started = startedAudioSources_.begin(); started != startedAudioSources_.end();) {
        if (seenAudioSourcesScratch_.find(*started) == seenAudioSourcesScratch_.end()) {
            started = startedAudioSources_.erase(started);
        } else {
            ++started;
        }
    }
    ensureAudioOutput(deltaSeconds);
}

void Renderer::ensureAudioOutput(float deltaSeconds) {
    if (androidAudioOutput_.running()) {
        audioOutputRetrySeconds_ = 0.0f;
        audioOutputFailureLogged_ = false;
        return;
    }
    if (audioEngine_.activeVoiceCount() == 0U) return;
    if (audioOutputFailureLogged_) {
        audioOutputRetrySeconds_ += std::isfinite(deltaSeconds)
                ? std::clamp(deltaSeconds, 0.0f, 0.05f) : 0.0f;
        if (audioOutputRetrySeconds_ < 1.0f) return;
    }
    audioOutputRetrySeconds_ = 0.0f;
    if (!androidAudioOutput_.start(audioEngine_)) {
        if (!audioOutputFailureLogged_) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "OpenSL ES output unavailable; game continues silently");
            audioOutputFailureLogged_ = true;
        }
    } else {
        audioOutputFailureLogged_ = false;
    }
}

void Renderer::playAudioClip(const std::shared_ptr<const AudioClip>& clip,
                             float gain, std::uint8_t priority) {
    if (!clip) return;
    AudioPlayOptions options;
    options.bus = AudioBus::Effects;
    options.gain = gain;
    options.priority = priority;
    audioEngine_.play(clip, options);
}

void Renderer::initialize(AAssetManager* assetManager) {
    androidAudioOutput_.stop();
    audioEngine_.stopAll();
    clearSceneAudioSources();
    audioClipCache_.clear();
    failedAudioClipAssets_.clear();
    audioOutputRetrySeconds_ = 0.0f;
    audioOutputFailureLogged_ = false;
    relicAudioClip_.reset();
    jumpAudioClip_.reset();
    gateAudioClip_.reset();
    // Join previous CPU asset work before changing its captured manager source.
    asyncAssetTasks_.cancelAll();
    asyncAssetTasks_.waitUntilIdle(std::chrono::seconds(10));
    pendingMeshAssets_.clear();
    failedMeshAssets_.clear();
    pendingGpuUploads_.clear();
    textureUploadTurn_ = true;
    pendingTextureAssets_.clear();
    failedTextureAssets_.clear();
    assetMemoryCache_.clear();
    textureGpuBytes_.clear();
    embeddedImageData_.clear();
    embeddedImagesByModel_.clear();
    assetUseSequence_ = 0U;
    adventureGame_.setZoneSceneReader({});
    assetManager_ = assetManager;
    adventureGame_.setZoneSceneReader([this](const std::string& assetPath,
                                             Scene& fragment, std::string& error) {
        std::string contents;
        if (!readTextAsset(assetManager_, assetPath, 4U * 1024U * 1024U, contents)) {
            error = "could not read zone scene asset: " + assetPath;
            __android_log_print(ANDROID_LOG_WARN, kLogTag, "%s", error.c_str());
            return false;
        }
        std::istringstream input(contents);
        if (!SceneSerializer::load(fragment, input, error)) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "invalid zone scene %s: %s", assetPath.c_str(), error.c_str());
            return false;
        }
        return true;
    });
    adventureGame_.setZoneManifestReader([this](WorldZoneManifest& manifest,
                                                    std::string& error) {
        std::string contents;
        if (!readTextAsset(assetManager_, "worlds/sun_ruins.world",
                           1024U * 1024U, contents)) {
            error = "could not read world zone manifest";
            return false;
        }
        return parseWorldZoneManifest(contents, manifest, error);
    });
    adventureGame_.setZoneAssetsChangedCallback([this]() { loadSceneAssets(); });
    demoMode_ = 0;
    gameMode_ = false;
    adventureMode_ = false;
    baseMeshesInitialized_ = false;
    importedMeshes_.clear();
    textures_.clear();
    fallbackRenderer_ = false;
    csmShaderSupported_ = false;
    shadowsAvailable_ = false;
    shadowCascadesValid_ = false;
    postProcessingAvailable_ = false;
    rendererFailureReason_.clear();
    GLint maxFragmentTextureUnits = 0;
    GLint maxCombinedTextureUnits = 0;
    GLint maxFragmentUniformVectors = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxFragmentTextureUnits);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxCombinedTextureUnits);
    glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &maxFragmentUniformVectors);
    program_ = createProgram(kVertexShader, kFragmentShader);
    const std::string csmBuildError = program_ == 0 ? lastProgramBuildError_ : std::string{};
    if (program_ != 0) {
        csmShaderSupported_ = true;
    } else {
        std::string pbrWithoutCsm = kFragmentShader;
        const std::size_t versionStart = pbrWithoutCsm.find("#version");
        const std::size_t versionEnd = versionStart == std::string::npos
                ? std::string::npos : pbrWithoutCsm.find('\n', versionStart);
        if (versionEnd != std::string::npos) {
            pbrWithoutCsm.insert(versionEnd + 1, "#define NUBY_DISABLE_CSM\n");
            program_ = createProgram(kVertexShader, pbrWithoutCsm.c_str());
        }
        if (program_ == 0) {
            const std::string pbrWithoutCsmError = lastProgramBuildError_;
            fallbackRenderer_ = true;
            rendererFailureReason_ = "GL limits: fragment texture units="
                    + std::to_string(maxFragmentTextureUnits)
                    + ", combined texture units=" + std::to_string(maxCombinedTextureUnits)
                    + ", fragment uniform vectors=" + std::to_string(maxFragmentUniformVectors)
                    + "\nPBR with cascaded shadows: " + csmBuildError
                    + "\nPBR without cascaded shadows: " + pbrWithoutCsmError;
            if (rendererFailureReason_.size() > 4096U) {
                rendererFailureReason_.resize(4096U);
            }
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "Advanced PBR shaders unavailable; trying compatibility renderer");
            program_ = createProgram(kFallbackVertexShader, kFallbackFragmentShader);
            if (program_ == 0) {
                rendererFailureReason_ += "\nCompatibility renderer: "
                        + lastProgramBuildError_;
                if (rendererFailureReason_.size() > 4096U) {
                    rendererFailureReason_.resize(4096U);
                }
            }
        } else {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                    "PBR shader enabled without cascaded shadows on this GPU");
        }
    }
    if (program_ == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                "Renderer initialization failed: no compatible shader program");
        return;
    }

    glGenSamplers(1, &repeatSampler_);
    glGenSamplers(1, &clampSampler_);
    if (repeatSampler_ != 0 && clampSampler_ != 0) {
        for (GLuint sampler : {repeatSampler_, clampSampler_}) {
            glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        glSamplerParameteri(repeatSampler_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glSamplerParameteri(repeatSampler_, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glSamplerParameteri(clampSampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(clampSampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                "MTL per-map clamp samplers unavailable; texture repeat remains active");
        if (repeatSampler_ != 0) glDeleteSamplers(1, &repeatSampler_);
        if (clampSampler_ != 0) glDeleteSamplers(1, &clampSampler_);
        repeatSampler_ = 0;
        clampSampler_ = 0;
    }

    uniforms_.viewProjection = glGetUniformLocation(program_, "uViewProjection");
    uniforms_.model = glGetUniformLocation(program_, "uModel");
    uniforms_.useInstancing = glGetUniformLocation(program_, "uUseInstancing");
    uniforms_.skinningEnabled = glGetUniformLocation(program_, "uSkinningEnabled");
    uniforms_.hasTangents = glGetUniformLocation(program_, "uHasTangents");
    const GLuint skinPaletteBlock = glGetUniformBlockIndex(program_, "SkinPalette");
    if (skinPaletteBlock != GL_INVALID_INDEX) {
        glUniformBlockBinding(program_, skinPaletteBlock, kSkinPaletteBindingPoint);
        glGenBuffers(1, &skinPaletteBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, skinPaletteBuffer_);
        glBufferData(GL_UNIFORM_BUFFER,
                static_cast<GLsizeiptr>(kMaximumGpuJoints * sizeof(SkeletalMatrix4)),
                nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, kSkinPaletteBindingPoint, skinPaletteBuffer_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
    glGenBuffers(1, &instanceBuffer_);
    uniforms_.color = glGetUniformLocation(program_, "uColor");
    uniforms_.emission = glGetUniformLocation(program_, "uEmission");
    uniforms_.metallic = glGetUniformLocation(program_, "uMetallic");
    uniforms_.roughness = glGetUniformLocation(program_, "uRoughness");
    uniforms_.opacity = glGetUniformLocation(program_, "uOpacity");
    uniforms_.alphaMaskEnabled = glGetUniformLocation(program_, "uAlphaMaskEnabled");
    uniforms_.alphaCutoff = glGetUniformLocation(program_, "uAlphaCutoff");
    uniforms_.normalMapScale = glGetUniformLocation(program_, "uNormalMapScale");
    uniforms_.emissiveStrength = glGetUniformLocation(program_, "uEmissiveStrength");
    uniforms_.indexOfRefraction = glGetUniformLocation(program_, "uIndexOfRefraction");
    uniforms_.clearcoatFactor = glGetUniformLocation(program_, "uClearcoatFactor");
    uniforms_.clearcoatRoughness = glGetUniformLocation(program_, "uClearcoatRoughness");
    uniforms_.clearcoatNormalScale = glGetUniformLocation(program_, "uClearcoatNormalScale");
    uniforms_.specularFactor = glGetUniformLocation(program_, "uSpecularFactor");
    uniforms_.specularColorFactor = glGetUniformLocation(program_, "uSpecularColorFactor");
    uniforms_.sheenColorFactor = glGetUniformLocation(program_, "uSheenColorFactor");
    uniforms_.sheenRoughnessFactor = glGetUniformLocation(program_, "uSheenRoughnessFactor");
    uniforms_.unlit = glGetUniformLocation(program_, "uUnlit");
    uniforms_.ambientOcclusion = glGetUniformLocation(program_, "uAmbientOcclusion");
    uniforms_.ambientSphericalHarmonics = glGetUniformLocation(program_, "uAmbientSH[0]");
    uniforms_.albedoTexture = glGetUniformLocation(program_, "uAlbedoTexture");
    uniforms_.normalTexture = glGetUniformLocation(program_, "uNormalTexture");
    uniforms_.metallicRoughnessTexture = glGetUniformLocation(program_, "uMetallicRoughnessTexture");
    uniforms_.metallicTexture = glGetUniformLocation(program_, "uMetallicTexture");
    uniforms_.roughnessTexture = glGetUniformLocation(program_, "uRoughnessTexture");
    uniforms_.occlusionTexture = glGetUniformLocation(program_, "uOcclusionTexture");
    uniforms_.opacityTexture = glGetUniformLocation(program_, "uOpacityTexture");
    uniforms_.emissiveTexture = glGetUniformLocation(program_, "uEmissiveTexture");
    uniforms_.clearcoatTexture = glGetUniformLocation(program_, "uClearcoatTexture");
    uniforms_.clearcoatRoughnessTexture = glGetUniformLocation(
            program_, "uClearcoatRoughnessTexture");
    uniforms_.clearcoatNormalTexture = glGetUniformLocation(
            program_, "uClearcoatNormalTexture");
    uniforms_.specularTexture = glGetUniformLocation(program_, "uSpecularTexture");
    uniforms_.specularColorTexture = glGetUniformLocation(program_, "uSpecularColorTexture");
    uniforms_.sheenColorTexture = glGetUniformLocation(program_, "uSheenColorTexture");
    uniforms_.sheenRoughnessTexture = glGetUniformLocation(
            program_, "uSheenRoughnessTexture");
    uniforms_.opacityUvScale = glGetUniformLocation(program_, "uOpacityUvScale");
    uniforms_.opacityUvOffset = glGetUniformLocation(program_, "uOpacityUvOffset");
    uniforms_.opacityUvRotation = glGetUniformLocation(program_, "uOpacityUvRotation");
    uniforms_.emissiveUvScale = glGetUniformLocation(program_, "uEmissiveUvScale");
    uniforms_.emissiveUvOffset = glGetUniformLocation(program_, "uEmissiveUvOffset");
    uniforms_.opacityMapChannel = glGetUniformLocation(program_, "uOpacityMapChannel");
    const std::array<const char*, 8> mapValueRangeNames{{
            "uAlbedoMapBaseGain", "uNormalMapBaseGain",
            "uMetallicRoughnessMapBaseGain", "uMetallicMapBaseGain",
            "uRoughnessMapBaseGain", "uOcclusionMapBaseGain",
            "uOpacityMapBaseGain", "uEmissiveMapBaseGain"}};
    for (std::size_t map = 0; map < mapValueRangeNames.size(); ++map) {
        uniforms_.textureMapValueRange[map] =
                glGetUniformLocation(program_, mapValueRangeNames[map]);
    }
    uniforms_.hasAlbedoTexture = glGetUniformLocation(program_, "uHasAlbedoTexture");
    uniforms_.hasNormalTexture = glGetUniformLocation(program_, "uHasNormalTexture");
    uniforms_.hasMetallicRoughnessTexture = glGetUniformLocation(program_, "uHasMetallicRoughnessTexture");
    uniforms_.hasMetallicTexture = glGetUniformLocation(program_, "uHasMetallicTexture");
    uniforms_.hasRoughnessTexture = glGetUniformLocation(program_, "uHasRoughnessTexture");
    uniforms_.hasOcclusionTexture = glGetUniformLocation(program_, "uHasOcclusionTexture");
    uniforms_.hasOpacityTexture = glGetUniformLocation(program_, "uHasOpacityTexture");
    uniforms_.hasEmissiveTexture = glGetUniformLocation(program_, "uHasEmissiveTexture");
    uniforms_.hasClearcoatTexture = glGetUniformLocation(program_, "uHasClearcoatTexture");
    uniforms_.hasClearcoatRoughnessTexture = glGetUniformLocation(
            program_, "uHasClearcoatRoughnessTexture");
    uniforms_.hasClearcoatNormalTexture = glGetUniformLocation(
            program_, "uHasClearcoatNormalTexture");
    uniforms_.hasSpecularTexture = glGetUniformLocation(program_, "uHasSpecularTexture");
    uniforms_.hasSpecularColorTexture = glGetUniformLocation(
            program_, "uHasSpecularColorTexture");
    uniforms_.hasSheenColorTexture = glGetUniformLocation(
            program_, "uHasSheenColorTexture");
    uniforms_.hasSheenRoughnessTexture = glGetUniformLocation(
            program_, "uHasSheenRoughnessTexture");
    const std::array<const char*, 14> uvScaleNames{{
            "uAlbedoUvScale", "uNormalUvScale", "uMetallicRoughnessUvScale",
            "uMetallicUvScale", "uRoughnessUvScale", "uOcclusionUvScale",
            "uEmissiveUvScale", "uClearcoatUvScale",
            "uClearcoatRoughnessUvScale", "uClearcoatNormalUvScale",
            "uSpecularUvScale", "uSpecularColorUvScale",
            "uSheenColorUvScale", "uSheenRoughnessUvScale"}};
    const std::array<const char*, 14> uvOffsetNames{{
            "uAlbedoUvOffset", "uNormalUvOffset", "uMetallicRoughnessUvOffset",
            "uMetallicUvOffset", "uRoughnessUvOffset", "uOcclusionUvOffset",
            "uEmissiveUvOffset", "uClearcoatUvOffset",
            "uClearcoatRoughnessUvOffset", "uClearcoatNormalUvOffset",
            "uSpecularUvOffset", "uSpecularColorUvOffset",
            "uSheenColorUvOffset", "uSheenRoughnessUvOffset"}};
    const std::array<const char*, 14> uvRotationNames{{
            "uAlbedoUvRotation", "uNormalUvRotation", "uMetallicRoughnessUvRotation",
            "uMetallicUvRotation", "uRoughnessUvRotation", "uOcclusionUvRotation",
            "uEmissiveUvRotation", "uClearcoatUvRotation",
            "uClearcoatRoughnessUvRotation", "uClearcoatNormalUvRotation",
            "uSpecularUvRotation", "uSpecularColorUvRotation",
            "uSheenColorUvRotation", "uSheenRoughnessUvRotation"}};
    for (std::size_t map = 0; map < uniforms_.textureUvScale.size(); ++map) {
        uniforms_.textureUvScale[map] = glGetUniformLocation(program_, uvScaleNames[map]);
        uniforms_.textureUvOffset[map] = glGetUniformLocation(program_, uvOffsetNames[map]);
        uniforms_.textureUvRotation[map] = glGetUniformLocation(program_, uvRotationNames[map]);
    }
    uniforms_.textureUvSet = glGetUniformLocation(program_, "uTextureUvSet[0]");
    uniforms_.albedoUvSet = glGetUniformLocation(program_, "uAlbedoUvSet");
    uniforms_.cameraPosition = glGetUniformLocation(program_, "uCameraPosition");
    uniforms_.lightDirection = glGetUniformLocation(program_, "uLightDirection");
    uniforms_.directionalLightColor = glGetUniformLocation(program_, "uDirectionalLightColor");
    uniforms_.directionalLightIntensity = glGetUniformLocation(
            program_, "uDirectionalLightIntensity");
    uniforms_.shadowStrength = glGetUniformLocation(program_, "uShadowStrength");
    uniforms_.fogEnabled = glGetUniformLocation(program_, "uFogEnabled");
    uniforms_.fogColor = glGetUniformLocation(program_, "uFogColor");
    uniforms_.fogStart = glGetUniformLocation(program_, "uFogStart");
    uniforms_.fogEnd = glGetUniformLocation(program_, "uFogEnd");
    uniforms_.skyAmbientStrength = glGetUniformLocation(program_, "uSkyAmbientStrength");
    uniforms_.skyZenithColor = glGetUniformLocation(program_, "uSkyZenithColor");
    uniforms_.skyHorizonColor = glGetUniformLocation(program_, "uSkyHorizonColor");
    uniforms_.skyGroundColor = glGetUniformLocation(program_, "uSkyGroundColor");
    uniforms_.view = glGetUniformLocation(program_, "uView");
    uniforms_.lightViewProjection = {{
            glGetUniformLocation(program_, "uLightViewProjection0"),
            glGetUniformLocation(program_, "uLightViewProjection1"),
            glGetUniformLocation(program_, "uLightViewProjection2")}};
    uniforms_.cascadeFarSplits = glGetUniformLocation(program_, "uCascadeFarSplits");
    uniforms_.shadowMap = glGetUniformLocation(program_, "uShadowMap");
    uniforms_.shadowsEnabled = glGetUniformLocation(program_, "uShadowsEnabled");
    uniforms_.grid = glGetUniformLocation(program_, "uGrid");
    uniforms_.pointLightCount = glGetUniformLocation(program_, "uPointLightCount");
    uniforms_.pointLightPositionRange = glGetUniformLocation(program_, "uPointLightPositionRange[0]");
    uniforms_.pointLightColorIntensity = glGetUniformLocation(program_, "uPointLightColorIntensity[0]");

    buildScene();
    if (gameMode_) sampleGame_.start(scene_);
    initializeShadowMap();
    initializePostProcessing();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClearColor(0.025f, 0.035f, 0.085f, 1.0f);
    loadAudioAssets();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Nuby GLES 3 renderer initialized");
}

void Renderer::resize(int width, int height) {
    width_ = std::max(width, 1);
    height_ = std::max(height, 1);
    glViewport(0, 0, width_, height_);
    resizePostProcessing();
}

void Renderer::uploadSkinPalette(const SkeletalPose* pose) {
    if (skinPaletteBuffer_ == 0 || pose == nullptr || pose->jointMatrices.empty()
            || pose->jointMatrices.size() > static_cast<std::size_t>(kMaximumGpuJoints)) return;
    glBindBuffer(GL_UNIFORM_BUFFER, skinPaletteBuffer_);
    glBufferSubData(GL_UNIFORM_BUFFER, 0,
            static_cast<GLsizeiptr>(pose->jointMatrices.size() * sizeof(SkeletalMatrix4)),
            pose->jointMatrices.data());
    glBindBufferBase(GL_UNIFORM_BUFFER, kSkinPaletteBindingPoint, skinPaletteBuffer_);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

void Renderer::draw(const Mesh& mesh, const Mat4& model, const Material& material,
                    const std::vector<Mat4>* instanceModels, const SkeletalPose* pose,
                    bool alphaMask, float alphaCutoff) {
    if (program_ == 0 || mesh.vao == 0) return;

    glUseProgram(program_);
    glUniformMatrix4fv(uniforms_.viewProjection, 1, GL_FALSE, viewProjection_.data());
    glUniformMatrix4fv(uniforms_.view, 1, GL_FALSE, view_.data());
    for (std::size_t cascade = 0; cascade < uniforms_.lightViewProjection.size(); ++cascade) {
        glUniformMatrix4fv(uniforms_.lightViewProjection[cascade], 1, GL_FALSE,
                           shadowViewProjections_[cascade].data());
    }
    glUniform3fv(uniforms_.cascadeFarSplits, 1, shadowCascadeFarSplits_.data());
    glUniformMatrix4fv(uniforms_.model, 1, GL_FALSE, model.data());
    const bool useSkinning = skinPaletteBuffer_ != 0 && pose != nullptr
            && !pose->jointMatrices.empty()
            && pose->jointMatrices.size() <= static_cast<std::size_t>(kMaximumGpuJoints);
    if (uniforms_.skinningEnabled >= 0) {
        glUniform1i(uniforms_.skinningEnabled, useSkinning ? 1 : 0);
    }
    glUniform1i(uniforms_.hasTangents, mesh.hasTangents ? 1 : 0);
    if (useSkinning) uploadSkinPalette(pose);
    const bool useInstances = !useSkinning && uniforms_.useInstancing >= 0 && instanceBuffer_ != 0
            && instanceModels != nullptr && instanceModels->size() > 1
            && instanceModels->size()
                    <= static_cast<std::size_t>(std::numeric_limits<GLsizei>::max());
    if (uniforms_.useInstancing >= 0) {
        glUniform1i(uniforms_.useInstancing, useInstances ? 1 : 0);
    }
    glUniform3fv(uniforms_.color, 1, material.color.data());
    glUniform3fv(uniforms_.emission, 1, material.emission.data());
    glUniform1f(uniforms_.metallic, material.metallic);
    glUniform1f(uniforms_.roughness, material.roughness);
    glUniform1f(uniforms_.opacity, material.opacity);
    glUniform1i(uniforms_.alphaMaskEnabled, alphaMask ? 1 : 0);
    glUniform1f(uniforms_.alphaCutoff, alphaCutoff);
    glUniform1f(uniforms_.normalMapScale, material.normalMapScale);
    glUniform1f(uniforms_.emissiveStrength, material.emissiveStrength);
    glUniform1f(uniforms_.indexOfRefraction, material.indexOfRefraction);
    glUniform1f(uniforms_.clearcoatFactor, material.clearcoatFactor);
    glUniform1f(uniforms_.clearcoatRoughness, material.clearcoatRoughness);
    glUniform1f(uniforms_.clearcoatNormalScale, material.clearcoatNormalScale);
    glUniform1f(uniforms_.specularFactor, material.specularFactor);
    glUniform3fv(uniforms_.specularColorFactor, 1, material.specularColorFactor.data());
    glUniform3fv(uniforms_.sheenColorFactor, 1, material.sheenColorFactor.data());
    glUniform1f(uniforms_.sheenRoughnessFactor, material.sheenRoughness);
    glUniform1i(uniforms_.unlit, material.unlit ? 1 : 0);
    glUniform1f(uniforms_.ambientOcclusion, material.ambientOcclusion);
    glUniform3fv(uniforms_.ambientSphericalHarmonics, 9,
                 ambientSphericalHarmonics_.data());
    const auto textureFor = [this](const std::string& path) -> GLuint {
        if (path.empty()) return 0U;
        const std::string assetPath = projectAssetPath(path);
        const auto found = textures_.find(assetPath);
        return assetPath.empty() || found == textures_.end() ? 0U : found->second;
    };
    const GLuint albedoTexture = textureFor(material.albedoTexture);
    const GLuint normalTexture = textureFor(material.normalTexture);
    const GLuint metallicRoughnessTexture = textureFor(material.metallicRoughnessTexture);
    const GLuint metallicTexture = textureFor(material.metallicTexture);
    const GLuint roughnessTexture = textureFor(material.roughnessTexture);
    const GLuint occlusionTexture = textureFor(material.occlusionTexture);
    const GLuint opacityTexture = textureFor(material.opacityTexture);
    const GLuint emissiveTexture = textureFor(material.emissiveTexture);
    const GLuint clearcoatTexture = textureFor(material.clearcoatTexture);
    const GLuint clearcoatRoughnessTexture =
            textureFor(material.clearcoatRoughnessTexture);
    const GLuint clearcoatNormalTexture = textureFor(material.clearcoatNormalTexture);
    const GLuint specularTexture = textureFor(material.specularTexture);
    const GLuint specularColorTexture = textureFor(material.specularColorTexture);
    const GLuint sheenColorTexture = textureFor(material.sheenColorTexture);
    const GLuint sheenRoughnessTexture = textureFor(material.sheenRoughnessTexture);
    glUniform1i(uniforms_.albedoTexture, 1);
    glUniform1i(uniforms_.normalTexture, 2);
    glUniform1i(uniforms_.metallicRoughnessTexture, 3);
    glUniform1i(uniforms_.occlusionTexture, 4);
    glUniform1i(uniforms_.metallicTexture, 5);
    glUniform1i(uniforms_.roughnessTexture, 6);
    glUniform1i(uniforms_.opacityTexture, 7);
    glUniform1i(uniforms_.emissiveTexture, 8);
    glUniform1i(uniforms_.clearcoatTexture, 9);
    glUniform1i(uniforms_.clearcoatRoughnessTexture, 10);
    glUniform1i(uniforms_.clearcoatNormalTexture, 11);
    glUniform1i(uniforms_.specularTexture, 12);
    glUniform1i(uniforms_.specularColorTexture, 13);
    glUniform1i(uniforms_.sheenColorTexture, 14);
    glUniform1i(uniforms_.sheenRoughnessTexture, 15);
    glUniform1i(uniforms_.hasAlbedoTexture, albedoTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasNormalTexture, normalTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasMetallicRoughnessTexture,
                metallicRoughnessTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasMetallicTexture, metallicTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasRoughnessTexture, roughnessTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasOcclusionTexture, occlusionTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasOpacityTexture, opacityTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasEmissiveTexture, emissiveTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasClearcoatTexture, clearcoatTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasClearcoatRoughnessTexture,
                clearcoatRoughnessTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasClearcoatNormalTexture,
                clearcoatNormalTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasSpecularTexture, specularTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasSpecularColorTexture,
                specularColorTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasSheenColorTexture, sheenColorTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.hasSheenRoughnessTexture,
                sheenRoughnessTexture != 0U ? 1 : 0);
    glUniform1i(uniforms_.opacityMapChannel,
                static_cast<GLint>(material.opacityMapChannel));
    glUniform2fv(uniforms_.opacityUvScale, 1,
                 material.opacityMapTransform.uvScale.data());
    glUniform2fv(uniforms_.opacityUvOffset, 1,
                 material.opacityMapTransform.uvOffset.data());
    const std::array<float, 2> opacityUvRotation =
            textureRotationPair(material.opacityMapTransform.uvRotation);
    glUniform2fv(uniforms_.opacityUvRotation, 1, opacityUvRotation.data());
    const std::array<const TextureMapTransform*, 14> mapTransforms{{
            &material.albedoMapTransform,
            &material.normalMapTransform,
            &material.metallicRoughnessMapTransform,
            &material.metallicMapTransform,
            &material.roughnessMapTransform,
            &material.occlusionMapTransform,
            &material.emissiveMapTransform,
            &material.clearcoatMapTransform,
            &material.clearcoatRoughnessMapTransform,
            &material.clearcoatNormalMapTransform,
            &material.specularMapTransform,
            &material.specularColorMapTransform,
            &material.sheenColorMapTransform,
            &material.sheenRoughnessMapTransform
    }};
    for (std::size_t map = 0; map < mapTransforms.size(); ++map) {
        glUniform2fv(uniforms_.textureUvScale[map], 1,
                     mapTransforms[map]->uvScale.data());
        glUniform2fv(uniforms_.textureUvOffset[map], 1,
                     mapTransforms[map]->uvOffset.data());
        const std::array<float, 2> uvRotation =
                textureRotationPair(mapTransforms[map]->uvRotation);
        glUniform2fv(uniforms_.textureUvRotation[map], 1, uvRotation.data());
    }
    const std::array<const TextureMapTransform*, 15> uvSetTransforms{{
            &material.albedoMapTransform, &material.normalMapTransform,
            &material.metallicRoughnessMapTransform, &material.metallicMapTransform,
            &material.roughnessMapTransform, &material.occlusionMapTransform,
            &material.opacityMapTransform, &material.emissiveMapTransform,
            &material.clearcoatMapTransform, &material.clearcoatRoughnessMapTransform,
            &material.clearcoatNormalMapTransform, &material.specularMapTransform,
            &material.specularColorMapTransform, &material.sheenColorMapTransform,
            &material.sheenRoughnessMapTransform}};
    std::array<GLint, 15> uvSets{};
    for (std::size_t map = 0; map < uvSetTransforms.size(); ++map) {
        uvSets[map] = static_cast<GLint>(uvSetTransforms[map]->uvSet);
    }
    glUniform1iv(uniforms_.textureUvSet, static_cast<GLsizei>(uvSets.size()), uvSets.data());
    glUniform1i(uniforms_.albedoUvSet,
                static_cast<GLint>(material.albedoMapTransform.uvSet));
    const std::array<const TextureMapValueRange*, 8> mapValueRanges{{
            &material.albedoMapValueRange,
            &material.normalMapValueRange,
            &material.metallicRoughnessMapValueRange,
            &material.metallicMapValueRange,
            &material.roughnessMapValueRange,
            &material.occlusionMapValueRange,
            &material.opacityMapValueRange,
            &material.emissiveMapValueRange
    }};
    for (std::size_t map = 0; map < mapValueRanges.size(); ++map) {
        const std::array<float, 2> baseGain{{
                mapValueRanges[map]->base, mapValueRanges[map]->gain}};
        glUniform2fv(uniforms_.textureMapValueRange[map], 1, baseGain.data());
    }
    glUniform3fv(uniforms_.cameraPosition, 1, cameraPosition_.data());
    glUniform3fv(uniforms_.lightDirection, 1, frameDirectionalLighting_.direction.data());
    glUniform3fv(uniforms_.directionalLightColor, 1,
                 frameDirectionalLighting_.color.data());
    glUniform1f(uniforms_.directionalLightIntensity,
                frameDirectionalLighting_.intensity);
    glUniform1f(uniforms_.shadowStrength, frameDirectionalLighting_.shadowStrength);
    glUniform1i(uniforms_.fogEnabled, frameAtmosphere_.fogEnabled ? 1 : 0);
    glUniform3fv(uniforms_.fogColor, 1, frameAtmosphere_.fogColor.data());
    glUniform1f(uniforms_.fogStart, frameAtmosphere_.fogStart);
    glUniform1f(uniforms_.fogEnd, frameAtmosphere_.fogEnd);
    glUniform1f(uniforms_.skyAmbientStrength,
                frameSky_.enabled ? frameSky_.ambientStrength : 0.0f);
    glUniform3fv(uniforms_.skyZenithColor, 1, frameSky_.zenithColor.data());
    glUniform3fv(uniforms_.skyHorizonColor, 1, frameSky_.horizonColor.data());
    glUniform3fv(uniforms_.skyGroundColor, 1, frameSky_.groundColor.data());
    glUniform1i(uniforms_.shadowMap, 0);
    glUniform1i(uniforms_.shadowsEnabled,
                shadowsAvailable_ && shadowCascadesValid_ ? 1 : 0);
    glUniform1i(uniforms_.grid, material.proceduralGrid ? 1 : 0);
    glUniform1i(uniforms_.pointLightCount, static_cast<GLint>(pointLights_.size()));
    glUniform4fv(uniforms_.pointLightPositionRange, kMaxPointLights,
                 pointLightPositionRanges_.data());
    glUniform4fv(uniforms_.pointLightColorIntensity, kMaxPointLights,
                 pointLightColorIntensities_.data());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, albedoTexture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, normalTexture);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, metallicRoughnessTexture);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, occlusionTexture);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, metallicTexture);
    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, roughnessTexture);
    glActiveTexture(GL_TEXTURE7);
    glBindTexture(GL_TEXTURE_2D, opacityTexture);
    glActiveTexture(GL_TEXTURE8);
    glBindTexture(GL_TEXTURE_2D, emissiveTexture);
    glActiveTexture(GL_TEXTURE9);
    glBindTexture(GL_TEXTURE_2D, clearcoatTexture);
    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, clearcoatRoughnessTexture);
    glActiveTexture(GL_TEXTURE11);
    glBindTexture(GL_TEXTURE_2D, clearcoatNormalTexture);
    glActiveTexture(GL_TEXTURE12);
    glBindTexture(GL_TEXTURE_2D, specularTexture);
    glActiveTexture(GL_TEXTURE13);
    glBindTexture(GL_TEXTURE_2D, specularColorTexture);
    glActiveTexture(GL_TEXTURE14);
    glBindTexture(GL_TEXTURE_2D, sheenColorTexture);
    glActiveTexture(GL_TEXTURE15);
    glBindTexture(GL_TEXTURE_2D, sheenRoughnessTexture);
    const auto samplerFor = [this](const TextureMapTransform& map) {
        return map.clampToEdge ? clampSampler_ : repeatSampler_;
    };
    glBindSampler(1, samplerFor(material.albedoMapTransform));
    glBindSampler(2, samplerFor(material.normalMapTransform));
    glBindSampler(3, samplerFor(material.metallicRoughnessMapTransform));
    glBindSampler(4, samplerFor(material.occlusionMapTransform));
    glBindSampler(5, samplerFor(material.metallicMapTransform));
    glBindSampler(6, samplerFor(material.roughnessMapTransform));
    glBindSampler(7, samplerFor(material.opacityMapTransform));
    glBindSampler(8, samplerFor(material.emissiveMapTransform));
    glBindSampler(9, samplerFor(material.clearcoatMapTransform));
    glBindSampler(10, samplerFor(material.clearcoatRoughnessMapTransform));
    glBindSampler(11, samplerFor(material.clearcoatNormalMapTransform));
    glBindSampler(12, samplerFor(material.specularMapTransform));
    glBindSampler(13, samplerFor(material.specularColorMapTransform));
    glBindSampler(14, samplerFor(material.sheenColorMapTransform));
    glBindSampler(15, samplerFor(material.sheenRoughnessMapTransform));
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(mesh.vao);
    if (useInstances) {
        bindInstanceAttributes(instanceBuffer_, *instanceModels);
        if (mesh.ebo != 0) {
            glDrawElementsInstanced(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr,
                    static_cast<GLsizei>(instanceModels->size()));
        } else {
            glDrawArraysInstanced(GL_TRIANGLES, 0, mesh.vertexCount,
                    static_cast<GLsizei>(instanceModels->size()));
        }
        unbindInstanceAttributes();
    } else if (mesh.ebo != 0) {
        glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh.vertexCount);
    }
    glBindVertexArray(0);
}

void Renderer::render(float deltaSeconds) {
    const auto frameStart = std::chrono::steady_clock::now();
    const auto recordFrameTime = [this, &frameStart]() {
        const auto elapsed = std::chrono::steady_clock::now() - frameStart;
        frameTimingWindow_.record(std::chrono::duration<double, std::milli>(elapsed).count());
    };
    if (program_ == 0) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        recordFrameTime();
        return;
    }

    // Consume at most one CPU result per frame. Completion callbacks own all
    // OpenGL work and therefore always run on the renderer thread.
    processAssetTasks();
    const float dt = std::clamp(deltaSeconds, 0.0f, 0.05f);
    synchronizeSkeletalClipDurations(activeScene());
    if (projectRuntime_.loaded()) projectRuntime_.update(dt);
    else if (editorSession_.active()) editorSession_.update(dt);
    else if (gameMode_) sampleGame_.update(scene_, dt);
    else if (adventureMode_) {
        const int relicsBefore = adventureGame_.relicsCollected();
        const bool completedBefore = adventureGame_.completed();
        adventureGame_.update(scene_, dt);
        for (int relic = relicsBefore; relic < adventureGame_.relicsCollected(); ++relic) {
            playAudioClip(relicAudioClip_, 0.8f, 112U);
        }
        if (!completedBefore && adventureGame_.completed()) {
            playAudioClip(gateAudioClip_, 0.86f, 120U);
        }
    } else scene_.update(dt);
    if (androidAudioOutput_.running()) {
        if (audioEngine_.activeVoiceCount() == 0U) {
            audioIdleSeconds_ += dt;
            if (audioIdleSeconds_ >= 2.0f) {
                androidAudioOutput_.stop();
                audioIdleSeconds_ = 0.0f;
            }
        } else {
            audioIdleSeconds_ = 0.0f;
        }
    }
    Scene& frameScene = activeScene();
    evaluatedSkeletalPoses_.clear();
    evaluatedSkeletalPoses_.reserve(frameScene.entities().size());
    for (const Entity& entity : frameScene.entities()) {
        if (!entity.visible || entity.primitive != Primitive::ImportedMesh
                || !entity.skeletalAnimation.enabled || entity.skeletalAnimation.clipName.empty()) {
            continue;
        }
        const auto model = importedMeshes_.find(projectAssetPath(entity.meshAsset));
        if (model == importedMeshes_.end() || model->second.skeleton.empty()) continue;
        const auto clip = std::find_if(model->second.animations.begin(), model->second.animations.end(),
                [&entity](const GltfAnimation& animation) {
                    return animation.name == entity.skeletalAnimation.clipName;
                });
        if (clip == model->second.animations.end()) continue;
        SkeletalPose pose;
        std::string poseError;
        const SkeletalAnimationComponent& playback = entity.skeletalAnimation;
        bool evaluated = false;
        const float transitionDuration = entity.skeletalLocomotion.transitionDurationSeconds;
        if (!playback.transitionFromClip.empty() && std::isfinite(transitionDuration)
                && transitionDuration > 0.0f) {
            const auto fromClip = std::find_if(model->second.animations.begin(),
                    model->second.animations.end(), [&playback](const GltfAnimation& animation) {
                        return animation.name == playback.transitionFromClip;
                    });
            if (fromClip != model->second.animations.end()) {
                const float blendAmount = std::clamp(
                        playback.transitionElapsedSeconds / transitionDuration, 0.0f, 1.0f);
                evaluated = evaluateBlendedSkeletalPose(model->second.skeleton,
                        fromClip->clip, playback.transitionFromTimeSeconds,
                        playback.transitionFromLooping, clip->clip,
                        playback.timeSeconds, playback.looping, blendAmount,
                        pose, poseError);
            }
        }
        if (!evaluated) {
            evaluated = evaluateSkeletalPose(model->second.skeleton, clip->clip,
                    playback.timeSeconds, playback.looping, pose, poseError);
        }
        if (evaluated) evaluatedSkeletalPoses_.emplace(entity.id, std::move(pose));
    }
    frameDirectionalLighting_ = frameScene.directionalLighting();
    float directionLengthSquared = 0.0f;
    for (float component : frameDirectionalLighting_.direction) {
        if (!std::isfinite(component)) {
            directionLengthSquared = 0.0f;
            break;
        }
        directionLengthSquared += component * component;
    }
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared < 1.0e-8f
            || directionLengthSquared > 1.0e8f) {
        frameDirectionalLighting_.direction = DirectionalLighting{}.direction;
    }
    for (float& component : frameDirectionalLighting_.color) {
        component = std::isfinite(component) ? std::clamp(component, 0.0f, 1.0f) : 1.0f;
    }
    frameDirectionalLighting_.intensity = std::isfinite(frameDirectionalLighting_.intensity)
            ? std::clamp(frameDirectionalLighting_.intensity, 0.0f, 100.0f) : 1.0f;
    frameDirectionalLighting_.shadowStrength =
            std::isfinite(frameDirectionalLighting_.shadowStrength)
                    ? std::clamp(frameDirectionalLighting_.shadowStrength, 0.0f, 1.0f)
                    : 0.82f;
    frameAtmosphere_ = frameScene.atmosphere();
    for (float& component : frameAtmosphere_.fogColor) {
        component = std::isfinite(component) ? std::clamp(component, 0.0f, 1.0f) : 0.12f;
    }
    frameAtmosphere_.fogStart = std::isfinite(frameAtmosphere_.fogStart)
            ? std::max(frameAtmosphere_.fogStart, 0.0f) : 24.0f;
    frameAtmosphere_.fogEnd = std::isfinite(frameAtmosphere_.fogEnd)
            ? std::max(frameAtmosphere_.fogEnd, frameAtmosphere_.fogStart + 0.01f)
            : frameAtmosphere_.fogStart + 48.0f;
    frameSky_ = frameScene.sky();
    for (float* color : {&frameSky_.zenithColor[0], &frameSky_.horizonColor[0],
                         &frameSky_.groundColor[0], &frameSky_.sunColor[0]}) {
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            color[channel] = std::isfinite(color[channel])
                    ? std::clamp(color[channel], 0.0f, 1.0f) : 0.0f;
        }
    }
    frameSky_.sunAngularRadius = std::isfinite(frameSky_.sunAngularRadius)
            ? std::clamp(frameSky_.sunAngularRadius, 0.001f, 0.5f) : 0.018f;
    frameSky_.ambientStrength = std::isfinite(frameSky_.ambientStrength)
            ? std::clamp(frameSky_.ambientStrength, 0.0f, 5.0f) : 0.0f;
    const auto& ambientCoefficients = frameScene.ambientLighting().sphericalHarmonics;
    const auto& defaultAmbient = AmbientLighting{}.sphericalHarmonics;
    for (std::size_t index = 0; index < ambientSphericalHarmonics_.size(); ++index) {
        const float coefficient = ambientCoefficients[index];
        ambientSphericalHarmonics_[index] = std::isfinite(coefficient)
                && std::fabs(coefficient) <= 1000.0f ? coefficient : defaultAmbient[index];
    }
    frameScene.collectPointLights(kMaxPointLights, pointLights_);
    pointLightPositionRanges_.fill(0.0f);
    pointLightColorIntensities_.fill(0.0f);
    for (std::size_t index = 0; index < pointLights_.size(); ++index) {
        const PointLightData& light = pointLights_[index];
        const std::size_t offset = index * 4;
        pointLightPositionRanges_[offset] = light.position[0];
        pointLightPositionRanges_[offset + 1] = light.position[1];
        pointLightPositionRanges_[offset + 2] = light.position[2];
        pointLightPositionRanges_[offset + 3] = light.range;
        pointLightColorIntensities_[offset] = light.color[0];
        pointLightColorIntensities_[offset + 1] = light.color[1];
        pointLightColorIntensities_[offset + 2] = light.color[2];
        pointLightColorIntensities_[offset + 3] = light.intensity;
    }

    const Entity* cameraEntity = frameScene.activeCamera();
    const CameraComponent camera = cameraEntity == nullptr
            ? CameraComponent{} : cameraEntity->camera;
    Mat4 view{};
    if (cameraEntity != nullptr && cameraEntity->orbitCamera.enabled) {
        const OrbitCameraController& orbit = cameraEntity->orbitCamera;
        const float requestedDistance = orbit.resolvedDistance > 0.0f
                ? orbit.resolvedDistance : orbit.distance;
        const float distance = std::max(requestedDistance, 0.1f);
        const float horizontal = std::cos(orbit.pitch) * distance;
        const Vec3 target = orbit.target;
        cameraPosition_ = {target[0] + std::sin(orbit.yaw) * horizontal,
                           target[1] + std::sin(orbit.pitch) * distance + 0.12f,
                           target[2] + std::cos(orbit.yaw) * horizontal};
        view = lookAt(cameraPosition_, target, {0.0f, 1.0f, 0.0f});
    } else if (cameraEntity != nullptr) {
        const Mat4 cameraWorld = frameScene.worldMatrix(cameraEntity->id);
        cameraPosition_ = {cameraWorld[12], cameraWorld[13], cameraWorld[14]};
        const Vec3 forward{-cameraWorld[8], -cameraWorld[9], -cameraWorld[10]};
        const Vec3 target{cameraPosition_[0] + forward[0],
                          cameraPosition_[1] + forward[1],
                          cameraPosition_[2] + forward[2]};
        const Vec3 up{cameraWorld[4], cameraWorld[5], cameraWorld[6]};
        view = lookAt(cameraPosition_, target, up);
    } else {
        const OrbitCameraController fallback{};
        const float horizontal = std::cos(fallback.pitch) * fallback.distance;
        cameraPosition_ = {std::sin(fallback.yaw) * horizontal,
                           std::sin(fallback.pitch) * fallback.distance + 0.12f,
                           std::cos(fallback.yaw) * horizontal};
        view = lookAt(cameraPosition_, fallback.target, {0.0f, 1.0f, 0.0f});
    }
    AudioListener listener;
    listener.position = cameraPosition_;
    listener.right = {view[0], view[4], view[8]};
    audioEngine_.setListener(listener);
    syncSceneAudioSources(frameScene, dt);
    const float nearPlane = std::max(camera.nearPlane, 0.01f);
    const float farPlane = std::max(camera.farPlane, nearPlane + 0.01f);
    const float aspectRatio = static_cast<float>(width_)
            / static_cast<float>(std::max(height_, 1));
    const float verticalFov = std::clamp(camera.verticalFovRadians, 0.1f, 2.8f);
    const Mat4 projection = perspective(verticalFov, aspectRatio, nearPlane, farPlane);
    view_ = view;
    viewProjection_ = multiply(projection, view);
    ViewFrustum cameraFrustum;
    extractViewFrustum(viewProjection_, cameraFrustum);

    if (shadowsAvailable_) {
        const ShadowCameraFrustum shadowCamera{
                cameraPosition_,
                {-view[2], -view[6], -view[10]},
                {view[1], view[5], view[9]},
                verticalFov, aspectRatio, nearPlane, farPlane};
        std::array<DirectionalShadowCascade, kDirectionalShadowCascadeCount> cascades{};
        shadowCascadesValid_ = buildDirectionalShadowCascades(
                shadowCamera, frameDirectionalLighting_.direction, kShadowMapSize, cascades);
        if (shadowCascadesValid_) {
            for (std::size_t cascade = 0; cascade < cascades.size(); ++cascade) {
                shadowViewProjections_[cascade] = cascades[cascade].lightViewProjection;
                shadowCascadeFarSplits_[cascade] = cascades[cascade].farSplit;
            }
            renderShadowMaps();
        }
    } else {
        shadowCascadesValid_ = false;
    }

    if (postProcessingAvailable_) {
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
        glViewport(0, 0, width_, height_);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
    }
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawSky(view, verticalFov, aspectRatio);
    opaqueDrawItems_.clear();
    transparentDrawItems_.clear();
    const auto queueDraw = [this, &cameraFrustum](const Mesh& mesh, const Mat4& model,
                                                   const Material& material,
                                                   const SkeletalPose* pose,
                                                   bool alphaMask, float alphaCutoff) {
        if ((!alphaMask && material.opacity <= 0.0f)
                || (pose == nullptr && !meshIntersectsFrustum(mesh, model, cameraFrustum))) return;
        const float dx = model[12] - cameraPosition_[0];
        const float dy = model[13] - cameraPosition_[1];
        const float dz = model[14] - cameraPosition_[2];
        std::size_t sortKey = materialBatchHash(material);
        hashValue(sortKey, alphaMask);
        hashValue(sortKey, alphaCutoff);
        DrawItem item{&mesh, model, &material, sortKey,
                      dx * dx + dy * dy + dz * dz, pose, alphaMask, alphaCutoff};
        if (!alphaMask && (material.opacity < 1.0f || !material.opacityTexture.empty())) {
            transparentDrawItems_.push_back(item);
        } else {
            opaqueDrawItems_.push_back(item);
        }
    };
    for (const Entity& entity : frameScene.entities()) {
        if (!entity.visible) continue;
        const Mat4 model = frameScene.worldMatrix(entity.id);
        if (entity.primitive == Primitive::ImportedMesh) {
            const std::string meshPath = projectAssetPath(entity.meshAsset);
            const auto imported = importedMeshes_.find(meshPath);
            if (imported != importedMeshes_.end()) {
                const auto pose = evaluatedSkeletalPoses_.find(entity.id);
                const SkeletalPose* poseData = pose == evaluatedSkeletalPoses_.end()
                        ? nullptr : &pose->second;
                for (const ImportedPart& part : imported->second.parts) {
                    if (part.mesh.vao == 0) continue;
                    const bool useImportedMaterial = part.hasMaterial
                            && !entity.overrideMtlMaterials;
                    const Material& material = useImportedMaterial
                            ? part.material : entity.material;
                    queueDraw(part.mesh, model, material, part.skinned ? poseData : nullptr,
                             useImportedMaterial && part.alphaMask, part.alphaCutoff);
                }
                continue;
            }
        }
        const Mesh* mesh = meshFor(entity);
        if (mesh == nullptr) continue;
        queueDraw(*mesh, model, entity.material, nullptr, false, 0.5f);
    }
    std::sort(opaqueDrawItems_.begin(), opaqueDrawItems_.end(),
            [](const DrawItem& left, const DrawItem& right) {
                if (left.mesh != right.mesh) {
                    return std::less<const Mesh*>{}(left.mesh, right.mesh);
                }
                return left.materialSortKey < right.materialSortKey;
            });
    for (std::size_t begin = 0; begin < opaqueDrawItems_.size();) {
        const DrawItem& first = opaqueDrawItems_[begin];
        std::size_t end = begin + 1;
        while (end < opaqueDrawItems_.size()
                && opaqueDrawItems_[end].mesh == first.mesh
                && opaqueDrawItems_[end].materialSortKey == first.materialSortKey
                && opaqueDrawItems_[end].pose == first.pose
                && opaqueDrawItems_[end].alphaMask == first.alphaMask
                && opaqueDrawItems_[end].alphaCutoff == first.alphaCutoff
                && sameMaterial(*opaqueDrawItems_[end].material, *first.material)) {
            ++end;
        }
        if (first.pose == nullptr && end - begin > 1 && uniforms_.useInstancing >= 0
                && instanceBuffer_ != 0
                && end - begin <= static_cast<std::size_t>(
                        std::numeric_limits<GLsizei>::max())) {
            instanceModelsScratch_.clear();
            instanceModelsScratch_.reserve(end - begin);
            for (std::size_t index = begin; index < end; ++index) {
                instanceModelsScratch_.push_back(opaqueDrawItems_[index].model);
            }
            draw(*first.mesh, first.model, *first.material, &instanceModelsScratch_,
                 nullptr, first.alphaMask, first.alphaCutoff);
        } else {
            for (std::size_t index = begin; index < end; ++index) {
                const DrawItem& item = opaqueDrawItems_[index];
                draw(*item.mesh, item.model, *item.material, nullptr, item.pose,
                     item.alphaMask, item.alphaCutoff);
            }
        }
        begin = end;
    }
    if (!transparentDrawItems_.empty()) {
        std::stable_sort(transparentDrawItems_.begin(), transparentDrawItems_.end(),
                [](const DrawItem& left, const DrawItem& right) {
                    return left.cameraDistanceSquared > right.cameraDistanceSquared;
                });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const DrawItem& item : transparentDrawItems_) {
            draw(*item.mesh, item.model, *item.material, nullptr, item.pose);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    if (postProcessingAvailable_) renderBloom();
    recordFrameTime();
}

void Renderer::touch(int action, float x, float y, float pinchDistance, int pointerCount) {
    constexpr int kActionDown = 0;
    constexpr int kActionUp = 1;
    constexpr int kActionMove = 2;
    constexpr int kActionCancel = 3;
    constexpr int kActionPointerDown = 5;
    constexpr int kActionPointerUp = 6;

    if (action == kActionDown || action == kActionPointerDown) {
        lastTouchX_ = x;
        lastTouchY_ = y;
        gameSwipeDistance_ = 0.0f;
        lastPinchDistance_ = pointerCount >= 2 && pinchDistance > 0.0f
                ? pinchDistance : 0.0f;
        if (demoMode_ == 3 && pointerCount == 1 && width_ > 0 && height_ > 0) {
            const Entity* camera = activeScene().activeCamera();
            const float fov = camera == nullptr ? 0.76f
                    : std::clamp(camera->camera.verticalFovRadians, 0.1f, 2.8f);
            const float ndcX = 2.0f * x / static_cast<float>(width_) - 1.0f;
            const float ndcY = 1.0f - 2.0f * y / static_cast<float>(height_);
            const float halfHeight = std::tan(fov * 0.5f);
            const float halfWidth = halfHeight * static_cast<float>(width_)
                    / static_cast<float>(height_);
            const Vec3 side{view_[0], view_[4], view_[8]};
            const Vec3 up{view_[1], view_[5], view_[9]};
            const Vec3 forward{-view_[2], -view_[6], -view_[10]};
            const Vec3 rayDirection = normalize({
                    forward[0] + side[0] * ndcX * halfWidth + up[0] * ndcY * halfHeight,
                    forward[1] + side[1] * ndcX * halfWidth + up[1] * ndcY * halfHeight,
                    forward[2] + side[2] * ndcX * halfWidth + up[2] * ndcY * halfHeight});
            RaycastHit hit;
            if (activeScene().raycast(cameraPosition_, rayDirection, 100.0f, hit)) {
                editorSelectEntity(hit.entityId);
            }
        }
        return;
    }
    if (action == kActionUp || action == kActionCancel || action == kActionPointerUp) {
        lastTouchX_ = x;
        lastTouchY_ = y;
        gameSwipeDistance_ = 0.0f;
        lastPinchDistance_ = 0.0f;
        return;
    }
    if (action != kActionMove) return;

    const float deltaX = x - lastTouchX_;
    const float deltaY = y - lastTouchY_;
    if (gameMode_) {
        if (pointerCount == 1) {
            gameSwipeDistance_ += deltaX;
            const float threshold = std::max(48.0f, static_cast<float>(width_) * 0.08f);
            while (gameSwipeDistance_ >= threshold) {
                sampleGame_.shiftLane(1);
                gameSwipeDistance_ -= threshold;
            }
            while (gameSwipeDistance_ <= -threshold) {
                sampleGame_.shiftLane(-1);
                gameSwipeDistance_ += threshold;
            }
        }
        lastTouchX_ = x;
        lastTouchY_ = y;
        lastPinchDistance_ = 0.0f;
        return;
    }
    if (pointerCount >= 2 && pinchDistance > 0.0f) {
        if (lastPinchDistance_ > 0.0f) {
            activeScene().zoomActiveCamera(lastPinchDistance_ / pinchDistance);
        }
        lastPinchDistance_ = pinchDistance;
    } else {
        lastPinchDistance_ = 0.0f;
        activeScene().orbitActiveCamera(deltaX, deltaY);
    }
    lastTouchX_ = x;
    lastTouchY_ = y;
}

}  // namespace nuby
