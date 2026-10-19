#include "nuby/adventure_game.hpp"
#include "nuby/scene_io.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::uint32_t findEntityId(const nuby::Scene& scene, const std::string& name);

void advance(nuby::AdventureGame& game, nuby::Scene& scene, int frames) {
    for (int frame = 0; frame < frames; ++frame) {
        game.update(scene, 1.0f / 60.0f);
    }
}

template <typename Predicate>
bool waitForGameUpdate(nuby::AdventureGame& game, nuby::Scene& scene,
                       Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        game.update(scene, 0.0f);
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    game.update(scene, 0.0f);
    return predicate();
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
    expect(game.statusText().find("0/3") != std::string::npos
                   && game.statusText().find("ZONAS 1/9") != std::string::npos,
           "the HUD reports mission progress and live residency within the nine-cell world");
    expect(game.zoneCount() == 9U && game.residentZoneCount() == 1U,
           "the adventure starts with a single resident cell and the full world map configured");
}

void testWorldLayoutCanBeAuthoredByManifest() {
    nuby::AdventureGame game;
    game.setZoneManifestReader([](nuby::WorldZoneManifest& manifest, std::string&) {
        manifest.maximumResidentZones = 1U;
        manifest.zones = {
                {1U, {0.0f, 0.0f, 0.0f}, 5.0f, 7.0f, ""},
                {2U, {30.0f, 0.0f, -4.0f}, 5.0f, 7.0f, ""}
        };
        return true;
    });
    nuby::Scene scene;
    game.start(scene);
    expect(game.zoneCount() == 2U && game.residentZoneCount() == 1U
                   && game.statusText().find("ZONAS 1/2") != std::string::npos,
           "adventure consumes authored manifest definitions and resident budget");
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
    const nuby::Entity* hair = scene.find(findEntityId(scene, "Wayfarer Hair"));
    const nuby::Entity* satchel = scene.find(findEntityId(scene, "Wayfarer Satchel"));
    expect(body != nullptr && body->skeletalAnimation.clipName == "idle",
           "Wayfarer starts with the idle clip");
    expect(hair != nullptr && satchel != nullptr && satchel->transform.position[2] < 0.0f,
           "the detailed explorer model has a hair silhouette and a back-mounted satchel");
    advance(game, scene, 1);

    expect(game.setMoveInput(scene, 0.0f, 1.0f), "walking input is accepted");
    advance(game, scene, 1);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "walk"
                   && body->skeletalAnimation.transitionFromClip == "idle"
                   && body->skeletalAnimation.transitionElapsedSeconds == 0.0f,
           "low movement speed begins a smooth idle-to-walk crossfade");
    advance(game, scene, 15);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "run",
           "high movement speed selects the run clip");

    expect(game.setMoveInput(scene, 0.0f, 0.0f), "idle input is accepted");
    advance(game, scene, 30);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "idle"
                   && body->skeletalAnimation.transitionFromClip.empty(),
           "stopping movement returns to idle and lets the previous blend finish");
    expect(game.jump(scene), "animation-state test can jump from rest");
    advance(game, scene, 1);
    body = scene.find(bodyId);
    expect(body != nullptr && body->skeletalAnimation.clipName == "jump"
                   && !body->skeletalAnimation.looping
                   && body->skeletalAnimation.transitionFromClip == "idle"
                   && body->skeletalAnimation.transitionFromLooping,
           "jumping starts a smooth idle-to-non-looping-jump crossfade");
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

void testAdventureStreamsAcrossNineCellsWithBoundedResidency() {
    nuby::AdventureGame game;
    nuby::Scene scene;
    game.start(scene);
    expect(game.zoneCount() == 9U && game.residentZoneCount() == 1U
                   && findEntityId(scene, "Ruin Zone 1 Pillar 1") != 0
                   && findEntityId(scene, "Ruin Zone 1 Pillar 1 East Cell") == 0,
           "adventure begins in the center cell of a nine-cell world and keeps distant scenery unloaded");

    const std::uint32_t playerId = game.playerId();
    const std::uint32_t cameraId = scene.activeCamera() == nullptr
            ? 0U : scene.activeCamera()->id;
    movePlayerTo(scene, playerId, -2.15f, -3.0f);
    advance(game, scene, 1);
    expect(game.relicsCollected() == 1,
           "the existing mission remains playable while the world is split into cells");

    movePlayerTo(scene, playerId, 5.0f, -5.25f);
    advance(game, scene, 1);
    const nuby::Entity* eastPillar = scene.find(
            findEntityId(scene, "Ruin Zone 1 Pillar 1 East Cell"));
    expect(game.residentZoneCount() == 4U && eastPillar != nullptr
                   && std::fabs(eastPillar->transform.position[0] - 6.65f) < 1.0e-5f,
           "crossing the middle of the map loads translated east cells while respecting the four-cell budget");
    expect(scene.find(playerId) != nullptr && scene.activeCamera() != nullptr
                   && scene.activeCamera()->id == cameraId && game.relicsCollected() == 1,
           "cell transitions preserve the player, camera and collected mission progress");

    movePlayerTo(scene, playerId, -5.0f, -5.25f);
    advance(game, scene, 1);
    const nuby::Entity* westPillar = scene.find(
            findEntityId(scene, "Ruin Zone 1 Pillar 1 West Cell"));
    expect(game.residentZoneCount() == 4U && westPillar != nullptr
                   && std::fabs(westPillar->transform.position[0] + 15.35f) < 1.0e-5f
                   && findEntityId(scene, "Ruin Zone 1 Pillar 1 East Cell") == 0,
           "moving west releases east-cell entities and places the west-cell scene at its world offset");
}

