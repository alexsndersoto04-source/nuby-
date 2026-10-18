#pragma once

#include "nuby/async_scene_asset_loader.hpp"
#include "nuby/scene.hpp"
#include "nuby/world_streaming.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
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
    using ZoneSceneReader = AsyncSceneAssetLoader::Reader;
    using ZoneAssetsChangedCallback = std::function<void()>;
    void setZoneSceneReader(ZoneSceneReader reader);
    void setZoneAssetsChangedCallback(ZoneAssetsChangedCallback callback) {
        zoneAssetsChangedCallback_ = std::move(callback);
    }

    bool completed() const { return completed_; }
    std::size_t zoneLoadFailureCount() const { return zoneLoadFailureCount_; }
    int relicsCollected() const { return relicsCollected_; }
    std::uint32_t playerId() const { return playerId_; }
    std::string statusText() const;

private:
    void buildScene(Scene& scene);
    void applyZoneTransitions(Scene& scene);
    void loadZone(Scene& scene, std::uint32_t zoneId);
    void loadZoneFallback(Scene& scene, std::uint32_t zoneId);
    void integrateReadyZoneScenes(Scene& scene);
    void unloadZone(Scene& scene, std::uint32_t zoneId);

    std::uint32_t playerId_ = 0;
    std::uint32_t cameraId_ = 0;
    std::uint32_t gateTriggerId_ = 0;
    std::vector<std::uint32_t> relicIds_;
    std::array<float, 2> moveInput_{0.0f, 0.0f};
    std::array<std::vector<std::uint32_t>, 3> streamedZoneEntityIds_;
    std::array<WorldZoneDefinition, 3> zoneDefinitions_{};
    std::array<bool, 3> streamedZoneLoaded_{};
    std::array<bool, 3> streamedZonePending_{};
    std::unique_ptr<AsyncSceneAssetLoader> zoneSceneLoader_;
    ZoneAssetsChangedCallback zoneAssetsChangedCallback_;
    std::vector<WorldZoneTransition> zoneTransitionsScratch_;
    std::vector<SceneAssetLoadResult> zoneLoadResultsScratch_;
    std::size_t zoneLoadFailureCount_ = 0;
    WorldZoneStreamer worldZoneStreamer_;
    int relicsCollected_ = 0;
    bool completed_ = false;
};

}  // namespace nuby
