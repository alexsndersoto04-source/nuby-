#include "nuby/scene_io.hpp"
#include "nuby/scene.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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

std::string withoutTrailingFields(const std::string& line, std::size_t fieldCount) {
    std::istringstream input(line);
    std::vector<std::string> fields;
    std::string field;
    while (input >> field) fields.push_back(field);
    std::ostringstream output;
    const std::size_t retained = fields.size() > fieldCount ? fields.size() - fieldCount : 0;
    for (std::size_t index = 0; index < retained; ++index) {
        if (index != 0) output << ' ';
        output << fields[index];
    }
    return output.str();
}

bool replaceRecordField(std::string& text, const std::string& tag,
                        std::size_t fieldIndex, const std::string& replacement,
                        std::size_t occurrence = 0) {
    std::size_t start = 0;
    for (std::size_t index = 0; index <= occurrence; ++index) {
        start = text.find(tag + " ", start);
        if (start == std::string::npos) return false;
        if (index < occurrence) start += tag.size() + 1;
    }
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) return false;
    std::istringstream input(text.substr(start, end - start));
    std::vector<std::string> fields;
    std::string field;
    while (input >> field) fields.push_back(field);
    if (fieldIndex >= fields.size()) return false;
    fields[fieldIndex] = replacement;
    std::ostringstream output;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) output << ' ';
        output << fields[index];
    }
    text.replace(start, end - start, output.str());
    return true;
}

std::string downgradeTextureSchema(const std::string& encoded,
                                   unsigned int targetVersion,
                                   unsigned int textureMapsToKeep) {
    std::istringstream input(encoded);
    std::ostringstream legacy;
    std::string line;
    while (std::getline(input, line)) {
        if (line == "NUBY_SCENE 32") {
            legacy << "NUBY_SCENE " << targetVersion << '\n';
        } else if (line.rfind("MATERIAL_SHEEN ", 0) == 0
                && targetVersion < 32) {
            continue;
        } else if (line.rfind("MATERIAL_SPECULAR ", 0) == 0
                && targetVersion < 31) {
            continue;
        } else if (line.rfind("MATERIAL_CLEARCOAT ", 0) == 0
                && targetVersion < 30) {
            continue;
        } else if (line.rfind("MATERIAL_PBR_EXT ", 0) == 0 && targetVersion < 28) {
            continue;
        } else if (line.rfind("SKY_AMBIENT ", 0) == 0 && targetVersion < 26) {
            continue;
        } else if (line.rfind("SKY ", 0) == 0 && targetVersion < 25) {
            continue;
        } else if (line.rfind("ATMOSPHERE ", 0) == 0 && targetVersion < 24) {
            continue;
        } else if (line.rfind("DIRECTIONAL_LIGHT ", 0) == 0 && targetVersion < 23) {
            continue;
        } else if (line.rfind("AUDIO_SOURCE ", 0) == 0 && targetVersion < 22) {
            continue;
        } else if (line.rfind("ORBIT_CAMERA_COLLISION ", 0) == 0 && targetVersion < 21) {
            continue;
        } else if (line.rfind("CHARACTER_CONTROLLER ", 0) == 0 && targetVersion < 20) {
            std::istringstream record(line);
            std::string tag;
            std::vector<std::string> fields;
            std::string field;
            record >> tag;
            while (record >> field) fields.push_back(field);
            legacy << tag;
            for (std::size_t index = 0U; index < fields.size(); ++index) {
                if (index == fields.size() - 3U || index == fields.size() - 2U) continue;
                legacy << ' ' << fields[index];
            }
            legacy << '\n';
        } else if (line.rfind("SKELETAL_LOCOMOTION ", 0) == 0) {
            if (targetVersion < 18) continue;
            legacy << (targetVersion < 19 ? withoutTrailingFields(line, 1) : line) << '\n';
        } else if (line.rfind("SKELETAL_PLAYER ", 0) == 0) {
            if (targetVersion < 17) continue;
            legacy << (targetVersion < 19 ? withoutTrailingFields(line, 4) : line) << '\n';
        } else if (line.rfind("AMBIENT_SH ", 0) == 0 && targetVersion < 16) {
            continue;
        } else if (line.rfind("SPHERE_COLLIDER ", 0) == 0 && targetVersion < 15) {
            legacy << withoutTrailingFields(line, 2) << '\n';
        } else if (line.rfind("BOX_COLLIDER ", 0) == 0) {
            if (targetVersion >= 14) {
                legacy << (targetVersion < 15 ? withoutTrailingFields(line, 2) : line) << '\n';
            }
        } else if (line.rfind("PHYSICS ", 0) == 0 && targetVersion < 15) {
            legacy << withoutTrailingFields(line, 2) << '\n';
        } else if (line.rfind("MATERIAL_MAP_UV ", 0) == 0) {
            if (targetVersion < 7) continue;
            std::istringstream mapRecord(line);
            std::string tag;
            mapRecord >> tag;
            std::vector<std::string> fields;
            std::string field;
            while (mapRecord >> field) fields.push_back(field);
            legacy << tag;
            const std::size_t mapsToKeep = targetVersion == 7 ? 6U
                    : targetVersion < 10 ? 6U : targetVersion < 12 ? 7U : 8U;
            const bool keepSamplerSettings = targetVersion >= 8;
            for (std::size_t map = 0; map < mapsToKeep; ++map) {
                const std::size_t base = map * 7U;
                for (std::size_t component = 0; component < 4U; ++component) {
                    legacy << ' ' << fields[base + component];
                }
                if (targetVersion >= 27) legacy << ' ' << fields[base + 4U];
                if (keepSamplerSettings) legacy << ' ' << fields[base + 5U];
                if (targetVersion >= 29) legacy << ' ' << fields[base + 6U];
            }
            legacy << '\n';
        } else if (line.rfind("RIGIDBODY ", 0) == 0 && targetVersion < 13) {
            std::istringstream rigidbody(line);
            std::string tag;
            rigidbody >> tag;
            legacy << tag;
            for (int value = 0; value < 7; ++value) {
                std::string field;
                rigidbody >> field;
                legacy << ' ' << field;
            }
            legacy << '\n';
        } else if (line.rfind("OPACITY_MAP_CHANNEL ", 0) == 0
                && targetVersion < 10) {
            continue;
        } else if (line.rfind("MATERIAL_MAP_MM ", 0) == 0
                && targetVersion < 11) {
            continue;
        } else if (line.rfind("MATERIAL_MAP_MM ", 0) == 0
                && targetVersion < 12) {
            std::istringstream ranges(line);
            std::string tag;
            ranges >> tag;
            legacy << tag;
            for (int value = 0; value < 14; ++value) {
                std::string field;
                ranges >> field;
                legacy << ' ' << field;
            }
            legacy << '\n';
        } else if (line.rfind("OVERRIDE_MTL_MATERIALS ", 0) == 0
                && targetVersion < 6) {
            continue;
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
            std::string opacity;
            material >> opacity;
            if (targetVersion >= 9) legacy << ' ' << opacity;
            for (unsigned int map = 0; map < 6; ++map) {
                std::string path;
                material >> std::quoted(path);
                if (map < textureMapsToKeep) legacy << ' ' << std::quoted(path);
            }
            std::string opacityPath;
            material >> std::quoted(opacityPath);
            if (targetVersion >= 10) legacy << ' ' << std::quoted(opacityPath);
            std::string emissivePath;
            material >> std::quoted(emissivePath);
            if (targetVersion >= 12) legacy << ' ' << std::quoted(emissivePath);
            legacy << '\n';
        } else {
            legacy << line << '\n';
        }
    }
    return legacy.str();
}

