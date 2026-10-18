#include "nuby/adventure_game.hpp"
#include "nuby/scene_io.hpp"

#include <cmath>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void advance(nuby::AdventureGame& game, nuby::Scene& scene, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        game.update(scene, 1.0f / 60.0f);
    }
}

void movePlayerTo(nuby::Scene& scene, std::uint32_t playerId, float x, float z) {
    nuby::Entity* player = scene.find(playerId);
    if (player == nullptr) return;
    player->transform.position = {x, -0.74f, z};
    player->rigidbody.velocity = {0.0f, 0.0f, 0.0f};
}

std::uint32_t findEntityId(const nuby::Scene& scene, const std::string& name) {
    for (const nuby::Entity& entity : scene.entities()) {
        if (entity.name == name) return entity.id;
    }
    return 0;
}

std::size_t countEntityName(const nuby::Scene& scene, const std::string& name) {
    std::size_t count = 0;
    for (const nuby::Entity& entity : scene.entities()) {
        if (entity.name == name) ++count;
    }
    return count;
}

void testAdventureBuildsItsOwnCharacterAndScene() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    scene.create("old prototype scene", nuby::Primitive::Sphere);
    game.start(scene);

    const nuby::Entity* player = scene.find(game.playerId());
    const nuby::Entity* camera = scene.activeCamera();
    expect(player != nullptr && player->name == "Wayfarer",
           "adventure creates its own player instead of reusing the runner");
    expect(player != nullptr && player->characterController.enabled
                   && player->rigidbody.enabled && player->sphereCollider.enabled
                   && !player->visible,
           "adventure keeps its physics/controller body while hiding its render proxy");
    const nuby::Entity* animatedBody = scene.find(findEntityId(scene, "Wayfarer Animated Body"));
    expect(animatedBody != nullptr && animatedBody->parentId == game.playerId()
                   && animatedBody->primitive == nuby::Primitive::ImportedMesh
                   && animatedBody->meshAsset == "characters/wayfarer_skeletal.glb"
                   && animatedBody->skeletalAnimation.enabled
                   && animatedBody->skeletalAnimation.clipName == "idle"
                   && animatedBody->skeletalAnimation.looping
                   && animatedBody->skeletalLocomotion.enabled
                   && animatedBody->skeletalLocomotion.idleClip == "idle"
                   && animatedBody->skeletalLocomotion.walkClip == "walk"
                   && animatedBody->skeletalLocomotion.runClip == "run"
                   && animatedBody->skeletalLocomotion.jumpClip == "jump",
           "wayfarer has a reusable idle/walk/run/jump skeletal locomotion controller");
    expect(camera != nullptr && camera->orbitCamera.enabled,
           "adventure has an orbit-follow camera");
    const nuby::Entity* head = scene.find(findEntityId(scene, "Wayfarer Head"));
    expect(head != nullptr && head->parentId == game.playerId()
                   && head->material.albedoTexture == "textures/wayfarer_face.tga",
           "wayfarer has a face-textured head attached to the moving player");
    expect(scene.physicsSettings().floorEnabled && scene.entities().size() >= 15,
           "adventure constructs a standalone courtyard with its starting streamed sector");
    expect(game.relicsCollected() == 0 && !game.completed(),
           "a new adventure starts with three relics to find and a sealed gate");
    expect(game.statusText().find("0/3") != std::string::npos,
           "adventure exposes objective progress for the HUD");
}

void testCharacterFacesItsMovementDirection() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    game.start(scene);
    const nuby::Entity* camera = scene.activeCamera();
    const float cameraYaw = camera == nullptr ? 0.0f : camera->orbitCamera.yaw;
    expect(game.setMoveInput(scene, 1.0f, 0.0f),
           "sideways input is accepted for character-facing test");
    advance(game, scene, 1);
    const nuby::Entity* player = scene.find(game.playerId());
    const float expectedYaw = std::atan2(-std::cos(cameraYaw), std::sin(cameraYaw));
    expect(player != nullptr && std::fabs(player->transform.rotation[1]
                    - expectedYaw) < 0.001f,
           "character turns toward its camera-relative world-space movement direction");
}

void testWayfarerSelectsIdleWalkRunAndJumpClips() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    game.start(scene);
    const std::uint32_t bodyId = findEntityId(scene, "Wayfarer Animated Body");
    nuby::Entity* body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "idle",
           "Wayfarer starts with the idle clip");
    advance(game, scene, 1);

    expect(game.setMoveInput(scene, 0.0f, 1.0f), "walking input is accepted");
    advance(game, scene, 1);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "walk",
           "low movement speed selects the walk clip");
    advance(game, scene, 15);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "run",
           "high movement speed selects the run clip");

    expect(game.setMoveInput(scene, 0.0f, 0.0f), "idle input is accepted");
    advance(game, scene, 30);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "idle",
           "stopping movement returns the character to idle");
    expect(game.jump(scene), "animation-state test can jump from rest");
    advance(game, scene, 1);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "jump"
                   && !body->skeletalAnimation.looping,
           "airborne movement selects a non-looping jump clip");
    advance(game, scene, 75);
    body = scene.find(bodyId);
    const nuby::Entity* player = scene.find(game.playerId());
    expect(player != nullptr && player->characterController.grounded
                   && body != nullptr && body->skeletalAnimation.clipName == "idle",
           "landing restores the idle clip after the jump");
}

