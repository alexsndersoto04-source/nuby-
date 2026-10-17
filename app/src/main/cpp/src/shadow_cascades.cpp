#include "nuby/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace nuby {
namespace {
using Vec3 = ShadowVector3;
using Mat4 = ShadowMatrix4;

bool finite(const Vec3& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1])
            && std::isfinite(value[2]);
}

float dot(const Vec3& left, const Vec3& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Vec3 add(const Vec3& left, const Vec3& right) {
    return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

Vec3 subtract(const Vec3& left, const Vec3& right) {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

Vec3 multiply(const Vec3& value, float scalar) {
    return {value[0] * scalar, value[1] * scalar, value[2] * scalar};
}

Vec3 cross(const Vec3& left, const Vec3& right) {
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

bool normalize(Vec3& value) {
    const float lengthSquared = dot(value, value);
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-10f) return false;
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    value = multiply(value, inverseLength);
    return finite(value);
}

Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 forward = [&]() {
        Vec3 value = subtract(center, eye);
        normalize(value);
        return value;
    }();
    Vec3 side = cross(forward, up);
    normalize(side);
    const Vec3 correctedUp = cross(side, forward);

    Mat4 result{};
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
    result[15] = 1.0f;
    return result;
}

Mat4 orthographic(float left, float right, float bottom, float top,
                  float nearPlane, float farPlane) {
    Mat4 result{};
    result[0] = 2.0f / (right - left);
    result[5] = 2.0f / (top - bottom);
    result[10] = -2.0f / (farPlane - nearPlane);
    result[12] = -(right + left) / (right - left);
    result[13] = -(top + bottom) / (top - bottom);
    result[14] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    result[15] = 1.0f;
    return result;
}

Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            for (int index = 0; index < 4; ++index) {
                result[static_cast<std::size_t>(column * 4 + row)] +=
                        left[static_cast<std::size_t>(index * 4 + row)]
                        * right[static_cast<std::size_t>(column * 4 + index)];
            }
        }
    }
    return result;
}

float blendedSplit(float nearPlane, float farPlane, float fraction) {
    constexpr float logarithmicWeight = 0.65f;
    const float logarithmic = nearPlane * std::pow(farPlane / nearPlane, fraction);
    const float uniform = nearPlane + (farPlane - nearPlane) * fraction;
    return logarithmicWeight * logarithmic + (1.0f - logarithmicWeight) * uniform;
}

}  // namespace

