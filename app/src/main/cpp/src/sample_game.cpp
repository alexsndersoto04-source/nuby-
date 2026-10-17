#include "nuby/sample_game.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace nuby {
namespace {
constexpr float kFloorHeight = -1.22f;
constexpr float kPlayerHeight = -0.84f;
constexpr float kLaneSpacing = 1.3f;
constexpr float kSpawnZ = -11.5f;
constexpr float kPlayerZ = 0.0f;
constexpr float kObjectSpeed = 5.2f;
constexpr float kSpawnInterval = 1.1f;
constexpr float kObjectCleanupZ = 1.4f;
constexpr float kTau = 6.28318530717958647692f;
}  // namespace

void SampleGame::buildScene(Scene& scene) {
    scene.clear();
    PhysicsSettings& physics = scene.physicsSettings();
    physics.gravity = {0.0f, -9.81f, 0.0f};
    physics.floorEnabled = true;
    physics.floorHeight = kFloorHeight;
    physics.floorRestitution = 0.0f;
    physics.floorFriction = 0.0f;

    Entity& camera = scene.create("Runner Camera", Primitive::None);
    const std::uint32_t cameraId = camera.id;
    camera.visible = false;
    camera.transform.position = {0.0f, 0.65f, 6.3f};
    camera.camera.enabled = true;
    camera.camera.verticalFovRadians = 0.76f;
    camera.camera.nearPlane = 0.1f;
    camera.camera.farPlane = 80.0f;
    scene.setActiveCamera(cameraId);

    Entity& ground = scene.create("Neon Track", Primitive::Ground);
    ground.material.color = {0.018f, 0.035f, 0.075f};
    ground.material.proceduralGrid = true;

    Entity& distantGate = scene.create("Aether Gate", Primitive::Torus);
    distantGate.transform.position = {0.0f, -0.1f, -13.5f};
    distantGate.transform.rotation[0] = 1.57079632679f;
    distantGate.transform.scale = {2.1f, 2.1f, 2.1f};
    distantGate.material.color = {0.08f, 0.72f, 0.78f};
    distantGate.material.emission = {0.025f, 0.22f, 0.26f};
    distantGate.material.metallic = 0.35f;
    distantGate.material.roughness = 0.24f;
    distantGate.spin.radiansPerSecond[2] = 0.08f;

    Entity& farGate = scene.create("Far Gate", Primitive::Torus);
    farGate.transform.position = {0.0f, 0.15f, -23.0f};
    farGate.transform.rotation[0] = 1.57079632679f;
    farGate.transform.scale = {2.7f, 2.7f, 2.7f};
    farGate.material.color = {0.88f, 0.4f, 0.18f};
    farGate.material.emission = {0.18f, 0.04f, 0.008f};
    farGate.material.metallic = 0.42f;
    farGate.material.roughness = 0.3f;

    Entity& player = scene.create("Aether Runner", Primitive::Sphere);
    playerId_ = player.id;
    player.transform.position = {0.0f, kPlayerHeight, kPlayerZ};
    player.transform.scale = {0.38f, 0.38f, 0.38f};
    player.rigidbody.enabled = true;
    player.rigidbody.mass = 1.0f;
    player.rigidbody.linearDamping = 0.02f;
    player.sphereCollider.enabled = true;
    player.sphereCollider.radius = 1.0f;
    player.sphereCollider.restitution = 0.0f;
    player.sphereCollider.friction = 0.0f;
    player.material.color = {0.2f, 0.57f, 0.96f};
    player.material.emission = {0.04f, 0.13f, 0.36f};
    player.material.metallic = 0.32f;
    player.material.roughness = 0.2f;

    movingObjects_.clear();
}

void SampleGame::start(Scene& scene) {
    buildScene(scene);
    state_ = SampleGameState::Playing;
    lane_ = 1;
    score_ = 0;
    elapsedSeconds_ = 0.0f;
    spawnCountdown_ = 0.55f;
    spawnIndex_ = 0;
}

bool SampleGame::shiftLane(int laneDelta) {
    if (state_ != SampleGameState::Playing || laneDelta == 0) return false;
    const int previousLane = lane_;
    const int safeDelta = std::clamp(laneDelta, -2, 2);
    lane_ = std::clamp(lane_ + safeDelta, 0, 2);
    return lane_ != previousLane;
}

