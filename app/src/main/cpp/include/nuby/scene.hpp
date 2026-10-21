#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nuby/audio_engine.hpp"

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
    float uvRotation = 0.0f;
    std::uint8_t uvSet = 0U;
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
    float normalMapScale = 1.0f;
    float emissiveStrength = 1.0f;
    float indexOfRefraction = 1.5f;
    float clearcoatFactor = 0.0f;
    float clearcoatRoughness = 0.0f;
    float clearcoatNormalScale = 1.0f;
    float specularFactor = 1.0f;
    std::array<float, 3> specularColorFactor{1.0f, 1.0f, 1.0f};
    std::array<float, 3> sheenColorFactor{0.0f, 0.0f, 0.0f};
    float sheenRoughness = 0.0f;
    bool unlit = false;
    bool proceduralGrid = false;
    std::string albedoTexture;
    std::string normalTexture;
    std::string metallicRoughnessTexture;
    std::string metallicTexture;
    std::string roughnessTexture;
    std::string occlusionTexture;
    std::string opacityTexture;
    std::string emissiveTexture;
    std::string clearcoatTexture;
    std::string clearcoatRoughnessTexture;
    std::string clearcoatNormalTexture;
    std::string specularTexture;
    std::string specularColorTexture;
    std::string sheenColorTexture;
    std::string sheenRoughnessTexture;
    TextureMapTransform albedoMapTransform;
    TextureMapTransform normalMapTransform;
    TextureMapTransform metallicRoughnessMapTransform;
    TextureMapTransform metallicMapTransform;
    TextureMapTransform roughnessMapTransform;
    TextureMapTransform occlusionMapTransform;
    TextureMapTransform opacityMapTransform;
    TextureMapTransform emissiveMapTransform;
    TextureMapTransform clearcoatMapTransform;
    TextureMapTransform clearcoatRoughnessMapTransform;
    TextureMapTransform clearcoatNormalMapTransform;
    TextureMapTransform specularMapTransform;
    TextureMapTransform specularColorMapTransform;
    TextureMapTransform sheenColorMapTransform;
    TextureMapTransform sheenRoughnessMapTransform;
    TextureMapValueRange albedoMapValueRange;
    TextureMapValueRange normalMapValueRange;
    TextureMapValueRange metallicRoughnessMapValueRange;
    TextureMapValueRange metallicMapValueRange;
    TextureMapValueRange roughnessMapValueRange;
    TextureMapValueRange occlusionMapValueRange;
    TextureMapValueRange opacityMapValueRange;
    TextureMapValueRange emissiveMapValueRange;
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

// Playback state for a clip stored with an external skeletal mesh asset.
// The renderer/importer resolves clipName and computes the joint palette.
struct SkeletalAnimationComponent {
    bool enabled = false;
    bool playing = true;
    bool looping = true;
    float playbackRate = 1.0f;
    float timeSeconds = 0.0f;
    float durationSeconds = 0.0f;
    std::string clipName;
    std::string transitionFromClip;
    float transitionFromTimeSeconds = 0.0f;
    float transitionElapsedSeconds = 0.0f;
    bool transitionFromLooping = true;
};

// Selects animation clips from a parent character controller's movement. The
// renderer resolves clip names from the skinned mesh asset.
struct SkeletalLocomotionComponent {
    bool enabled = false;
    std::string idleClip = "idle";
    std::string walkClip = "walk";
    std::string runClip = "run";
    std::string jumpClip = "jump";
    float walkSpeedThreshold = 0.18f;
    float runSpeedThreshold = 2.4f;
    float transitionDurationSeconds = 0.18f;
};

struct RigidbodyComponent {
    bool enabled = false;
    float mass = 1.0f;
    std::array<float, 3> velocity{0.0f, 0.0f, 0.0f};
    // World-space radians per second. Sphere contacts apply frictional torque.
    std::array<float, 3> angularVelocity{0.0f, 0.0f, 0.0f};
    float linearDamping = 0.02f;
    float angularDamping = 0.05f;
    float gravityScale = 1.0f;
};