nuby::Scene makeScene() {
    nuby::Scene scene;
    scene.ambientLighting().sphericalHarmonics[0] = 0.7f;
    scene.ambientLighting().sphericalHarmonics[4] = -0.12f;
    scene.ambientLighting().sphericalHarmonics[26] = 0.23f;
    scene.directionalLighting().direction = {-0.2f, -0.9f, -0.3f};
    scene.directionalLighting().color = {1.0f, 0.72f, 0.48f};
    scene.directionalLighting().intensity = 1.35f;
    scene.directionalLighting().shadowStrength = 0.64f;
    scene.atmosphere().fogEnabled = true;
    scene.atmosphere().fogColor = {0.16f, 0.22f, 0.31f};
    scene.atmosphere().fogStart = 11.0f;
    scene.atmosphere().fogEnd = 48.0f;
    scene.sky().enabled = true;
    scene.sky().sunDiskEnabled = false;
    scene.sky().zenithColor = {0.1f, 0.2f, 0.4f};
    scene.sky().horizonColor = {0.7f, 0.6f, 0.5f};
    scene.sky().groundColor = {0.08f, 0.09f, 0.1f};
    scene.sky().sunColor = {1.0f, 0.8f, 0.5f};
    scene.sky().sunAngularRadius = 0.025f;
    scene.sky().ambientStrength = 0.6f;
    scene.physicsSettings().floorCollisionLayer = 4U;
    scene.physicsSettings().floorCollisionMask = 0x0000000CU;
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
    root.rigidbody.angularVelocity = {0.15f, -0.25f, 0.35f};
    root.rigidbody.linearDamping = 0.08f;
    root.rigidbody.angularDamping = 0.12f;
    root.rigidbody.gravityScale = 0.0f;
    root.sphereCollider.enabled = true;
    root.sphereCollider.radius = 0.65f;
    root.sphereCollider.restitution = 0.6f;
    root.sphereCollider.friction = 0.25f;
    root.sphereCollider.trigger = true;
    root.sphereCollider.collisionLayer = 2U;
    root.sphereCollider.collisionMask = 0x00000008U;

    nuby::Entity& mesh = scene.create("mesh", nuby::Primitive::ImportedMesh);
    const std::uint32_t meshId = mesh.id;
    mesh.meshAsset = "meshes/shard \"A\".obj";
    mesh.overrideMtlMaterials = true;
    mesh.transform.position = {0.25f, -0.5f, 1.25f};
    mesh.transform.rotation = {0.1f, 0.2f, 0.3f};
    mesh.transform.scale = {0.5f, 0.6f, 0.7f};
    mesh.material.roughness = 0.3f;
    mesh.material.opacity = 0.42f;
    mesh.material.normalMapScale = 0.35f;
    mesh.material.emissiveStrength = 3.5f;
    mesh.material.indexOfRefraction = 1.33f;
    mesh.material.clearcoatFactor = 0.72f;
    mesh.material.clearcoatRoughness = 0.31f;
    mesh.material.clearcoatNormalScale = 0.45f;
    mesh.material.specularFactor = 0.63f;
    mesh.material.specularColorFactor = {0.8f, 0.65f, 0.9f};
    mesh.material.sheenColorFactor = {0.2f, 0.35f, 0.5f};
    mesh.material.sheenRoughness = 0.4f;
    mesh.material.unlit = true;
    mesh.skeletalAnimation.enabled = true;
    mesh.skeletalAnimation.playing = false;
    mesh.skeletalAnimation.looping = true;
    mesh.skeletalAnimation.playbackRate = 0.75f;
    mesh.skeletalAnimation.timeSeconds = 0.5f;
    mesh.skeletalAnimation.durationSeconds = 1.5f;
    mesh.skeletalAnimation.clipName = "walk";
    mesh.skeletalAnimation.transitionFromClip = "idle";
    mesh.skeletalAnimation.transitionFromTimeSeconds = 0.3f;
    mesh.skeletalAnimation.transitionElapsedSeconds = 0.05f;
    mesh.skeletalAnimation.transitionFromLooping = true;
    mesh.skeletalLocomotion.enabled = true;
    mesh.skeletalLocomotion.idleClip = "idle";
    mesh.skeletalLocomotion.walkClip = "walk";
    mesh.skeletalLocomotion.runClip = "run";
    mesh.skeletalLocomotion.jumpClip = "jump";
    mesh.skeletalLocomotion.walkSpeedThreshold = 0.25f;
    mesh.skeletalLocomotion.runSpeedThreshold = 2.5f;
    mesh.skeletalLocomotion.transitionDurationSeconds = 0.2f;
    mesh.material.albedoTexture = "textures/hero diffuse.tga";
    mesh.material.normalTexture = "textures/hero normal.tga";
    mesh.material.metallicRoughnessTexture = "textures/hero metal-rough.tga";
    mesh.material.occlusionTexture = "textures/hero ao.tga";
    mesh.material.metallicTexture = "textures/hero metallic.tga";
    mesh.material.roughnessTexture = "textures/hero roughness.tga";
    mesh.material.opacityTexture = "textures/hero opacity.tga";
    mesh.material.emissiveTexture = "textures/hero emission.tga";
    mesh.material.clearcoatTexture = "textures/hero clearcoat.tga";
    mesh.material.clearcoatRoughnessTexture = "textures/hero clearcoat rough.tga";
    mesh.material.clearcoatNormalTexture = "textures/hero clearcoat normal.tga";
    mesh.material.specularTexture = "textures/hero specular.tga";
    mesh.material.specularColorTexture = "textures/hero specular color.tga";
    mesh.material.sheenColorTexture = "textures/hero sheen color.tga";
    mesh.material.sheenRoughnessTexture = "textures/hero sheen rough.tga";
    mesh.material.opacityMapChannel = nuby::ScalarTextureChannel::Matte;
    mesh.boxCollider.enabled = true;
    mesh.boxCollider.halfExtents = {0.75f, 1.25f, 0.5f};
    mesh.boxCollider.restitution = 0.65f;
    mesh.boxCollider.friction = 0.18f;
    mesh.boxCollider.trigger = true;
    mesh.boxCollider.collisionLayer = 8U;
    mesh.boxCollider.collisionMask = 0x00000006U;
    mesh.material.opacityMapTransform.uvScale = {0.7f, 0.8f};
    mesh.material.opacityMapTransform.uvOffset = {0.12f, -0.08f};
    mesh.material.opacityMapTransform.uvRotation = -0.35f;
    mesh.material.opacityMapTransform.clampToEdge = true;
    mesh.material.emissiveMapTransform.uvScale = {1.25f, 0.75f};
    mesh.material.emissiveMapTransform.uvOffset = {-0.2f, 0.15f};
    mesh.material.emissiveMapTransform.clampToEdge = true;
    mesh.material.clearcoatMapTransform.uvScale = {0.65f, 1.25f};
    mesh.material.clearcoatMapTransform.uvOffset = {0.12f, -0.3f};
    mesh.material.clearcoatMapTransform.uvRotation = 0.4f;
    mesh.material.clearcoatMapTransform.uvSet = 1U;
    mesh.material.clearcoatMapTransform.clampToEdge = true;
    mesh.material.clearcoatRoughnessMapTransform.uvScale = {0.8f, 0.9f};
    mesh.material.clearcoatRoughnessMapTransform.uvOffset = {-0.1f, 0.2f};
    mesh.material.clearcoatNormalMapTransform.uvRotation = -0.6f;
    mesh.material.clearcoatNormalMapTransform.uvSet = 1U;
    mesh.material.specularMapTransform.uvScale = {1.4f, 0.75f};
    mesh.material.specularMapTransform.uvOffset = {0.1f, -0.2f};
    mesh.material.specularMapTransform.uvRotation = 0.3f;
    mesh.material.specularMapTransform.uvSet = 1U;
    mesh.material.specularColorMapTransform.clampToEdge = true;
    mesh.material.specularColorMapTransform.uvRotation = -0.25f;
    mesh.material.sheenColorMapTransform.uvSet = 1U;
    mesh.material.sheenColorMapTransform.uvScale = {1.3f, 0.7f};
    mesh.material.sheenColorMapTransform.uvOffset = {0.15f, -0.1f};
    mesh.material.sheenRoughnessMapTransform.uvRotation = 0.55f;
    mesh.material.sheenRoughnessMapTransform.clampToEdge = true;
    mesh.material.albedoMapValueRange = {-0.1f, 1.35f};
    mesh.material.normalMapValueRange = {0.1f, 0.8f};
    mesh.material.metallicRoughnessMapValueRange = {0.2f, 0.7f};
    mesh.material.metallicMapValueRange = {-0.3f, 1.2f};
    mesh.material.roughnessMapValueRange = {0.04f, 0.9f};
    mesh.material.occlusionMapValueRange = {0.0f, 1.4f};
    mesh.material.opacityMapValueRange = {0.05f, 0.95f};
    mesh.material.emissiveMapValueRange = {0.08f, 1.25f};
    mesh.material.albedoMapTransform.uvScale = {0.5f, 0.75f};
    mesh.material.albedoMapTransform.uvOffset = {0.1f, 0.2f};
    mesh.material.albedoMapTransform.uvRotation = 0.6f;
    mesh.material.albedoMapTransform.uvSet = 1U;
    mesh.material.albedoMapTransform.clampToEdge = true;
    mesh.material.normalMapTransform.uvScale = {2.0f, 1.5f};
    mesh.material.normalMapTransform.uvOffset = {-0.25f, 0.35f};
    mesh.material.roughnessMapTransform.uvScale = {0.8f, 0.9f};
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
    camera.orbitCamera.collisionEnabled = true;
    camera.orbitCamera.collisionRadius = 0.31f;
    camera.orbitCamera.collisionPadding = 0.22f;
    camera.orbitCamera.collisionMinimumDistance = 0.42f;
    camera.orbitCamera.collisionRecoverySpeed = 6.5f;
    camera.orbitCamera.collisionLayer = 8U;
    camera.orbitCamera.collisionMask = 0x00000014U;
    camera.audioSource.enabled = true;
    camera.audioSource.assetPath = "audio/looped cue.wav";
    camera.audioSource.bus = nuby::AudioBus::Music;
    camera.audioSource.gain = 0.55f;
    camera.audioSource.priority = 201U;
    camera.audioSource.looping = true;
    camera.audioSource.positional = false;
    camera.audioSource.playOnAwake = false;
    camera.audioSource.referenceDistance = 2.0f;
    camera.audioSource.maximumDistance = 18.0f;
    camera.audioSource.rolloff = 0.8f;
    camera.characterController.enabled = true;
    camera.characterController.moveInput = {0.6f, -0.8f};
    camera.characterController.maximumSpeed = 5.2f;
    camera.characterController.acceleration = 14.0f;
    camera.characterController.jumpSpeed = 6.1f;
    camera.characterController.coyoteTimeSeconds = 0.12f;
    camera.characterController.jumpBufferSeconds = 0.20f;
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
    physics.floorCollisionLayer = 4U;
    physics.floorCollisionMask = 0x0000000CU;
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
    expect(loaded.activeCamera() != nullptr
                   && loaded.activeCamera()->orbitCamera.collisionEnabled
                   && near(loaded.activeCamera()->orbitCamera.collisionRadius, 0.31f)
                   && near(loaded.activeCamera()->orbitCamera.collisionPadding, 0.22f)
                   && near(loaded.activeCamera()->orbitCamera.collisionMinimumDistance, 0.42f)
                   && near(loaded.activeCamera()->orbitCamera.collisionRecoverySpeed, 6.5f)
                   && loaded.activeCamera()->orbitCamera.collisionLayer == 8U
                   && loaded.activeCamera()->orbitCamera.collisionMask == 0x00000014U,
           "round-trip preserves spring-arm collision controls and filtering");
    expect(loaded.activeCamera() != nullptr
                   && loaded.activeCamera()->audioSource.enabled
                   && loaded.activeCamera()->audioSource.assetPath == "audio/looped cue.wav"
                   && loaded.activeCamera()->audioSource.bus == nuby::AudioBus::Music
                   && near(loaded.activeCamera()->audioSource.gain, 0.55f)
                   && loaded.activeCamera()->audioSource.priority == 201U
                   && loaded.activeCamera()->audioSource.looping
                   && !loaded.activeCamera()->audioSource.positional
                   && !loaded.activeCamera()->audioSource.playOnAwake
                   && near(loaded.activeCamera()->audioSource.referenceDistance, 2.0f)
                   && near(loaded.activeCamera()->audioSource.maximumDistance, 18.0f)
                   && near(loaded.activeCamera()->audioSource.rolloff, 0.8f),
           "round-trip preserves serialized audio emitter and spatial mix settings");
    expect(near(loaded.physicsSettings().gravity[1], -12.0f)
                   && near(loaded.physicsSettings().fixedTimeStep, 1.0f / 120.0f)
                   && loaded.physicsSettings().maximumSubsteps == 8
                   && loaded.physicsSettings().floorEnabled
                   && near(loaded.physicsSettings().floorHeight, -1.5f)
                   && loaded.physicsSettings().floorCollisionLayer == 4U
                   && loaded.physicsSettings().floorCollisionMask == 0x0000000CU,
           "round-trip preserves world physics settings");
    expect(near(loaded.ambientLighting().sphericalHarmonics[0], 0.7f)
                   && near(loaded.ambientLighting().sphericalHarmonics[4], -0.12f)
                   && near(loaded.ambientLighting().sphericalHarmonics[26], 0.23f),
           "round-trip preserves the scene's spherical-harmonic environment light");
    expect(near(loaded.directionalLighting().direction[0], -0.2f)
                   && near(loaded.directionalLighting().direction[1], -0.9f)
                   && near(loaded.directionalLighting().color[1], 0.72f)
                   && near(loaded.directionalLighting().intensity, 1.35f)
                   && near(loaded.directionalLighting().shadowStrength, 0.64f),
           "round-trip preserves scene-authored directional light and shadow strength");
    expect(loaded.atmosphere().fogEnabled
                   && near(loaded.atmosphere().fogColor[2], 0.31f)
                   && near(loaded.atmosphere().fogStart, 11.0f)
                   && near(loaded.atmosphere().fogEnd, 48.0f),
           "round-trip preserves distance-fog color and range");
    expect(loaded.sky().enabled && !loaded.sky().sunDiskEnabled
                   && near(loaded.sky().zenithColor[2], 0.4f)
                   && near(loaded.sky().horizonColor[0], 0.7f)
                   && near(loaded.sky().sunColor[1], 0.8f)
                   && near(loaded.sky().sunAngularRadius, 0.025f)
                   && near(loaded.sky().ambientStrength, 0.6f),
           "round-trip preserves procedural sky palette and sun-disc settings");

    const nuby::Entity* root = loaded.find(1);
    const nuby::Entity* mesh = loaded.find(2);
    const nuby::Entity* camera = loaded.find(3);
    expect(root != nullptr && root->name == "root \"quoted\" \\ node",
           "quoted entity names are escaped and restored");
    expect(root != nullptr && root->rigidbody.enabled && near(root->rigidbody.mass, 2.5f)
                   && near(root->rigidbody.velocity[1], -2.0f, 0.005f)
                   && near(root->rigidbody.angularVelocity[2], 0.35f, 0.005f)
                   && near(root->rigidbody.angularDamping, 0.12f)
                   && root->sphereCollider.enabled && root->sphereCollider.trigger
                   && near(root->sphereCollider.radius, 0.65f)
                   && root->sphereCollider.collisionLayer == 2U
                   && root->sphereCollider.collisionMask == 0x00000008U,
           "rigidbody and sphere collider components are restored");
    expect(mesh != nullptr && mesh->parentId == 1
                   && mesh->meshAsset == "meshes/shard \"A\".obj",
           "mesh path and parent link are restored");
    if (mesh != nullptr) {
        expect(mesh->primitive == nuby::Primitive::ImportedMesh,
               "primitive type is restored");
        expect(near(mesh->transform.rotation[1], 0.2f), "transform rotation is restored");
        expect(near(mesh->material.roughness, 0.3f)
                       && near(mesh->material.opacity, 0.42f)
                       && near(mesh->material.normalMapScale, 0.35f)
                       && near(mesh->material.emissiveStrength, 3.5f)
                       && near(mesh->material.indexOfRefraction, 1.33f)
                       && near(mesh->material.clearcoatFactor, 0.72f)
                       && near(mesh->material.clearcoatRoughness, 0.31f)
                       && near(mesh->material.clearcoatNormalScale, 0.45f)
                       && near(mesh->material.specularFactor, 0.63f)
                       && near(mesh->material.specularColorFactor[0], 0.8f)
                       && near(mesh->material.specularColorFactor[2], 0.9f)
                       && near(mesh->material.sheenColorFactor[1], 0.35f)
                       && near(mesh->material.sheenColorFactor[2], 0.5f)
                       && near(mesh->material.sheenRoughness, 0.4f)
                       && mesh->material.unlit,
               "PBR factors, IOR, clearcoat, normal scale, emissive strength and unlit state are restored");
        expect(mesh->material.albedoTexture == "textures/hero diffuse.tga"
                       && mesh->material.normalTexture == "textures/hero normal.tga"
                       && mesh->material.metallicRoughnessTexture
                               == "textures/hero metal-rough.tga"
                       && mesh->material.occlusionTexture == "textures/hero ao.tga"
                       && mesh->material.metallicTexture == "textures/hero metallic.tga"
                       && mesh->material.roughnessTexture == "textures/hero roughness.tga"
                       && mesh->material.opacityTexture == "textures/hero opacity.tga"
                       && mesh->material.emissiveTexture == "textures/hero emission.tga"
                       && mesh->material.clearcoatTexture == "textures/hero clearcoat.tga"
                       && mesh->material.clearcoatRoughnessTexture
                               == "textures/hero clearcoat rough.tga"
                       && mesh->material.clearcoatNormalTexture
                               == "textures/hero clearcoat normal.tga"
                       && mesh->material.specularTexture == "textures/hero specular.tga"
                       && mesh->material.specularColorTexture
                               == "textures/hero specular color.tga"
                       && mesh->material.sheenColorTexture
                               == "textures/hero sheen color.tga"
                       && mesh->material.sheenRoughnessTexture
                               == "textures/hero sheen rough.tga",
               "PBR, opacity, clearcoat, specular and sheen texture references are restored");
        expect(mesh->overrideMtlMaterials,
               "explicit entity-level override of imported MTL materials is restored");
        expect(mesh->boxCollider.enabled && mesh->boxCollider.trigger
                       && near(mesh->boxCollider.halfExtents[0], 0.75f)
                       && near(mesh->boxCollider.halfExtents[1], 1.25f)
                       && near(mesh->boxCollider.halfExtents[2], 0.5f)
                       && near(mesh->boxCollider.restitution, 0.65f)
                       && near(mesh->boxCollider.friction, 0.18f)
                       && mesh->boxCollider.collisionLayer == 8U
                       && mesh->boxCollider.collisionMask == 0x00000006U,
               "box collider dimensions, response and trigger state are restored");
        expect(near(mesh->material.albedoMapTransform.uvScale[0], 0.5f)
                       && near(mesh->material.albedoMapTransform.uvScale[1], 0.75f)
                       && near(mesh->material.albedoMapTransform.uvOffset[0], 0.1f)
                       && near(mesh->material.albedoMapTransform.uvRotation, 0.6f)
                       && mesh->material.albedoMapTransform.uvSet == 1U
                       && near(mesh->material.normalMapTransform.uvScale[0], 2.0f)
                       && near(mesh->material.normalMapTransform.uvOffset[0], -0.25f)
                       && near(mesh->material.roughnessMapTransform.uvScale[1], 0.9f)
                       && mesh->material.albedoMapTransform.clampToEdge
                       && !mesh->material.normalMapTransform.clampToEdge
                       && near(mesh->material.opacityMapTransform.uvScale[0], 0.7f)
                       && near(mesh->material.opacityMapTransform.uvOffset[1], -0.08f)
                       && near(mesh->material.opacityMapTransform.uvRotation, -0.35f)
                       && mesh->material.opacityMapTransform.clampToEdge
                       && near(mesh->material.emissiveMapTransform.uvScale[0], 1.25f)
                       && near(mesh->material.emissiveMapTransform.uvOffset[1], 0.15f)
                       && mesh->material.emissiveMapTransform.clampToEdge
                       && near(mesh->material.clearcoatMapTransform.uvScale[0], 0.65f)
                       && near(mesh->material.clearcoatMapTransform.uvOffset[1], -0.3f)
                       && near(mesh->material.clearcoatMapTransform.uvRotation, 0.4f)
                       && mesh->material.clearcoatMapTransform.uvSet == 1U
                       && mesh->material.clearcoatMapTransform.clampToEdge
                       && near(mesh->material.clearcoatRoughnessMapTransform.uvScale[1], 0.9f)
                       && near(mesh->material.clearcoatNormalMapTransform.uvRotation, -0.6f)
                       && mesh->material.clearcoatNormalMapTransform.uvSet == 1U
                       && near(mesh->material.specularMapTransform.uvScale[0], 1.4f)
                       && near(mesh->material.specularMapTransform.uvOffset[1], -0.2f)
                       && near(mesh->material.specularMapTransform.uvRotation, 0.3f)
                       && mesh->material.specularMapTransform.uvSet == 1U
                       && near(mesh->material.specularColorMapTransform.uvRotation, -0.25f)
                       && mesh->material.specularColorMapTransform.clampToEdge
                       && mesh->material.sheenColorMapTransform.uvSet == 1U
                       && near(mesh->material.sheenColorMapTransform.uvScale[0], 1.3f)
                       && near(mesh->material.sheenColorMapTransform.uvOffset[1], -0.1f)
                       && near(mesh->material.sheenRoughnessMapTransform.uvRotation, 0.55f)
                       && mesh->material.sheenRoughnessMapTransform.clampToEdge
                       && mesh->material.opacityMapChannel
                               == nuby::ScalarTextureChannel::Matte
                       && near(mesh->material.albedoMapValueRange.base, -0.1f)
                       && near(mesh->material.albedoMapValueRange.gain, 1.35f)
                       && near(mesh->material.normalMapValueRange.base, 0.1f)
                       && near(mesh->material.normalMapValueRange.gain, 0.8f)
                       && near(mesh->material.metallicRoughnessMapValueRange.base, 0.2f)
                       && near(mesh->material.metallicRoughnessMapValueRange.gain, 0.7f)
                       && near(mesh->material.metallicMapValueRange.base, -0.3f)
                       && near(mesh->material.metallicMapValueRange.gain, 1.2f)
                       && near(mesh->material.roughnessMapValueRange.base, 0.04f)
                       && near(mesh->material.roughnessMapValueRange.gain, 0.9f)
                       && near(mesh->material.occlusionMapValueRange.base, 0.0f)
                       && near(mesh->material.occlusionMapValueRange.gain, 1.4f)
                       && near(mesh->material.opacityMapValueRange.base, 0.05f)
                       && near(mesh->material.opacityMapValueRange.gain, 0.95f)
                       && near(mesh->material.emissiveMapValueRange.base, 0.08f)
                       && near(mesh->material.emissiveMapValueRange.gain, 1.25f),
               "per-map UV, sampler, channel and base/gain settings survive serialization");
        expect(near(mesh->orbit.angularSpeed, 0.7f), "orbit component is restored");
        expect(near(mesh->pulse.amplitude, 0.1f), "pulse component is restored");
        expect(mesh->animation.position.keyframes.size() == 2
                       && near(mesh->animation.position.keyframes[1].value[2], 6.0f),
               "animation tracks and keyframes are restored");
        expect(mesh->skeletalAnimation.enabled && !mesh->skeletalAnimation.playing
                       && mesh->skeletalAnimation.looping
                       && near(mesh->skeletalAnimation.playbackRate, 0.75f)
                       && near(mesh->skeletalAnimation.timeSeconds, 0.5f)
                       && near(mesh->skeletalAnimation.durationSeconds, 1.5f)
                       && mesh->skeletalAnimation.clipName == "walk"
                       && mesh->skeletalAnimation.transitionFromClip == "idle"
                       && near(mesh->skeletalAnimation.transitionFromTimeSeconds, 0.31875f)
                       && near(mesh->skeletalAnimation.transitionElapsedSeconds, 0.075f)
                       && mesh->skeletalAnimation.transitionFromLooping,
               "skeletal playback and crossfade state are restored from the scene");
        expect(mesh->skeletalLocomotion.enabled
                       && mesh->skeletalLocomotion.idleClip == "idle"
                       && mesh->skeletalLocomotion.walkClip == "walk"
                       && mesh->skeletalLocomotion.runClip == "run"
                       && mesh->skeletalLocomotion.jumpClip == "jump"
                       && near(mesh->skeletalLocomotion.walkSpeedThreshold, 0.25f)
                       && near(mesh->skeletalLocomotion.runSpeedThreshold, 2.5f)
                       && near(mesh->skeletalLocomotion.transitionDurationSeconds, 0.2f),
               "skeletal locomotion clips, thresholds and blend duration are restored");
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
                   && near(camera->characterController.coyoteTimeSeconds, 0.12f)
                   && near(camera->characterController.jumpBufferSeconds, 0.20f)
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

    std::string invalidOverride = valid.str();
    const std::string overrideRecord = "OVERRIDE_MTL_MATERIALS 1";
    const std::size_t overridePosition = invalidOverride.find(overrideRecord);
    expect(overridePosition != std::string::npos, "v6 scene stores imported-material policy");
    if (overridePosition != std::string::npos) {
        invalidOverride.replace(overridePosition, overrideRecord.size(),
                                "OVERRIDE_MTL_MATERIALS 2");
    }
    std::stringstream malformedOverride(invalidOverride);
    expect(!nuby::SceneSerializer::load(target, malformedOverride, error),
           "invalid imported-material override flags are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid material policy cannot partially replace target scene");

    std::string invalidSampler = valid.str();
    const std::size_t samplerStart = invalidSampler.find("MATERIAL_MAP_UV ");
    const std::size_t samplerEnd = invalidSampler.find('\n', samplerStart);
    expect(samplerStart != std::string::npos && samplerEnd != std::string::npos,
           "v8 scene stores per-map sampler settings");
    if (samplerStart != std::string::npos && samplerEnd != std::string::npos) {
        const std::size_t lastField = invalidSampler.rfind(' ', samplerEnd - 1);
        invalidSampler.replace(lastField + 1, samplerEnd - lastField - 1, "2");
    }
    std::stringstream malformedSampler(invalidSampler);
    expect(!nuby::SceneSerializer::load(target, malformedSampler, error),
           "invalid serialized edge-clamp flags are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid sampler state cannot partially replace target scene");

    std::string invalidOpacityChannelText = valid.str();
    const std::string matteChannel = "OPACITY_MAP_CHANNEL 3";
    const std::size_t opacityChannelPosition = invalidOpacityChannelText.find(matteChannel);
    expect(opacityChannelPosition != std::string::npos,
           "v10 scene stores the selected opacity-map channel");
    if (opacityChannelPosition != std::string::npos) {
        invalidOpacityChannelText.replace(opacityChannelPosition, matteChannel.size(),
                                          "OPACITY_MAP_CHANNEL 9");
    }
    std::stringstream malformedOpacityChannel(invalidOpacityChannelText);
    expect(!nuby::SceneSerializer::load(target, malformedOpacityChannel, error),
           "invalid serialized opacity channels are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid opacity channel cannot partially replace target scene");

    std::string invalidSheen = valid.str();
    expect(replaceRecordField(invalidSheen, "MATERIAL_SHEEN", 4, "1.1"),
           "v32 scene stores bounded sheen roughness");
    std::stringstream malformedSheen(invalidSheen);
    expect(!nuby::SceneSerializer::load(target, malformedSheen, error),
           "out-of-range serialized sheen roughness is rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid sheen data cannot partially replace target scene");

    std::string invalidBoxCollider = valid.str();
    const std::string boxDimensions = "BOX_COLLIDER 1 0.75";
    const std::size_t boxPosition = invalidBoxCollider.find(boxDimensions);
    expect(boxPosition != std::string::npos, "v15 scene stores box collider dimensions");
    if (boxPosition != std::string::npos) {
        invalidBoxCollider.replace(boxPosition + std::string("BOX_COLLIDER 1 ").size(),
                                   std::string("0.75").size(), "0");
    }
    std::stringstream malformedBox(invalidBoxCollider);
    expect(!nuby::SceneSerializer::load(target, malformedBox, error),
           "box colliders with zero half-extents are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid box collider data cannot partially replace the target scene");

    std::string invalidCollisionLayer = valid.str();
    expect(replaceRecordField(invalidCollisionLayer, "SPHERE_COLLIDER", 6, "0"),
           "v15 scene stores a sphere-collider category layer");
    std::stringstream malformedCollisionLayer(invalidCollisionLayer);
    expect(!nuby::SceneSerializer::load(target, malformedCollisionLayer, error),
           "zero collider layers are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid collider filters cannot partially replace the target scene");

    std::string invalidFloorLayer = valid.str();
    expect(replaceRecordField(invalidFloorLayer, "PHYSICS", 10, "0"),
           "v15 scene stores a world-floor collision layer");
    std::stringstream malformedFloorLayer(invalidFloorLayer);
    expect(!nuby::SceneSerializer::load(target, malformedFloorLayer, error),
           "zero world-floor layers are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid floor filter cannot partially replace the target scene");

    std::string invalidAmbient = valid.str();
    expect(replaceRecordField(invalidAmbient, "AMBIENT_SH", 1, "1001"),
           "v16 scene stores spherical-harmonic ambient-light coefficients");
    std::stringstream malformedAmbient(invalidAmbient);
    expect(!nuby::SceneSerializer::load(target, malformedAmbient, error),
           "excessive ambient-light coefficients are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid ambient lighting cannot partially replace the target scene");

    std::string invalidDirectionalLight = valid.str();
    expect(replaceRecordField(invalidDirectionalLight, "DIRECTIONAL_LIGHT", 8, "1.1"),
           "v23 scene stores bounded directional-light shadow strength");
    std::stringstream malformedDirectionalLight(invalidDirectionalLight);
    expect(!nuby::SceneSerializer::load(target, malformedDirectionalLight, error),
           "directional-light shadow strength above one is rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid directional lighting cannot partially replace the target scene");

    std::string invalidDirectionalColor = valid.str();
    expect(replaceRecordField(invalidDirectionalColor, "DIRECTIONAL_LIGHT", 4, "1.1"),
           "v23 scene stores normalized directional-light color");
    std::stringstream malformedDirectionalColor(invalidDirectionalColor);
    expect(!nuby::SceneSerializer::load(target, malformedDirectionalColor, error),
           "directional-light colors outside zero-to-one are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid directional color cannot partially replace the target scene");

    std::string invalidFogRange = valid.str();
    expect(replaceRecordField(invalidFogRange, "ATMOSPHERE", 5, "50"),
           "v24 scene stores an ordered fog range");
    std::stringstream malformedFogRange(invalidFogRange);
    expect(!nuby::SceneSerializer::load(target, malformedFogRange, error),
           "fog ranges with end not beyond start are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid atmosphere settings cannot partially replace the target scene");

    std::string invalidSkyRadius = valid.str();
    expect(replaceRecordField(invalidSkyRadius, "SKY", 15, "0.8"),
           "v25 scene stores a bounded sun angular radius");
    std::stringstream malformedSkyRadius(invalidSkyRadius);
    expect(!nuby::SceneSerializer::load(target, malformedSkyRadius, error),
           "procedural sun discs wider than the supported angular bound are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid sky settings cannot partially replace the target scene");

    std::string invalidSkyAmbient = valid.str();
    expect(replaceRecordField(invalidSkyAmbient, "SKY_AMBIENT", 1, "5.1"),
           "v26 scene stores bounded sky-ambient intensity");
    std::stringstream malformedSkyAmbient(invalidSkyAmbient);
    expect(!nuby::SceneSerializer::load(target, malformedSkyAmbient, error),
           "excessive sky-ambient intensity is rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid sky-ambient lighting cannot partially replace the target scene");

    std::string invalidTextureRotation = valid.str();
    expect(replaceRecordField(invalidTextureRotation, "MATERIAL_MAP_UV", 5,
                              "10001", 1),
           "v27 material UV records store bounded texture rotations");
    std::stringstream malformedTextureRotation(invalidTextureRotation);
    expect(!nuby::SceneSerializer::load(target, malformedTextureRotation, error),
           "out-of-range material UV rotations are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid UV rotations cannot partially replace a scene");

    std::string invalidUvSet = valid.str();
    expect(replaceRecordField(invalidUvSet, "MATERIAL_MAP_UV", 6,
                              "2", 1),
           "v29 material UV records store a bounded coordinate-set selector");
    std::stringstream malformedUvSet(invalidUvSet);
    expect(!nuby::SceneSerializer::load(target, malformedUvSet, error),
           "unsupported serialized UV coordinate sets are rejected");

    std::string invalidNormalMapScale = valid.str();
    expect(replaceRecordField(invalidNormalMapScale, "MATERIAL_PBR_EXT", 2,
                              "-0.1", 1),
           "v28 material extension records include bounded normal-map scale");
    std::stringstream malformedNormalMapScale(invalidNormalMapScale);
    expect(!nuby::SceneSerializer::load(target, malformedNormalMapScale, error),
           "negative normal-map scales are rejected from scene files");
    std::string invalidEmissiveStrength = valid.str();
    expect(replaceRecordField(invalidEmissiveStrength, "MATERIAL_PBR_EXT", 3,
                              "10001", 1),
           "v28 material extension records include bounded emissive strength");
    std::stringstream malformedEmissiveStrength(invalidEmissiveStrength);
    expect(!nuby::SceneSerializer::load(target, malformedEmissiveStrength, error),
           "excessive emissive strengths are rejected from scene files");
    std::string invalidClearcoatFactor = valid.str();
    expect(replaceRecordField(invalidClearcoatFactor, "MATERIAL_CLEARCOAT", 2,
                              "1.1", 1),
           "v30 material records persist bounded clearcoat factors");
    std::stringstream malformedClearcoatFactor(invalidClearcoatFactor);
    expect(!nuby::SceneSerializer::load(target, malformedClearcoatFactor, error),
           "clearcoat factors above one are rejected from scene files");
    std::string invalidSheenRoughness = valid.str();
    expect(replaceRecordField(invalidSheenRoughness, "MATERIAL_SHEEN", 4,
                              "1.1", 1),
           "v32 material records persist bounded sheen roughness");
    std::stringstream malformedSheenRoughness(invalidSheenRoughness);
    expect(!nuby::SceneSerializer::load(target, malformedSheenRoughness, error),
           "sheen roughness above one is rejected from scene files");
    std::string invalidSpecularFactor = valid.str();
    expect(replaceRecordField(invalidSpecularFactor, "MATERIAL_SPECULAR", 1,
                              "1.1", 1),
           "v31 material records persist bounded specular strength");
    std::stringstream malformedSpecularFactor(invalidSpecularFactor);
    expect(!nuby::SceneSerializer::load(target, malformedSpecularFactor, error),
           "specular strength above one is rejected from scene files");
    std::string invalidIor = valid.str();
    expect(replaceRecordField(invalidIor, "MATERIAL_CLEARCOAT", 1,
                              "0.9", 1),
           "v30 material records persist dielectric IOR");
    std::stringstream malformedIor(invalidIor);
    expect(!nuby::SceneSerializer::load(target, malformedIor, error),
           "out-of-range IOR values are rejected from scene files");
    std::string invalidUnlitFlag = valid.str();
    expect(replaceRecordField(invalidUnlitFlag, "MATERIAL_PBR_EXT", 1,
                              "2", 1),
           "v28 unlit state is encoded as a strict boolean");
    std::stringstream malformedUnlitFlag(invalidUnlitFlag);
    expect(!nuby::SceneSerializer::load(target, malformedUnlitFlag, error),
           "invalid serialized unlit flags are rejected");

    std::string invalidSkeletalPlayback = valid.str();
    expect(replaceRecordField(invalidSkeletalPlayback, "SKELETAL_PLAYER", 6, "0", 1),
           "v17 scene stores skeletal playback duration");
    std::stringstream malformedSkeletalPlayback(invalidSkeletalPlayback);
    expect(!nuby::SceneSerializer::load(target, malformedSkeletalPlayback, error),
           "skeletal playback time beyond clip duration is rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid skeletal playback state cannot partially replace the target scene");

    std::string invalidLocomotion = valid.str();
    expect(replaceRecordField(invalidLocomotion, "SKELETAL_LOCOMOTION", 7, "0.1", 1),
           "v18 scene stores configurable locomotion thresholds");
    std::stringstream malformedLocomotion(invalidLocomotion);
    expect(!nuby::SceneSerializer::load(target, malformedLocomotion, error),
           "run threshold not greater than walk threshold is rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid locomotion settings cannot partially replace the target scene");

    std::string invalidBlendDuration = valid.str();
    expect(replaceRecordField(invalidBlendDuration, "SKELETAL_LOCOMOTION", 8, "-0.1", 1),
           "v20 scene stores a bounded locomotion crossfade duration");
    std::stringstream malformedBlendDuration(invalidBlendDuration);
    expect(!nuby::SceneSerializer::load(target, malformedBlendDuration, error),
           "negative locomotion crossfade durations are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid crossfade settings cannot partially replace the target scene");

    std::string invalidBlendSource = valid.str();
    expect(replaceRecordField(invalidBlendSource, "SKELETAL_PLAYER", 9, "-0.1", 1),
           "v20 scene stores the source clip's playback time");
    std::stringstream malformedBlendSource(invalidBlendSource);
    expect(!nuby::SceneSerializer::load(target, malformedBlendSource, error),
           "negative crossfade source times are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid crossfade source data cannot partially replace the target scene");

    std::string invalidAudioGain = valid.str();
    expect(replaceRecordField(invalidAudioGain, "AUDIO_SOURCE", 4, "4.1", 2),
           "v22 scene stores bounded audio-source gain");
    std::stringstream malformedAudioGain(invalidAudioGain);
    expect(!nuby::SceneSerializer::load(target, malformedAudioGain, error),
           "audio-source gains above the supported bound are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "invalid audio settings cannot partially replace the target scene");

    std::string invalidAudioPath = valid.str();
    expect(replaceRecordField(invalidAudioPath, "AUDIO_SOURCE", 2,
                              "\"../escape.wav\"", 2),
           "v22 scene stores a confined audio asset path");
    std::stringstream malformedAudioPath(invalidAudioPath);
    expect(!nuby::SceneSerializer::load(target, malformedAudioPath, error),
           "audio-source paths escaping the asset root are rejected");
    expect(target.entities().size() == 1 && target.find(keepId) != nullptr,
           "unsafe audio paths cannot partially replace the target scene");

    std::stringstream legacyV31(downgradeTextureSchema(valid.str(), 31, 8));
    expect(nuby::SceneSerializer::load(target, legacyV31, error),
           "version-thirty-one scenes remain readable before sheen persistence");
    const nuby::Entity* legacyV31Mesh = target.find(2);
    expect(legacyV31Mesh != nullptr
                   && near(legacyV31Mesh->material.sheenColorFactor[0], 0.0f)
                   && near(legacyV31Mesh->material.sheenColorFactor[2], 0.0f)
                   && near(legacyV31Mesh->material.sheenRoughness, 0.0f),
           "legacy v31 materials migrate to disabled sheen defaults");

    std::stringstream legacyV30(downgradeTextureSchema(valid.str(), 30, 8));
    expect(nuby::SceneSerializer::load(target, legacyV30, error),
           "version-thirty scenes remain readable before specular persistence");
    const nuby::Entity* legacyV30Mesh = target.find(2);
    expect(legacyV30Mesh != nullptr
                   && near(legacyV30Mesh->material.specularFactor, 1.0f)
                   && near(legacyV30Mesh->material.specularColorFactor[0], 1.0f)
                   && near(legacyV30Mesh->material.specularColorFactor[2], 1.0f),
           "legacy v30 materials migrate to default KHR_materials_specular values");

    std::stringstream legacyV29(downgradeTextureSchema(valid.str(), 29, 8));
    expect(nuby::SceneSerializer::load(target, legacyV29, error),
           "version-twenty-nine scenes remain readable before clearcoat persistence");
    const nuby::Entity* legacyV29Mesh = target.find(2);
    expect(legacyV29Mesh != nullptr
                   && near(legacyV29Mesh->material.indexOfRefraction, 1.5f)
                   && near(legacyV29Mesh->material.clearcoatFactor, 0.0f)
                   && near(legacyV29Mesh->material.clearcoatRoughness, 0.0f),
           "legacy v29 materials migrate to default IOR and no clearcoat");

    std::stringstream legacyV28(downgradeTextureSchema(valid.str(), 28, 8));
    expect(nuby::SceneSerializer::load(target, legacyV28, error),
           "version-twenty-eight scenes remain readable before UV-set selection");
    const nuby::Entity* legacyV28Mesh = target.find(2);
    expect(legacyV28Mesh != nullptr
                   && legacyV28Mesh->material.albedoMapTransform.uvSet == 0U,
           "legacy scene texture maps default to TEXCOORD_0");

    std::stringstream legacyV27(downgradeTextureSchema(valid.str(), 27, 8));
    expect(nuby::SceneSerializer::load(target, legacyV27, error),
           "version-twenty-seven scenes remain readable before material extensions");
    const nuby::Entity* legacyV27Mesh = target.find(2);
    expect(legacyV27Mesh != nullptr && !legacyV27Mesh->material.unlit
                   && near(legacyV27Mesh->material.normalMapScale, 1.0f)
                   && near(legacyV27Mesh->material.emissiveStrength, 1.0f),
           "legacy scenes default to lit materials, unit normal scale and unit emission strength");

    std::stringstream legacyV26(downgradeTextureSchema(valid.str(), 26, 8));
    expect(nuby::SceneSerializer::load(target, legacyV26, error),
           "version-twenty-six scenes remain readable before per-map UV rotation");
    const nuby::Entity* legacyV26Mesh = target.find(2);
    expect(legacyV26Mesh != nullptr
                   && near(legacyV26Mesh->material.albedoMapTransform.uvRotation, 0.0f)
                   && near(legacyV26Mesh->material.opacityMapTransform.uvRotation, 0.0f),
           "version-twenty-six texture transforms receive zero-rotation defaults");

    std::stringstream legacyV25(downgradeTextureSchema(valid.str(), 25, 8));
    expect(nuby::SceneSerializer::load(target, legacyV25, error),
           "version-twenty-five scenes remain readable before sky-ambient fill");
    expect(target.sky().enabled && near(target.sky().ambientStrength, 0.0f),
           "version-twenty-five sky does not add extra ambient light by default");

    std::stringstream legacyV24(downgradeTextureSchema(valid.str(), 24, 8));
    expect(nuby::SceneSerializer::load(target, legacyV24, error),
           "version-twenty-four scenes remain readable before procedural sky settings");
    expect(!target.sky().enabled && target.sky().sunDiskEnabled
                   && near(target.sky().sunAngularRadius, 0.018f),
           "version-twenty-four scenes keep the procedural sky disabled by default");

    std::stringstream legacyV23(downgradeTextureSchema(valid.str(), 23, 8));
    expect(nuby::SceneSerializer::load(target, legacyV23, error),
           "version-twenty-three scenes remain readable before atmosphere controls");
    expect(!target.atmosphere().fogEnabled
                   && near(target.atmosphere().fogStart, 24.0f)
                   && near(target.atmosphere().fogEnd, 72.0f),
           "version-twenty-three scenes keep fog disabled with safe defaults");

    std::stringstream legacyV22(downgradeTextureSchema(valid.str(), 22, 8));
    expect(nuby::SceneSerializer::load(target, legacyV22, error),
           "version-twenty-two scenes remain readable before directional-light controls");
    expect(near(target.directionalLighting().direction[0], -0.45f)
                   && near(target.directionalLighting().direction[1], -0.82f)
                   && near(target.directionalLighting().intensity, 1.0f)
                   && near(target.directionalLighting().shadowStrength, 0.82f),
           "version-twenty-two scenes receive renderer-compatible directional defaults");

    std::stringstream legacyV21(downgradeTextureSchema(valid.str(), 21, 8));
    expect(nuby::SceneSerializer::load(target, legacyV21, error),
           "version-twenty-one scenes remain readable before audio emitters");
    expect(target.activeCamera() != nullptr
                   && target.activeCamera()->orbitCamera.collisionEnabled
                   && !target.activeCamera()->audioSource.enabled
                   && target.activeCamera()->audioSource.assetPath.empty(),
           "version-twenty-one camera data is preserved and audio defaults stay disabled");

    std::stringstream legacyV20(downgradeTextureSchema(valid.str(), 20, 8));
    expect(nuby::SceneSerializer::load(target, legacyV20, error),
           "version-twenty scenes remain readable without spring-arm data");
    expect(target.activeCamera() != nullptr
                   && !target.activeCamera()->orbitCamera.collisionEnabled
                   && near(target.activeCamera()->orbitCamera.collisionRadius, 0.22f),
           "version-twenty cameras receive compatible collision defaults");

    std::stringstream legacyV19(downgradeTextureSchema(valid.str(), 19, 8));
    expect(nuby::SceneSerializer::load(target, legacyV19, error),
           "version-nineteen scenes remain readable before jump-assist settings");
    expect(target.find(2) != nullptr
                   && near(target.find(2)->characterController.coyoteTimeSeconds, 0.10f)
                   && near(target.find(2)->characterController.jumpBufferSeconds, 0.16f),
           "version-nineteen character controllers receive safe jump-assist defaults");

    std::stringstream legacyV18(downgradeTextureSchema(valid.str(), 18, 8));
    expect(nuby::SceneSerializer::load(target, legacyV18, error),
           "version-eighteen scenes remain readable after animation crossfades");
    expect(target.find(2) != nullptr
                   && target.find(2)->skeletalAnimation.transitionFromClip.empty()
                   && near(target.find(2)->skeletalLocomotion.transitionDurationSeconds, 0.18f),
           "version-eighteen scenes receive safe default crossfade state");

    std::stringstream legacyV17(downgradeTextureSchema(valid.str(), 17, 8));
    expect(nuby::SceneSerializer::load(target, legacyV17, error),
           "version-seventeen scenes remain readable after locomotion support");
    expect(target.find(2) != nullptr && !target.find(2)->skeletalLocomotion.enabled,
           "version-seventeen scenes receive default skeletal locomotion state");

    std::stringstream legacyV16(downgradeTextureSchema(valid.str(), 16, 8));
    expect(nuby::SceneSerializer::load(target, legacyV16, error),
           "version-sixteen scenes remain readable after skeletal playback state");
    expect(target.find(2) != nullptr && !target.find(2)->skeletalAnimation.enabled
                   && target.find(2)->skeletalAnimation.clipName.empty(),
           "version-sixteen scenes receive default skeletal playback state");

    std::stringstream legacyV15(downgradeTextureSchema(valid.str(), 15, 8));
    expect(nuby::SceneSerializer::load(target, legacyV15, error),
           "version-fifteen scenes remain readable after ambient-light support");
    expect(near(target.ambientLighting().sphericalHarmonics[0], 0.52287f)
                   && near(target.ambientLighting().sphericalHarmonics[1], 0.66467f),
           "legacy scenes receive the backward-compatible default environment light");
    target.clear();
    expect(target.create("keep", nuby::Primitive::Sphere).id == keepId,
           "malformed-file checks restore their sentinel scene after legacy load");

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
    const std::string versionEightText = downgradeTextureSchema(valid.str(), 8, 6);
    std::stringstream legacyV8(versionEightText);
    expect(nuby::SceneSerializer::load(target, legacyV8, error),
           "version-eight scenes remain readable after adding material opacity");
    const nuby::Entity* legacyV8Mesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyV8Mesh = &entity;
    }
    expect(legacyV8Mesh != nullptr
                   && near(legacyV8Mesh->material.opacity, 1.0f)
                   && near(legacyV8Mesh->material.albedoMapTransform.uvScale[0], 0.5f)
                   && legacyV8Mesh->material.albedoMapTransform.clampToEdge
                   && legacyV8Mesh->material.opacityTexture.empty(),
           "version-eight UV and edge-clamp data load while newer opacity-map fields default");

    const std::string versionNineText = downgradeTextureSchema(valid.str(), 9, 6);
    std::stringstream legacyV9(versionNineText);
    expect(nuby::SceneSerializer::load(target, legacyV9, error),
           "version-nine scalar opacity remains compatible after adding opacity maps");
    const nuby::Entity* legacyV9Mesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyV9Mesh = &entity;
    }
    expect(legacyV9Mesh != nullptr
                   && near(legacyV9Mesh->material.opacity, 0.42f)
                   && legacyV9Mesh->material.opacityTexture.empty()
                   && legacyV9Mesh->material.opacityMapChannel
                           == nuby::ScalarTextureChannel::Luminance,
           "version-nine opacity loads and opacity-map additions receive safe defaults");

    const std::string versionTenText = downgradeTextureSchema(valid.str(), 10, 6);
    std::stringstream legacyV10(versionTenText);
    expect(nuby::SceneSerializer::load(target, legacyV10, error),
           "version-ten opacity maps remain readable after adding per-map value ranges");
    const nuby::Entity* legacyV10Mesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyV10Mesh = &entity;
    }
    expect(legacyV10Mesh != nullptr
                   && near(legacyV10Mesh->material.opacity, 0.42f)
                   && legacyV10Mesh->material.opacityTexture == "textures/hero opacity.tga"
                   && near(legacyV10Mesh->material.opacityMapValueRange.base, 0.0f)
                   && near(legacyV10Mesh->material.opacityMapValueRange.gain, 1.0f),
           "version-ten map data loads and newly added base/gain ranges default to identity");

    const std::string versionThirteenText = downgradeTextureSchema(valid.str(), 13, 6);
    std::stringstream legacyV13(versionThirteenText);
    expect(nuby::SceneSerializer::load(target, legacyV13, error),
           "version-thirteen scenes load with default box collider components");
    const nuby::Entity* legacyV13Mesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyV13Mesh = &entity;
    }
    expect(legacyV13Mesh != nullptr && !legacyV13Mesh->boxCollider.enabled,
           "version-thirteen scenes receive disabled default box colliders");

    const std::string versionFourteenText = downgradeTextureSchema(valid.str(), 14, 6);
    std::stringstream legacyV14(versionFourteenText);
    expect(nuby::SceneSerializer::load(target, legacyV14, error),
           "version-fourteen OBB scenes remain readable after adding collision filters");
    const nuby::Entity* legacyV14Root = target.find(1);
    const nuby::Entity* legacyV14Mesh = target.find(2);
    expect(legacyV14Root != nullptr
                   && legacyV14Root->sphereCollider.collisionLayer == 1U
                   && legacyV14Root->sphereCollider.collisionMask == 0xFFFFFFFFU
                   && legacyV14Mesh != nullptr
                   && legacyV14Mesh->boxCollider.collisionLayer == 1U
                   && legacyV14Mesh->boxCollider.collisionMask == 0xFFFFFFFFU
                   && target.physicsSettings().floorCollisionLayer == 1U
                   && target.physicsSettings().floorCollisionMask == 0xFFFFFFFFU,
           "version-fourteen colliders and floor receive permissive filter defaults");

    const std::string versionTwelveText = downgradeTextureSchema(valid.str(), 12, 6);
    std::stringstream legacyV12(versionTwelveText);
    expect(nuby::SceneSerializer::load(target, legacyV12, error),
           "version-twelve scenes load without the newly added angular rigidbody state");
    const nuby::Entity* legacyV12Root = target.find(1);
    expect(legacyV12Root != nullptr
                   && near(legacyV12Root->rigidbody.angularVelocity[0], 0.0f)
                   && near(legacyV12Root->rigidbody.angularDamping, 0.05f),
           "version-twelve rigidbodies receive safe angular velocity and damping defaults");

    const std::string versionElevenText = downgradeTextureSchema(valid.str(), 11, 6);
    std::stringstream legacyV11(versionElevenText);
    expect(nuby::SceneSerializer::load(target, legacyV11, error),
           "version-eleven base/gain scenes remain compatible after emissive-map support");
    const nuby::Entity* legacyV11Mesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyV11Mesh = &entity;
    }
    expect(legacyV11Mesh != nullptr
                   && legacyV11Mesh->material.emissiveTexture.empty()
                   && near(legacyV11Mesh->material.emissiveMapValueRange.base, 0.0f)
                   && near(legacyV11Mesh->material.emissiveMapValueRange.gain, 1.0f)
                   && near(legacyV11Mesh->material.emissiveMapTransform.uvScale[0], 1.0f),
           "version-eleven scenes receive empty emissive maps with identity defaults");

    const std::string versionSevenText = downgradeTextureSchema(valid.str(), 7, 6);
    std::stringstream legacyV7(versionSevenText);
    expect(nuby::SceneSerializer::load(target, legacyV7, error),
           "version-seven scenes remain readable after adding sampler settings");
    const nuby::Entity* legacyV7Mesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyV7Mesh = &entity;
    }
    expect(legacyV7Mesh != nullptr
                   && near(legacyV7Mesh->material.albedoMapTransform.uvScale[0], 0.5f)
                   && !legacyV7Mesh->material.albedoMapTransform.clampToEdge,
           "version-seven UV transforms load and new edge-clamp flags default off");

    const std::string versionSixText = downgradeTextureSchema(valid.str(), 6, 6);
    std::stringstream legacyV6(versionSixText);
    expect(nuby::SceneSerializer::load(target, legacyV6, error),
           "version-six scenes remain readable after adding per-map UV transforms");
    const nuby::Entity* legacyMesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyMesh = &entity;
    }
    expect(legacyMesh != nullptr && legacyMesh->overrideMtlMaterials
                   && legacyMesh->material.metallicTexture == "textures/hero metallic.tga"
                   && near(legacyMesh->material.albedoMapTransform.uvScale[0], 1.0f)
                   && near(legacyMesh->material.albedoMapTransform.uvOffset[0], 0.0f),
           "version-six material policy is restored and new UV transforms default to identity");

    const std::string versionFiveText = downgradeTextureSchema(valid.str(), 5, 6);
    std::stringstream legacyV5(versionFiveText);
    expect(nuby::SceneSerializer::load(target, legacyV5, error),
           "version-five scenes remain readable after adding explicit imported-material overrides");
    legacyMesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyMesh = &entity;
    }
    expect(legacyMesh != nullptr && !legacyMesh->overrideMtlMaterials
                   && legacyMesh->material.metallicTexture == "textures/hero metallic.tga"
                   && legacyMesh->material.roughnessTexture == "textures/hero roughness.tga",
           "version-five scalar PBR maps load and material override defaults off");

    const std::string versionFourText = downgradeTextureSchema(valid.str(), 4, 4);
    std::stringstream legacyV4(versionFourText);
    expect(nuby::SceneSerializer::load(target, legacyV4, error),
           "version-four scenes remain readable after adding scalar PBR texture maps");
    legacyMesh = nullptr;
    for (const nuby::Entity& entity : target.entities()) {
        if (entity.name == "mesh") legacyMesh = &entity;
    }
    expect(legacyMesh != nullptr
                   && legacyMesh->material.albedoTexture == "textures/hero diffuse.tga"
                   && legacyMesh->material.occlusionTexture == "textures/hero ao.tga"
                   && legacyMesh->material.metallicTexture.empty()
                   && legacyMesh->material.roughnessTexture.empty()
                   && !legacyMesh->overrideMtlMaterials,
           "legacy version-four maps load and v5/v6 material fields default safely");

    const std::string versionThreeText = downgradeTextureSchema(valid.str(), 3, 0);
    std::stringstream legacyV3(versionThreeText);
    expect(nuby::SceneSerializer::load(target, legacyV3, error),
           "version-three scenes remain readable after PBR texture references are added");
    legacyMesh = nullptr;
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

    nuby::Scene invalidAudioSource;
    nuby::Entity& invalidAudio = invalidAudioSource.create("invalid audio", nuby::Primitive::None);
    invalidAudio.audioSource.enabled = true;
    invalidAudio.audioSource.assetPath = "../outside.wav";
    std::stringstream invalidAudioOutput;
    expect(!nuby::SceneSerializer::save(invalidAudioSource, invalidAudioOutput, error),
           "scene serialization rejects audio source paths outside the asset root");

    nuby::Scene invalidPhysics;
    invalidPhysics.physicsSettings().fixedTimeStep = 0.0f;
    std::stringstream invalidPhysicsOutput;
    expect(!nuby::SceneSerializer::save(invalidPhysics, invalidPhysicsOutput, error),
           "scene serialization rejects invalid physics settings");

    nuby::Scene invalidAngularDamping;
    invalidAngularDamping.create("invalid angular damping", nuby::Primitive::Sphere)
            .rigidbody.angularDamping = -0.1f;
    std::stringstream invalidAngularDampingOutput;
    expect(!nuby::SceneSerializer::save(
                   invalidAngularDamping, invalidAngularDampingOutput, error),
           "scene serialization rejects negative angular damping");

    nuby::Scene invalidUvTransform;
    invalidUvTransform.create("invalid UV transform", nuby::Primitive::ImportedMesh)
            .material.normalMapTransform.uvScale[0] = 1001.0f;
    std::stringstream invalidUvOutput;
    expect(!nuby::SceneSerializer::save(invalidUvTransform, invalidUvOutput, error),
           "scene serialization rejects unbounded material UV transforms");

    nuby::Scene invalidOpacity;
    invalidOpacity.create("invalid opacity", nuby::Primitive::Sphere).material.opacity = 1.1f;
    std::stringstream invalidOpacityOutput;
    expect(!nuby::SceneSerializer::save(invalidOpacity, invalidOpacityOutput, error),
           "scene serialization rejects opacity outside the normalized range");

    nuby::Scene invalidOpacityMap;
    invalidOpacityMap.create("invalid opacity map", nuby::Primitive::ImportedMesh)
            .material.opacityTexture = "../outside.tga";
    std::stringstream invalidOpacityMapOutput;
    expect(!nuby::SceneSerializer::save(invalidOpacityMap, invalidOpacityMapOutput, error),
           "scene serialization rejects opacity map paths outside the asset root");

    nuby::Scene invalidOpacityChannel;
    invalidOpacityChannel.create("invalid opacity channel", nuby::Primitive::ImportedMesh)
            .material.opacityMapChannel = static_cast<nuby::ScalarTextureChannel>(9);
    std::stringstream invalidOpacityChannelOutput;
    expect(!nuby::SceneSerializer::save(
                   invalidOpacityChannel, invalidOpacityChannelOutput, error),
           "scene serialization rejects unknown opacity-map channel values");

    nuby::Scene invalidMapValueRange;
    invalidMapValueRange.create("invalid map range", nuby::Primitive::ImportedMesh)
            .material.albedoMapValueRange.gain = 16.1f;
    std::stringstream invalidMapValueRangeOutput;
    expect(!nuby::SceneSerializer::save(
                   invalidMapValueRange, invalidMapValueRangeOutput, error),
           "scene serialization rejects unbounded texture map base/gain values");
}