void testAdventureStreamsSceneryByPlayerZone() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    game.start(scene);

    const std::string firstZonePillar = "Ruin Zone 1 Pillar 1";
    const std::string secondZonePillar = "Ruin Zone 2 Pillar 5";
    const std::string thirdZonePillar = "Ruin Zone 3 Pillar 9";
    expect(findEntityId(scene, firstZonePillar) != 0
                   && findEntityId(scene, secondZonePillar) == 0
                   && findEntityId(scene, thirdZonePillar) == 0,
           "adventure starts with nearby scenery only");

    movePlayerTo(scene, game.playerId(), 0.0f, -9.0f);
    advance(game, scene, 1);
    expect(findEntityId(scene, secondZonePillar) != 0
                   && findEntityId(scene, thirdZonePillar) != 0,
           "walking into the route loads adjacent scenery sectors");
    expect(findEntityId(scene, firstZonePillar) != 0,
           "hysteresis keeps the previous sector resident near its boundary");

    movePlayerTo(scene, game.playerId(), 0.0f, -12.0f);
    advance(game, scene, 1);
    expect(findEntityId(scene, firstZonePillar) == 0
                   && findEntityId(scene, secondZonePillar) != 0
                   && findEntityId(scene, thirdZonePillar) != 0,
           "distant scenery unloads while current and next sectors remain loaded");
    expect(findEntityId(scene, "Sun Relic 1") != 0
                   && findEntityId(scene, "Sun Relic 2") != 0
                   && findEntityId(scene, "Sun Relic 3") != 0,
           "streaming scenery does not unload mission-critical relics");

    movePlayerTo(scene, game.playerId(), 0.0f, 0.0f);
    advance(game, scene, 1);
    expect(findEntityId(scene, firstZonePillar) != 0
                   && findEntityId(scene, thirdZonePillar) == 0
                   && countEntityName(scene, firstZonePillar) == 1,
           "returning to the starting area reloads scenery once without duplicates");
}

void testAdventureLoadsSerializedZoneScenesAndPreservesMissionState() {
    nuby::AdventureGame game;
    int sceneAssetsRead = 0;
    int resourceChanges = 0;
    game.setZoneAssetsChangedCallback([&resourceChanges]() { ++resourceChanges; });
    game.setZoneSceneReader([&sceneAssetsRead](const std::string& assetPath,
                                                nuby::Scene& fragment,
                                                std::string& error) {
        ++sceneAssetsRead;
        std::ifstream input("app/src/main/assets/" + assetPath, std::ios::binary);
        if (!input) {
            error = "zone asset not found";
            return false;
        }
        return nuby::SceneSerializer::load(fragment, input, error);
    });
    nuby::Scene scene;
    game.start(scene);

    expect(sceneAssetsRead == 1 && resourceChanges == 1
                   && game.zoneLoadFailureCount() == 0,
           "adventure reads the nearby scene and notifies the renderer to load its resources");
    const nuby::Entity* loadedPillar = scene.find(
            findEntityId(scene, "Ruin Zone 1 Pillar 1"));
    expect(loadedPillar != nullptr && loadedPillar->primitive == nuby::Primitive::ImportedMesh
                   && loadedPillar->meshAsset == "meshes/ruin_pillar.obj"
                   && findEntityId(scene, "Ruin Zone 2 Pillar 5") == 0,
           "resident serialized scenes reference file-backed mesh resources without preloading distant zones");
    expect(scene.find(game.playerId()) != nullptr && scene.activeCamera() != nullptr,
           "appending a zone fragment does not replace the game player or active camera");

    const std::uint32_t firstRelic = findEntityId(scene, "Sun Relic 1");
    movePlayerTo(scene, game.playerId(), -2.15f, -3.0f);
    advance(game, scene, 1);
    expect(game.relicsCollected() == 1 && scene.find(firstRelic) == nullptr,
           "mission progress can advance while asset-backed zones are active");

    movePlayerTo(scene, game.playerId(), 0.0f, -13.0f);
    advance(game, scene, 1);
    expect(findEntityId(scene, "Ruin Zone 1 Pillar 1") == 0
                   && findEntityId(scene, "Ruin Zone 3 Pillar 9") != 0,
           "streaming unloads distant asset entities and loads the new zone scene");
    expect(scene.find(game.playerId()) != nullptr && scene.activeCamera() != nullptr
                   && game.relicsCollected() == 1 && scene.find(firstRelic) == nullptr,
           "zone transitions preserve player, camera, and collected mission progress");
    expect(sceneAssetsRead >= 3 && resourceChanges >= 3
                   && game.zoneLoadFailureCount() == 0,
           "each new zone scene triggers resource loading without falling back");
}

