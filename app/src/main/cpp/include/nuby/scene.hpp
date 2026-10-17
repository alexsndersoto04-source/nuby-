#pragma once

#include <array>
#include <cstddef>
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

struct TextureMapTransform {
    std::array<float, 2> uvScale{1.0f, 1.0f};
    std::array<float, 2> uvOffset{0.0f, 0.0f};
    bool clampToEdge = false;
};

// Wavefront -mm range adjustment applied as clamp(base + gain * sample, 0, 1).
struct TextureMapValueRange {
    float base = 0.0f;
    float gain = 1.0f;
};

enum class ScalarTextureChannel : std::uint8_t {
    Red,
    Green,
    Blue,
    Matte,
    Luminance
};

struct Material {
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    std::array<float, 3> emission{0.0f, 0.0f, 0.0f};
    float metallic = 0.0f;
    float roughness = 0.58f;
    float opacity = 1.0f;
    float ambientOcclusion = 1.0f;
    bool proceduralGrid = false;
    std::string albedoTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    std::string metallicTexture;
    std::string roughnessTexture;
    std::string occlusionTexture;
    std::string opacityTexture;
    TextureMapTransform albedoMapTransform;
    TextureMapTransform normalMapTransform;
    TextureMapTransform metallicRoughnessMapTransform;
    TextureMapTransform metallicMapTransform;
    TextureMapTransform roughnessMapTransform;
    TextureMapTransform occlusionMapTransform;
    TextureMapTransform opacityMapTransform;
    TextureMapValueRange albedoMapValueRange;
    TextureMapValueRange normalMapValueRange;
    TextureMapValueRange metallicRoughnessMapValueRange;
    TextureMapValueRange metallicMapValueRange;
    TextureMapValueRange roughnessMapValueRange;
    TextureMapValueRange occlusionMapValueRange;
    TextureMapValueRange opacityMapValueRange;
    ScalarTextureChannel opacityMapChannel = ScalarTextureChannel::Luminance;
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

struct RigidbodyComponent {
    bool enabled = false;
    float mass = 1.0f;
    std::array<float, 3> velocity{0.0f, 0.0f, 0.0f};
    float linearDamping = 0.02f;
    float gravityScale = 1.0f;
};

struct SphereColliderComponent {
    bool enabled = false;
    float radius = 0.5f;
    float restitution = 0.2f;
    float friction = 0.35f;
    bool trigger = false;
};

struct CharacterControllerComponent {
    bool enabled = false;
    std::array<float, 2> moveInput{0.0f, 0.0f};
    float maximumSpeed = 4.0f;
    float acceleration = 24.0f;
    float jumpSpeed = 5.5f;
    bool grounded = false;
};

struct PhysicsSettings {
    std::array<float, 3> gravity{0.0f, -9.81f, 0.0f};
    float fixedTimeStep = 1.0f / 60.0f;
    unsigned int maximumSubsteps = 4;
    bool floorEnabled = false;
    float floorHeight = 0.0f;
    float floorRestitution = 0.2f;
    float floorFriction = 0.35f;
};

// Trigger transitions are emitted at fixed simulation steps; consume them after
// Scene::update. The queue is bounded to protect long-running scenes.
enum class PhysicsEventType : std::uint8_t {
    TriggerEnter,
    TriggerExit
};

struct PhysicsEvent {
    PhysicsEventType type = PhysicsEventType::TriggerEnter;
    std::uint32_t firstEntityId = 0;
    std::uint32_t secondEntityId = 0;
};

struct PointLightComponent {
    bool enabled = false;
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 5.0f;
};

struct PointLightData {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 5.0f;
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
    RigidbodyComponent rigidbody;
    SphereColliderComponent sphereCollider;
    CharacterControllerComponent characterController;
    PointLightComponent pointLight;
    CameraComponent camera;
    OrbitCameraController orbitCamera;
    bool visible = true;
    bool overrideMtlMaterials = false;
};

// Scene state is platform independent. Entity transforms are local to their
// parent; rendering consumes the composed world matrix.
class Scene final {
public:
    Entity& create(std::string name, Primitive primitive);
    bool destroyEntity(std::uint32_t id);
    Entity* find(std::uint32_t id);
    const Entity* find(std::uint32_t id) const;
    bool setParent(std::uint32_t childId, std::uint32_t parentId);
    bool setActiveCamera(std::uint32_t id);
    bool setCharacterMoveInput(std::uint32_t id, float horizontal, float forward);
    bool jumpCharacter(std::uint32_t id);
    Entity* activeCamera();
    const Entity* activeCamera() const;
    bool orbitActiveCamera(float deltaX, float deltaY);
    bool zoomActiveCamera(float distanceFactor);
    void collectPointLights(std::size_t maximum, std::vector<PointLightData>& output) const;
    std::array<float, 16> worldMatrix(std::uint32_t id) const;
    void update(float deltaSeconds);
    void clear();

    const std::vector<Entity>& entities() const { return entities_; }
    std::vector<Entity>& entities() { return entities_; }
    float elapsedSeconds() const { return elapsedSeconds_; }
    PhysicsSettings& physicsSettings() { return physicsSettings_; }
    const PhysicsSettings& physicsSettings() const { return physicsSettings_; }
    void consumePhysicsEvents(std::vector<PhysicsEvent>& output);

private:
    void simulatePhysics(float deltaSeconds);
    friend class SceneSerializer;

    std::vector<Entity> entities_;
    std::uint32_t nextId_ = 1;
    std::uint32_t activeCameraId_ = 0;
    float elapsedSeconds_ = 0.0f;
    PhysicsSettings physicsSettings_;
    float physicsAccumulator_ = 0.0f;
    std::vector<std::uint64_t> activeTriggerPairs_;
    std::vector<std::uint64_t> triggerPairsScratch_;
    std::vector<std::size_t> colliderIndicesScratch_;
    std::vector<PhysicsEvent> physicsEvents_;
};

}  // namespace nuby