void testAdventureLoadsSerializedZoneScenesAndPreservesMissionState() {
    std::atomic<int> sceneAssetsRead{0};
    int resourceChanges = 0;
    nuby::AdventureGame game;
    game.setZoneAssetsChangedCallback([&resourceChanges]() { ++resourceChanges; });
    game.setZoneSceneReader([&sceneAssetsRead](const std::string& assetPath,
                                                nuby::Scene& fragment,
                                                std::string& error) {
        sceneAssetsRead.fetch_add(1);
        std::ifstream input("app/src/main/assets/" + assetPath, std::ios::binary);
        if (!input) {
            error = "zone asset not found";
            return false;
        }
        return nuby::SceneSerializer::load(fragment, input, error);
    });
    nuby::Scene scene;
    game.start(scene);
    const bool firstZoneLoaded = waitForGameUpdate(game, scene, [&scene]() {
        return findEntityId(scene, "Ruin Zone 1 Pillar 1") != 0;
    });

    expect(firstZoneLoaded && sceneAssetsRead.load() == 1 && resourceChanges == 1
                   && game.zoneLoadFailureCount() == 0,
           "adventure asynchronously reads the nearby scene and notifies the renderer to load its resources");
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
    const bool thirdZoneLoaded = waitForGameUpdate(game, scene, [&scene]() {
        return findEntityId(scene, "Ruin Zone 1 Pillar 1") == 0
                && findEntityId(scene, "Ruin Zone 3 Pillar 9") != 0;
    });
    expect(thirdZoneLoaded,
           "async streaming unloads distant assets and integrates the new scene without blocking the game loop");
    expect(scene.find(game.playerId()) != nullptr && scene.activeCamera() != nullptr
                   && game.relicsCollected() == 1 && scene.find(firstRelic) == nullptr,
           "zone transitions preserve player, camera, and collected mission progress");
    expect(sceneAssetsRead.load() >= 3 && resourceChanges >= 3
                   && game.zoneLoadFailureCount() == 0,
           "each new zone scene triggers resource loading without falling back");
}

void testSerializedEastCellIsTranslatedAndUsesUniqueNames() {
    nuby::AdventureGame game;
    game.setZoneSceneReader([](const std::string& assetPath, nuby::Scene& fragment,
                               std::string& error) {
        std::ifstream input("app/src/main/assets/" + assetPath, std::ios::binary);
        if (!input) {
            error = "zone asset not found";
            return false;
        }
        return nuby::SceneSerializer::load(fragment, input, error);
    });
    nuby::Scene scene;
    game.start(scene);
    expect(waitForGameUpdate(game, scene, [&scene]() {
        return findEntityId(scene, "Ruin Zone 1 Pillar 1") != 0;
    }), "the serialized center cell becomes resident before traversing east");

    movePlayerTo(scene, game.playerId(), 5.0f, -5.25f);
    advance(game, scene, 1);
    const bool eastCellLoaded = waitForGameUpdate(game, scene, [&scene]() {
        return findEntityId(scene, "Ruin Zone 1 Pillar 1 East Cell") != 0;
    });
    const nuby::Entity* eastPillar = scene.find(
            findEntityId(scene, "Ruin Zone 1 Pillar 1 East Cell"));
    expect(eastCellLoaded && eastPillar != nullptr
                   && eastPillar->primitive == nuby::Primitive::ImportedMesh
                   && eastPillar->meshAsset == "meshes/ruin_pillar.obj"
                   && std::fabs(eastPillar->transform.position[0] - 6.65f) < 1.0e-5f,
           "a real serialized scene fragment is translated into the east cell without replacing its asset references");
    expect(findEntityId(scene, "Ruin Zone 1 Pillar 1") != 0
                   && findEntityId(scene, "Ruin Zone 1 Pillar 1 East Cell") != 0
                   && game.residentZoneCount() <= 4U,
           "neighbor cells coexist with unique entity names under the residency budget");
}

