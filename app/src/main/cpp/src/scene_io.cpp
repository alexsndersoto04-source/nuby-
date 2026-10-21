#include "nuby/scene_io.hpp"

#include "nuby/scene.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <istream>
#include <limits>
#include <locale>
#include <ostream>
#include <unordered_set>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumEntities = 4096;
constexpr std::size_t kMaximumKeyframesPerTrack = 65536;
constexpr std::size_t kMaximumTextLength = 4096;

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool validClipName(const std::string& name) {
    if (name.size() > 256) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return character >= 0x20U && character != 0x7FU;
    });
}

bool validTexturePath(const std::string& path) {
    if (path.empty()) return true;
    if (path.size() > kMaximumTextLength || path.front() == '/' || path.front() == '\\'
            || path.find(':') != std::string::npos || path.find('\\') != std::string::npos) {
        return false;
    }
    std::size_t segmentStart = 0;
    while (segmentStart < path.size()) {
        const std::size_t separator = path.find('/', segmentStart);
        const std::size_t segmentEnd = separator == std::string::npos ? path.size() : separator;
        if (segmentEnd == segmentStart) return false;
        const std::string segment = path.substr(segmentStart, segmentEnd - segmentStart);
        if (segment == "." || segment == "..") return false;
        for (unsigned char character : segment) {
            if (character < 0x20U || character == 0x7FU) return false;
        }
        if (separator == std::string::npos) break;
        segmentStart = separator + 1;
    }
    return true;
}

bool readTag(std::istream& input, const char* expected, std::string& error) {
    std::string actual;
    if (!(input >> actual) || actual != expected) {
        return fail(error, "Unexpected or missing scene record");
    }
    return true;
}

bool readBool(std::istream& input, bool& value) {
    int encoded = -1;
    if (!(input >> encoded) || (encoded != 0 && encoded != 1)) return false;
    value = encoded != 0;
    return true;
}

bool readUint32(std::istream& input, std::uint32_t& value) {
    std::uint64_t encoded = 0;
    if (!(input >> encoded) || encoded > std::numeric_limits<std::uint32_t>::max()) return false;
    value = static_cast<std::uint32_t>(encoded);
    return true;
}

template <std::size_t N>
bool readFloatArray(std::istream& input, std::array<float, N>& values) {
    for (float& value : values) {
        if (!(input >> value) || !std::isfinite(value)) return false;
    }
    return true;
}

template <std::size_t N>
void writeFloatArray(std::ostream& output, const std::array<float, N>& values) {
    for (float value : values) output << ' ' << value;
}

bool validTextureMapTransform(const TextureMapTransform& transform) {
    for (float scale : transform.uvScale) {
        if (!std::isfinite(scale) || std::fabs(scale) > 1000.0f) return false;
    }
    for (float offset : transform.uvOffset) {
        if (!std::isfinite(offset) || std::fabs(offset) > 10000.0f) return false;
    }
    return std::isfinite(transform.uvRotation)
            && std::fabs(transform.uvRotation) <= 10000.0f
            && transform.uvSet <= 1U;
}

bool validTextureMapValueRange(const TextureMapValueRange& valueRange) {
    return std::isfinite(valueRange.base) && std::isfinite(valueRange.gain)
            && std::fabs(valueRange.base) <= 16.0f
            && std::fabs(valueRange.gain) <= 16.0f;
}

bool readTextureMapValueRange(std::istream& input, TextureMapValueRange& valueRange) {
    return (input >> valueRange.base >> valueRange.gain)
            && validTextureMapValueRange(valueRange);
}

void writeTextureMapValueRange(std::ostream& output,
                               const TextureMapValueRange& valueRange) {
    output << ' ' << valueRange.base << ' ' << valueRange.gain;
}

bool readTextureMapTransform(std::istream& input, TextureMapTransform& transform,
                             bool hasSamplerSettings, bool hasRotation, bool hasUvSet) {
    if (!readFloatArray(input, transform.uvScale)
            || !readFloatArray(input, transform.uvOffset)) return false;
    if (hasRotation && !(input >> transform.uvRotation)) return false;
    if (hasSamplerSettings && !readBool(input, transform.clampToEdge)) return false;
    if (hasUvSet) {
        unsigned int uvSet = 0U;
        if (!(input >> uvSet) || uvSet > 1U) return false;
        transform.uvSet = static_cast<std::uint8_t>(uvSet);
    }
    return validTextureMapTransform(transform);
}

void writeTextureMapTransform(std::ostream& output,
                              const TextureMapTransform& transform) {
    writeFloatArray(output, transform.uvScale);
    writeFloatArray(output, transform.uvOffset);
    output << ' ' << transform.uvRotation
           << ' ' << (transform.clampToEdge ? 1 : 0)
           << ' ' << static_cast<unsigned int>(transform.uvSet);
}

bool validTrack(const Vector3Track& track) {
    if (track.keyframes.size() > kMaximumKeyframesPerTrack) return false;
    float previousTime = -1.0f;
    for (const Vector3Keyframe& keyframe : track.keyframes) {
        if (!std::isfinite(keyframe.timeSeconds) || keyframe.timeSeconds < 0.0f
                || keyframe.timeSeconds <= previousTime
                || !std::isfinite(keyframe.value[0])
                || !std::isfinite(keyframe.value[1])
                || !std::isfinite(keyframe.value[2])) {
            return false;
        }
        previousTime = keyframe.timeSeconds;
    }
    return true;
}

