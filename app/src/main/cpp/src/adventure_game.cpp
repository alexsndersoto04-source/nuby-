#include "nuby/adventure_game.hpp"
#include "nuby/scene_io.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <utility>
#include <vector>

namespace nuby {
namespace {
constexpr float kFloorHeight = -1.22f;
constexpr float kPlayerCenterHeight = -0.84f;
constexpr float kWorldLimitX = 16.0f;
constexpr float kWorldNearZ = 3.0f;
constexpr float kWorldFarZ = -20.0f;
constexpr std::size_t kZoneGridSide = 3U;
constexpr std::size_t kMaximumResidentAdventureZones = 4U;
constexpr float kGateZ = -11.5f;
constexpr float kPiOverTwo = 1.57079632679489661923f;
}

void AdventureGame::setZoneSceneReader(ZoneSceneReader reader) {
    if (zoneSceneLoader_) {
        zoneSceneLoader_->cancelAll();
        zoneSceneLoader_.reset();
    }
    if (reader) zoneSceneLoader_ = std::make_unique<AsyncSceneAssetLoader>(std::move(reader));
    std::fill(streamedZonePending_.begin(), streamedZonePending_.end(), false);
}

void AdventureGame::buildScene(Scene& scene) {
    if (zoneSceneLoader_) zoneSceneLoader_->cancelAll();
    std::fill(streamedZonePending_.begin(), streamedZonePending_.end(), false);
    scene.clear();
    PhysicsSettings& physics = scene.physicsSettings();
    physics.gravity = {0.0f, -9.81f, 0.0f};
    physics.floorEnabled = true;
    physics.floorHeight = kFloorHeight;
    physics.floorRestitution = 0.0f;
    physics.floorFriction = 0.0f;

    Entity& camera = scene.create("Adventure Follow Camera", Primitive::None);
    cameraId_ = camera.id;
    camera.visible = false;
    camera.camera.enabled = true;
    camera.camera.verticalFovRadians = 0.82f;
    camera.camera.nearPlane = 0.1f;
    camera.camera.farPlane = 65.0f;
    camera.orbitCamera.enabled = true;
    camera.orbitCamera.distance = 7.2f;
    camera.orbitCamera.minimumDistance = 4.8f;
    camera.orbitCamera.maximumDistance = 10.0f;
    camera.orbitCamera.pitch = 0.3f;
    camera.orbitCamera.minimumPitch = 0.08f;
    camera.orbitCamera.maximumPitch = 0.72f;
    camera.orbitCamera.target = {0.0f, -0.48f, 0.0f};
    scene.setActiveCamera(cameraId_);

    Entity& ground = scene.create("Ruins Courtyard", Primitive::Ground);
    ground.material.color = {0.18f, 0.20f, 0.17f};
    ground.material.roughness = 0.91f;
    ground.material.proceduralGrid = true;

    const std::array<std::array<float, 3>, 3> relicPositions{{
            {{-2.15f, -0.42f, -3.0f}},
            {{2.05f, -0.42f, -5.8f}},
            {{-1.4f, -0.42f, -8.5f}}
    }};
    relicIds_.clear();
    relicIds_.reserve(relicPositions.size());
    relicEntityIds_.fill(0U);
    for (std::size_t index = 0; index < relicPositions.size(); ++index) {
        Entity& relic = scene.create("Sun Relic " + std::to_string(index + 1),
                                     Primitive::Sphere);
        relic.transform.position = relicPositions[index];
        relic.transform.scale = {0.36f, 0.36f, 0.36f};
        relic.spin.radiansPerSecond = {0.0f, 1.0f, 0.0f};
        relic.pulse.enabled = true;
        relic.pulse.baseScale = relic.transform.scale;
        relic.pulse.amplitude = 0.12f;
        relic.pulse.frequency = 1.2f;
        relic.sphereCollider.enabled = true;
        relic.sphereCollider.radius = 1.0f;
        relic.sphereCollider.trigger = true;
        relic.material.color = {1.0f, 1.0f, 1.0f};
        relic.material.emission = {0.055f, 0.025f, 0.003f};
        relic.material.metallic = 0.72f;
        relic.material.roughness = 0.42f;
        relic.material.albedoTexture = "textures/sun_relic_albedo.tga";
        relic.material.normalTexture = "textures/sun_relic_normal.tga";
        relic.material.metallicRoughnessTexture = "textures/sun_relic_metal_rough.tga";
        relicIds_.push_back(relic.id);
        relicEntityIds_[index] = relic.id;
    }

    Entity& gate = scene.create("Sun Gate", Primitive::Torus);
    gateId_ = gate.id;
    gate.transform.position = {0.0f, 0.18f, kGateZ};
    gate.transform.rotation[0] = kPiOverTwo;
    gate.transform.scale = {1.6f, 1.6f, 1.6f};
    gate.material.color = {0.25f, 0.68f, 0.62f};
    gate.material.emission = {0.035f, 0.18f, 0.12f};
    gate.material.metallic = 0.28f;
    gate.material.roughness = 0.31f;

    Entity& gateTrigger = scene.create("Sun Gate Trigger", Primitive::Sphere);
    gateTriggerId_ = gateTrigger.id;
    gateTrigger.visible = false;
    gateTrigger.transform.position = {0.0f, kPlayerCenterHeight, kGateZ};
    gateTrigger.transform.scale = {1.15f, 1.15f, 1.15f};
    gateTrigger.sphereCollider.enabled = true;
    gateTrigger.sphereCollider.radius = 1.0f;
    gateTrigger.sphereCollider.trigger = true;

    Entity& player = scene.create("Wayfarer", Primitive::Sphere);
    playerId_ = player.id;
    player.transform.position = {0.0f, kPlayerCenterHeight, 0.0f};
    player.transform.scale = {0.36f, 0.48f, 0.36f};
    player.rigidbody.enabled = true;
    player.rigidbody.mass = 1.0f;
    player.rigidbody.linearDamping = 0.04f;
    player.sphereCollider.enabled = true;
    player.sphereCollider.radius = 1.0f;
    player.sphereCollider.restitution = 0.0f;
    player.sphereCollider.friction = 0.0f;
    player.characterController.enabled = true;
    player.characterController.maximumSpeed = 3.6f;
    player.characterController.acceleration = 18.0f;
    player.characterController.jumpSpeed = 4.8f;
    player.material.color = {0.24f, 0.55f, 0.74f};
    player.material.emission = {0.025f, 0.08f, 0.12f};
    player.material.metallic = 0.18f;
    player.material.roughness = 0.43f;
    const std::array<float, 3> playerScale = player.transform.scale;
    const Material playerMaterial = player.material;
    player.visible = false;  // Keep the physics body, render its skinned child instead.

    Entity& animatedBody = scene.create("Wayfarer Animated Body", Primitive::ImportedMesh);
    animatedBody.meshAsset = "characters/wayfarer_skeletal.glb";
    animatedBody.transform.position = {
        0.0f, (kFloorHeight - kPlayerCenterHeight) / playerScale[1], 0.0f};
    animatedBody.transform.scale = {
        1.0f / playerScale[0],
        1.0f / playerScale[1],
        1.0f / playerScale[2]};
    animatedBody.skeletalAnimation.enabled = true;
    animatedBody.skeletalAnimation.clipName = "idle";
    animatedBody.skeletalAnimation.durationSeconds = 1.0f;
    animatedBody.skeletalAnimation.looping = true;
    animatedBody.skeletalLocomotion.enabled = true;
    animatedBody.material = playerMaterial;
    scene.setParent(animatedBody.id, playerId_);

    Entity& head = scene.create("Wayfarer Head", Primitive::Sphere);
    head.transform.position = {0.0f, 1.1f, 0.0f};
    head.transform.scale = {0.5f, 0.5f, 0.5f};
    head.material.color = {0.73f, 0.48f, 0.31f};
    head.material.albedoTexture = "textures/wayfarer_face.tga";
    head.material.roughness = 0.72f;
    scene.setParent(head.id, playerId_);

    Entity& hair = scene.create("Wayfarer Hair", Primitive::Sphere);
    hair.transform.position = {0.0f, 1.5f, 0.0f};
    hair.transform.scale = {0.5f, 0.14f, 0.5f};
    hair.material.color = {0.075f, 0.045f, 0.035f};
    hair.material.roughness = 0.92f;
    scene.setParent(hair.id, playerId_);

    Entity& pack = scene.create("Wayfarer Satchel", Primitive::Sphere);
    pack.transform.position = {0.0f, 0.15f, -0.72f};
    pack.transform.scale = {0.43f, 0.48f, 0.38f};
    pack.material.color = {0.55f, 0.27f, 0.11f};
    pack.material.roughness = 0.84f;
    scene.setParent(pack.id, playerId_);

    zoneLoadResultsScratch_.clear();
    zoneLoadFailureCount_ = 0;
    std::vector<WorldZoneDefinition> zones;
    std::size_t maximumResidentZones = kMaximumResidentAdventureZones;
    WorldZoneManifest manifest;
    std::string manifestError;
    if (zoneManifestReader_ && zoneManifestReader_(manifest, manifestError)) {
        zones = std::move(manifest.zones);
        maximumResidentZones = manifest.maximumResidentZones;
        if (!manifest.objectives.empty()) {
            applyAuthoredObjectives(scene, manifest.objectives);
        }
    } else {
        // The packaged manifest is authoritative; this safe fallback keeps the
        // adventure playable in native tests and if a future manifest is bad.
        constexpr std::array<float, 3> xCenters{{-11.0f, 0.0f, 11.0f}};
        constexpr std::array<float, 3> zCenters{{-2.5f, -8.0f, -13.5f}};
        constexpr std::array<const char*, 3> rowAssets{{
                "scenes/zones/ruins_01.scene",
                "scenes/zones/ruins_02.scene",
                "scenes/zones/ruins_03.scene"
        }};
        zones.reserve(kZoneGridSide * kZoneGridSide);
        for (std::size_t row = 0; row < kZoneGridSide; ++row) {
            for (std::size_t column = 0; column < kZoneGridSide; ++column) {
                const std::size_t index = row * kZoneGridSide + column;
                const std::uint32_t id = static_cast<std::uint32_t>(index + 1U);
                zones.push_back({id, {xCenters[column], 0.0f, zCenters[row]},
                        7.5f, 9.2f, rowAssets[row], 10.5f});
            }
        }
        maximumResidentZones = kMaximumResidentAdventureZones;
    }
    zoneDefinitions_ = std::move(zones);
    zones = zoneDefinitions_;
    streamedZoneEntityIds_.assign(zones.size(), {});
    streamedZoneLoaded_.assign(zones.size(), false);
    streamedZonePending_.assign(zones.size(), false);
    streamedZoneOffsets_.assign(zones.size(), {0.0f, 0.0f, 0.0f});
    for (std::size_t index = 0; index < zones.size(); ++index) {
        streamedZoneOffsets_[index] = {zones[index].center[0], 0.0f, 0.0f};
    }

    std::string streamingError;
    if (worldZoneStreamer_.configure(zones, streamingError, maximumResidentZones)) {
        const Entity* playerEntity = scene.find(playerId_);
        if (playerEntity != nullptr
                && worldZoneStreamer_.update(playerEntity->transform.position,
                                             zoneTransitionsScratch_)) {
            applyZoneTransitions(scene);
        }
    } else {
        // Keep the scene playable if a future authored zone manifest is invalid.
        for (std::uint32_t zoneId = 1; zoneId <= streamedZoneEntityIds_.size(); ++zoneId) {
            loadZone(scene, zoneId);
        }
    }
}

bool AdventureGame::applyAuthoredObjectives(
        Scene& scene, const std::vector<WorldObjectiveDefinition>& objectives) {
    constexpr std::size_t kExpectedObjectiveCount =
            static_cast<std::size_t>(kRelicGoal) + 1U;
    if (objectives.size() != kExpectedObjectiveCount) return false;
    std::array<std::array<float, 3>, static_cast<std::size_t>(kRelicGoal)> relicPositions{};
    std::array<bool, static_cast<std::size_t>(kRelicGoal)> foundRelics{};
    std::array<float, 3> gatePosition{};
    bool foundGate = false;
    for (const WorldObjectiveDefinition& objective : objectives) {
        if (objective.type == WorldObjectiveType::Relic
                && objective.id >= 1U && objective.id <= kRelicGoal) {
            const std::size_t slot = static_cast<std::size_t>(objective.id - 1U);
            if (foundRelics[slot]) return false;
            relicPositions[slot] = objective.position;
            foundRelics[slot] = true;
        } else if (objective.type == WorldObjectiveType::Gate
                && objective.id == kExpectedObjectiveCount && !foundGate) {
            gatePosition = objective.position;
            foundGate = true;
        } else {
            return false;
        }
        if (objective.position[0] < -kWorldLimitX || objective.position[0] > kWorldLimitX
                || objective.position[1] < -3.0f || objective.position[1] > 8.0f
                || objective.position[2] < kWorldFarZ || objective.position[2] > kWorldNearZ) {
            return false;
        }
    }
    if (!foundGate || !std::all_of(foundRelics.begin(), foundRelics.end(),
                                    [](bool found) { return found; })) return false;
    std::array<Entity*, static_cast<std::size_t>(kRelicGoal)> relicEntities{};
    for (std::size_t slot = 0U; slot < relicEntities.size(); ++slot) {
        relicEntities[slot] = scene.find(relicEntityIds_[slot]);
        if (relicEntities[slot] == nullptr) return false;
    }
    Entity* gate = scene.find(gateId_);
    Entity* gateTrigger = scene.find(gateTriggerId_);
    if (gate == nullptr || gateTrigger == nullptr) return false;
    for (std::size_t slot = 0U; slot < relicEntities.size(); ++slot) {
        relicEntities[slot]->transform.position = relicPositions[slot];
    }
    gate->transform.position = gatePosition;
    gateTrigger->transform.position = {gatePosition[0], kPlayerCenterHeight, gatePosition[2]};
    return true;
}

void AdventureGame::loadZone(Scene& scene, std::uint32_t zoneId, double priority) {
    if (zoneId == 0 || zoneId > streamedZoneEntityIds_.size()) return;
    const std::size_t zoneIndex = zoneId - 1U;
    if (streamedZoneLoaded_[zoneIndex] || streamedZonePending_[zoneIndex]) return;

    if (zoneSceneLoader_ && !zoneDefinitions_[zoneIndex].sceneAssetPath.empty()) {
        if (zoneSceneLoader_->request(zoneId, zoneDefinitions_[zoneIndex].sceneAssetPath,
                                      priority)) {
            streamedZonePending_[zoneIndex] = true;
            return;
        }
        ++zoneLoadFailureCount_;
    }
    loadZoneFallback(scene, zoneId);
}

void AdventureGame::loadZoneFallback(Scene& scene, std::uint32_t zoneId) {
    if (zoneId == 0 || zoneId > streamedZoneEntityIds_.size()) return;
    const std::size_t zoneIndex = zoneId - 1U;
    std::vector<std::uint32_t>& entityIds = streamedZoneEntityIds_[zoneIndex];
    if (streamedZoneLoaded_[zoneIndex]) return;

    // A missing/corrupt scene asset falls back to deterministic scenery so the
    // adventure remains playable; the failure count stays observable in tests.
    const std::size_t row = zoneIndex / kZoneGridSide;
    const std::size_t column = zoneIndex % kZoneGridSide;
    const int firstPillar = static_cast<int>(row * 2U);
    const std::string cellSuffix = column == 0U ? " West Cell"
            : column == 2U ? " East Cell" : "";
    for (int localIndex = 0; localIndex < 2; ++localIndex) {
        const int pillarIndex = firstPillar + localIndex;
        const float z = -2.0f - static_cast<float>(pillarIndex) * 2.25f;
        for (int side : {-1, 1}) {
            Entity& pillar = scene.create("Ruin Zone " + std::to_string(row + 1U)
                    + " Pillar " + std::to_string(pillarIndex * 2
                            + (side > 0 ? 2 : 1)) + cellSuffix,
                    Primitive::Sphere);
            pillar.transform.position = {
                    streamedZoneOffsets_[zoneIndex][0] + static_cast<float>(side) * 4.35f,
                    -0.12f, z};
            pillar.transform.scale = {0.38f, 1.18f, 0.42f};
            pillar.material.color = {0.34f, 0.30f, 0.22f};
            pillar.material.roughness = 0.96f;
            entityIds.push_back(pillar.id);
        }
    }
    streamedZoneLoaded_[zoneIndex] = true;
}

void AdventureGame::integrateReadyZoneScenes(Scene& scene) {
    if (!zoneSceneLoader_) return;
    if (zoneLoadResultsScratch_.empty()) zoneSceneLoader_->poll(zoneLoadResultsScratch_);
    if (zoneLoadResultsScratch_.empty()) return;

    // Integrate at most one parsed scene per frame. File IO and parsing happen
    // on the worker; this also bounds scene mutation and GPU upload bursts.
    SceneAssetLoadResult result = std::move(zoneLoadResultsScratch_.front());
    zoneLoadResultsScratch_.erase(zoneLoadResultsScratch_.begin());
    if (result.requestId == 0U || result.requestId > streamedZoneEntityIds_.size()) return;
    const std::size_t zoneIndex = result.requestId - 1U;
    streamedZonePending_[zoneIndex] = false;
    if (!worldZoneStreamer_.isLoaded(result.requestId)
            || streamedZoneLoaded_[zoneIndex]) {
        return;
    }
    std::vector<std::uint32_t>& entityIds = streamedZoneEntityIds_[zoneIndex];
    const std::size_t column = zoneIndex % kZoneGridSide;
    if (result.success) {
        const std::string cellSuffix = column == 0U ? " West Cell"
                : column == 2U ? " East Cell" : "";
        for (Entity& entity : result.scene.entities()) {
            if (entity.parentId == 0U) {
                entity.transform.position[0] += streamedZoneOffsets_[zoneIndex][0];
            }
            entity.name += cellSuffix;
        }
    }
    if (result.success && scene.appendEntities(result.scene, entityIds)) {
        streamedZoneLoaded_[zoneIndex] = true;
        if (zoneAssetsChangedCallback_) zoneAssetsChangedCallback_();
    } else {
        ++zoneLoadFailureCount_;
        loadZoneFallback(scene, result.requestId);
    }
}

void AdventureGame::unloadZone(Scene& scene, std::uint32_t zoneId) {
    if (zoneId == 0 || zoneId > streamedZoneEntityIds_.size()) return;
    const std::size_t zoneIndex = zoneId - 1U;
    const bool hadResidentScene = streamedZoneLoaded_[zoneIndex];
    if (zoneSceneLoader_) zoneSceneLoader_->cancel(zoneId);
    streamedZonePending_[zoneIndex] = false;
    zoneLoadResultsScratch_.erase(std::remove_if(zoneLoadResultsScratch_.begin(),
            zoneLoadResultsScratch_.end(), [zoneId](const SceneAssetLoadResult& result) {
                return result.requestId == zoneId;
            }), zoneLoadResultsScratch_.end());
    std::vector<std::uint32_t>& entityIds = streamedZoneEntityIds_[zoneIndex];
    for (auto entity = entityIds.rbegin(); entity != entityIds.rend(); ++entity) {
        scene.destroyEntity(*entity);
    }
    entityIds.clear();
    streamedZoneLoaded_[zoneIndex] = false;
    if (hadResidentScene && zoneAssetsChangedCallback_) zoneAssetsChangedCallback_();
}

void AdventureGame::applyZoneTransitions(Scene& scene) {
    for (const WorldZoneTransition& transition : zoneTransitionsScratch_) {
        if (transition.load) loadZone(scene, transition.zoneId, transition.priority);
        else unloadZone(scene, transition.zoneId);
    }
}

void AdventureGame::start(Scene& scene) {
    buildScene(scene);
    moveInput_ = {0.0f, 0.0f};
    relicsCollected_ = 0;
    collectedRelicMask_ = 0U;
    completed_ = false;
}

bool AdventureGame::setMoveInput(Scene& scene, float horizontal, float forward) {
    Entity* player = scene.find(playerId_);
    if (player == nullptr || completed_ || !std::isfinite(horizontal)
            || !std::isfinite(forward)) {
        return false;
    }
    const float magnitude = std::hypot(horizontal, forward);
    if (magnitude > 1.0f) {
        horizontal /= magnitude;
        forward /= magnitude;
    }
    moveInput_ = {horizontal, forward};
    return true;
}

bool AdventureGame::jump(Scene& scene) {
    return !completed_ && scene.jumpCharacter(playerId_);
}

void AdventureGame::update(Scene& scene, float deltaSeconds) {
    const float dt = std::isfinite(deltaSeconds)
            ? std::clamp(deltaSeconds, 0.0f, 0.05f) : 0.0f;
    integrateReadyZoneScenes(scene);
    Entity* camera = scene.find(cameraId_);
    Entity* player = scene.find(playerId_);
    if (camera != nullptr && player != nullptr) {
        const float yaw = camera->orbitCamera.yaw;
        const float worldX = std::cos(yaw) * moveInput_[0]
                - std::sin(yaw) * moveInput_[1];
        const float worldZ = -std::sin(yaw) * moveInput_[0]
                - std::cos(yaw) * moveInput_[1];
        scene.setCharacterMoveInput(playerId_, worldX, worldZ);
        if (std::hypot(worldX, worldZ) > 0.05f) {
            player->transform.rotation[1] = std::atan2(-worldX, -worldZ);
        }
        camera->orbitCamera.target = {player->transform.position[0],
                player->transform.position[1] + 0.36f,
                player->transform.position[2]};
    }

    scene.update(dt);
    player = scene.find(playerId_);
    if (player != nullptr) {
        player->transform.position[0] = std::clamp(player->transform.position[0],
                -kWorldLimitX, kWorldLimitX);
        player->transform.position[2] = std::clamp(player->transform.position[2],
                kWorldFarZ, kWorldNearZ);
        if (player->transform.position[0] == -kWorldLimitX
                || player->transform.position[0] == kWorldLimitX) {
            player->rigidbody.velocity[0] = 0.0f;
        }
        if (player->transform.position[2] == kWorldFarZ
                || player->transform.position[2] == kWorldNearZ) {
            player->rigidbody.velocity[2] = 0.0f;
        }
        camera = scene.find(cameraId_);
        if (camera != nullptr) {
            camera->orbitCamera.target = {player->transform.position[0],
                    player->transform.position[1] + 0.36f,
                    player->transform.position[2]};
        }
    }

    if (player != nullptr
            && worldZoneStreamer_.update(player->transform.position,
                    {player->rigidbody.velocity[0], 0.0f, player->rigidbody.velocity[2]},
                    zoneTransitionsScratch_)) {
        applyZoneTransitions(scene);
    }

    std::vector<PhysicsEvent> events;
    scene.consumePhysicsEvents(events);
    std::array<std::uint32_t, kRelicGoal> collected{};
    std::size_t collectedCount = 0;
    for (const PhysicsEvent& event : events) {
        if (event.type != PhysicsEventType::TriggerEnter) continue;
        const std::uint32_t otherId = event.firstEntityId == playerId_
                ? event.secondEntityId : event.secondEntityId == playerId_
                    ? event.firstEntityId : 0;
        if (otherId == 0 || otherId == gateTriggerId_
                || std::find(relicIds_.begin(), relicIds_.end(), otherId) == relicIds_.end()
                || std::find(collected.begin(), collected.begin() + collectedCount, otherId)
                        != collected.begin() + collectedCount) {
            continue;
        }
        if (collectedCount < collected.size()) collected[collectedCount++] = otherId;
    }
    for (std::size_t index = 0; index < collectedCount; ++index) {
        if (!scene.destroyEntity(collected[index])) continue;
        const auto relic = std::find(relicEntityIds_.begin(), relicEntityIds_.end(),
                                     collected[index]);
        if (relic == relicEntityIds_.end()) continue;
        const std::size_t slot = static_cast<std::size_t>(
                std::distance(relicEntityIds_.begin(), relic));
        collectedRelicMask_ = static_cast<std::uint8_t>(
                collectedRelicMask_ | static_cast<std::uint8_t>(1U << slot));
        ++relicsCollected_;
    }
    relicIds_.erase(std::remove_if(relicIds_.begin(), relicIds_.end(),
            [&collected, collectedCount](std::uint32_t id) {
                return std::find(collected.begin(), collected.begin() + collectedCount, id)
                        != collected.begin() + collectedCount;
            }), relicIds_.end());

    player = scene.find(playerId_);
    const Entity* gate = scene.find(gateTriggerId_);
    if (!completed_ && relicsCollected_ >= kRelicGoal && player != nullptr && gate != nullptr) {
        const float dx = player->transform.position[0] - gate->transform.position[0];
        const float dz = player->transform.position[2] - gate->transform.position[2];
        completed_ = dx * dx + dz * dz <= 1.8f * 1.8f;
        if (completed_) moveInput_ = {0.0f, 0.0f};
    }
}

bool AdventureGame::saveData(const Scene& scene, AdventureSaveData& output,
                             std::string& error) const {
    const Entity* player = scene.find(playerId_);
    const Entity* camera = scene.find(cameraId_);
    const Entity* gate = scene.find(gateId_);
    if (player == nullptr || camera == nullptr || gate == nullptr) {
        error = "adventure player, camera, or gate is unavailable";
        return false;
    }
    AdventureSaveData candidate;
    candidate.playerX = player->transform.position[0];
    candidate.playerZ = player->transform.position[2];
    candidate.playerYaw = player->transform.rotation[1];
    candidate.cameraYaw = camera->orbitCamera.yaw;
    candidate.cameraPitch = camera->orbitCamera.pitch;
    candidate.gateX = gate->transform.position[0];
    candidate.gateZ = gate->transform.position[2];
    candidate.collectedRelicMask = collectedRelicMask_;
    candidate.completed = completed_;
    if (!validateAdventureSaveData(candidate, error)) return false;
    output = candidate;
    error.clear();
    return true;
}

bool AdventureGame::loadData(Scene& scene, const AdventureSaveData& data,
                             std::string& error) {
    if (!validateAdventureSaveData(data, error)) return false;
    Entity* player = scene.find(playerId_);
    Entity* camera = scene.find(cameraId_);
    const Entity* gate = scene.find(gateId_);
    if (player == nullptr || camera == nullptr || gate == nullptr) {
        error = "adventure player, camera, or gate is unavailable";
        return false;
    }
    if (std::fabs(data.gateX - gate->transform.position[0]) > 0.001f
            || std::fabs(data.gateZ - gate->transform.position[2]) > 0.001f) {
        error = "save gate layout does not match the current world";
        return false;
    }
    for (std::size_t slot = 0U; slot < relicEntityIds_.size(); ++slot) {
        if ((data.collectedRelicMask & static_cast<std::uint8_t>(1U << slot)) == 0U) {
            continue;
        }
        if (relicEntityIds_[slot] == 0U || scene.find(relicEntityIds_[slot]) == nullptr) {
            error = "adventure relic layout does not match the save";
            return false;
        }
    }

    player->transform.position[0] = data.playerX;
    player->transform.position[1] = kPlayerCenterHeight;
    player->transform.position[2] = data.playerZ;
    player->transform.rotation[1] = std::remainder(data.playerYaw, 6.2831853071795864769f);
    player->rigidbody.velocity = {0.0f, 0.0f, 0.0f};
    camera->orbitCamera.yaw = std::remainder(data.cameraYaw, 6.2831853071795864769f);
    camera->orbitCamera.pitch = data.cameraPitch;
    camera->orbitCamera.target = {data.playerX, kPlayerCenterHeight + 0.36f,
                                  data.playerZ};
    collectedRelicMask_ = data.collectedRelicMask;
    relicsCollected_ = 0;
    for (std::size_t slot = 0U; slot < relicEntityIds_.size(); ++slot) {
        if ((collectedRelicMask_ & static_cast<std::uint8_t>(1U << slot)) == 0U) continue;
        ++relicsCollected_;
        scene.destroyEntity(relicEntityIds_[slot]);
    }
    relicIds_.erase(std::remove_if(relicIds_.begin(), relicIds_.end(), [this](std::uint32_t id) {
        const auto found = std::find(relicEntityIds_.begin(), relicEntityIds_.end(), id);
        if (found == relicEntityIds_.end()) return true;
        const std::size_t slot = static_cast<std::size_t>(
                std::distance(relicEntityIds_.begin(), found));
        return (collectedRelicMask_ & static_cast<std::uint8_t>(1U << slot)) != 0U;
    }), relicIds_.end());
    completed_ = data.completed;
    moveInput_ = {0.0f, 0.0f};
    error.clear();
    return true;
}

std::string AdventureGame::statusText() const {
    const std::string objective = completed_ ? "AVENTURA COMPLETADA  3/3"
            : "RUINAS DEL SOL  " + std::to_string(relicsCollected_) + "/"
                    + std::to_string(kRelicGoal) + " RELIQUIAS";
    return objective + "  ·  ZONAS " + std::to_string(residentZoneCount())
            + "/" + std::to_string(zoneCount());
}

}  // namespace nuby
