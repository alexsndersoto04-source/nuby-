#include "nuby/scene_io.hpp"
#include "nuby/scene.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool near(float actual, float expected, float epsilon = 0.00001f) {
    return std::fabs(actual - expected) <= epsilon;
}

std::string stripVersionFourTextureFields(const std::string& encoded) {
    std::istringstream input(encoded);
    std::ostringstream legacy;
    std::string line;
    while (std::getline(input, line)) {
        if (line == "NUBY_SCENE 4") {
            legacy << "NUBY_SCENE 3\n";
        } else if (line.rfind("MATERIAL ", 0) == 0) {
            std::istringstream material(line);
            std::string tag;
            material >> tag;
            legacy << tag;
            for (int value = 0; value < 10; ++value) {
                std::string field;
                material >> field;
                legacy << ' ' << field;
            }
            legacy << '\n';
        } else {
            legacy << line << '\n';
        }
    }
    return legacy.str();
}

nuby::Scene makeScene() {
    nuby::Scene scene;
    nuby::Entity& root = scene.create("root \"quoted\" \\ node", nuby::Primitive::None);
    const std::uint32_t rootId = root.id;
    root.transform.position = {1.0f, 2.0f, 3.0f};
    root.material.color = {0.25f, 0.5f, 0.75f};
    root.material.emission = {0.01f, 0.02f, 0.03f};
    root.material.metallic = 0.4f;
    root.material.roughness = 0.3f;
    root.material.ambientOcclusion = 0.8f;
    root.rigidbody.enabled = true;
    root.rigidbody.mass = 2.5f;
    root.rigidbody.velocity = {1.0f, -2.0f, 0.5f};
    root.rigidbody.linearDamping = 0.08f;
    root.rigidbody.gravityScale = 0.0f;
    root.sphereCollider.enabled = true;
    root.sphereCollider.radius = 0.65f;
    root.sphereCollider.restitution = 0.6f;
    root.sphereCollider.friction = 0.25f;
    root.sphereCollider.trigger = true;

    nuby::Entity& mesh = scene.create("mesh", nuby::Primitive::ImportedMesh);
    const std::uint32_t meshId = mesh.id;
    mesh.meshAsset = "meshes/shard \"A\".obj";
    mesh.transform.position = {0.25f, -0.5f, 1.25f};
    mesh.transform.rotation = {0.1f, 0.2f, 0.3f};
    mesh.transform.scale = {0.5f, 0.6f, 0.7f};
    mesh.material.roughness = 0.3f;
    mesh.material.albedoTexture = "textures/hero diffuse.tga";
    mesh.material.normalTexture = "textures/hero normal.tga";
    mesh.material.metallicRoughnessTexture = "textures/hero metal-rough.tga";
    mesh.material.occlusionTexture = "textures/hero ao.tga";
    mesh.spin.radiansPerSecond = {0.2f, 0.3f, 0.4f};
    mesh.orbit.enabled = true;
    mesh.orbit.center = {1.0f, 2.0f, 3.0f};
    mesh.orbit.radius = 2.5f;
    mesh.orbit.angularSpeed = 0.7f;
    mesh.orbit.phase = 0.2f;
    mesh.orbit.verticalAmplitude = 0.4f;
    mesh.orbit.verticalFrequency = 1.5f;
    mesh.pulse.enabled = true;
    mesh.pulse.baseScale = mesh.transform.scale;
    mesh.pulse.amplitude = 0.1f;
    mesh.pulse.frequency = 0.8f;
    mesh.pulse.phase = 0.25f;
    mesh.animation.enabled = true;
    mesh.animation.playing = false;
    mesh.animation.looping = false;
    mesh.animation.playbackRate = 1.25f;
    mesh.animation.timeSeconds = 0.5f;
    mesh.animation.durationSeconds = 2.0f;
    mesh.animation.position.addKeyframe(0.0f, {0.0f, 0.0f, 0.0f});
    mesh.animation.position.addKeyframe(2.0f, {2.0f, 4.0f, 6.0f});
    mesh.animation.rotation.addKeyframe(0.5f, {0.1f, 0.2f, 0.3f});
    mesh.animation.scale.addKeyframe(0.0f, {1.0f, 1.0f, 1.0f});
    mesh.animation.scale.addKeyframe(2.0f, {2.0f, 2.0f, 2.0f});
    mesh.pointLight.enabled = true;
    mesh.pointLight.color = {0.2f, 0.4f, 0.8f};
    mesh.pointLight.intensity = 3.5f;
    mesh.pointLight.range = 8.0f;
    expect(scene.setParent(meshId, rootId), "sample mesh attaches to root");

    nuby::Entity& camera = scene.create("camera", nuby::Primitive::None);
    const std::uint32_t cameraId = camera.id;
    camera.camera.enabled = true;
    camera.camera.verticalFovRadians = 0.9f;
    camera.camera.nearPlane = 0.2f;
    camera.camera.farPlane = 120.0f;
    camera.orbitCamera.enabled = true;
    camera.orbitCamera.target = {0.5f, 0.25f, -0.75f};
    camera.orbitCamera.distance = 6.0f;
    camera.orbitCamera.minimumDistance = 1.0f;
    camera.orbitCamera.maximumDistance = 20.0f;
    camera.orbitCamera.yaw = 1.2f;
    camera.orbitCamera.pitch = -0.3f;
    camera.orbitCamera.minimumPitch = -0.8f;
    camera.orbitCamera.maximumPitch = 0.8f;
    camera.orbitCamera.yawPerPixel = 0.01f;
    camera.orbitCamera.pitchPerPixel = 0.02f;
    camera.characterController.enabled = true;
    camera.characterController.moveInput = {0.6f, -0.8f};
    camera.characterController.maximumSpeed = 5.2f;
    camera.characterController.acceleration = 14.0f;
    camera.characterController.jumpSpeed = 6.1f;
    camera.characterController.grounded = true;
    expect(scene.setParent(cameraId, rootId), "camera attaches to root");
    expect(scene.setActiveCamera(cameraId), "sample camera is activated");

    nuby::Entity& removed = scene.create("removed", nuby::Primitive::Sphere);
    expect(scene.destroyEntity(removed.id), "sample entity is removed to create an ID gap");
    nuby::PhysicsSettings& physics = scene.physicsSettings();
    physics.gravity = {0.0f, -12.0f, 0.0f};
    physics.fixedTimeStep = 1.0f / 120.0f;
    physics.maximumSubsteps = 8;
    physics.floorEnabled = true;
    physics.floorHeight = -1.5f;
    physics.floorRestitution = 0.45f;
    physics.floorFriction = 0.2f;
    scene.update(0.025f);
    return scene;
}

