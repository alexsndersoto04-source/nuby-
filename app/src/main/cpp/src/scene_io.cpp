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
    return true;
}

bool readTextureMapTransform(std::istream& input, TextureMapTransform& transform,
                             bool hasSamplerSettings) {
    return readFloatArray(input, transform.uvScale)
            && readFloatArray(input, transform.uvOffset)
            && validTextureMapTransform(transform)
            && (!hasSamplerSettings || readBool(input, transform.clampToEdge));
}

void writeTextureMapTransform(std::ostream& output,
                              const TextureMapTransform& transform) {
    writeFloatArray(output, transform.uvScale);
    writeFloatArray(output, transform.uvOffset);
    output << ' ' << (transform.clampToEdge ? 1 : 0);
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
            || !validTexturePath(entity.material.albedoTexture)
            || !validTexturePath(entity.material.normalTexture)
            || !validTexturePath(entity.material.metallicRoughnessTexture)
            || !validTexturePath(entity.material.metallicTexture)
            || !validTexturePath(entity.material.roughnessTexture)
            || !validTexturePath(entity.material.occlusionTexture)
            || static_cast<unsigned int>(entity.primitive)
                    > static_cast<unsigned int>(Primitive::None)) {
        return false;
    }
    if (!validTextureMapTransform(entity.material.albedoMapTransform)
            || !validTextureMapTransform(entity.material.normalMapTransform)
            || !validTextureMapTransform(entity.material.metallicRoughnessMapTransform)
            || !validTextureMapTransform(entity.material.metallicMapTransform)
            || !validTextureMapTransform(entity.material.roughnessMapTransform)
            || !validTextureMapTransform(entity.material.occlusionMapTransform)) {
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
            || !finite3(entity.spin.radiansPerSecond) || !finite3(entity.orbit.center)
            || !finite(entity.orbit.radius) || !finite(entity.orbit.angularSpeed)
            || !finite(entity.orbit.phase) || !finite(entity.orbit.verticalAmplitude)
            || !finite(entity.orbit.verticalFrequency) || !finite3(entity.pulse.baseScale)
            || !finite(entity.pulse.amplitude) || !finite(entity.pulse.frequency)
            || !finite(entity.pulse.phase) || !finite(entity.animation.playbackRate)
            || !finite(entity.animation.timeSeconds) || !finite(entity.animation.durationSeconds)
            || !finite(entity.rigidbody.mass) || !finite3(entity.rigidbody.velocity)
            || !finite(entity.rigidbody.linearDamping) || !finite(entity.rigidbody.gravityScale)
            || !finite(entity.sphereCollider.radius)
            || !finite(entity.sphereCollider.restitution)
            || !finite(entity.sphereCollider.friction)
            || !finite(entity.characterController.moveInput[0])
            || !finite(entity.characterController.moveInput[1])
            || !finite(entity.characterController.maximumSpeed)
            || !finite(entity.characterController.acceleration)
            || !finite(entity.characterController.jumpSpeed)
            || !finite3(entity.pointLight.color) || !finite(entity.pointLight.intensity)
            || !finite(entity.pointLight.range) || !finite(entity.camera.verticalFovRadians)
            || !finite(entity.camera.nearPlane) || !finite(entity.camera.farPlane)
            || !finite3(entity.orbitCamera.target) || !finite(entity.orbitCamera.distance)
            || !finite(entity.orbitCamera.minimumDistance)
            || !finite(entity.orbitCamera.maximumDistance) || !finite(entity.orbitCamera.yaw)
            || !finite(entity.orbitCamera.pitch) || !finite(entity.orbitCamera.minimumPitch)
            || !finite(entity.orbitCamera.maximumPitch) || !finite(entity.orbitCamera.yawPerPixel)
            || !finite(entity.orbitCamera.pitchPerPixel)) {
        return false;
    }
    if ((entity.rigidbody.enabled && entity.rigidbody.mass < 1.0e-6f)
            || entity.rigidbody.linearDamping < 0.0f
            || (entity.sphereCollider.enabled && entity.sphereCollider.radius <= 0.0f)
            || entity.sphereCollider.restitution < 0.0f || entity.sphereCollider.restitution > 1.0f
            || entity.sphereCollider.friction < 0.0f || entity.sphereCollider.friction > 1.0f
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
            || entity.characterController.jumpSpeed < 0.0f) {
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
    const PhysicsSettings& physics = scene.physicsSettings();
    if (!std::isfinite(physics.gravity[0]) || !std::isfinite(physics.gravity[1])
            || !std::isfinite(physics.gravity[2]) || !std::isfinite(physics.fixedTimeStep)
            || physics.fixedTimeStep <= 0.0f || physics.maximumSubsteps == 0
            || physics.maximumSubsteps > 16 || !std::isfinite(physics.floorHeight)
            || !std::isfinite(physics.floorRestitution)
            || physics.floorRestitution < 0.0f || physics.floorRestitution > 1.0f
            || !std::isfinite(physics.floorFriction) || physics.floorFriction < 0.0f
            || physics.floorFriction > 1.0f) {
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
           << std::quoted(entity.material.roughnessTexture) << '\n';
    output << "OVERRIDE_MTL_MATERIALS "
           << (entity.overrideMtlMaterials ? 1 : 0) << '\n';
    output << "MATERIAL_MAP_UV";
    writeTextureMapTransform(output, entity.material.albedoMapTransform);
    writeTextureMapTransform(output, entity.material.normalMapTransform);
    writeTextureMapTransform(output, entity.material.metallicRoughnessMapTransform);
    writeTextureMapTransform(output, entity.material.metallicMapTransform);
    writeTextureMapTransform(output, entity.material.roughnessMapTransform);
    writeTextureMapTransform(output, entity.material.occlusionMapTransform);
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
    output << "RIGIDBODY " << (entity.rigidbody.enabled ? 1 : 0) << ' '
           << entity.rigidbody.mass;
    writeFloatArray(output, entity.rigidbody.velocity);
    output << ' ' << entity.rigidbody.linearDamping << ' ' << entity.rigidbody.gravityScale << '\n';

    output << "SPHERE_COLLIDER " << (entity.sphereCollider.enabled ? 1 : 0) << ' '
           << entity.sphereCollider.radius << ' ' << entity.sphereCollider.restitution << ' '
           << entity.sphereCollider.friction << ' '
           << (entity.sphereCollider.trigger ? 1 : 0) << '\n';

    output << "CHARACTER_CONTROLLER " << (entity.characterController.enabled ? 1 : 0)
           << ' ' << entity.characterController.moveInput[0]
           << ' ' << entity.characterController.moveInput[1]
           << ' ' << entity.characterController.maximumSpeed
           << ' ' << entity.characterController.acceleration
           << ' ' << entity.characterController.jumpSpeed
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
                        version >= 8)
                || !readTextureMapTransform(input, entity.material.normalMapTransform,
                        version >= 8)
                || !readTextureMapTransform(input,
                        entity.material.metallicRoughnessMapTransform, version >= 8)
                || !readTextureMapTransform(input, entity.material.metallicMapTransform,
                        version >= 8)
                || !readTextureMapTransform(input, entity.material.roughnessMapTransform,
                        version >= 8)
                || !readTextureMapTransform(input, entity.material.occlusionMapTransform,
                        version >= 8))) {
        return fail(error, "Material UV transform or sampler settings are invalid");
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
        if (!readTag(input, "SPHERE_COLLIDER", error)
                || !readBool(input, entity.sphereCollider.enabled)
                || !(input >> entity.sphereCollider.radius >> entity.sphereCollider.restitution
                            >> entity.sphereCollider.friction)
                || !readBool(input, entity.sphereCollider.trigger)
                || !std::isfinite(entity.sphereCollider.radius)
                || !std::isfinite(entity.sphereCollider.restitution)
                || !std::isfinite(entity.sphereCollider.friction)) {
            return fail(error, "Entity sphere collider component is invalid");
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
           << physics.floorRestitution << ' ' << physics.floorFriction << '\n';
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
                || physics.floorFriction > 1.0f) {
            return fail(error, "Scene physics settings are invalid");
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