void testMissingZoneSceneFallsBackWithoutBreakingAdventure() {
    nuby::AdventureGame game;
    game.setZoneSceneReader([](const std::string&, nuby::Scene&, std::string& error) {
        error = "simulated missing or corrupt scene";
        return false;
    });
    nuby::Scene scene;
    game.start(scene);
    expect(game.zoneLoadFailureCount() == 1
                   && findEntityId(scene, "Ruin Zone 1 Pillar 1") != 0,
           "a missing zone scene records an error and keeps deterministic fallback scenery");
    expect(scene.find(game.playerId()) != nullptr && scene.activeCamera() != nullptr
                   && findEntityId(scene, "Sun Relic 1") != 0,
           "scene asset failure leaves mission-critical adventure state playable");
}

void testMovementJumpAndCameraFollow() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    game.start(scene);

    expect(!game.setMoveInput(scene, std::numeric_limits<float>::quiet_NaN(), 0.0f),
           "adventure rejects non-finite touch-stick input");
    expect(game.setMoveInput(scene, 0.0f, 1.0f),
           "adventure accepts forward movement input");
    advance(game, scene, 45);
    const nuby::Entity* player = scene.find(game.playerId());
    const nuby::Entity* camera = scene.activeCamera();
    expect(player != nullptr && player->transform.position[2] < -0.7f,
           "forward input moves the player into the courtyard");
    expect(player != nullptr && player->characterController.grounded,
           "adventure player remains grounded while walking");
    expect(player != nullptr && camera != nullptr
                   && std::fabs(camera->orbitCamera.target[2]
                           - player->transform.position[2]) < 0.01f,
           "follow camera tracks the character position");

    expect(game.jump(scene), "grounded adventure character can jump");
    expect(!game.jump(scene), "adventure character cannot jump twice before landing");
    advance(game, scene, 1);
    player = scene.find(game.playerId());
    expect(player != nullptr && player->transform.position[1] > -0.74f,
           "jump input raises the player above the courtyard floor");
}

void testRelicsUnlockTheGateAndRestartResetsProgress() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    game.start(scene);
    const std::uint32_t playerId = game.playerId();
    const std::uint32_t firstRelic = findEntityId(scene, "Sun Relic 1");
    const std::uint32_t secondRelic = findEntityId(scene, "Sun Relic 2");
    const std::uint32_t thirdRelic = findEntityId(scene, "Sun Relic 3");
    expect(scene.find(firstRelic) != nullptr && scene.find(secondRelic) != nullptr
                   && scene.find(thirdRelic) != nullptr,
           "scene has three trigger relics in the courtyard");

    movePlayerTo(scene, playerId, 0.0f, -11.5f);
    advance(game, scene, 1);
    expect(!game.completed(), "sealed gate cannot complete the adventure before relics");

    movePlayerTo(scene, playerId, -2.15f, -3.0f);
    advance(game, scene, 1);
    expect(game.relicsCollected() == 1 && scene.find(firstRelic) == nullptr,
           "touching a relic trigger collects and removes that relic");
    movePlayerTo(scene, playerId, 2.05f, -5.8f);
    advance(game, scene, 1);
    expect(game.relicsCollected() == 2,
           "second relic advances objective progress");
    movePlayerTo(scene, playerId, -1.4f, -8.5f);
    advance(game, scene, 1);
    expect(game.relicsCollected() == nuby::AdventureGame::kRelicGoal,
           "collecting all relics unlocks the gate objective");
    expect(!game.completed(), "collecting relics alone does not end the adventure");

    movePlayerTo(scene, playerId, 0.0f, -11.5f);
    advance(game, scene, 1);
    expect(game.completed() && game.statusText().find("COMPLET") != std::string::npos,
           "reaching the sun gate with all relics completes the adventure slice");
    expect(!game.setMoveInput(scene, 1.0f, 0.0f),
           "movement input is ignored after the adventure ends");

    game.start(scene);
    expect(!game.completed() && game.relicsCollected() == 0
                   && scene.find(game.playerId()) != nullptr,
           "restarting builds a clean new adventure and resets progress");
}
}  // namespace

int main() {
    testAdventureBuildsItsOwnCharacterAndScene();
    testCharacterFacesItsMovementDirection();
    testWayfarerSelectsIdleWalkRunAndJumpClips();
    testAdventureStreamsSceneryByPlayerZone();
    testAdventureLoadsSerializedZoneScenesAndPreservesMissionState();
    testMissingZoneSceneFallsBackWithoutBreakingAdventure();
    testMovementJumpAndCameraFollow();
    testRelicsUnlockTheGateAndRestartResetsProgress();
    if (failures != 0) {
        std::cerr << failures << " adventure game test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All adventure game tests passed\n";
    return EXIT_SUCCESS;
}