bool buildDirectionalShadowCascades(
        const ShadowCameraFrustum& camera,
        const ShadowVector3& lightRayDirection,
        int shadowMapResolution,
        std::array<DirectionalShadowCascade, kDirectionalShadowCascadeCount>& output) {
    if (!finite(camera.position) || !finite(camera.forward) || !finite(camera.up)
            || !finite(lightRayDirection)
            || !std::isfinite(camera.verticalFovRadians)
            || !std::isfinite(camera.aspectRatio) || !std::isfinite(camera.nearPlane)
            || !std::isfinite(camera.farPlane) || camera.verticalFovRadians <= 0.05f
            || camera.verticalFovRadians >= 3.0f || camera.aspectRatio <= 0.0f
            || camera.nearPlane <= 0.0f || camera.farPlane <= camera.nearPlane
            || camera.farPlane > 100000.0f || shadowMapResolution < 16
            || shadowMapResolution > 8192) {
        return false;
    }

    Vec3 forward = camera.forward;
    Vec3 up = camera.up;
    Vec3 rayDirection = lightRayDirection;
    if (!normalize(forward) || !normalize(rayDirection)) return false;
    Vec3 right = cross(forward, up);
    if (!normalize(right)) return false;
    up = cross(right, forward);
    if (!normalize(up)) return false;

    Vec3 lightUp{0.0f, 1.0f, 0.0f};
    if (std::fabs(dot(rayDirection, lightUp)) > 0.96f) {
        lightUp = {0.0f, 0.0f, 1.0f};
    }

    std::array<DirectionalShadowCascade, kDirectionalShadowCascadeCount> candidate{};
    float sliceNear = camera.nearPlane;
    const float tangent = std::tan(camera.verticalFovRadians * 0.5f);
    for (std::size_t cascadeIndex = 0;
            cascadeIndex < kDirectionalShadowCascadeCount; ++cascadeIndex) {
        const float fraction = static_cast<float>(cascadeIndex + 1)
                / static_cast<float>(kDirectionalShadowCascadeCount);
        const float sliceFar = cascadeIndex + 1 == kDirectionalShadowCascadeCount
                ? camera.farPlane : blendedSplit(camera.nearPlane, camera.farPlane, fraction);
        if (!std::isfinite(sliceFar) || sliceFar <= sliceNear) return false;

        Vec3 corners[8]{};
        std::size_t cornerIndex = 0;
        for (float distance : {sliceNear, sliceFar}) {
            const float halfHeight = tangent * distance;
            const float halfWidth = halfHeight * camera.aspectRatio;
            const Vec3 sliceCenter = add(camera.position, multiply(forward, distance));
            for (float verticalSign : {-1.0f, 1.0f}) {
                for (float horizontalSign : {-1.0f, 1.0f}) {
                    const Vec3 offset = add(multiply(right, horizontalSign * halfWidth),
                                            multiply(up, verticalSign * halfHeight));
                    corners[cornerIndex++] = add(sliceCenter, offset);
                }
            }
        }

        Vec3 center{0.0f, 0.0f, 0.0f};
        for (const Vec3& corner : corners) center = add(center, multiply(corner, 1.0f / 8.0f));
        float radius = 0.0f;
        for (const Vec3& corner : corners) {
            radius = std::max(radius, std::sqrt(dot(subtract(corner, center),
                                                   subtract(corner, center))));
        }
        if (!std::isfinite(radius) || radius <= 0.001f) return false;

        const Vec3 lightEye = subtract(center, multiply(rayDirection, radius + 25.0f));
        const Mat4 lightView = lookAt(lightEye, center, lightUp);
        float minimumX = std::numeric_limits<float>::max();
        float maximumX = std::numeric_limits<float>::lowest();
        float minimumY = std::numeric_limits<float>::max();
        float maximumY = std::numeric_limits<float>::lowest();
        float minimumZ = std::numeric_limits<float>::max();
        float maximumZ = std::numeric_limits<float>::lowest();
        for (const Vec3& corner : corners) {
            const float x = lightView[0] * corner[0] + lightView[4] * corner[1]
                    + lightView[8] * corner[2] + lightView[12];
            const float y = lightView[1] * corner[0] + lightView[5] * corner[1]
                    + lightView[9] * corner[2] + lightView[13];
            const float z = lightView[2] * corner[0] + lightView[6] * corner[1]
                    + lightView[10] * corner[2] + lightView[14];
            minimumX = std::min(minimumX, x);
            maximumX = std::max(maximumX, x);
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            minimumZ = std::min(minimumZ, z);
            maximumZ = std::max(maximumZ, z);
        }

        float extent = std::max(maximumX - minimumX, maximumY - minimumY) * 0.5f;
        extent = std::max(extent * 1.025f, 0.01f);
        const float texelWorldSize = (2.0f * extent) / static_cast<float>(shadowMapResolution);
        const float centerX = std::round(((minimumX + maximumX) * 0.5f) / texelWorldSize)
                * texelWorldSize;
        const float centerY = std::round(((minimumY + maximumY) * 0.5f) / texelWorldSize)
                * texelWorldSize;
        const float nearDistance = std::max(0.1f, -maximumZ - 8.0f);
        const float farDistance = std::max(nearDistance + 1.0f, -minimumZ + 8.0f);
        if (!std::isfinite(centerX) || !std::isfinite(centerY)
                || !std::isfinite(nearDistance) || !std::isfinite(farDistance)) {
            return false;
        }

        const Mat4 lightProjection = orthographic(centerX - extent, centerX + extent,
                centerY - extent, centerY + extent, nearDistance, farDistance);
        candidate[cascadeIndex].lightViewProjection = multiply(lightProjection, lightView);
        candidate[cascadeIndex].farSplit = sliceFar;
        for (float value : candidate[cascadeIndex].lightViewProjection) {
            if (!std::isfinite(value)) return false;
        }
        sliceNear = sliceFar;
    }
    output = candidate;
    return true;
}

}  // namespace nuby
