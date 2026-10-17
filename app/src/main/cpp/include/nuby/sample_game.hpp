#pragma once

#include "nuby/scene.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nuby {

enum class SampleGameState : std::uint8_t {
    Playing,
    Won,
    Lost
};

// A compact, engine-owned 3D runner used to exercise scene, physics and trigger APIs.
class SampleGame final {
public:
    static constexpr int kScoreGoal = 6;
    static constexpr int kRunDurationSeconds = 45;

    void start(Scene& scene);
    void update(Scene& scene, float deltaSeconds);
    bool shiftLane(int laneDelta);

    SampleGameState state() const { return state_; }
    int score() const { return score_; }
    int lane() const { return lane_; }
    int secondsRemaining() const;
    std::string statusText() const;

private:
    enum class HazardType : std::uint8_t {
        Collectible,
        Obstacle
    };

    void buildScene(Scene& scene);
    void spawnObject(Scene& scene);

    SampleGameState state_ = SampleGameState::Lost;
    std::uint32_t playerId_ = 0;
    int lane_ = 1;
    int score_ = 0;
    float elapsedSeconds_ = 0.0f;
    float spawnCountdown_ = 0.0f;
    std::uint32_t spawnIndex_ = 0;
    std::unordered_map<std::uint32_t, HazardType> movingObjects_;
};

}  // namespace nuby
