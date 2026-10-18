#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nuby {

// A horizontal world sector. Vertical position is intentionally ignored so a
// jump or height change does not churn terrain/scenery residency.
struct WorldZoneDefinition {
    std::uint32_t id = 0;
    std::array<float, 3> center{0.0f, 0.0f, 0.0f};
    float loadRadius = 0.0f;
    float unloadRadius = 0.0f;
};

struct WorldZoneTransition {
    std::uint32_t zoneId = 0;
    bool load = false;
};

// Deterministic distance-based residency policy. Content ownership stays with
// the caller, which applies the returned load/unload transitions.
class WorldZoneStreamer final {
public:
    bool configure(const std::vector<WorldZoneDefinition>& zones, std::string& error);
    bool update(const std::array<float, 3>& worldPosition,
                std::vector<WorldZoneTransition>& transitions);
    bool isLoaded(std::uint32_t zoneId) const;
    std::size_t loadedCount() const;
    std::size_t zoneCount() const { return zones_.size(); }

private:
    struct ZoneState {
        WorldZoneDefinition definition;
        bool loaded = false;
    };

    std::vector<ZoneState> zones_;
};

}  // namespace nuby