bool validEntity(const Entity& entity) {
    if (entity.id == 0 || entity.name.size() > kMaximumTextLength
            || entity.meshAsset.size() > kMaximumTextLength
            || !validClipName(entity.skeletalAnimation.clipName)
            || !validClipName(entity.skeletalAnimation.transitionFromClip)
            || !validClipName(entity.skeletalLocomotion.idleClip)
            || !validClipName(entity.skeletalLocomotion.walkClip)
            || !validClipName(entity.skeletalLocomotion.runClip)
            || !validClipName(entity.skeletalLocomotion.jumpClip)
            || !validTexturePath(entity.material.albedoTexture)
            || !validTexturePath(entity.material.normalTexture)
            || !validTexturePath(entity.material.metallicRoughnessTexture)
            || !validTexturePath(entity.material.metallicTexture)
            || !validTexturePath(entity.material.roughnessTexture)
            || !validTexturePath(entity.material.occlusionTexture)
            || !validTexturePath(entity.material.opacityTexture)
            || !validTexturePath(entity.material.emissiveTexture)
            || !validTexturePath(entity.material.clearcoatTexture)
            || !validTexturePath(entity.material.clearcoatRoughnessTexture)
            || !validTexturePath(entity.material.clearcoatNormalTexture)
            || static_cast<unsigned int>(entity.material.opacityMapChannel)
                    > static_cast<unsigned int>(ScalarTextureChannel::Luminance)
            || static_cast<unsigned int>(entity.primitive)
                    > static_cast<unsigned int>(Primitive::None)) {
        return false;
    }
    if (!validTextureMapTransform(entity.material.albedoMapTransform)
            || !validTextureMapTransform(entity.material.normalMapTransform)
            || !validTextureMapTransform(entity.material.metallicRoughnessMapTransform)
            || !validTextureMapTransform(entity.material.metallicMapTransform)
            || !validTextureMapTransform(entity.material.roughnessMapTransform)
            || !validTextureMapTransform(entity.material.occlusionMapTransform)
            || !validTextureMapTransform(entity.material.opacityMapTransform)
            || !validTextureMapTransform(entity.material.emissiveMapTransform)
            || !validTextureMapTransform(entity.material.clearcoatMapTransform)
            || !validTextureMapTransform(entity.material.clearcoatRoughnessMapTransform)
            || !validTextureMapTransform(entity.material.clearcoatNormalMapTransform)
            || !validTextureMapValueRange(entity.material.albedoMapValueRange)
            || !validTextureMapValueRange(entity.material.normalMapValueRange)
            || !validTextureMapValueRange(entity.material.metallicRoughnessMapValueRange)
            || !validTextureMapValueRange(entity.material.metallicMapValueRange)
            || !validTextureMapValueRange(entity.material.roughnessMapValueRange)
            || !validTextureMapValueRange(entity.material.occlusionMapValueRange)
            || !validTextureMapValueRange(entity.material.opacityMapValueRange)
            || !validTextureMapValueRange(entity.material.emissiveMapValueRange)) {
        return false;
    }
    const auto finite = [](float value) { return std::isfinite(value); };
    const auto finite3 = [&finite](const std::array<float, 3>& value) {
        return finite(value[0]) && finite(value[1]) && finite(value[2]);
    };
    if (!finite3(entity.transform.position) || !finite3(entity.transform.rotation)
            || !finite3(entity.transform.scale) || !finite3(entity.material.color)
            || !finite3(entity.material.emission) || !finite(entity.material.metallic)
            || !finite(entity.material.roughness) || !finite(entity.material.opacity)
            || entity.material.opacity < 0.0f || entity.material.opacity > 1.0f
            || !finite(entity.material.ambientOcclusion)
            || !finite(entity.material.normalMapScale)
            || entity.material.normalMapScale < 0.0f
            || entity.material.normalMapScale > 1000.0f
            || !finite(entity.material.emissiveStrength)
            || entity.material.emissiveStrength < 0.0f
            || entity.material.emissiveStrength > 10000.0f
            || !finite(entity.material.indexOfRefraction)
            || entity.material.indexOfRefraction < 1.0f
            || entity.material.indexOfRefraction > 2.333f
            || !finite(entity.material.clearcoatFactor)
            || entity.material.clearcoatFactor < 0.0f
            || entity.material.clearcoatFactor > 1.0f
            || !finite(entity.material.clearcoatRoughness)
            || entity.material.clearcoatRoughness < 0.0f
            || entity.material.clearcoatRoughness > 1.0f
            || !finite(entity.material.clearcoatNormalScale)
            || entity.material.clearcoatNormalScale < 0.0f
            || entity.material.clearcoatNormalScale > 1000.0f
            || !finite3(entity.spin.radiansPerSecond) || !finite3(entity.orbit.center)
            || !finite(entity.orbit.radius) || !finite(entity.orbit.angularSpeed)
            || !finite(entity.orbit.phase) || !finite(entity.orbit.verticalAmplitude)
            || !finite(entity.orbit.verticalFrequency) || !finite3(entity.pulse.baseScale)
            || !finite(entity.pulse.amplitude) || !finite(entity.pulse.frequency)
            || !finite(entity.pulse.phase) || !finite(entity.animation.playbackRate)
            || !finite(entity.animation.timeSeconds) || !finite(entity.animation.durationSeconds)
            || !finite(entity.skeletalAnimation.playbackRate)
            || !finite(entity.skeletalAnimation.timeSeconds)
            || !finite(entity.skeletalAnimation.durationSeconds)
            || !finite(entity.skeletalAnimation.transitionFromTimeSeconds)
            || !finite(entity.skeletalAnimation.transitionElapsedSeconds)
            || !finite(entity.skeletalLocomotion.walkSpeedThreshold)
            || !finite(entity.skeletalLocomotion.runSpeedThreshold)
            || !finite(entity.skeletalLocomotion.transitionDurationSeconds)
            || !finite(entity.rigidbody.mass) || !finite3(entity.rigidbody.velocity)
            || !finite3(entity.rigidbody.angularVelocity)
            || !finite(entity.rigidbody.linearDamping)
            || !finite(entity.rigidbody.angularDamping) || !finite(entity.rigidbody.gravityScale)
            || !finite(entity.sphereCollider.radius)
            || !finite(entity.sphereCollider.restitution)
            || !finite(entity.sphereCollider.friction)
            || !finite3(entity.boxCollider.halfExtents)
            || !finite(entity.boxCollider.restitution)
            || !finite(entity.boxCollider.friction)
            || !finite(entity.characterController.moveInput[0])
            || !finite(entity.characterController.moveInput[1])
            || !finite(entity.characterController.maximumSpeed)
            || !finite(entity.characterController.acceleration)
            || !finite(entity.characterController.jumpSpeed)
            || !finite(entity.characterController.coyoteTimeSeconds)
            || !finite(entity.characterController.jumpBufferSeconds)
            || !finite3(entity.pointLight.color) || !finite(entity.pointLight.intensity)
            || !finite(entity.pointLight.range) || !finite(entity.camera.verticalFovRadians)
            || !finite(entity.camera.nearPlane) || !finite(entity.camera.farPlane)
            || !finite3(entity.orbitCamera.target) || !finite(entity.orbitCamera.distance)
            || !finite(entity.orbitCamera.minimumDistance)
            || !finite(entity.orbitCamera.maximumDistance) || !finite(entity.orbitCamera.yaw)
            || !finite(entity.orbitCamera.pitch) || !finite(entity.orbitCamera.minimumPitch)
            || !finite(entity.orbitCamera.maximumPitch) || !finite(entity.orbitCamera.yawPerPixel)
            || !finite(entity.orbitCamera.pitchPerPixel)
            || !finite(entity.orbitCamera.collisionRadius)
            || !finite(entity.orbitCamera.collisionPadding)
            || !finite(entity.orbitCamera.collisionMinimumDistance)
            || !finite(entity.orbitCamera.collisionRecoverySpeed)
            || !finite(entity.audioSource.gain)
            || !finite(entity.audioSource.referenceDistance)
            || !finite(entity.audioSource.maximumDistance)
            || !finite(entity.audioSource.rolloff)) {
        return false;
    }
    if (entity.orbitCamera.collisionRadius <= 0.0f
            || entity.orbitCamera.collisionPadding < 0.0f
            || entity.orbitCamera.collisionMinimumDistance <= 0.0f
            || entity.orbitCamera.collisionRecoverySpeed < 0.0f
            || entity.orbitCamera.collisionLayer == 0U
            || (entity.orbitCamera.collisionEnabled
                && entity.orbitCamera.collisionMask == 0U)
            || !validTexturePath(entity.audioSource.assetPath)
            || (entity.audioSource.enabled && entity.audioSource.assetPath.empty())
            || static_cast<std::uint8_t>(entity.audioSource.bus)
                    > static_cast<std::uint8_t>(AudioBus::Music)
            || entity.audioSource.gain < 0.0f || entity.audioSource.gain > 4.0f
            || entity.audioSource.referenceDistance <= 0.0f
            || entity.audioSource.maximumDistance < entity.audioSource.referenceDistance
            || entity.audioSource.rolloff < 0.0f
            || entity.sphereCollider.collisionLayer == 0U
            || entity.boxCollider.collisionLayer == 0U
            || (entity.rigidbody.enabled && entity.rigidbody.mass < 1.0e-6f)
            || entity.rigidbody.linearDamping < 0.0f || entity.rigidbody.angularDamping < 0.0f
            || (entity.sphereCollider.enabled && entity.sphereCollider.radius <= 0.0f)
            || entity.sphereCollider.restitution < 0.0f || entity.sphereCollider.restitution > 1.0f
            || entity.sphereCollider.friction < 0.0f || entity.sphereCollider.friction > 1.0f
            || (entity.boxCollider.enabled && entity.sphereCollider.enabled)
            || (entity.boxCollider.enabled
                && (entity.boxCollider.halfExtents[0] <= 0.0f
                    || entity.boxCollider.halfExtents[1] <= 0.0f
                    || entity.boxCollider.halfExtents[2] <= 0.0f))
            || entity.boxCollider.restitution < 0.0f || entity.boxCollider.restitution > 1.0f
            || entity.boxCollider.friction < 0.0f || entity.boxCollider.friction > 1.0f
            || (entity.characterController.enabled
                && (entity.characterController.maximumSpeed <= 0.0f
                    || entity.characterController.acceleration <= 0.0f
                    || entity.characterController.jumpSpeed <= 0.0f))
            || entity.characterController.moveInput[0] < -1.0f
            || entity.characterController.moveInput[0] > 1.0f
            || entity.characterController.moveInput[1] < -1.0f
            || entity.characterController.moveInput[1] > 1.0f
            || entity.characterController.maximumSpeed < 0.0f
            || entity.characterController.acceleration < 0.0f
            || entity.characterController.jumpSpeed < 0.0f
            || entity.characterController.coyoteTimeSeconds < 0.0f
            || entity.characterController.coyoteTimeSeconds > 1.0f
            || entity.characterController.jumpBufferSeconds < 0.0f
            || entity.characterController.jumpBufferSeconds > 1.0f
            || std::fabs(entity.skeletalAnimation.playbackRate) > 1000.0f
            || entity.skeletalAnimation.timeSeconds < 0.0f
            || entity.skeletalAnimation.durationSeconds < 0.0f
            || entity.skeletalAnimation.durationSeconds > 1000000.0f
            || entity.skeletalAnimation.timeSeconds > entity.skeletalAnimation.durationSeconds
            || entity.skeletalAnimation.transitionFromTimeSeconds < 0.0f
            || entity.skeletalAnimation.transitionFromTimeSeconds > 1000000.0f
            || entity.skeletalAnimation.transitionElapsedSeconds < 0.0f
            || entity.skeletalAnimation.transitionElapsedSeconds > 5.0f
            || (entity.skeletalAnimation.transitionFromClip.empty()
                && (entity.skeletalAnimation.transitionFromTimeSeconds != 0.0f
                    || entity.skeletalAnimation.transitionElapsedSeconds != 0.0f))
            || (!entity.skeletalAnimation.transitionFromClip.empty()
                && (entity.skeletalAnimation.transitionFromClip
                                == entity.skeletalAnimation.clipName
                    || entity.skeletalAnimation.transitionElapsedSeconds
                            >= entity.skeletalLocomotion.transitionDurationSeconds))
            || entity.skeletalLocomotion.transitionDurationSeconds < 0.0f
            || entity.skeletalLocomotion.transitionDurationSeconds > 5.0f
            || (entity.skeletalAnimation.enabled && entity.skeletalAnimation.clipName.empty())
            || (entity.skeletalLocomotion.enabled
                && (entity.skeletalLocomotion.idleClip.empty()
                    || entity.skeletalLocomotion.walkClip.empty()
                    || entity.skeletalLocomotion.runClip.empty()
                    || entity.skeletalLocomotion.jumpClip.empty()
                    || entity.skeletalLocomotion.walkSpeedThreshold < 0.0f
                    || entity.skeletalLocomotion.runSpeedThreshold
                            <= entity.skeletalLocomotion.walkSpeedThreshold))) {
        return false;
    }
    return validTrack(entity.animation.position) && validTrack(entity.animation.rotation)
            && validTrack(entity.animation.scale);
}

