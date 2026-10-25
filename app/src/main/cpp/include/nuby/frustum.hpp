#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace nuby {

struct FrustumPlane {
    std::array<float, 3> normal{0.0f, 0.0f, 0.0f};
    float distance = 0.0f;
};

struct ViewFrustum {
    std::array<FrustumPlane, 6> planes{};
    bool valid = false;
};

// Extracts normalized OpenGL clip planes from a column-major view-projection matrix.
// On invalid input, leaves an invalid frustum so callers can safely skip culling.
inline bool extractViewFrustum(const std::array<float, 16>& viewProjection,
                               ViewFrustum& frustum) {
    frustum = ViewFrustum{};
    const std::array<std::array<float, 4>, 6> coefficients{{
            {{viewProjection[3] + viewProjection[0],
              viewProjection[7] + viewProjection[4],
              viewProjection[11] + viewProjection[8],
              viewProjection[15] + viewProjection[12]}},
            {{viewProjection[3] - viewProjection[0],
              viewProjection[7] - viewProjection[4],
              viewProjection[11] - viewProjection[8],
              viewProjection[15] - viewProjection[12]}},
            {{viewProjection[3] + viewProjection[1],
              viewProjection[7] + viewProjection[5],
              viewProjection[11] + viewProjection[9],
              viewProjection[15] + viewProjection[13]}},
            {{viewProjection[3] - viewProjection[1],
              viewProjection[7] - viewProjection[5],
              viewProjection[11] - viewProjection[9],
              viewProjection[15] - viewProjection[13]}},
            {{viewProjection[3] + viewProjection[2],
              viewProjection[7] + viewProjection[6],
              viewProjection[11] + viewProjection[10],
              viewProjection[15] + viewProjection[14]}},
            {{viewProjection[3] - viewProjection[2],
              viewProjection[7] - viewProjection[6],
              viewProjection[11] - viewProjection[10],
              viewProjection[15] - viewProjection[14]}}
    }};

    for (std::size_t index = 0; index < coefficients.size(); ++index) {
        const auto& plane = coefficients[index];
        const float lengthSquared = plane[0] * plane[0] + plane[1] * plane[1]
                + plane[2] * plane[2];
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f
                || !std::isfinite(plane[3])) return false;
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        frustum.planes[index].normal = {plane[0] * inverseLength,
                plane[1] * inverseLength, plane[2] * inverseLength};
        frustum.planes[index].distance = plane[3] * inverseLength;
        if (!std::isfinite(frustum.planes[index].distance)) {
            frustum = ViewFrustum{};
            return false;
        }
    }
    frustum.valid = true;
    return true;
}

// Conservative world-space sphere transform for column-major affine matrices.
// Returns false for invalid inputs; callers should fail open and skip culling.
inline bool transformSphereBounds(const std::array<float, 3>& localCenter,
                                  float localRadius,
                                  const std::array<float, 16>& model,
                                  std::array<float, 3>& worldCenter,
                                  float& worldRadius) {
    if (!std::isfinite(localRadius) || localRadius < 0.0f
            || !std::isfinite(localCenter[0]) || !std::isfinite(localCenter[1])
            || !std::isfinite(localCenter[2])) return false;
    worldCenter = {
            model[0] * localCenter[0] + model[4] * localCenter[1]
                    + model[8] * localCenter[2] + model[12],
            model[1] * localCenter[0] + model[5] * localCenter[1]
                    + model[9] * localCenter[2] + model[13],
            model[2] * localCenter[0] + model[6] * localCenter[1]
                    + model[10] * localCenter[2] + model[14]};
    if (!std::isfinite(worldCenter[0]) || !std::isfinite(worldCenter[1])
            || !std::isfinite(worldCenter[2])) return false;

    const float rowNorm = std::max({
            std::fabs(model[0]) + std::fabs(model[4]) + std::fabs(model[8]),
            std::fabs(model[1]) + std::fabs(model[5]) + std::fabs(model[9]),
            std::fabs(model[2]) + std::fabs(model[6]) + std::fabs(model[10])});
    const float columnNorm = std::max({
            std::fabs(model[0]) + std::fabs(model[1]) + std::fabs(model[2]),
            std::fabs(model[4]) + std::fabs(model[5]) + std::fabs(model[6]),
            std::fabs(model[8]) + std::fabs(model[9]) + std::fabs(model[10])});
    worldRadius = localRadius * std::sqrt(rowNorm * columnNorm);
    return std::isfinite(worldRadius) && worldRadius >= 0.0f;
}

// Invalid bounds/frusta fail open so numerical issues never make objects disappear.
inline bool sphereIntersectsFrustum(const ViewFrustum& frustum,
                                    const std::array<float, 3>& center, float radius) {
    if (!frustum.valid || !std::isfinite(radius) || radius < 0.0f
            || !std::isfinite(center[0]) || !std::isfinite(center[1])
            || !std::isfinite(center[2])) return true;
    for (const FrustumPlane& plane : frustum.planes) {
        const float signedDistance = plane.normal[0] * center[0]
                + plane.normal[1] * center[1] + plane.normal[2] * center[2]
                + plane.distance;
        if (!std::isfinite(signedDistance)) return true;
        if (signedDistance < -radius) return false;
    }
    return true;
}

}  // namespace nuby
