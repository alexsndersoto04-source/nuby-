#include "nuby/scene.hpp"

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

bool near(float actual, float expected, float epsilon = 0.001f) {
    return std::fabs(actual - expected) <= epsilon;
}

void testEntityIdentityAndSpin() {
    nuby::Scene scene;
    nuby::Entity& spinner = scene.create("spinner", nuby::Primitive::Sphere);
    spinner.spin.radiansPerSecond = {0.0f, 2.0f, 0.0f};
    scene.update(0.025f);
    expect(spinner.id == 1, "first entity receives ID 1");
    expect(near(spinner.transform.rotation[1], 0.05f), "spin integrates angular velocity");

    const nuby::Entity& second = scene.create("second", nuby::Primitive::Torus);
    expect(second.id == 2, "entity IDs increase monotonically");
}

void testDeltaTimeIsClamped() {
    nuby::Scene scene;
    nuby::Entity& spinner = scene.create("spinner", nuby::Primitive::Sphere);
    spinner.spin.radiansPerSecond[0] = 1.0f;
    scene.update(5.0f);
    expect(near(spinner.transform.rotation[0], 0.05f), "large frame delta is clamped");
}

void testOrbitIsStable() {
    nuby::Scene scene;
    nuby::Entity& orbiting = scene.create("orbiting", nuby::Primitive::Sphere);
    orbiting.orbit.enabled = true;
    orbiting.orbit.radius = 2.0f;
    orbiting.orbit.phase = 1.57079632679f;
    orbiting.orbit.angularSpeed = 0.0f;
    scene.update(1.0f);
    expect(near(orbiting.transform.position[0], 0.0f), "orbit phase positions entity on expected axis");
    expect(near(orbiting.transform.position[2], 2.0f), "orbit radius is preserved");
}

void testTransformHierarchyAndCycleProtection() {
    nuby::Scene scene;
    nuby::Entity& parent = scene.create("root", nuby::Primitive::Sphere);
    const std::uint32_t parentId = parent.id;
    parent.transform.position = {1.0f, 0.0f, 0.0f};
    parent.transform.rotation[1] = 1.57079632679f;
    parent.transform.scale = {2.0f, 2.0f, 2.0f};

    nuby::Entity& child = scene.create("child", nuby::Primitive::Sphere);
    const std::uint32_t childId = child.id;
    child.transform.position = {1.0f, 0.0f, 0.0f};
    child.transform.scale = {0.5f, 0.5f, 0.5f};

    nuby::Entity& leaf = scene.create("leaf", nuby::Primitive::Sphere);
    const std::uint32_t leafId = leaf.id;
    expect(scene.setParent(childId, parentId), "valid parent link is accepted");
    expect(scene.setParent(leafId, childId), "nested parent link is accepted");
    expect(!scene.setParent(parentId, leafId), "parent links cannot create cycles");
    expect(!scene.setParent(childId, childId), "entity cannot parent itself");
    expect(!scene.setParent(childId, 9999), "missing parent is rejected");
    expect(scene.find(childId) != nullptr, "entity lookup resolves stable IDs");
    expect(scene.find(9999) == nullptr, "entity lookup returns null for missing IDs");

    const auto world = scene.worldMatrix(childId);
    expect(near(world[12], 1.0f), "parent translation is included in world matrix");
    expect(near(world[14], -2.0f), "parent rotation and scale transform child position");
    expect(near(world[5], 1.0f), "parent and child scales compose");

    expect(scene.setParent(childId, 0), "zero parent ID detaches an entity");
    const auto detached = scene.worldMatrix(childId);
    expect(near(detached[12], 1.0f) && near(detached[14], 0.0f),
           "detached entity returns to its local transform");
}

void testPulseDoesNotAccumulateScaleError() {
    nuby::Scene scene;
    nuby::Entity& pulsing = scene.create("pulsing", nuby::Primitive::Sphere);
    pulsing.transform.scale = {0.25f, 0.25f, 0.25f};
    pulsing.pulse.enabled = true;
    pulsing.pulse.baseScale = pulsing.transform.scale;
    pulsing.pulse.amplitude = 0.1f;
    pulsing.pulse.frequency = 0.5f;

    for (int frame = 0; frame < 600; ++frame) scene.update(1.0f / 60.0f);
    for (float value : pulsing.transform.scale) {
        expect(value >= 0.225f && value <= 0.275f,
               "pulse remains bounded around its authored base scale");
    }
}
}  // namespace

int main() {
    testEntityIdentityAndSpin();
    testDeltaTimeIsClamped();
    testOrbitIsStable();
    testTransformHierarchyAndCycleProtection();
    testPulseDoesNotAccumulateScaleError();
    if (failures != 0) {
        std::cerr << failures << " scene test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All scene tests passed\n";
    return EXIT_SUCCESS;
}