void SampleGame::spawnObject(Scene& scene) {
    constexpr std::array<int, 12> kLanePattern{{1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0}};
    const bool isObstacle = spawnIndex_ % 4U == 3U;
    const int lane = kLanePattern[spawnIndex_ % kLanePattern.size()];
    Entity& object = scene.create(isObstacle ? "Red Pulse Hazard" : "Aether Shard",
                                  Primitive::Sphere);
    const std::uint32_t objectId = object.id;
    const float laneX = static_cast<float>(lane - 1) * kLaneSpacing;
    object.transform.position = {laneX, kPlayerHeight, kSpawnZ};
    object.transform.scale = isObstacle
            ? std::array<float, 3>{0.32f, 0.32f, 0.32f}
            : std::array<float, 3>{0.19f, 0.19f, 0.19f};
    object.spin.radiansPerSecond = {0.2f, isObstacle ? 1.1f : 1.8f, 0.15f};
    object.sphereCollider.enabled = true;
    object.sphereCollider.radius = 1.0f;
    object.sphereCollider.trigger = true;
    if (isObstacle) {
        object.material.color = {0.96f, 0.12f, 0.19f};
        object.material.emission = {0.52f, 0.018f, 0.028f};
        object.material.metallic = 0.18f;
        object.material.roughness = 0.23f;
        movingObjects_.emplace(objectId, HazardType::Obstacle);
    } else {
        object.material.color = {0.12f, 0.9f, 0.86f};
        object.material.emission = {0.025f, 0.42f, 0.38f};
        object.material.metallic = 0.24f;
        object.material.roughness = 0.18f;
        movingObjects_.emplace(objectId, HazardType::Collectible);
    }
    ++spawnIndex_;
}

void SampleGame::update(Scene& scene, float deltaSeconds) {
    const float dt = std::isfinite(deltaSeconds)
            ? std::clamp(deltaSeconds, 0.0f, 0.05f) : 0.0f;
    if (state_ == SampleGameState::Playing) {
        elapsedSeconds_ += dt;
        spawnCountdown_ -= dt;
        while (spawnCountdown_ <= 0.0f) {
            spawnObject(scene);
            spawnCountdown_ += kSpawnInterval;
        }

        Entity* player = scene.find(playerId_);
        if (player != nullptr) {
            const float targetX = static_cast<float>(lane_ - 1) * kLaneSpacing;
            const float blend = 1.0f - std::exp(-12.0f * dt);
            player->transform.position[0] += (targetX - player->transform.position[0]) * blend;
        }

        for (auto object = movingObjects_.begin(); object != movingObjects_.end();) {
            Entity* entity = scene.find(object->first);
            if (entity == nullptr) {
                object = movingObjects_.erase(object);
                continue;
            }
            entity->transform.position[2] += kObjectSpeed * dt;
            if (entity->transform.position[2] > kObjectCleanupZ) {
                scene.destroyEntity(object->first);
                object = movingObjects_.erase(object);
            } else {
                ++object;
            }
        }
    }

    scene.update(dt);
    std::vector<PhysicsEvent> events;
    scene.consumePhysicsEvents(events);
    if (state_ != SampleGameState::Playing) return;

    for (const PhysicsEvent& event : events) {
        if (event.type != PhysicsEventType::TriggerEnter) continue;
        const std::uint32_t objectId = event.firstEntityId == playerId_
                ? event.secondEntityId : event.secondEntityId == playerId_
                    ? event.firstEntityId : 0;
        const auto object = movingObjects_.find(objectId);
        if (objectId == 0 || object == movingObjects_.end()) continue;
        if (object->second == HazardType::Obstacle) {
            state_ = SampleGameState::Lost;
            break;
        }
        ++score_;
        scene.destroyEntity(objectId);
        movingObjects_.erase(object);
        if (score_ >= kScoreGoal) {
            state_ = SampleGameState::Won;
            break;
        }
    }
    if (state_ == SampleGameState::Playing && elapsedSeconds_ >= kRunDurationSeconds) {
        state_ = SampleGameState::Lost;
    }
}

int SampleGame::secondsRemaining() const {
    return std::max(0, static_cast<int>(std::ceil(kRunDurationSeconds - elapsedSeconds_)));
}

std::string SampleGame::statusText() const {
    if (state_ == SampleGameState::Won) return "AETHER RUN COMPLETE  6/6";
    if (state_ == SampleGameState::Lost) {
        return "RUN OVER  " + std::to_string(score_) + "/" + std::to_string(kScoreGoal);
    }
    return "AETHER RUN  " + std::to_string(score_) + "/" + std::to_string(kScoreGoal)
            + "  " + std::to_string(secondsRemaining()) + "s";
}

}  // namespace nuby