bool validateScene(const Scene& scene, std::uint32_t activeCameraId,
                   std::uint32_t nextEntityId, std::string& error) {
    if (scene.entities().size() > kMaximumEntities || !std::isfinite(scene.elapsedSeconds())
            || scene.elapsedSeconds() < 0.0f || nextEntityId == 0
            || nextEntityId == std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, "Scene exceeds limits or has invalid elapsed time");
    }

    std::unordered_set<std::uint32_t> ids;
    ids.reserve(scene.entities().size());
    std::uint32_t maximumId = 0;
    for (const Entity& entity : scene.entities()) {
        if (!validEntity(entity) || !ids.insert(entity.id).second) {
            return fail(error, "Scene contains invalid entity data or duplicate IDs");
        }
        maximumId = std::max(maximumId, entity.id);
    }
    const AmbientLighting& ambient = scene.ambientLighting();
    for (const float coefficient : ambient.sphericalHarmonics) {
        if (!std::isfinite(coefficient) || std::fabs(coefficient) > 1000.0f) {
            return fail(error, "Scene ambient-light coefficients are invalid");
        }
    }
    const DirectionalLighting& directional = scene.directionalLighting();
    const float directionLengthSquared = directional.direction[0] * directional.direction[0]
            + directional.direction[1] * directional.direction[1]
            + directional.direction[2] * directional.direction[2];
    if (!std::isfinite(directionLengthSquared) || directionLengthSquared < 1.0e-8f
            || directionLengthSquared > 1.0e8f
            || !std::isfinite(directional.color[0]) || directional.color[0] < 0.0f
            || directional.color[0] > 1.0f || !std::isfinite(directional.color[1])
            || directional.color[1] < 0.0f || directional.color[1] > 1.0f
            || !std::isfinite(directional.color[2]) || directional.color[2] < 0.0f
            || directional.color[2] > 1.0f || !std::isfinite(directional.intensity)
            || directional.intensity < 0.0f || directional.intensity > 100.0f
            || !std::isfinite(directional.shadowStrength)
            || directional.shadowStrength < 0.0f || directional.shadowStrength > 1.0f) {
        return fail(error, "Scene directional-light settings are invalid");
    }
    const AtmosphereSettings& atmosphere = scene.atmosphere();
    if (!std::isfinite(atmosphere.fogColor[0]) || atmosphere.fogColor[0] < 0.0f
            || atmosphere.fogColor[0] > 1.0f || !std::isfinite(atmosphere.fogColor[1])
            || atmosphere.fogColor[1] < 0.0f || atmosphere.fogColor[1] > 1.0f
            || !std::isfinite(atmosphere.fogColor[2]) || atmosphere.fogColor[2] < 0.0f
            || atmosphere.fogColor[2] > 1.0f || !std::isfinite(atmosphere.fogStart)
            || !std::isfinite(atmosphere.fogEnd) || atmosphere.fogStart < 0.0f
            || atmosphere.fogEnd <= atmosphere.fogStart || atmosphere.fogEnd > 100000.0f) {
        return fail(error, "Scene atmosphere settings are invalid");
    }
    const SkySettings& sky = scene.sky();
    for (const float component : sky.zenithColor) {
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f) {
            return fail(error, "Scene sky colors are invalid");
        }
    }
    for (const float component : sky.horizonColor) {
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f) {
            return fail(error, "Scene sky colors are invalid");
        }
    }
    for (const float component : sky.groundColor) {
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f) {
            return fail(error, "Scene sky colors are invalid");
        }
    }
    for (const float component : sky.sunColor) {
        if (!std::isfinite(component) || component < 0.0f || component > 1.0f) {
            return fail(error, "Scene sky colors are invalid");
        }
    }
    if (!std::isfinite(sky.sunAngularRadius) || sky.sunAngularRadius < 0.001f
            || sky.sunAngularRadius > 0.5f || !std::isfinite(sky.ambientStrength)
            || sky.ambientStrength < 0.0f || sky.ambientStrength > 5.0f) {
        return fail(error, "Scene sky lighting settings are invalid");
    }
    const PhysicsSettings& physics = scene.physicsSettings();
    if (!std::isfinite(physics.gravity[0]) || !std::isfinite(physics.gravity[1])
            || !std::isfinite(physics.gravity[2]) || !std::isfinite(physics.fixedTimeStep)
            || physics.fixedTimeStep <= 0.0f || physics.maximumSubsteps == 0
            || physics.maximumSubsteps > 16 || !std::isfinite(physics.floorHeight)
            || !std::isfinite(physics.floorRestitution)
            || physics.floorRestitution < 0.0f || physics.floorRestitution > 1.0f
            || !std::isfinite(physics.floorFriction) || physics.floorFriction < 0.0f
            || physics.floorFriction > 1.0f || physics.floorCollisionLayer == 0U) {
        return fail(error, "Scene physics settings are invalid");
    }
    if (nextEntityId <= maximumId) {
        return fail(error, "Next entity ID would be reused");
    }
    for (const Entity& entity : scene.entities()) {
        if (entity.parentId != 0 && ids.find(entity.parentId) == ids.end()) {
            return fail(error, "Scene contains a missing parent reference");
        }
    }

    // Validate parent chains with a bounded walk; this also catches cycles
    // introduced through direct edits to the public entity data.
    for (const Entity& entity : scene.entities()) {
        const Entity* current = &entity;
        for (std::size_t depth = 0; current->parentId != 0; ++depth) {
            if (depth >= scene.entities().size()) {
                return fail(error, "Scene hierarchy contains a cycle");
            }
            current = scene.find(current->parentId);
            if (current == nullptr) return fail(error, "Scene contains a missing parent reference");
        }
    }

    if (activeCameraId != 0) {
        const Entity* active = scene.find(activeCameraId);
        if (active == nullptr || !active->camera.enabled) {
            return fail(error, "Scene active-camera reference is invalid");
        }
    }
    error.clear();
    return true;
}

void writeTrack(std::ostream& output, const char* name, const Vector3Track& track) {
    output << name << ' ' << track.keyframes.size() << '\n';
    for (const Vector3Keyframe& keyframe : track.keyframes) {
        output << "KEY " << keyframe.timeSeconds;
        writeFloatArray(output, keyframe.value);
        output << '\n';
    }
}

