#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nuby {

enum class Primitive : std::uint8_t {
    Sphere,
    Torus,
    Ground,
    ImportedMesh,
    None
};

struct Transform {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
};

struct Material {
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    std::array<float, 3> emission{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.58f;
    float ambientOcclusion = 1.0f;
    bool proceduralGrid = false;
};

struct SpinComponent {
    std::array<float, 3> radiansPerSecond{0.0f, 0.0f, 0.0f};
};

struct OrbitComponent {
    bool enabled = false;
    std::array<float, 3> center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    float angularSpeed = 0.0f;
    float phase = 0.0f;
    float verticalAmplitude = 0.0f;
    float verticalFrequency = 1.0f;
};

struct PulseComponent {
    bool enabled = false;
    std::array<float, 3> baseScale{1.0f, 1.0f, 1.0f};
    float amplitude = 0.0f;
    float frequency = 1.0f;
    float phase = 0.0f;
};

struct Vector3Keyframe {
    float timeSeconds = 0.0f;
    std::array<float, 3> value{0.0f, 0.0f, 0.0f};
};

struct Vector3Track {
    std::vector<Vector3Keyframe> keyframes;
    bool addKeyframe(float timeSeconds, const std::array<float, 3>& value);
    std::array<float, 3> sample(float timeSeconds,
                                const std::array<float, 3>& fallback) const;
    float durationSeconds() const;
};

struct AnimationComponent {
    bool enabled = false;
    bool playing = true;
    bool looping = true;
    float playbackRate = 1.0f;
    float timeSeconds = 0.0f;
    float durationSeconds = 0.0f;
    Vector3Track position;
    Vector3Track rotation;
    Vector3Track scale;
};

struct CameraComponent {
    bool enabled = false;
    float verticalFovRadians = 0.76f;
    float nearPlane = 0.1f;
    float farPlane = 80.0f;
};

struct OrbitCameraController {
    bool enabled = false;
    std::array<float, 3> target{0.0f, 0.0f, 0.0f};
    float distance = 5.4f;
    float minimumDistance = 2.8f;
    float maximumDistance = 10.0f;
    float yaw = 0.45f;
    float pitch = 0.18f;
    float minimumPitch = -0.62f;
    float maximumPitch = 0.62f;
    float yawPerPixel = 0.006f;
    float pitchPerPixel = 0.004f;
};

struct Entity {
    std::uint32_t id = 0;
    std::uint32_t parentId = 0;
    std::string name;
    std::string meshAsset;
    Primitive primitive = Primitive::Sphere;
    Transform transform;
    Material material;
    SpinComponent spin;
    OrbitComponent orbit;
    PulseComponent pulse;
    AnimationComponent animation;
    CameraComponent camera;
    OrbitCameraController orbitCamera;
    bool visible = true;
};

// Scene state is platform independent. Entity transforms are local to their
// parent; rendering consumes the composed world matrix.
class Scene final {
public:
    Entity& create(std::string name, Primitive primitive);
    Entity* find(std::uint32_t id);
    const Entity* find(std::uint32_t id) const;
    bool setParent(std::uint32_t childId, std::uint32_t parentId);
    bool setActiveCamera(std::uint32_t id);
    Entity* activeCamera();
    const Entity* activeCamera() const;
    bool orbitActiveCamera(float deltaX, float deltaY);
    bool zoomActiveCamera(float distanceFactor);
    std::array<float, 16> worldMatrix(std::uint32_t id) const;
    void update(float deltaSeconds);
    void clear();

    const std::vector<Entity>& entities() const { return entities_; }
    std::vector<Entity>& entities() { return entities_; }
    float elapsedSeconds() const { return elapsedSeconds_; }

private:
    std::vector<Entity> entities_;
    std::uint32_t nextId_ = 1;
    std::uint32_t activeCameraId_ = 0;
    float elapsedSeconds_ = 0.0f;
};

}  // namespace nuby
