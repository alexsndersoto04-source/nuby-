#pragma once

#include "nuby/scene.hpp"
#include "nuby/world_streaming.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nuby {

// A small, standalone third-person adventure slice used to exercise the
// character controller, camera follow, physics triggers and mobile controls.
class AdventureGame final {
public:
    static constexpr int kRelicGoal = 3;

    void start(Scene& scene);
    void update(Scene& scene, float deltaSeconds);
    bool setMoveInput(Scene& scene, float horizontal, float forward);
    bool jump(Scene& scene);

    bool completed() const { return completed_; }
    int relicsCollected() const { return relicsCollected_; }
    std::uint32_t playerId() const { return playerId_; }
    std::string statusText() const;

private:
    void buildScene(Scene& scene);
    void applyZoneTransitions(Scene& scene);
    void loadZone(Scene& scene, std::uint32_t zoneId);
    void unloadZone(Scene& scene, std::uint32_t zoneId);

    std::uint32_t playerId_ = 0;
    std::uint32_t cameraId_ = 0;
    std::uint32_t gateTriggerId_ = 0;
    std::vector<std::uint32_t> relicIds_;
    std::array<float, 2> moveInput_{0.0f, 0.0f};
    std::array<std::vector<std::uint32_t>, 3> streamedZoneEntityIds_;
    std::vector<WorldZoneTransition> zoneTransitionsScratch_;
    WorldZoneStreamer worldZoneStreamer_;
    int relicsCollected_ = 0;
    bool completed_ = false;
};

}  // namespace nuby
