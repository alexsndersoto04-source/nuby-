#include "nuby/scene.hpp"

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
    scene.update(std::numeric_limits<float>::quiet_NaN());
    expect(near(scene.elapsedSeconds(), 0.0f), "non-finite frame delta is ignored");
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

void testEntityDestructionCascadesAndPreservesOtherEntities() {
    nuby::Scene scene;
    nuby::Entity& root = scene.create("root", nuby::Primitive::None);
    const std::uint32_t rootId = root.id;
    nuby::Entity& camera = scene.create("camera child", nuby::Primitive::None);
    const std::uint32_t cameraId = camera.id;
    camera.camera.enabled = true;
    nuby::Entity& leaf = scene.create("nested child", nuby::Primitive::Sphere);
    const std::uint32_t leafId = leaf.id;
    nuby::Entity& independent = scene.create("independent", nuby::Primitive::Sphere);
    const std::uint32_t independentId = independent.id;
    scene.setParent(cameraId, rootId);
    scene.setParent(leafId, cameraId);
    scene.setActiveCamera(cameraId);

    expect(scene.destroyEntity(rootId), "existing entity can be destroyed");
    expect(scene.find(rootId) == nullptr && scene.find(cameraId) == nullptr
                   && scene.find(leafId) == nullptr,
           "destroying a parent removes its full descendant subtree");
    expect(scene.find(independentId) != nullptr, "unrelated entities survive subtree destruction");
    expect(scene.activeCamera() == nullptr, "destroying the active camera clears the camera handle");
    expect(!scene.destroyEntity(rootId), "destroying a missing entity returns false");

    const nuby::Entity& next = scene.create("next", nuby::Primitive::Sphere);
    expect(next.id > independentId, "destroyed entity IDs are not reused");
}

void testActiveCameraAndOrbitInput() {
    nuby::Scene scene;
    nuby::Entity& cameraEntity = scene.create("camera", nuby::Primitive::None);
    const std::uint32_t cameraId = cameraEntity.id;
    cameraEntity.camera.enabled = true;
    cameraEntity.orbitCamera.enabled = true;
    cameraEntity.orbitCamera.minimumPitch = -0.5f;
    cameraEntity.orbitCamera.maximumPitch = 0.5f;

    nuby::Entity& notACamera = scene.create("ordinary entity", nuby::Primitive::Sphere);
    expect(!scene.setActiveCamera(notACamera.id), "entities without cameras cannot be activated as cameras");
    expect(!scene.setActiveCamera(9999), "missing entity cannot become the active camera");
    expect(scene.setActiveCamera(cameraId), "enabled camera can become active");
    expect(scene.activeCamera() != nullptr && scene.activeCamera()->id == cameraId,
           "active camera lookup returns the selected camera");

    expect(scene.orbitActiveCamera(100.0f, 1000.0f), "orbit input updates an active orbit camera");
    const nuby::Entity* active = scene.activeCamera();
    expect(near(active->orbitCamera.yaw, 1.05f), "orbit yaw uses configured pixel sensitivity");
    expect(near(active->orbitCamera.pitch, 0.5f), "orbit pitch is clamped to camera limits");
    expect(!scene.orbitActiveCamera(std::numeric_limits<float>::infinity(), 1.0f),
           "non-finite orbit input is rejected");
    expect(scene.zoomActiveCamera(0.5f), "pinch zoom scales camera distance");
    expect(near(scene.activeCamera()->orbitCamera.distance, 2.8f),
           "pinch zoom clamps to minimum camera distance");
    expect(scene.zoomActiveCamera(100.0f), "zoom-out input is accepted");
    expect(near(scene.activeCamera()->orbitCamera.distance, 10.0f),
           "pinch zoom clamps to maximum camera distance");
    expect(!scene.zoomActiveCamera(0.0f), "zero zoom factor is rejected");
    expect(!scene.zoomActiveCamera(std::numeric_limits<float>::infinity()),
           "non-finite zoom factor is rejected");

    scene.clear();
    expect(scene.activeCamera() == nullptr, "clearing a scene resets its active camera");
}

