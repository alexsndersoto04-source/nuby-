#pragma once

#include "nuby/adventure_save.hpp"
#include "nuby/async_scene_asset_loader.hpp"
#include "nuby/scene.hpp"
#include "nuby/world_streaming.hpp"
#include "nuby/world_zone_manifest.hpp"

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
    using ZoneManifestReader = std::function<bool(WorldZoneManifest&, std::string&)>;
    using ZoneAssetsChangedCallback = std::function<void()>;
    void setZoneSceneReader(ZoneSceneReader reader);
    void setZoneManifestReader(ZoneManifestReader reader) {
        zoneManifestReader_ = std::move(reader);
    }
    void setZoneAssetsChangedCallback(ZoneAssetsChangedCallback callback) {
        zoneAssetsChangedCallback_ = std::move(callback);
    }

    bool completed() const { return completed_; }
    std::size_t zoneLoadFailureCount() const { return zoneLoadFailureCount_; }
    std::size_t residentZoneCount() const { return worldZoneStreamer_.loadedCount(); }
    std::size_t zoneCount() const { return zoneDefinitions_.size(); }
    int relicsCollected() const { return relicsCollected_; }
    std::uint32_t playerId() const { return playerId_; }
    std::string statusText() const;
    bool saveData(const Scene& scene, AdventureSaveData& output, std::string& error) const;
    bool loadData(Scene& scene, const AdventureSaveData& data, std::string& error);

private:
    void buildScene(Scene& scene);
    bool applyAuthoredObjectives(Scene& scene,
                                 const std::vector<WorldObjectiveDefinition>& objectives);
    void applyZoneTransitions(Scene& scene);
    void loadZone(Scene& scene, std::uint32_t zoneId, double priority = 0.0);
    void loadZoneFallback(Scene& scene, std::uint32_t zoneId);
    void integrateReadyZoneScenes(Scene& scene);
    void unloadZone(Scene& scene, std::uint32_t zoneId);

    std::uint32_t playerId_ = 0;
    std::uint32_t cameraId_ = 0;
    std::uint32_t gateId_ = 0;
    std::uint32_t gateTriggerId_ = 0;
    std::vector<std::uint32_t> relicIds_;
    std::array<std::uint32_t, static_cast<std::size_t>(kRelicGoal)> relicEntityIds_{};
    std::array<float, 2> moveInput_{0.0f, 0.0f};
    std::vector<std::vector<std::uint32_t>> streamedZoneEntityIds_;
    std::vector<WorldZoneDefinition> zoneDefinitions_;
    std::vector<std::array<float, 3>> streamedZoneOffsets_;
    std::vector<bool> streamedZoneLoaded_;
    std::vector<bool> streamedZonePending_;
    std::unique_ptr<AsyncSceneAssetLoader> zoneSceneLoader_;
    ZoneManifestReader zoneManifestReader_;
    ZoneAssetsChangedCallback zoneAssetsChangedCallback_;
    std::vector<WorldZoneTransition> zoneTransitionsScratch_;
    std::vector<SceneAssetLoadResult> zoneLoadResultsScratch_;
    std::size_t zoneLoadFailureCount_ = 0;
    WorldZoneStreamer worldZoneStreamer_;
    int relicsCollected_ = 0;
    std::uint8_t collectedRelicMask_ = 0U;
    bool completed_ = false;
};

}  // namespace nuby