bool readTrack(std::istream& input, const char* name, Vector3Track& track,
               std::size_t& totalKeyframes, std::string& error) {
    if (!readTag(input, name, error)) return false;
    std::size_t count = 0;
    if (!(input >> count) || count > kMaximumKeyframesPerTrack
            || totalKeyframes > kMaximumKeyframesPerTrack * 3 - count) {
        return fail(error, "Animation track exceeds keyframe limits");
    }
    totalKeyframes += count;
    float previousTime = -1.0f;
    for (std::size_t index = 0; index < count; ++index) {
        if (!readTag(input, "KEY", error)) return false;
        float timeSeconds = 0.0f;
        std::array<float, 3> value{};
        if (!(input >> timeSeconds) || !std::isfinite(timeSeconds) || timeSeconds < 0.0f
                || timeSeconds <= previousTime || !readFloatArray(input, value)) {
            return fail(error, "Animation keyframe is invalid or out of order");
        }
        if (!track.addKeyframe(timeSeconds, value)) {
            return fail(error, "Could not add animation keyframe");
        }
        previousTime = timeSeconds;
    }
    return true;
}

void writeEntity(std::ostream& output, const Entity& entity) {
    output << "ENTITY " << entity.id << ' ' << entity.parentId << ' '
           << static_cast<unsigned int>(entity.primitive) << ' ' << (entity.visible ? 1 : 0)
           << ' ' << std::quoted(entity.name) << ' ' << std::quoted(entity.meshAsset) << '\n';

    output << "TRANSFORM";
    writeFloatArray(output, entity.transform.position);
    writeFloatArray(output, entity.transform.rotation);
    writeFloatArray(output, entity.transform.scale);
    output << '\n';

    output << "MATERIAL";
    writeFloatArray(output, entity.material.color);
    writeFloatArray(output, entity.material.emission);
    output << ' ' << entity.material.metallic << ' ' << entity.material.roughness
           << ' ' << entity.material.ambientOcclusion << ' '
           << (entity.material.proceduralGrid ? 1 : 0) << ' '
           << entity.material.opacity << ' '
           << std::quoted(entity.material.albedoTexture) << ' '
           << std::quoted(entity.material.normalTexture) << ' '
           << std::quoted(entity.material.metallicRoughnessTexture) << ' '
           << std::quoted(entity.material.occlusionTexture) << ' '
           << std::quoted(entity.material.metallicTexture) << ' '
           << std::quoted(entity.material.roughnessTexture) << ' '
           << std::quoted(entity.material.opacityTexture) << ' '
           << std::quoted(entity.material.emissiveTexture) << '\n';
    output << "MATERIAL_PBR_EXT " << (entity.material.unlit ? 1 : 0) << ' '
           << entity.material.normalMapScale << ' '
           << entity.material.emissiveStrength << '\n';
    output << "OVERRIDE_MTL_MATERIALS "
           << (entity.overrideMtlMaterials ? 1 : 0) << '\n';
    output << "MATERIAL_MAP_UV";
    writeTextureMapTransform(output, entity.material.albedoMapTransform);
    writeTextureMapTransform(output, entity.material.normalMapTransform);
    writeTextureMapTransform(output, entity.material.metallicRoughnessMapTransform);
    writeTextureMapTransform(output, entity.material.metallicMapTransform);
    writeTextureMapTransform(output, entity.material.roughnessMapTransform);
    writeTextureMapTransform(output, entity.material.occlusionMapTransform);
    writeTextureMapTransform(output, entity.material.opacityMapTransform);
    writeTextureMapTransform(output, entity.material.emissiveMapTransform);
    output << '\n';
    output << "MATERIAL_CLEARCOAT " << entity.material.indexOfRefraction << ' '
           << entity.material.clearcoatFactor << ' '
           << entity.material.clearcoatRoughness << ' '
           << entity.material.clearcoatNormalScale << ' '
           << std::quoted(entity.material.clearcoatTexture) << ' '
           << std::quoted(entity.material.clearcoatRoughnessTexture) << ' '
           << std::quoted(entity.material.clearcoatNormalTexture);
    writeTextureMapTransform(output, entity.material.clearcoatMapTransform);
    writeTextureMapTransform(output, entity.material.clearcoatRoughnessMapTransform);
    writeTextureMapTransform(output, entity.material.clearcoatNormalMapTransform);
    output << '\n';
    output << "OPACITY_MAP_CHANNEL "
           << static_cast<unsigned int>(entity.material.opacityMapChannel) << '\n';
    output << "MATERIAL_MAP_MM";
    writeTextureMapValueRange(output, entity.material.albedoMapValueRange);
    writeTextureMapValueRange(output, entity.material.normalMapValueRange);
    writeTextureMapValueRange(output, entity.material.metallicRoughnessMapValueRange);
    writeTextureMapValueRange(output, entity.material.metallicMapValueRange);
    writeTextureMapValueRange(output, entity.material.roughnessMapValueRange);
    writeTextureMapValueRange(output, entity.material.occlusionMapValueRange);
    writeTextureMapValueRange(output, entity.material.opacityMapValueRange);
    writeTextureMapValueRange(output, entity.material.emissiveMapValueRange);
    output << '\n';

    output << "SPIN";
    writeFloatArray(output, entity.spin.radiansPerSecond);
    output << '\n';

    output << "ORBIT " << (entity.orbit.enabled ? 1 : 0);
    writeFloatArray(output, entity.orbit.center);
    output << ' ' << entity.orbit.radius << ' ' << entity.orbit.angularSpeed
           << ' ' << entity.orbit.phase << ' ' << entity.orbit.verticalAmplitude
           << ' ' << entity.orbit.verticalFrequency << '\n';

    output << "PULSE " << (entity.pulse.enabled ? 1 : 0);
    writeFloatArray(output, entity.pulse.baseScale);
    output << ' ' << entity.pulse.amplitude << ' ' << entity.pulse.frequency
           << ' ' << entity.pulse.phase << '\n';

    output << "ANIMATION " << (entity.animation.enabled ? 1 : 0) << ' '
           << (entity.animation.playing ? 1 : 0) << ' '
           << (entity.animation.looping ? 1 : 0) << ' '
           << entity.animation.playbackRate << ' ' << entity.animation.timeSeconds
           << ' ' << entity.animation.durationSeconds << '\n';
    writeTrack(output, "POSITION_KEYS", entity.animation.position);
    writeTrack(output, "ROTATION_KEYS", entity.animation.rotation);
    writeTrack(output, "SCALE_KEYS", entity.animation.scale);

    output << "SKELETAL_PLAYER " << (entity.skeletalAnimation.enabled ? 1 : 0) << ' '
           << (entity.skeletalAnimation.playing ? 1 : 0) << ' '
           << (entity.skeletalAnimation.looping ? 1 : 0) << ' '
           << entity.skeletalAnimation.playbackRate << ' '
           << entity.skeletalAnimation.timeSeconds << ' '
           << entity.skeletalAnimation.durationSeconds << ' '
           << std::quoted(entity.skeletalAnimation.clipName) << ' '
           << std::quoted(entity.skeletalAnimation.transitionFromClip) << ' '
           << entity.skeletalAnimation.transitionFromTimeSeconds << ' '
           << (entity.skeletalAnimation.transitionFromLooping ? 1 : 0) << ' '
           << entity.skeletalAnimation.transitionElapsedSeconds << '\n';

    output << "SKELETAL_LOCOMOTION " << (entity.skeletalLocomotion.enabled ? 1 : 0) << ' '
           << std::quoted(entity.skeletalLocomotion.idleClip) << ' '
           << std::quoted(entity.skeletalLocomotion.walkClip) << ' '
           << std::quoted(entity.skeletalLocomotion.runClip) << ' '
           << std::quoted(entity.skeletalLocomotion.jumpClip) << ' '
           << entity.skeletalLocomotion.walkSpeedThreshold << ' '
           << entity.skeletalLocomotion.runSpeedThreshold << ' '
           << entity.skeletalLocomotion.transitionDurationSeconds << '\n';

    output << "POINT_LIGHT " << (entity.pointLight.enabled ? 1 : 0);
    writeFloatArray(output, entity.pointLight.color);
    output << ' ' << entity.pointLight.intensity << ' ' << entity.pointLight.range << '\n';

    output << "CAMERA " << (entity.camera.enabled ? 1 : 0) << ' '
           << entity.camera.verticalFovRadians << ' ' << entity.camera.nearPlane
           << ' ' << entity.camera.farPlane << '\n';

    output << "ORBIT_CAMERA " << (entity.orbitCamera.enabled ? 1 : 0);
    writeFloatArray(output, entity.orbitCamera.target);
    output << ' ' << entity.orbitCamera.distance << ' '
           << entity.orbitCamera.minimumDistance << ' ' << entity.orbitCamera.maximumDistance
           << ' ' << entity.orbitCamera.yaw << ' ' << entity.orbitCamera.pitch
           << ' ' << entity.orbitCamera.minimumPitch << ' ' << entity.orbitCamera.maximumPitch
           << ' ' << entity.orbitCamera.yawPerPixel << ' ' << entity.orbitCamera.pitchPerPixel
           << '\n';
    output << "ORBIT_CAMERA_COLLISION "
           << (entity.orbitCamera.collisionEnabled ? 1 : 0) << ' '
           << entity.orbitCamera.collisionRadius << ' '
           << entity.orbitCamera.collisionPadding << ' '
           << entity.orbitCamera.collisionMinimumDistance << ' '
           << entity.orbitCamera.collisionRecoverySpeed << ' '
           << entity.orbitCamera.collisionLayer << ' '
           << entity.orbitCamera.collisionMask << '\n';
    output << "AUDIO_SOURCE " << (entity.audioSource.enabled ? 1 : 0) << ' '
           << std::quoted(entity.audioSource.assetPath) << ' '
           << static_cast<unsigned int>(entity.audioSource.bus) << ' '
           << entity.audioSource.gain << ' '
           << static_cast<unsigned int>(entity.audioSource.priority) << ' '
           << (entity.audioSource.looping ? 1 : 0) << ' '
           << (entity.audioSource.positional ? 1 : 0) << ' '
           << (entity.audioSource.playOnAwake ? 1 : 0) << ' '
           << entity.audioSource.referenceDistance << ' '
           << entity.audioSource.maximumDistance << ' '
           << entity.audioSource.rolloff << '\n';
    output << "RIGIDBODY " << (entity.rigidbody.enabled ? 1 : 0) << ' '
           << entity.rigidbody.mass;
    writeFloatArray(output, entity.rigidbody.velocity);
    output << ' ' << entity.rigidbody.linearDamping << ' ' << entity.rigidbody.gravityScale;
    writeFloatArray(output, entity.rigidbody.angularVelocity);
    output << ' ' << entity.rigidbody.angularDamping << '\n';

    output << "SPHERE_COLLIDER " << (entity.sphereCollider.enabled ? 1 : 0) << ' '
           << entity.sphereCollider.radius << ' ' << entity.sphereCollider.restitution << ' '
           << entity.sphereCollider.friction << ' '
           << (entity.sphereCollider.trigger ? 1 : 0) << ' '
           << entity.sphereCollider.collisionLayer << ' '
           << entity.sphereCollider.collisionMask << '\n';

    output << "BOX_COLLIDER " << (entity.boxCollider.enabled ? 1 : 0);
    writeFloatArray(output, entity.boxCollider.halfExtents);
    output << ' ' << entity.boxCollider.restitution << ' ' << entity.boxCollider.friction
           << ' ' << (entity.boxCollider.trigger ? 1 : 0) << ' '
           << entity.boxCollider.collisionLayer << ' '
           << entity.boxCollider.collisionMask << '\n';

    output << "CHARACTER_CONTROLLER " << (entity.characterController.enabled ? 1 : 0)
           << ' ' << entity.characterController.moveInput[0]
           << ' ' << entity.characterController.moveInput[1]
           << ' ' << entity.characterController.maximumSpeed
           << ' ' << entity.characterController.acceleration
           << ' ' << entity.characterController.jumpSpeed
           << ' ' << entity.characterController.coyoteTimeSeconds
           << ' ' << entity.characterController.jumpBufferSeconds
           << ' ' << (entity.characterController.grounded ? 1 : 0) << '\n';

    output << "END_ENTITY\n";
}