void testPointLightCollectionUsesWorldTransformsAndFiltersInvalidLights() {
    nuby::Scene scene;
    nuby::Entity& parent = scene.create("light rig", nuby::Primitive::None);
    const std::uint32_t parentId = parent.id;
    parent.transform.position = {1.0f, 2.0f, 3.0f};

    nuby::Entity& lightEntity = scene.create("point light", nuby::Primitive::None);
    const std::uint32_t lightId = lightEntity.id;
    lightEntity.transform.position = {2.0f, 0.0f, 0.0f};
    lightEntity.pointLight.enabled = true;
    lightEntity.pointLight.color = {0.2f, -0.4f, 0.8f};
    lightEntity.pointLight.intensity = 3.0f;
    lightEntity.pointLight.range = 7.0f;
    expect(scene.setParent(lightId, parentId), "light entity can inherit a parent transform");

    nuby::Entity& invalidLight = scene.create("invalid light", nuby::Primitive::None);
    invalidLight.pointLight.enabled = true;
    invalidLight.pointLight.intensity = std::numeric_limits<float>::quiet_NaN();
    for (int index = 0; index < 4; ++index) {
        nuby::Entity& extra = scene.create("extra light", nuby::Primitive::None);
        extra.pointLight.enabled = true;
        extra.pointLight.intensity = 1.0f;
    }

    std::vector<nuby::PointLightData> lights;
    scene.collectPointLights(4, lights);
    expect(lights.size() == 4, "invalid lights are filtered and the render budget is enforced");
    if (!lights.empty()) {
        expect(near(lights[0].position[0], 3.0f)
                       && near(lights[0].position[1], 2.0f)
                       && near(lights[0].position[2], 3.0f),
               "point-light collection composes parent world position");
        expect(near(lights[0].intensity, 3.0f) && near(lights[0].range, 7.0f),
               "point-light photometric settings are preserved");
        expect(near(lights[0].color[0], 0.2f) && near(lights[0].color[1], 0.0f)
                       && near(lights[0].color[2], 0.8f),
               "negative light color channels are clamped to zero");
    }
    scene.collectPointLights(1, lights);
    expect(lights.size() == 1, "point-light collection respects a smaller render budget");
    scene.collectPointLights(0, lights);
    expect(lights.empty(), "zero light budget produces an empty render list");
}

void testKeyframedAnimationInterpolationAndLooping() {
    nuby::Scene scene;
    nuby::Entity& animated = scene.create("animated", nuby::Primitive::Sphere);
    animated.animation.enabled = true;
    animated.animation.durationSeconds = 1.0f;
    animated.animation.position.addKeyframe(1.0f, {4.0f, 2.0f, 0.0f});
    animated.animation.position.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    animated.animation.rotation.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    animated.animation.rotation.addKeyframe(1.0f, {0.0f, 2.0f, 0.0f});
    animated.animation.scale.addKeyframe(0.0f, {1.0f, 1.0f, 1.0f});
    animated.animation.scale.addKeyframe(1.0f, {3.0f, 3.0f, 3.0f});
    animated.spin.radiansPerSecond[1] = 9.0f;
    expect(!animated.animation.position.addKeyframe(-1.0f, {0.0f, 0.0f, 0.0f}),
           "negative keyframe time is rejected");
    expect(!animated.animation.position.addKeyframe(
                   0.25f, {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}),
           "non-finite keyframe value is rejected");

    for (int frame = 0; frame < 10; ++frame) scene.update(0.05f);
    expect(near(animated.transform.position[0], 2.0f), "position track interpolates linearly");
    expect(near(animated.transform.position[1], 1.0f), "position track interpolates every axis");
    expect(near(animated.transform.rotation[1], 1.0f),
           "animation rotation track takes precedence over spin");
    expect(near(animated.transform.scale[0], 2.0f), "scale track interpolates linearly");

    for (int frame = 0; frame < 11; ++frame) scene.update(0.05f);
    expect(near(animated.animation.timeSeconds, 0.05f), "looping clip wraps at its duration");
    expect(near(animated.transform.position[0], 0.2f), "wrapped clip samples the start segment");

    nuby::Entity& oneShot = scene.create("one shot", nuby::Primitive::Sphere);
    oneShot.animation.enabled = true;
    oneShot.animation.looping = false;
    oneShot.animation.durationSeconds = 0.1f;
    oneShot.animation.position.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    oneShot.animation.position.addKeyframe(0.1f, {1.0f, 0.0f, 0.0f});
    scene.update(0.05f);
    expect(near(oneShot.transform.position[0], 0.5f), "one-shot animation samples between keys");
    scene.update(0.05f);
    expect(!oneShot.animation.playing && near(oneShot.transform.position[0], 1.0f),
           "one-shot animation holds its final pose");
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
    testEntityDestructionCascadesAndPreservesOtherEntities();
    testActiveCameraAndOrbitInput();
    testPointLightCollectionUsesWorldTransformsAndFiltersInvalidLights();
    testKeyframedAnimationInterpolationAndLooping();
    testPulseDoesNotAccumulateScaleError();
    if (failures != 0) {
        std::cerr << failures << " scene test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All scene tests passed\n";
    return EXIT_SUCCESS;
}
