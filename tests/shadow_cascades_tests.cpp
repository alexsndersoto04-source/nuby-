#include "nuby/shadow_cascades.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

float dot(const nuby::ShadowVector3& a, const nuby::ShadowVector3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

nuby::ShadowVector3 add(const nuby::ShadowVector3& a,
                        const nuby::ShadowVector3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

nuby::ShadowVector3 scale(const nuby::ShadowVector3& a, float amount) {
    return {a[0] * amount, a[1] * amount, a[2] * amount};
}

nuby::ShadowVector3 normalize(nuby::ShadowVector3 value) {
    const float inverseLength = 1.0f / std::sqrt(dot(value, value));
    return scale(value, inverseLength);
}

bool sliceCornersInside(const nuby::ShadowCameraFrustum& camera,
                        const nuby::DirectionalShadowCascade& cascade,
                        float sliceNear) {
    const auto forward = normalize(camera.forward);
    auto right = nuby::ShadowVector3{
            forward[1] * camera.up[2] - forward[2] * camera.up[1],
            forward[2] * camera.up[0] - forward[0] * camera.up[2],
            forward[0] * camera.up[1] - forward[1] * camera.up[0]};
    right = normalize(right);
    const auto up = normalize(nuby::ShadowVector3{
            right[1] * forward[2] - right[2] * forward[1],
            right[2] * forward[0] - right[0] * forward[2],
            right[0] * forward[1] - right[1] * forward[0]});
    const float tangent = std::tan(camera.verticalFovRadians * 0.5f);
    const auto& matrix = cascade.lightViewProjection;
    for (float depth : {sliceNear, cascade.farSplit}) {
        const float halfHeight = tangent * depth;
        const float halfWidth = halfHeight * camera.aspectRatio;
        const auto center = add(camera.position, scale(forward, depth));
        for (float vertical : {-1.0f, 1.0f}) {
            for (float horizontal : {-1.0f, 1.0f}) {
                auto corner = add(center, scale(right, horizontal * halfWidth));
                corner = add(corner, scale(up, vertical * halfHeight));
                const float x = matrix[0] * corner[0] + matrix[4] * corner[1]
                        + matrix[8] * corner[2] + matrix[12];
                const float y = matrix[1] * corner[0] + matrix[5] * corner[1]
                        + matrix[9] * corner[2] + matrix[13];
                const float z = matrix[2] * corner[0] + matrix[6] * corner[1]
                        + matrix[10] * corner[2] + matrix[14];
                const float w = matrix[3] * corner[0] + matrix[7] * corner[1]
                        + matrix[11] * corner[2] + matrix[15];
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
                        || !std::isfinite(w) || std::fabs(w) < 0.0001f
                        || std::fabs(x / w) > 1.01f || std::fabs(y / w) > 1.01f
                        || z / w < -1.01f || z / w > 1.01f) return false;
            }
        }
    }
    return true;
}

void testCameraFrustumIsCoveredByStableCascades() {
    nuby::ShadowCameraFrustum camera;
    camera.position = {3.0f, 2.0f, 7.0f};
    camera.forward = normalize({-0.2f, -0.08f, -1.0f});
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.verticalFovRadians = 1.02f;
    camera.aspectRatio = 16.0f / 9.0f;
    camera.nearPlane = 0.12f;
    camera.farPlane = 80.0f;

    std::array<nuby::DirectionalShadowCascade,
               nuby::kDirectionalShadowCascadeCount> cascades{};
    expect(nuby::buildDirectionalShadowCascades(
                   camera, {-0.45f, -0.82f, -0.34f}, 512, cascades),
           "three directional shadow cascades build for a perspective camera");
    float previousSplit = camera.nearPlane;
    bool sorted = true;
    bool covered = true;
    for (const auto& cascade : cascades) {
        sorted = sorted && cascade.farSplit > previousSplit;
        covered = covered && sliceCornersInside(camera, cascade, previousSplit);
        previousSplit = cascade.farSplit;
    }
    expect(sorted && std::fabs(previousSplit - camera.farPlane) < 0.0001f,
           "cascade split distances increase monotonically through the camera far plane");
    expect(covered,
           "each camera-frustum slice corner is contained by its fitted light projection");
}

void testVerticalLightAndInvalidInputs() {
    nuby::ShadowCameraFrustum camera;
    camera.position = {-4.0f, 1.5f, 2.0f};
    std::array<nuby::DirectionalShadowCascade,
               nuby::kDirectionalShadowCascadeCount> cascades{};
    expect(nuby::buildDirectionalShadowCascades(camera, {0.0f, -1.0f, 0.0f}, 256, cascades),
           "vertical directional light uses a safe alternate up axis");

    cascades[0].farSplit = 123.0f;
    cascades[0].lightViewProjection[0] = 456.0f;
    const auto before = cascades;
    camera.farPlane = camera.nearPlane;
    expect(!nuby::buildDirectionalShadowCascades(
                   camera, {0.0f, -1.0f, 0.0f}, 256, cascades),
           "invalid camera frustum is rejected");
    expect(cascades[0].farSplit == before[0].farSplit
                   && cascades[0].lightViewProjection[0]
                           == before[0].lightViewProjection[0],
           "failed cascade generation leaves the previous matrices untouched");
}

}  // namespace

int main() {
    testCameraFrustumIsCoveredByStableCascades();
    testVerticalLightAndInvalidInputs();
    if (failures != 0) {
        std::cerr << failures << " shadow cascade test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All shadow cascade tests passed\n";
    return EXIT_SUCCESS;
}