bool readEntity(std::istream& input, Scene& scene, std::uint32_t& maximumId,
                std::size_t& totalKeyframes, unsigned int version, std::string& error) {
    if (!readTag(input, "ENTITY", error)) return false;
    std::uint32_t id = 0;
    std::uint32_t parentId = 0;
    unsigned int primitiveValue = 0;
    int visible = -1;
    std::string name;
    std::string meshAsset;
    if (!(input >> id >> parentId >> primitiveValue >> visible >> std::quoted(name)
                >> std::quoted(meshAsset))
            || id == 0 || primitiveValue > static_cast<unsigned int>(Primitive::None)
            || (visible != 0 && visible != 1) || name.size() > kMaximumTextLength
            || meshAsset.size() > kMaximumTextLength) {
        return fail(error, "Entity header is malformed or exceeds limits");
    }
    if (scene.find(id) != nullptr) return fail(error, "Scene contains duplicate entity IDs");
    if (id == std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, "Maximum entity ID is reserved");
    }

    Entity& entity = scene.create(std::move(name), static_cast<Primitive>(primitiveValue));
    entity.id = id;
    entity.parentId = parentId;
    entity.visible = visible != 0;
    entity.meshAsset = std::move(meshAsset);
    maximumId = std::max(maximumId, id);

    if (!readTag(input, "TRANSFORM", error)
            || !readFloatArray(input, entity.transform.position)
            || !readFloatArray(input, entity.transform.rotation)
            || !readFloatArray(input, entity.transform.scale)) {
        return fail(error, "Entity transform is invalid");
    }

    int grid = -1;
    if (!readTag(input, "MATERIAL", error)
            || !readFloatArray(input, entity.material.color)
            || !readFloatArray(input, entity.material.emission)
            || !(input >> entity.material.metallic >> entity.material.roughness
                        >> entity.material.ambientOcclusion >> grid)
            || !std::isfinite(entity.material.metallic)
            || !std::isfinite(entity.material.roughness)
            || !std::isfinite(entity.material.ambientOcclusion)
            || (grid != 0 && grid != 1)) {
        return fail(error, "Entity material is invalid");
    }
    entity.material.proceduralGrid = grid != 0;
    if (version >= 9
            && (!(input >> entity.material.opacity)
                || !std::isfinite(entity.material.opacity)
                || entity.material.opacity < 0.0f || entity.material.opacity > 1.0f)) {
        return fail(error, "Entity material opacity is invalid");
    }
    if (version >= 4
            && (!(input >> std::quoted(entity.material.albedoTexture)
                         >> std::quoted(entity.material.normalTexture)
                         >> std::quoted(entity.material.metallicRoughnessTexture)
                         >> std::quoted(entity.material.occlusionTexture))
                || !validTexturePath(entity.material.albedoTexture)
                || !validTexturePath(entity.material.normalTexture)
                || !validTexturePath(entity.material.metallicRoughnessTexture)
                || !validTexturePath(entity.material.occlusionTexture))) {
        return fail(error, "Entity material texture path is invalid");
    }
    if (version >= 5
            && (!(input >> std::quoted(entity.material.metallicTexture)
                         >> std::quoted(entity.material.roughnessTexture))
                || !validTexturePath(entity.material.metallicTexture)
                || !validTexturePath(entity.material.roughnessTexture))) {
        return fail(error, "Entity scalar PBR texture path is invalid");
    }
    if (version >= 10
            && (!(input >> std::quoted(entity.material.opacityTexture))
                || !validTexturePath(entity.material.opacityTexture))) {
        return fail(error, "Entity opacity texture path is invalid");
    }
    if (version >= 12
            && (!(input >> std::quoted(entity.material.emissiveTexture))
                || !validTexturePath(entity.material.emissiveTexture))) {
        return fail(error, "Entity emissive texture path is invalid");
    }
    if (version >= 28) {
        int unlit = -1;
        if (!readTag(input, "MATERIAL_PBR_EXT", error)
                || !(input >> unlit >> entity.material.normalMapScale
                            >> entity.material.emissiveStrength)
                || (unlit != 0 && unlit != 1)
                || !std::isfinite(entity.material.normalMapScale)
                || entity.material.normalMapScale < 0.0f
                || entity.material.normalMapScale > 1000.0f
                || !std::isfinite(entity.material.emissiveStrength)
                || entity.material.emissiveStrength < 0.0f
                || entity.material.emissiveStrength > 10000.0f) {
            return fail(error, "Entity PBR extension settings are invalid");
        }
        entity.material.unlit = unlit != 0;
    }
    if (version >= 6) {
        int overrideMtlMaterials = -1;
        if (!readTag(input, "OVERRIDE_MTL_MATERIALS", error)
                || !(input >> overrideMtlMaterials)
                || (overrideMtlMaterials != 0 && overrideMtlMaterials != 1)) {
            return fail(error, "Imported mesh material override flag is invalid");
        }
        entity.overrideMtlMaterials = overrideMtlMaterials != 0;
    }
    if (version >= 7
            && (!readTag(input, "MATERIAL_MAP_UV", error)
                || !readTextureMapTransform(input, entity.material.albedoMapTransform,
                        version >= 8, version >= 27, version >= 29)
                || !readTextureMapTransform(input, entity.material.normalMapTransform,
                        version >= 8, version >= 27, version >= 29)
                || !readTextureMapTransform(input,
                        entity.material.metallicRoughnessMapTransform, version >= 8, version >= 27, version >= 29)
                || !readTextureMapTransform(input, entity.material.metallicMapTransform,
                        version >= 8, version >= 27, version >= 29)
                || !readTextureMapTransform(input, entity.material.roughnessMapTransform,
                        version >= 8, version >= 27, version >= 29)
                || !readTextureMapTransform(input, entity.material.occlusionMapTransform,
                        version >= 8, version >= 27, version >= 29)
                || (version >= 10
                    && !readTextureMapTransform(input,
                            entity.material.opacityMapTransform, true, version >= 27, version >= 29))
                || (version >= 12
                    && !readTextureMapTransform(input,
                            entity.material.emissiveMapTransform, true, version >= 27, version >= 29)))) {
        return fail(error, "Material UV transform or sampler settings are invalid");
    }
    if (version >= 30) {
        if (!readTag(input, "MATERIAL_CLEARCOAT", error)
                || !(input >> entity.material.indexOfRefraction
                            >> entity.material.clearcoatFactor
                            >> entity.material.clearcoatRoughness
                            >> entity.material.clearcoatNormalScale
                            >> std::quoted(entity.material.clearcoatTexture)
                            >> std::quoted(entity.material.clearcoatRoughnessTexture)
                            >> std::quoted(entity.material.clearcoatNormalTexture))
                || !validTexturePath(entity.material.clearcoatTexture)
                || !validTexturePath(entity.material.clearcoatRoughnessTexture)
                || !validTexturePath(entity.material.clearcoatNormalTexture)
                || !std::isfinite(entity.material.indexOfRefraction)
                || entity.material.indexOfRefraction < 1.0f
                || entity.material.indexOfRefraction > 2.333f
                || !std::isfinite(entity.material.clearcoatFactor)
                || entity.material.clearcoatFactor < 0.0f
                || entity.material.clearcoatFactor > 1.0f
                || !std::isfinite(entity.material.clearcoatRoughness)
                || entity.material.clearcoatRoughness < 0.0f
                || entity.material.clearcoatRoughness > 1.0f
                || !std::isfinite(entity.material.clearcoatNormalScale)
                || entity.material.clearcoatNormalScale < 0.0f
                || entity.material.clearcoatNormalScale > 1000.0f
                || !readTextureMapTransform(input,
                        entity.material.clearcoatMapTransform, true, true, true)
                || !readTextureMapTransform(input,
                        entity.material.clearcoatRoughnessMapTransform, true, true, true)
                || !readTextureMapTransform(input,
                        entity.material.clearcoatNormalMapTransform, true, true, true)) {
            return fail(error, "Entity clearcoat material settings are invalid");
        }
    }
    if (version >= 10) {
        int opacityMapChannel = -1;
        if (!readTag(input, "OPACITY_MAP_CHANNEL", error)
                || !(input >> opacityMapChannel) || opacityMapChannel < 0
                || opacityMapChannel > static_cast<int>(ScalarTextureChannel::Luminance)) {
            return fail(error, "Material opacity-map channel is invalid");
        }
        entity.material.opacityMapChannel =
                static_cast<ScalarTextureChannel>(opacityMapChannel);
    }
    if (version >= 11
            && (!readTag(input, "MATERIAL_MAP_MM", error)
                || !readTextureMapValueRange(input, entity.material.albedoMapValueRange)
                || !readTextureMapValueRange(input, entity.material.normalMapValueRange)
                || !readTextureMapValueRange(input,
                        entity.material.metallicRoughnessMapValueRange)
                || !readTextureMapValueRange(input, entity.material.metallicMapValueRange)
                || !readTextureMapValueRange(input, entity.material.roughnessMapValueRange)
                || !readTextureMapValueRange(input, entity.material.occlusionMapValueRange)
                || !readTextureMapValueRange(input, entity.material.opacityMapValueRange)
                || (version >= 12
                    && !readTextureMapValueRange(input,
                            entity.material.emissiveMapValueRange)))) {
        return fail(error, "Material map base/gain range is invalid");
    }

    if (!readTag(input, "SPIN", error)
            || !readFloatArray(input, entity.spin.radiansPerSecond)) {
        return fail(error, "Entity spin component is invalid");
    }

    if (!readTag(input, "ORBIT", error) || !readBool(input, entity.orbit.enabled)
            || !readFloatArray(input, entity.orbit.center)
            || !(input >> entity.orbit.radius >> entity.orbit.angularSpeed
                        >> entity.orbit.phase >> entity.orbit.verticalAmplitude
                        >> entity.orbit.verticalFrequency)
            || !std::isfinite(entity.orbit.radius)
            || !std::isfinite(entity.orbit.angularSpeed)
            || !std::isfinite(entity.orbit.phase)
            || !std::isfinite(entity.orbit.verticalAmplitude)
            || !std::isfinite(entity.orbit.verticalFrequency)) {
        return fail(error, "Entity orbit component is invalid");
    }

    if (!readTag(input, "PULSE", error) || !readBool(input, entity.pulse.enabled)
            || !readFloatArray(input, entity.pulse.baseScale)
            || !(input >> entity.pulse.amplitude >> entity.pulse.frequency >> entity.pulse.phase)
            || !std::isfinite(entity.pulse.amplitude)
            || !std::isfinite(entity.pulse.frequency)
            || !std::isfinite(entity.pulse.phase)) {
        return fail(error, "Entity pulse component is invalid");
    }

    if (!readTag(input, "ANIMATION", error)
            || !readBool(input, entity.animation.enabled)
            || !readBool(input, entity.animation.playing)
            || !readBool(input, entity.animation.looping)
            || !(input >> entity.animation.playbackRate >> entity.animation.timeSeconds
                        >> entity.animation.durationSeconds)
            || !std::isfinite(entity.animation.playbackRate)
            || !std::isfinite(entity.animation.timeSeconds)
            || !std::isfinite(entity.animation.durationSeconds)) {
        return fail(error, "Entity animation component is invalid");
    }
    if (!readTrack(input, "POSITION_KEYS", entity.animation.position, totalKeyframes, error)
            || !readTrack(input, "ROTATION_KEYS", entity.animation.rotation, totalKeyframes, error)
            || !readTrack(input, "SCALE_KEYS", entity.animation.scale, totalKeyframes, error)) {
        return false;
    }
    if (version >= 17) {
        if (!readTag(input, "SKELETAL_PLAYER", error)
                || !readBool(input, entity.skeletalAnimation.enabled)
                || !readBool(input, entity.skeletalAnimation.playing)
                || !readBool(input, entity.skeletalAnimation.looping)
                || !(input >> entity.skeletalAnimation.playbackRate
                             >> entity.skeletalAnimation.timeSeconds
                             >> entity.skeletalAnimation.durationSeconds
                             >> std::quoted(entity.skeletalAnimation.clipName))
                || !std::isfinite(entity.skeletalAnimation.playbackRate)
                || !std::isfinite(entity.skeletalAnimation.timeSeconds)
                || !std::isfinite(entity.skeletalAnimation.durationSeconds)) {
            return fail(error, "Entity skeletal-animation playback state is invalid");
        }
        if (version >= 19
                && (!(input >> std::quoted(entity.skeletalAnimation.transitionFromClip)
                                  >> entity.skeletalAnimation.transitionFromTimeSeconds)
                    || !readBool(input, entity.skeletalAnimation.transitionFromLooping)
                    || !(input >> entity.skeletalAnimation.transitionElapsedSeconds)
                    || !std::isfinite(entity.skeletalAnimation.transitionFromTimeSeconds)
                    || !std::isfinite(entity.skeletalAnimation.transitionElapsedSeconds))) {
            return fail(error, "Entity skeletal-animation transition state is invalid");
        }
    }
    if (version >= 18) {
        if (!readTag(input, "SKELETAL_LOCOMOTION", error)
                || !readBool(input, entity.skeletalLocomotion.enabled)
                || !(input >> std::quoted(entity.skeletalLocomotion.idleClip)
                             >> std::quoted(entity.skeletalLocomotion.walkClip)
                             >> std::quoted(entity.skeletalLocomotion.runClip)
                             >> std::quoted(entity.skeletalLocomotion.jumpClip)
                             >> entity.skeletalLocomotion.walkSpeedThreshold
                             >> entity.skeletalLocomotion.runSpeedThreshold)
                || !std::isfinite(entity.skeletalLocomotion.walkSpeedThreshold)
                || !std::isfinite(entity.skeletalLocomotion.runSpeedThreshold)) {
            return fail(error, "Entity skeletal-locomotion state is invalid");
        }
        if (version >= 19
                && (!(input >> entity.skeletalLocomotion.transitionDurationSeconds)
                    || !std::isfinite(entity.skeletalLocomotion.transitionDurationSeconds))) {
            return fail(error, "Entity skeletal-locomotion transition duration is invalid");
        }
    }

    if (!readTag(input, "POINT_LIGHT", error)
            || !readBool(input, entity.pointLight.enabled)
            || !readFloatArray(input, entity.pointLight.color)
            || !(input >> entity.pointLight.intensity >> entity.pointLight.range)
            || !std::isfinite(entity.pointLight.intensity)
            || !std::isfinite(entity.pointLight.range)) {
        return fail(error, "Entity point-light component is invalid");
    }

    if (!readTag(input, "CAMERA", error)
            || !readBool(input, entity.camera.enabled)
            || !(input >> entity.camera.verticalFovRadians >> entity.camera.nearPlane
                        >> entity.camera.farPlane)
            || !std::isfinite(entity.camera.verticalFovRadians)
            || !std::isfinite(entity.camera.nearPlane)
            || !std::isfinite(entity.camera.farPlane)) {
        return fail(error, "Entity camera component is invalid");
    }

    if (!readTag(input, "ORBIT_CAMERA", error)
            || !readBool(input, entity.orbitCamera.enabled)
            || !readFloatArray(input, entity.orbitCamera.target)
            || !(input >> entity.orbitCamera.distance >> entity.orbitCamera.minimumDistance
                        >> entity.orbitCamera.maximumDistance >> entity.orbitCamera.yaw
                        >> entity.orbitCamera.pitch >> entity.orbitCamera.minimumPitch
                        >> entity.orbitCamera.maximumPitch >> entity.orbitCamera.yawPerPixel
                        >> entity.orbitCamera.pitchPerPixel)
            || !std::isfinite(entity.orbitCamera.distance)
            || !std::isfinite(entity.orbitCamera.minimumDistance)
            || !std::isfinite(entity.orbitCamera.maximumDistance)
            || !std::isfinite(entity.orbitCamera.yaw)
            || !std::isfinite(entity.orbitCamera.pitch)
            || !std::isfinite(entity.orbitCamera.minimumPitch)
            || !std::isfinite(entity.orbitCamera.maximumPitch)
            || !std::isfinite(entity.orbitCamera.yawPerPixel)
            || !std::isfinite(entity.orbitCamera.pitchPerPixel)) {
        return fail(error, "Entity orbit camera component is invalid");
    }
    if (version >= 21) {
        if (!readTag(input, "ORBIT_CAMERA_COLLISION", error)
                || !readBool(input, entity.orbitCamera.collisionEnabled)
                || !(input >> entity.orbitCamera.collisionRadius
                            >> entity.orbitCamera.collisionPadding
                            >> entity.orbitCamera.collisionMinimumDistance
                            >> entity.orbitCamera.collisionRecoverySpeed
                            >> entity.orbitCamera.collisionLayer
                            >> entity.orbitCamera.collisionMask)
                || !std::isfinite(entity.orbitCamera.collisionRadius)
                || !std::isfinite(entity.orbitCamera.collisionPadding)
                || !std::isfinite(entity.orbitCamera.collisionMinimumDistance)
                || !std::isfinite(entity.orbitCamera.collisionRecoverySpeed)) {
            return fail(error, "Entity orbit-camera collision settings are invalid");
        }
    }
    if (version >= 22) {
        unsigned int audioBus = 0U;
        unsigned int audioPriority = 0U;
        if (!readTag(input, "AUDIO_SOURCE", error)
                || !readBool(input, entity.audioSource.enabled)
                || !(input >> std::quoted(entity.audioSource.assetPath)
                            >> audioBus >> entity.audioSource.gain >> audioPriority)
                || !readBool(input, entity.audioSource.looping)
                || !readBool(input, entity.audioSource.positional)
                || !readBool(input, entity.audioSource.playOnAwake)
                || !(input >> entity.audioSource.referenceDistance
                            >> entity.audioSource.maximumDistance
                            >> entity.audioSource.rolloff)
                || audioBus > static_cast<unsigned int>(AudioBus::Music)
                || audioPriority > 255U) {
            return fail(error, "Entity audio-source component is invalid");
        }
        entity.audioSource.bus = static_cast<AudioBus>(audioBus);
        entity.audioSource.priority = static_cast<std::uint8_t>(audioPriority);
    }

    if (version >= 2) {
        if (!readTag(input, "RIGIDBODY", error)
                || !readBool(input, entity.rigidbody.enabled)
                || !(input >> entity.rigidbody.mass)
                || !readFloatArray(input, entity.rigidbody.velocity)
                || !(input >> entity.rigidbody.linearDamping >> entity.rigidbody.gravityScale)
                || !std::isfinite(entity.rigidbody.mass)
                || !std::isfinite(entity.rigidbody.linearDamping)
                || !std::isfinite(entity.rigidbody.gravityScale)) {
            return fail(error, "Entity rigidbody component is invalid");
        }
        if (version >= 13
                && (!readFloatArray(input, entity.rigidbody.angularVelocity)
                    || !(input >> entity.rigidbody.angularDamping)
                    || !std::isfinite(entity.rigidbody.angularDamping)
                    || entity.rigidbody.angularDamping < 0.0f)) {
            return fail(error, "Entity angular rigidbody state is invalid");
        }
        if (!readTag(input, "SPHERE_COLLIDER", error)
                || !readBool(input, entity.sphereCollider.enabled)
                || !(input >> entity.sphereCollider.radius >> entity.sphereCollider.restitution
                            >> entity.sphereCollider.friction)
                || !readBool(input, entity.sphereCollider.trigger)
                || !std::isfinite(entity.sphereCollider.radius)
                || !std::isfinite(entity.sphereCollider.restitution)
                || !std::isfinite(entity.sphereCollider.friction)
                || (version >= 15
                    && (!readUint32(input, entity.sphereCollider.collisionLayer)
                        || !readUint32(input, entity.sphereCollider.collisionMask)
                        || entity.sphereCollider.collisionLayer == 0U))) {
            return fail(error, "Entity sphere collider component is invalid");
        }
        if (version >= 14
                && (!readTag(input, "BOX_COLLIDER", error)
                    || !readBool(input, entity.boxCollider.enabled)
                    || !readFloatArray(input, entity.boxCollider.halfExtents)
                    || !(input >> entity.boxCollider.restitution >> entity.boxCollider.friction)
                    || !readBool(input, entity.boxCollider.trigger)
                    || !std::isfinite(entity.boxCollider.restitution)
                    || !std::isfinite(entity.boxCollider.friction)
                    || (version >= 15
                        && (!readUint32(input, entity.boxCollider.collisionLayer)
                            || !readUint32(input, entity.boxCollider.collisionMask)
                            || entity.boxCollider.collisionLayer == 0U)))) {
            return fail(error, "Entity box collider component is invalid");
        }
    }
    if (version >= 3) {
        if (!readTag(input, "CHARACTER_CONTROLLER", error)
                || !readBool(input, entity.characterController.enabled)
                || !(input >> entity.characterController.moveInput[0]
                            >> entity.characterController.moveInput[1]
                            >> entity.characterController.maximumSpeed
                            >> entity.characterController.acceleration
                            >> entity.characterController.jumpSpeed)
                || (version >= 20
                    && (!(input >> entity.characterController.coyoteTimeSeconds
                                  >> entity.characterController.jumpBufferSeconds)
                        || !std::isfinite(entity.characterController.coyoteTimeSeconds)
                        || !std::isfinite(entity.characterController.jumpBufferSeconds)
                        || entity.characterController.coyoteTimeSeconds < 0.0f
                        || entity.characterController.coyoteTimeSeconds > 1.0f
                        || entity.characterController.jumpBufferSeconds < 0.0f
                        || entity.characterController.jumpBufferSeconds > 1.0f))
                || !readBool(input, entity.characterController.grounded)
                || !std::isfinite(entity.characterController.moveInput[0])
                || !std::isfinite(entity.characterController.moveInput[1])
                || !std::isfinite(entity.characterController.maximumSpeed)
                || !std::isfinite(entity.characterController.acceleration)
                || !std::isfinite(entity.characterController.jumpSpeed)) {
            return fail(error, "Entity character controller component is invalid");
        }
    }
    if (!readTag(input, "END_ENTITY", error)) return false;
    return true;
}
}  // namespace

