#pragma once

#include <array>
#include <cstddef>

namespace nuby {

inline constexpr std::size_t kDirectionalShadowCascadeCount = 3;
using ShadowVector3 = std::array<float, 3>;
using ShadowMatrix4 = std::array<float, 16>;

struct ShadowCameraFrustum {
    ShadowVector3 position{0.0f, 0.0f, 0.0f};
    ShadowVector3 forward{0.0f, 0.0f, -1.0f};
    ShadowVector3 up{0.0f, 1.0f, 0.0f};
    float verticalFovRadians = 0.785398163f;
    float aspectRatio = 1.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

struct DirectionalShadowCascade {
    ShadowMatrix4 lightViewProjection{};
    float farSplit = 0.0f;
};

// Fits stabilized directional-light depth projections to camera-frustum slices.
// Returns false and leaves `output` unchanged when any input is non-finite or
// the camera/light configuration is degenerate.
bool buildDirectionalShadowCascades(
        const ShadowCameraFrustum& camera,
        const ShadowVector3& lightRayDirection,
        int shadowMapResolution,
        std::array<DirectionalShadowCascade, kDirectionalShadowCascadeCount>& output);

}  // namespace nuby