// Collider pairs require reciprocal mask matches: each mask must include the
// other collider's nonzero category layer. A zero mask disables pair contacts.
struct SphereColliderComponent {
    bool enabled = false;
    float radius = 0.5f;
    float restitution = 0.2f;
    float friction = 0.35f;
    bool trigger = false;
    std::uint32_t collisionLayer = 1U;
    std::uint32_t collisionMask = 0xFFFFFFFFU;
};

// Oriented box collider. Half-extents are scaled by the entity's absolute
// scale and aligned by its world rotation; physics bodies remain root-only.
struct BoxColliderComponent {
    bool enabled = false;
    std::array<float, 3> halfExtents{0.5f, 0.5f, 0.5f};
    float restitution = 0.2f;
    float friction = 0.35f;
    bool trigger = false;
    std::uint32_t collisionLayer = 1U;
    std::uint32_t collisionMask = 0xFFFFFFFFU;
};

struct CharacterControllerComponent {
    bool enabled = false;
    std::array<float, 2> moveInput{0.0f, 0.0f};
    float maximumSpeed = 4.0f;
    float acceleration = 24.0f;
    float jumpSpeed = 5.5f;
    float coyoteTimeSeconds = 0.10f;
    float jumpBufferSeconds = 0.16f;
    float coyoteTimeRemaining = 0.0f;
    float jumpBufferRemaining = 0.0f;
    bool grounded = false;
};

// Global diffuse environment lighting stored as nine RGB spherical-harmonic
// coefficients. Basis order: Y00, Y1y, Y1z, Y1x, Y2xy, Y2yz, Y2z2, Y2xz, Y2x2-y2.
// The defaults preserve the renderer's previous sky/ground ambient gradient.
struct AmbientLighting {
    std::array<float, 27> sphericalHarmonics{
        0.52287f, 0.66467f, 1.06347f,
        0.14828f, 0.20958f, 0.32747f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f
    };
};

// Scene-authored sun used for PBR direct lighting and cascaded shadow projection.
// direction points along the rays (from the sun toward the world).
struct DirectionalLighting {
    std::array<float, 3> direction{-0.45f, -0.82f, -0.34f};
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float shadowStrength = 0.82f;
};

struct AtmosphereSettings {
    bool fogEnabled = false;
    std::array<float, 3> fogColor{0.12f, 0.17f, 0.24f};
    float fogStart = 24.0f;
    float fogEnd = 72.0f;
};

struct SkySettings {
    bool enabled = false;
    bool sunDiskEnabled = true;
    std::array<float, 3> zenithColor{0.075f, 0.16f, 0.31f};
    std::array<float, 3> horizonColor{0.48f, 0.57f, 0.66f};
    std::array<float, 3> groundColor{0.12f, 0.14f, 0.16f};
    std::array<float, 3> sunColor{1.0f, 0.82f, 0.58f};
    float sunAngularRadius = 0.018f;
    float ambientStrength = 0.0f;
};

struct PhysicsSettings {
    // The implicit world floor participates in the same reciprocal layer/mask filter.
    std::array<float, 3> gravity{0.0f, -9.81f, 0.0f};
    float fixedTimeStep = 1.0f / 60.0f;
    unsigned int maximumSubsteps = 4;
    bool floorEnabled = false;
    float floorHeight = 0.0f;
    float floorRestitution = 0.2f;
    float floorFriction = 0.35f;
    std::uint32_t floorCollisionLayer = 1U;
    std::uint32_t floorCollisionMask = 0xFFFFFFFFU;
};

// Trigger and solid-collision transitions are emitted at fixed simulation steps;
// consume them after Scene::update. The queue is bounded to protect long-running scenes.
enum class PhysicsEventType : std::uint8_t {
    TriggerEnter,
    TriggerExit,
    CollisionEnter,
    CollisionExit
};

