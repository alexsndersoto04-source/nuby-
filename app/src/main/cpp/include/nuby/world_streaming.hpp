#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nuby {

// A horizontal world sector. Vertical position is intentionally ignored so a
// jump or height change does not churn terrain/scenery residency.
struct WorldZoneDefinition {
    std::uint32_t id = 0;
    std::array<float, 3> center{0.0f, 0.0f, 0.0f};
    float loadRadius = 0.0f;
    float unloadRadius = 0.0f;
    std::string sceneAssetPath;
    // Optional forward-only look-ahead radius for streaming while moving.
    // Zero disables prefetch and preserves loadRadius behavior.
    float prefetchRadius = 0.0f;
};

struct WorldZoneTransition {
    std::uint32_t zoneId = 0;
    bool load = false;
    // Higher values are scheduled first by asynchronous scene readers.
    double priority = 0.0;
};

// Deterministic distance-based residency policy. Content ownership stays with
// the caller, which applies the returned load/unload transitions.
class WorldZoneStreamer final {
public:
    bool configure(const std::vector<WorldZoneDefinition>& zones, std::string& error,
                   std::size_t maximumResidentZones = 4096U);
    bool update(const std::array<float, 3>& worldPosition,
                std::vector<WorldZoneTransition>& transitions);
    bool update(const std::array<float, 3>& worldPosition,
                const std::array<float, 3>& worldVelocity,
                std::vector<WorldZoneTransition>& transitions);
    bool isLoaded(std::uint32_t zoneId) const;
    std::size_t loadedCount() const;
    std::size_t zoneCount() const { return zones_.size(); }

private:
    struct ZoneState {
        WorldZoneDefinition definition;
        bool loaded = false;
        bool prefetched = false;
    };

    std::vector<ZoneState> zones_;
    std::vector<std::pair<double, std::size_t>> residencyCandidates_;
    std::vector<std::pair<double, WorldZoneTransition>> transitionScratch_;
    std::vector<double> priorityByZone_;
    std::vector<bool> nextLoaded_;
    std::size_t maximumResidentZones_ = 4096U;
};

}  // namespace nuby