bool SceneSerializer::save(const Scene& scene, std::ostream& output, std::string& error) {
    if (!validateScene(scene, scene.activeCameraId_, scene.nextId_, error)) return false;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "NUBY_SCENE " << kCurrentVersion << '\n';
    output << "SCENE " << scene.elapsedSeconds_ << ' ' << scene.activeCameraId_ << ' '
           << scene.nextId_ << ' ' << scene.entities_.size() << '\n';
    const PhysicsSettings& physics = scene.physicsSettings_;
    output << "PHYSICS";
    writeFloatArray(output, physics.gravity);
    output << ' ' << physics.fixedTimeStep << ' ' << physics.maximumSubsteps << ' '
           << (physics.floorEnabled ? 1 : 0) << ' ' << physics.floorHeight << ' '
           << physics.floorRestitution << ' ' << physics.floorFriction << ' '
           << physics.floorCollisionLayer << ' ' << physics.floorCollisionMask << '\n';
    output << "AMBIENT_SH";
    writeFloatArray(output, scene.ambientLighting_.sphericalHarmonics);
    output << '\n';
    const DirectionalLighting& directional = scene.directionalLighting_;
    output << "DIRECTIONAL_LIGHT";
    writeFloatArray(output, directional.direction);
    writeFloatArray(output, directional.color);
    output << ' ' << directional.intensity << ' ' << directional.shadowStrength << '\n';
    const AtmosphereSettings& atmosphere = scene.atmosphere_;
    output << "ATMOSPHERE " << (atmosphere.fogEnabled ? 1 : 0);
    writeFloatArray(output, atmosphere.fogColor);
    output << ' ' << atmosphere.fogStart << ' ' << atmosphere.fogEnd << '\n';
    const SkySettings& sky = scene.sky_;
    output << "SKY " << (sky.enabled ? 1 : 0) << ' '
           << (sky.sunDiskEnabled ? 1 : 0);
    writeFloatArray(output, sky.zenithColor);
    writeFloatArray(output, sky.horizonColor);
    writeFloatArray(output, sky.groundColor);
    writeFloatArray(output, sky.sunColor);
    output << ' ' << sky.sunAngularRadius << '\n';
    output << "SKY_AMBIENT " << sky.ambientStrength << '\n';
    for (const Entity& entity : scene.entities_) writeEntity(output, entity);
    output << "END_SCENE\n";
    if (!output) return fail(error, "Could not write complete scene data");
    error.clear();
    return true;
}