struct PhysicsEvent {
    PhysicsEventType type = PhysicsEventType::TriggerEnter;
    std::uint32_t firstEntityId = 0;
    std::uint32_t secondEntityId = 0;
};

struct RaycastHit {
    std::uint32_t entityId = 0;
    float distance = 0.0f;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 0.0f, 0.0f};
};

// Sphere sweep result; position is the contact point on the target collider.
struct SphereCastHit {
    std::uint32_t entityId = 0;
    float distance = 0.0f;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 0.0f, 0.0f};
};

struct CapsuleCastHit {
    std::uint32_t entityId = 0;
    float distance = 0.0f;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 0.0f, 0.0f};
};

struct BoxCastHit {
    std::uint32_t entityId = 0;
    float distance = 0.0f;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{0.0f, 0.0f, 0.0f};
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
    // A spring arm retracts against collider queries; smoothing only applies while
    // recovering so an obstruction never pulls the camera through a wall.
    bool collisionEnabled = false;
    float collisionRadius = 0.22f;
    float collisionPadding = 0.16f;
    float collisionMinimumDistance = 0.28f;
    float collisionRecoverySpeed = 9.0f;
    std::uint32_t collisionLayer = 4U;
    std::uint32_t collisionMask = 0xFFFFFFFFU;
    float resolvedDistance = -1.0f;  // Runtime-only; reinitialized from distance.
    float lastRequestedDistance = -1.0f;  // Runtime-only; distinguishes zoom from recovery.
};

// Serialized audio emitter settings; playback handles remain renderer/runtime state.
struct AudioSourceComponent {
    bool enabled = false;
    std::string assetPath;
    AudioBus bus = AudioBus::Effects;
    float gain = 1.0f;
    std::uint8_t priority = 64U;
    bool looping = false;
    bool positional = true;
    bool playOnAwake = true;
    float referenceDistance = 1.0f;
    float maximumDistance = 24.0f;
    float rolloff = 1.0f;
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
    SkeletalAnimationComponent skeletalAnimation;
    SkeletalLocomotionComponent skeletalLocomotion;
    RigidbodyComponent rigidbody;
    SphereColliderComponent sphereCollider;
    BoxColliderComponent boxCollider;
    CharacterControllerComponent characterController;
    PointLightComponent pointLight;
    CameraComponent camera;
    OrbitCameraController orbitCamera;
    AudioSourceComponent audioSource;
    bool visible = true;
    bool overrideMtlMaterials = false;
};

