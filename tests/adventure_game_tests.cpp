#include "nuby/adventure_game.hpp"

#include <cmath>
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
                   && player->rigidbody.enabled && player->sphereCollider.enabled,
           "adventure player uses the character controller and scene physics");
    expect(camera != nullptr && camera->orbitCamera.enabled,
           "adventure has an orbit-follow camera");
    const nuby::Entity* head = scene.find(findEntityId(scene, "Wayfarer Head"));
    expect(head != nullptr && head->parentId == game.playerId()
                   && head->material.albedoTexture == "textures/wayfarer_face.tga",
           "wayfarer has a face-textured head attached to the moving player");
    expect(scene.physicsSettings().floorEnabled && scene.entities().size() > 15,
           "adventure constructs a standalone courtyard and its own scenery");
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
    testMovementJumpAndCameraFollow();
    testRelicsUnlockTheGateAndRestartResetsProgress();
    if (failures != 0) {
        std::cerr << failures << " adventure game test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All adventure game tests passed\n";
    return EXIT_SUCCESS;
}