bool SceneSerializer::load(Scene& scene, std::istream& input, std::string& error) {
    input.imbue(std::locale::classic());
    if (!readTag(input, "NUBY_SCENE", error)) return false;
    unsigned int version = 0;
    if (!(input >> version) || version < 1 || version > kCurrentVersion) {
        return fail(error, "Unsupported scene file version");
    }

    if (!readTag(input, "SCENE", error)) return false;
    float elapsedSeconds = 0.0f;
    std::uint32_t activeCameraId = 0;
    std::uint32_t nextEntityId = 0;
    std::size_t entityCount = 0;
    if (!(input >> elapsedSeconds >> activeCameraId >> nextEntityId >> entityCount)
            || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0f
            || nextEntityId == 0 || nextEntityId == std::numeric_limits<std::uint32_t>::max()
            || entityCount > kMaximumEntities) {
        return fail(error, "Scene header is invalid or exceeds limits");
    }

    Scene candidate;
    if (version >= 2) {
        PhysicsSettings& physics = candidate.physicsSettings_;
        if (!readTag(input, "PHYSICS", error)
                || !readFloatArray(input, physics.gravity)
                || !(input >> physics.fixedTimeStep >> physics.maximumSubsteps)
                || !readBool(input, physics.floorEnabled)
                || !(input >> physics.floorHeight >> physics.floorRestitution >> physics.floorFriction)
                || !std::isfinite(physics.fixedTimeStep) || physics.fixedTimeStep <= 0.0f
                || physics.maximumSubsteps == 0 || physics.maximumSubsteps > 16
                || !std::isfinite(physics.floorHeight)
                || !std::isfinite(physics.floorRestitution)
                || physics.floorRestitution < 0.0f || physics.floorRestitution > 1.0f
                || !std::isfinite(physics.floorFriction) || physics.floorFriction < 0.0f
                || physics.floorFriction > 1.0f
                || (version >= 15
                    && (!readUint32(input, physics.floorCollisionLayer)
                        || !readUint32(input, physics.floorCollisionMask)
                        || physics.floorCollisionLayer == 0U))) {
            return fail(error, "Scene physics settings are invalid");
        }
    }
    if (version >= 16) {
        AmbientLighting& ambient = candidate.ambientLighting_;
        if (!readTag(input, "AMBIENT_SH", error)
                || !readFloatArray(input, ambient.sphericalHarmonics)) {
            return fail(error, "Scene ambient-light coefficients are invalid");
        }
        for (const float coefficient : ambient.sphericalHarmonics) {
            if (std::fabs(coefficient) > 1000.0f) {
                return fail(error, "Scene ambient-light coefficients are invalid");
            }
        }
    }
    if (version >= 23) {
        DirectionalLighting& directional = candidate.directionalLighting_;
        if (!readTag(input, "DIRECTIONAL_LIGHT", error)
                || !readFloatArray(input, directional.direction)
                || !readFloatArray(input, directional.color)
                || !(input >> directional.intensity >> directional.shadowStrength)) {
            return fail(error, "Scene directional-light settings are invalid");
        }
    }
    if (version >= 24) {
        AtmosphereSettings& atmosphere = candidate.atmosphere_;
        if (!readTag(input, "ATMOSPHERE", error)
                || !readBool(input, atmosphere.fogEnabled)
                || !readFloatArray(input, atmosphere.fogColor)
                || !(input >> atmosphere.fogStart >> atmosphere.fogEnd)) {
            return fail(error, "Scene atmosphere settings are invalid");
        }
    }
    if (version >= 25) {
        SkySettings& sky = candidate.sky_;
        if (!readTag(input, "SKY", error)
                || !readBool(input, sky.enabled) || !readBool(input, sky.sunDiskEnabled)
                || !readFloatArray(input, sky.zenithColor)
                || !readFloatArray(input, sky.horizonColor)
                || !readFloatArray(input, sky.groundColor)
                || !readFloatArray(input, sky.sunColor)
                || !(input >> sky.sunAngularRadius)) {
            return fail(error, "Scene sky settings are invalid");
        }
    }
    if (version >= 26) {
        if (!readTag(input, "SKY_AMBIENT", error)
                || !(input >> candidate.sky_.ambientStrength)) {
            return fail(error, "Scene ambient sky lighting is invalid");
        }
    }
    std::uint32_t maximumId = 0;
    std::size_t totalKeyframes = 0;
    for (std::size_t index = 0; index < entityCount; ++index) {
        if (!readEntity(input, candidate, maximumId, totalKeyframes, version, error)) return false;
    }
    if (!readTag(input, "END_SCENE", error)) return false;
    std::string trailingData;
    if (input >> trailingData) return fail(error, "Unexpected data follows scene terminator");

    for (const Entity& entity : candidate.entities_) {
        if (entity.parentId != 0 && !candidate.setParent(entity.id, entity.parentId)) {
            return fail(error, "Scene hierarchy contains an invalid parent or cycle");
        }
    }
    if (activeCameraId != 0 && !candidate.setActiveCamera(activeCameraId)) {
        return fail(error, "Scene active-camera reference is invalid");
    }
    candidate.activeCameraId_ = activeCameraId;
    candidate.elapsedSeconds_ = elapsedSeconds;
    if (maximumId == std::numeric_limits<std::uint32_t>::max()) {
        return fail(error, "Maximum entity ID is reserved");
    }
    candidate.nextId_ = nextEntityId;
    if (!validateScene(candidate, activeCameraId, nextEntityId, error)) return false;

    scene = std::move(candidate);
    error.clear();
    return true;
}

}  // namespace nuby