// Scene state is platform independent. Entity transforms are local to their
// parent; rendering consumes the composed world matrix.
class Scene final {
public:
    Entity& create(std::string name, Primitive primitive);
    // Appends a validated scene fragment, remapping IDs and parent references.
    // Camera-bearing fragments are rejected so they cannot replace scene state.
    bool appendEntities(const Scene& fragment, std::vector<std::uint32_t>& appendedIds);
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
    // Direction is normalized internally; the distance is measured in world units.
    // Enabled triggers are queryable; query and collider filters must reciprocally match.
    // Queries cover root-level entity colliders only, not the implicit world floor.
    bool raycast(const std::array<float, 3>& origin,
                 const std::array<float, 3>& direction, float maximumDistance,
                 RaycastHit& hit, std::uint32_t queryLayer = 1U,
                 std::uint32_t queryMask = 0xFFFFFFFFU) const;
    // Returns all filtered hits sorted by distance; ties keep scene order.
    std::size_t raycastAll(const std::array<float, 3>& origin,
                           const std::array<float, 3>& direction, float maximumDistance,
                           std::vector<RaycastHit>& hits, std::uint32_t queryLayer = 1U,
                           std::uint32_t queryMask = 0xFFFFFFFFU) const;
    // Sweeps a fixed-radius sphere; starting overlaps report distance zero.
    // Position is the target-surface contact point; triggers and reciprocal filters apply.
    bool sphereCast(const std::array<float, 3>& origin,
                    const std::array<float, 3>& direction, float radius,
                    float maximumDistance, SphereCastHit& hit,
                    std::uint32_t queryLayer = 1U,
                    std::uint32_t queryMask = 0xFFFFFFFFU) const;
    // Sweeps a capsule segment by translating both endpoints; returns target-surface contact.
    bool capsuleCast(const std::array<float, 3>& start, const std::array<float, 3>& end,
                     const std::array<float, 3>& direction, float radius,
                     float maximumDistance, CapsuleCastHit& hit,
                     std::uint32_t queryLayer = 1U,
                     std::uint32_t queryMask = 0xFFFFFFFFU) const;
    // Sweeps a fixed-orientation OBB; no continuous rotation is applied during the cast.
    bool boxCast(const std::array<float, 3>& center,
                 const std::array<float, 3>& halfExtents,
                 const std::array<float, 3>& rotation,
                 const std::array<float, 3>& direction, float maximumDistance,
                 BoxCastHit& hit, std::uint32_t queryLayer = 1U,
                 std::uint32_t queryMask = 0xFFFFFFFFU) const;
    // Overlap queries include triggers; touching without penetration is excluded.
    std::size_t overlapSphere(const std::array<float, 3>& center, float radius,
                              std::vector<std::uint32_t>& entityIds,
                              std::uint32_t queryLayer = 1U,
                              std::uint32_t queryMask = 0xFFFFFFFFU) const;
    std::size_t overlapBox(const std::array<float, 3>& center,
                           const std::array<float, 3>& halfExtents,
                           const std::array<float, 3>& rotation,
                           std::vector<std::uint32_t>& entityIds,
                           std::uint32_t queryLayer = 1U,
                           std::uint32_t queryMask = 0xFFFFFFFFU) const;
    // Capsule endpoints form the center segment; tangential contact is excluded.
    std::size_t overlapCapsule(const std::array<float, 3>& start,
                               const std::array<float, 3>& end, float radius,
                               std::vector<std::uint32_t>& entityIds,
                               std::uint32_t queryLayer = 1U,
                               std::uint32_t queryMask = 0xFFFFFFFFU) const;
    std::array<float, 16> worldMatrix(std::uint32_t id) const;
    void update(float deltaSeconds);
    // Re-evaluate camera occlusion after external code edits targets/transforms.
    void refreshOrbitCameraCollisions(float deltaSeconds);
    void clear();

    const std::vector<Entity>& entities() const { return entities_; }
    std::vector<Entity>& entities() { return entities_; }
    float elapsedSeconds() const { return elapsedSeconds_; }
    AmbientLighting& ambientLighting() { return ambientLighting_; }
    const AmbientLighting& ambientLighting() const { return ambientLighting_; }
    DirectionalLighting& directionalLighting() { return directionalLighting_; }
    const DirectionalLighting& directionalLighting() const { return directionalLighting_; }
    AtmosphereSettings& atmosphere() { return atmosphere_; }
    const AtmosphereSettings& atmosphere() const { return atmosphere_; }
    SkySettings& sky() { return sky_; }
    const SkySettings& sky() const { return sky_; }
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
    AmbientLighting ambientLighting_;
    DirectionalLighting directionalLighting_;
    AtmosphereSettings atmosphere_;
    SkySettings sky_;
    PhysicsSettings physicsSettings_;
    float physicsAccumulator_ = 0.0f;
    std::vector<std::uint64_t> activeTriggerPairs_;
    std::vector<std::uint64_t> triggerPairsScratch_;
    std::vector<std::uint64_t> activeCollisionPairs_;
    std::vector<std::uint64_t> collisionPairsScratch_;
    std::vector<std::size_t> colliderIndicesScratch_;
    std::vector<std::size_t> dynamicColliderIndicesScratch_;
    std::vector<PhysicsEvent> physicsEvents_;
};

}  // namespace nuby
