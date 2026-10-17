#include "nuby/sample_game.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void advance(nuby::SampleGame& game, nuby::Scene& scene, int frames) {
    for (int frame = 0; frame < frames; ++frame) game.update(scene, 1.0f / 60.0f);
}

void testStartAndLaneControls() {
    nuby::SampleGame game;
    nuby::Scene scene;
    scene.create("discarded old scene", nuby::Primitive::Sphere);
    game.start(scene);
    expect(game.state() == nuby::SampleGameState::Playing,
           "starting the sample game enters the playing state");
    expect(game.score() == 0 && game.lane() == 1,
           "a new run resets score and begins in the center lane");
    expect(scene.find(1) != nullptr && scene.find(1)->camera.enabled,
           "sample game builds its own active camera scene");
    expect(scene.find(5) != nullptr && scene.find(5)->name == "Aether Runner",
           "sample game creates the player entity after camera and track scenery");
    expect(game.shiftLane(-1) && game.lane() == 0,
           "lane input moves left from the initial center lane");
    expect(!game.shiftLane(-5) && game.lane() == 0,
           "lane movement clamps to the left edge");
    expect(game.shiftLane(2) && game.lane() == 2,
           "lane movement can reach the right lane");
    expect(!game.shiftLane(1) && game.lane() == 2,
           "lane movement clamps to the right edge");
}

void testCollectiblesHazardsAndCompleteRunLoop() {
    nuby::SampleGame game;
    nuby::Scene scene;
    game.start(scene);

    // First three pickups occupy center, right, and left. The next object is a
    // center-lane hazard; the route below deliberately avoids it and collects six.
    advance(game, scene, 186);
    expect(game.score() == 1 && game.state() == nuby::SampleGameState::Playing,
           "triggering the first shard increments score without ending the run");
    expect(game.shiftLane(1), "player can steer to the right lane");
    advance(game, scene, 42);
    expect(game.score() == 2, "right-lane shard is collected through the trigger system");
    expect(game.shiftLane(-2), "player can steer across to the left lane");
    advance(game, scene, 78);
    expect(game.score() == 3, "left-lane shard is collected");
    expect(game.shiftLane(2), "player can return to the right lane");
    advance(game, scene, 135);
    expect(game.state() == nuby::SampleGameState::Playing,
           "avoiding the center hazard keeps the game active");
    expect(game.score() == 4, "the next right-lane shard is collected");
    expect(game.shiftLane(-2), "player returns to the left lane");
    advance(game, scene, 72);
    expect(game.score() == 5, "the fifth shard is collected");
    expect(game.shiftLane(1), "player moves toward the final center-lane shard");
    advance(game, scene, 72);
    expect(game.score() == nuby::SampleGame::kScoreGoal
                   && game.state() == nuby::SampleGameState::Won,
           "collecting the goal amount completes the run");
    expect(game.statusText().find("COMPLETE") != std::string::npos,
           "victory exposes clear HUD status text");

    game.start(scene);
    expect(game.state() == nuby::SampleGameState::Playing && game.score() == 0
                   && game.lane() == 1 && scene.find(4) != nullptr,
           "restarting rebuilds a clean scene and resets all run state");
}

void testHazardEndsTheRunAndTimerIsBounded() {
    nuby::SampleGame game;
    nuby::Scene scene;
    game.start(scene);
    advance(game, scene, 420);
    expect(game.score() == 1, "the stationary center runner collects its first shard");
    expect(game.state() == nuby::SampleGameState::Lost,
           "contact with the center-lane hazard ends the run");
    expect(game.statusText().find("RUN OVER") != std::string::npos,
           "hazard loss exposes clear HUD status text");

    game.start(scene);
    expect(game.secondsRemaining() == nuby::SampleGame::kRunDurationSeconds,
           "a fresh run starts with the complete timer");
}
}  // namespace

int main() {
    testStartAndLaneControls();
    testCollectiblesHazardsAndCompleteRunLoop();
    testHazardEndsTheRunAndTimerIsBounded();
    if (failures != 0) {
        std::cerr << failures << " sample game test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All sample game tests passed\n";
    return EXIT_SUCCESS;
}