void testRoundTripAllSceneData() {
    nuby::Scene original = makeScene();
    std::stringstream encoded;
    std::string error;
    expect(nuby::SceneSerializer::save(original, encoded, error), "valid scene serializes");
    expect(error.empty(), "successful save clears error text");

    nuby::Scene loaded;
    loaded.create("replace me", nuby::Primitive::Sphere);
    expect(nuby::SceneSerializer::load(loaded, encoded, error), "valid scene deserializes");
    expect(error.empty(), "successful load clears error text");
    expect(loaded.entities().size() == 3, "round-trip preserves entity count");
    expect(loaded.elapsedSeconds() == original.elapsedSeconds(), "round-trip preserves scene time");
    expect(loaded.activeCamera() != nullptr && loaded.activeCamera()->id == 3,
           "round-trip preserves the active camera ID");
    expect(near(loaded.physicsSettings().gravity[1], -12.0f)
                   && near(loaded.physicsSettings().fixedTimeStep, 1.0f / 120.0f)
                   && loaded.physicsSettings().maximumSubsteps == 8
                   && loaded.physicsSettings().floorEnabled
                   && near(loaded.physicsSettings().floorHeight, -1.5f),
           "round-trip preserves world physics settings");

    const nuby::Entity* root = loaded.find(1);
    const nuby::Entity* mesh = loaded.find(2);
    const nuby::Entity* camera = loaded.find(3);
    expect(root != nullptr && root->name == "root \"quoted\" \\ node",
           "quoted entity names are escaped and restored");
    expect(root != nullptr && root->rigidbody.enabled && near(root->rigidbody.mass, 2.5f)
                   && near(root->rigidbody.velocity[1], -2.0f, 0.005f)
                   && root->sphereCollider.enabled && root->sphereCollider.trigger
                   && near(root->sphereCollider.radius, 0.65f),
           "rigidbody and sphere collider components are restored");
    expect(mesh != nullptr && mesh->parentId == 1
                   && mesh->meshAsset == "meshes/shard \"A\".obj",
           "mesh path and parent link are restored");
    if (mesh != nullptr) {
        expect(mesh->primitive == nuby::Primitive::ImportedMesh,
               "primitive type is restored");
        expect(near(mesh->transform.rotation[1], 0.2f), "transform rotation is restored");
        expect(near(mesh->material.roughness, 0.3f), "material is restored");
        expect(mesh->material.albedoTexture == "textures/hero diffuse.tga"
                       && mesh->material.normalTexture == "textures/hero normal.tga"
                       && mesh->material.metallicRoughnessTexture
                               == "textures/hero metal-rough.tga"
                       && mesh->material.occlusionTexture == "textures/hero ao.tga",
               "PBR texture-map asset references are restored");
        expect(near(mesh->orbit.angularSpeed, 0.7f), "orbit component is restored");
        expect(near(mesh->pulse.amplitude, 0.1f), "pulse component is restored");
        expect(mesh->animation.position.keyframes.size() == 2
                       && near(mesh->animation.position.keyframes[1].value[2], 6.0f),
               "animation tracks and keyframes are restored");
        expect(mesh->pointLight.enabled && near(mesh->pointLight.intensity, 3.5f),
               "point-light component is restored");
    }
    expect(camera != nullptr && camera->parentId == 1
                   && near(camera->camera.farPlane, 120.0f)
                   && near(camera->orbitCamera.pitch, -0.3f),
           "camera projection and orbit controls are restored");
    expect(camera != nullptr && camera->characterController.enabled
                   && near(camera->characterController.moveInput[0], 0.6f)
                   && near(camera->characterController.moveInput[1], -0.8f)
                   && near(camera->characterController.maximumSpeed, 5.2f)
                   && near(camera->characterController.acceleration, 14.0f)
                   && near(camera->characterController.jumpSpeed, 6.1f)
                   && camera->characterController.grounded,
           "character controller settings and grounded state are restored");

    std::stringstream reencoded;
    expect(nuby::SceneSerializer::save(loaded, reencoded, error), "loaded scene can be saved again");
    expect(encoded.str() == reencoded.str(), "scene serialization is deterministic after round-trip");
    const nuby::Entity& next = loaded.create("after load", nuby::Primitive::Sphere);
    expect(next.id == 5, "round-trip preserves the next entity ID after deletion gaps");
}