void testAdventureZonePillarsBlockActorsWithoutRetractingCamera() {
    for (int zone = 1; zone <= 3; ++zone) {
        const std::string path = "app/src/main/assets/scenes/zones/ruins_0"
                + std::to_string(zone) + ".scene";
        std::ifstream input(path);
        nuby::Scene scene;
        std::string error;
        expect(input.good() && nuby::SceneSerializer::load(scene, input, error),
               "packaged adventure zone scene remains loadable");
        std::size_t pillars = 0U;
        std::uint32_t firstPillarId = 0U;
        for (const nuby::Entity& entity : scene.entities()) {
            if (entity.primitive != nuby::Primitive::ImportedMesh) continue;
            ++pillars;
            if (firstPillarId == 0U) firstPillarId = entity.id;
            expect(entity.boxCollider.enabled
                           && entity.boxCollider.collisionLayer == 4U
                           && entity.boxCollider.collisionMask == 2U,
                   "ruin pillar collider is solid to actors but filtered from camera queries");
        }
        expect(pillars == 4U, "each zone keeps its four authored pillar meshes");
        if (zone == 1 && firstPillarId != 0U) {
            const nuby::Entity* pillar = scene.find(firstPillarId);
            const std::array<float, 3> start = pillar->transform.position;
            nuby::SphereCastHit castHit;
            const std::array<float, 3> castOrigin{start[0], 0.0f, start[2] - 2.0f};
            expect(!scene.sphereCast(castOrigin, {0.0f, 0.0f, 1.0f}, 0.24f, 4.0f,
                                     castHit, 4U, 0xFFFFFFFDU),
                   "camera-layer sweep passes pillars without collapsing the zoom arm");
            expect(scene.sphereCast(castOrigin, {0.0f, 0.0f, 1.0f}, 0.24f, 4.0f,
                                    castHit, 2U, 0xFFFFFFFFU),
                   "actor-layer sweep still detects solid pillars");
            scene.physicsSettings().gravity = {0.0f, 0.0f, 0.0f};
            nuby::Entity& actor = scene.create("layer-2 actor probe", nuby::Primitive::Sphere);
            const std::uint32_t actorId = actor.id;
            actor.transform.position = start;
            actor.rigidbody.enabled = true;
            actor.rigidbody.linearDamping = 0.0f;
            actor.sphereCollider.enabled = true;
            actor.sphereCollider.radius = 0.25f;
            actor.sphereCollider.collisionLayer = 2U;
            actor.sphereCollider.collisionMask = 0xFFFFFFFFU;
            scene.update(1.0f / 60.0f);
            const nuby::Entity* displaced = scene.find(actorId);
            expect(displaced != nullptr
                           && std::fabs(displaced->transform.position[0] - start[0])
                                      + std::fabs(displaced->transform.position[1] - start[1])
                                      + std::fabs(displaced->transform.position[2] - start[2])
                                      > 0.01f,
                   "pillar collider physically blocks layer-2 actors that enter its bounds");
        }
    }
}