void testAdventureCancelsZoneReadAfterLeavingItsResidencyRange() {
    std::mutex mutex;
    std::condition_variable condition;
    bool firstZoneReadStarted = false;
    bool releaseFirstZoneRead = false;
    nuby::AdventureGame game;
    game.setZoneSceneReader([&mutex, &condition, &firstZoneReadStarted,
                             &releaseFirstZoneRead](const std::string& assetPath,
                                                    nuby::Scene& fragment,
                                                    std::string& error) {
        if (assetPath == "scenes/zones/ruins_01.scene") {
            std::unique_lock<std::mutex> lock(mutex);
            firstZoneReadStarted = true;
            condition.notify_all();
            condition.wait(lock, [&releaseFirstZoneRead]() { return releaseFirstZoneRead; });
        }
        std::ifstream input("app/src/main/assets/" + assetPath, std::ios::binary);
        if (!input) {
            error = "zone asset not found";
            return false;
        }
        return nuby::SceneSerializer::load(fragment, input, error);
    });
    nuby::Scene scene;
    game.start(scene);

    bool firstReadStarted = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        firstReadStarted = condition.wait_for(lock, std::chrono::seconds(2),
                [&firstZoneReadStarted]() { return firstZoneReadStarted; });
    }
    expect(firstReadStarted, "the first zone begins loading on the worker thread");
    if (!firstReadStarted) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            releaseFirstZoneRead = true;
        }
        condition.notify_all();
        return;
    }

    movePlayerTo(scene, game.playerId(), 0.0f, -13.0f);
    advance(game, scene, 1);
    {
        std::lock_guard<std::mutex> lock(mutex);
        releaseFirstZoneRead = true;
    }
    condition.notify_all();

    const bool farZoneLoaded = waitForGameUpdate(game, scene, [&scene]() {
        return findEntityId(scene, "Ruin Zone 3 Pillar 9") != 0;
    });
    expect(farZoneLoaded && findEntityId(scene, "Ruin Zone 1 Pillar 1") == 0
                   && findEntityId(scene, "Ruin Zone 3 Pillar 9") != 0
                   && game.zoneLoadFailureCount() == 0,
           "a stale in-flight read is discarded after zone exit while current-zone requests succeed");
}

void testMissingZoneSceneFallsBackWithoutBreakingAdventure() {
    nuby::AdventureGame game;
    game.setZoneSceneReader([](const std::string&, nuby::Scene&, std::string& error) {
        error = "simulated missing or corrupt scene";
        return false;
    });
    nuby::Scene scene;
    game.start(scene);
    const bool fallbackLoaded = waitForGameUpdate(game, scene, [&game, &scene]() {
        return game.zoneLoadFailureCount() == 1
                && findEntityId(scene, "Ruin Zone 1 Pillar 1") != 0;
    });
    expect(fallbackLoaded,
           "a missing scene asynchronously records its error and keeps fallback scenery");
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

void testAdventureProgressCanBeSavedAndRestoredIntoANewSession() {
    nuby::AdventureGame original;
    nuby::Scene originalScene;
    original.start(originalScene);
    movePlayerTo(originalScene, original.playerId(), -2.15f, -3.0f);
    advance(original, originalScene, 1);

    nuby::AdventureSaveData save;
    std::string error;
    expect(original.relicsCollected() == 1
                   && original.saveData(originalScene, save, error)
                   && save.collectedRelicMask == 0x01U,
           "adventure snapshot captures the collected relic and player progress");

    nuby::AdventureGame resumed;
    nuby::Scene resumedScene;
    resumed.start(resumedScene);
    const std::uint32_t resumedFirstRelic = findEntityId(resumedScene, "Sun Relic 1");
    expect(resumed.loadData(resumedScene, save, error),
           "valid saved progress restores into a freshly built adventure");
    const nuby::Entity* player = resumedScene.find(resumed.playerId());
    expect(resumed.relicsCollected() == 1
                   && resumedScene.find(resumedFirstRelic) == nullptr
                   && player != nullptr && std::fabs(player->transform.position[0] + 2.15f) < 0.001f
                   && std::fabs(player->transform.position[2] + 3.0f) < 0.001f,
           "restored session positions the player and keeps collected relics removed");

    movePlayerTo(resumedScene, resumed.playerId(), 2.05f, -5.8f);
    advance(resumed, resumedScene, 1);
    expect(resumed.relicsCollected() == 2,
           "resumed session continues collecting remaining relics normally");

    resumed.start(resumedScene);
    nuby::AdventureSaveData resetSave;
    expect(resumed.saveData(resumedScene, resetSave, error)
                   && resetSave.collectedRelicMask == 0U && !resetSave.completed,
           "explicit restart clears every persisted objective flag");
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
    testWorldLayoutCanBeAuthoredByManifest();
    testCharacterFacesItsMovementDirection();
    testWayfarerSelectsIdleWalkRunAndJumpClips();
    testAdventureStreamsSceneryByPlayerZone();
    testAdventureStreamsAcrossNineCellsWithBoundedResidency();
    testAdventureLoadsSerializedZoneScenesAndPreservesMissionState();
    testSerializedEastCellIsTranslatedAndUsesUniqueNames();
    testAdventureCancelsZoneReadAfterLeavingItsResidencyRange();
    testMissingZoneSceneFallsBackWithoutBreakingAdventure();
    testMovementJumpAndCameraFollow();
    testAdventureProgressCanBeSavedAndRestoredIntoANewSession();
    testRelicsUnlockTheGateAndRestartResetsProgress();
    if (failures != 0) {
        std::cerr << failures << " adventure game test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All adventure game tests passed\n";
    return EXIT_SUCCESS;
}