void testMalformedFilesDoNotPartiallyReplaceScene() {
    nuby::Scene target;
    const std::uint32_t keepId = target.create("keep", nuby::Primitive::Sphere).id;
    std::string error;
    std::stringstream unsupported("NUBY_SCENE 99\n");
    expect(!nuby::SceneSerializer::load(target, unsupported, error),
           "unsupported versions are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "failed load leaves the target scene unchanged");
    expect(!error.empty(), "failed load reports a useful error");

    nuby::Scene source = makeScene();
    std::stringstream valid;
    expect(nuby::SceneSerializer::save(source, valid, error), "test source scene serializes");
    std::string cyclic = valid.str();
    const std::string rootRecord = "ENTITY 1 0 ";
    const std::size_t position = cyclic.find(rootRecord);
    expect(position != std::string::npos, "test scene contains root record");
    if (position != std::string::npos) cyclic.replace(position, rootRecord.size(), "ENTITY 1 2 ");
    std::stringstream invalidHierarchy(cyclic);
    expect(!nuby::SceneSerializer::load(target, invalidHierarchy, error),
           "cyclic hierarchy is rejected during load");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid hierarchy cannot partially replace target scene");

    std::string truncated = valid.str();
    truncated.resize(truncated.size() - 8);
    std::stringstream incomplete(truncated);
    expect(!nuby::SceneSerializer::load(target, incomplete, error),
           "truncated scene is rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "truncated file leaves target scene unchanged");

    std::stringstream legacyV1("NUBY_SCENE 1\nSCENE 2.5 0 1 0\nEND_SCENE\n");
    expect(nuby::SceneSerializer::load(target, legacyV1, error),
           "version-one scenes remain readable after the physics schema upgrade");
    expect(target.entities().empty() && near(target.physicsSettings().gravity[1], -9.81f),
           "version-one scenes receive safe default physics settings");
    std::stringstream legacyV2("NUBY_SCENE 2\nSCENE 2.5 0 1 0\n"
            "PHYSICS 0 -9.81 0 0.0166666675 4 0 0 0.2 0.35\nEND_SCENE\n");
    expect(nuby::SceneSerializer::load(target, legacyV2, error),
           "version-two physics scenes remain readable after adding character controllers");
    expect(target.entities().empty() && near(target.physicsSettings().gravity[1], -9.81f),
           "version-two scenes receive default character controller state");
    const std::string versionThreeText = stripVersionFourTextureFields(valid.str());
    std::stringstream legacyV3(versionThreeText);
    expect(nuby::SceneSerializer::load(target, legacyV3, error),
           "version-three scenes remain readable after PBR texture references are added");
    const nuby::Entity* legacyMesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyMesh = &entity;
    }
    expect(legacyMesh != nullptr && legacyMesh->material.albedoTexture.empty()
                   && legacyMesh->material.normalTexture.empty(),
           "legacy version-three materials receive empty texture-map defaults");

    nuby::Scene invalidTexture;
    invalidTexture.create("invalid texture", nuby::Primitive::Sphere)
            .material.albedoTexture = "../outside.tga";
    std::stringstream invalidTextureOutput;
    expect(!nuby::SceneSerializer::save(invalidTexture, invalidTextureOutput, error),
           "scene serialization rejects texture paths that escape the project assets");

    nuby::Scene invalidPhysics;
    invalidPhysics.physicsSettings().fixedTimeStep = 0.0f;
    std::stringstream invalidPhysicsOutput;
    expect(!nuby::SceneSerializer::save(invalidPhysics, invalidPhysicsOutput, error),
           "scene serialization rejects invalid physics settings");
}
}  // namespace

int main() {
    testRoundTripAllSceneData();
    testMalformedFilesDoNotPartiallyReplaceScene();
    if (failures != 0) {
        std::cerr << failures << " scene IO test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All scene IO tests passed\n";
    return EXIT_SUCCESS;
}