void testAppendingSceneFragmentsRemapsHierarchyTransactionally() {
    nuby::Scene destination;
    const std::uint32_t existingId = destination.create(
            "existing", nuby::Primitive::Sphere).id;
    nuby::Scene fragment;
    const std::uint32_t sourceRootId = fragment.create(
            "zone root", nuby::Primitive::None).id;
    const std::uint32_t sourceChildId = fragment.create(
            "zone child", nuby::Primitive::Sphere).id;
    expect(fragment.setParent(sourceChildId, sourceRootId),
           "test fragment forms a valid parent-child hierarchy");

    std::vector<std::uint32_t> appendedIds;
    expect(destination.appendEntities(fragment, appendedIds)
                   && appendedIds.size() == 2,
           "valid scene fragment appends all entities");
    const nuby::Entity* appendedRoot = destination.find(appendedIds[0]);
    const nuby::Entity* appendedChild = destination.find(appendedIds[1]);
    expect(appendedIds[0] != sourceRootId && appendedIds[1] != sourceChildId
                   && appendedIds[0] != existingId && appendedIds[1] != existingId,
           "fragment IDs are remapped to avoid collisions with the host scene");
    expect(appendedRoot != nullptr && appendedRoot->name == "zone root"
                   && appendedChild != nullptr && appendedChild->name == "zone child"
                   && appendedChild->parentId == appendedIds[0],
           "fragment parent references are remapped with their entities");

    nuby::Scene cameraFragment;
    nuby::Entity& camera = cameraFragment.create("fragment camera", nuby::Primitive::None);
    camera.camera.enabled = true;
    expect(cameraFragment.setActiveCamera(camera.id),
           "test camera fragment has its own active camera");
    const std::size_t oldEntityCount = destination.entities().size();
    expect(!destination.appendEntities(cameraFragment, appendedIds)
                   && appendedIds.empty() && destination.entities().size() == oldEntityCount
                   && destination.find(existingId) != nullptr,
           "camera-bearing fragment is rejected without partially changing the host scene");
}
}  // namespace

int main() {
    testRoundTripAllSceneData();
    testAdventureZonePillarsBlockActorsWithoutRetractingCamera();
    testAppendingSceneFragmentsRemapsHierarchyTransactionally();
    testMalformedFilesDoNotPartiallyReplaceScene();
    if (failures != 0) {
        std::cerr << failures << " scene IO test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All scene IO tests passed\n";
    return EXIT_SUCCESS;
}
