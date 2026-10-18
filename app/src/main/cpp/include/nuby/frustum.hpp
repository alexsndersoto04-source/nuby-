#pragma once

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
