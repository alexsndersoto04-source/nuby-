#include "nuby/frustum.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::array<float, 16> identity() {
    std::array<float, 16> matrix{};
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    return matrix;
}

void testIdentityClipFrustumContainsAndRejectsSpheres() {
    nuby::ViewFrustum frustum;
    expect(nuby::extractViewFrustum(identity(), frustum) && frustum.valid,
           "identity clip matrix produces a valid frustum");
    expect(nuby::sphereIntersectsFrustum(frustum, {0.0f, 0.0f, 0.0f}, 0.0f),
           "sphere at clip-space origin remains visible");
    expect(nuby::sphereIntersectsFrustum(frustum, {1.05f, 0.0f, 0.0f}, 0.1f),
           "sphere intersecting a clip plane remains visible");
    expect(!nuby::sphereIntersectsFrustum(frustum, {1.2f, 0.0f, 0.0f}, 0.1f),
           "sphere fully beyond the right clip plane is culled");
    expect(!nuby::sphereIntersectsFrustum(frustum, {0.0f, 0.0f, -1.2f}, 0.1f),
           "sphere fully beyond the near clip plane is culled");
}

void testTranslatedClipMatrixAndInvalidInputsFailOpen() {
    auto translated = identity();
    translated[12] = -2.0f;
    nuby::ViewFrustum frustum;
    expect(nuby::extractViewFrustum(translated, frustum)
                   && nuby::sphereIntersectsFrustum(frustum, {2.0f, 0.0f, 0.0f}, 0.0f)
                   && !nuby::sphereIntersectsFrustum(frustum, {0.0f, 0.0f, 0.0f}, 0.0f),
           "column-major translation is reflected in the extracted clip planes");

    std::array<float, 16> invalid{};
    expect(!nuby::extractViewFrustum(invalid, frustum) && !frustum.valid,
           "degenerate matrix is rejected without producing invalid planes");
    expect(nuby::sphereIntersectsFrustum(frustum, {100.0f, 100.0f, 100.0f}, 1.0f),
           "invalid frustum fails open rather than hiding scene objects");
    expect(nuby::sphereIntersectsFrustum(nuby::ViewFrustum{},
                   {0.0f, 0.0f, 0.0f}, std::numeric_limits<float>::quiet_NaN()),
           "invalid bounds also fail open");
}
}  // namespace

int main() {
    testIdentityClipFrustumContainsAndRejectsSpheres();
    testTranslatedClipMatrixAndInvalidInputsFailOpen();
    if (failures != 0) {
        std::cerr << failures << " frustum test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All frustum tests passed\n";
    return EXIT_SUCCESS;
}
