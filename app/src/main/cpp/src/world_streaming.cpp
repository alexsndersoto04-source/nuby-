#include "nuby/world_streaming.hpp"

#include <cmath>
#include <unordered_set>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumWorldZones = 4096;
constexpr float kMaximumZoneRadius = 1000000.0f;

bool finitePosition(const std::array<float, 3>& position) {
    return std::isfinite(position[0]) && std::isfinite(position[1])
            && std::isfinite(position[2]);
}
}  // namespace

bool WorldZoneStreamer::configure(const std::vector<WorldZoneDefinition>& zones,
                                  std::string& error) {
    error.clear();
    if (zones.size() > kMaximumWorldZones) {
        error = "World contains more zones than the supported limit";
        return false;
    }

    std::unordered_set<std::uint32_t> ids;
    std::vector<ZoneState> candidate;
    candidate.reserve(zones.size());
    for (const WorldZoneDefinition& zone : zones) {
        if (zone.id == 0 || !ids.insert(zone.id).second || !finitePosition(zone.center)
                || !std::isfinite(zone.loadRadius) || !std::isfinite(zone.unloadRadius)
                || zone.loadRadius <= 0.0f || zone.unloadRadius <= zone.loadRadius
                || zone.unloadRadius > kMaximumZoneRadius) {
            error = "World zone definition is invalid or duplicated";
            return false;
        }
        candidate.push_back({zone, false});
    }

    zones_.swap(candidate);
    error.clear();
    return true;
}

bool WorldZoneStreamer::update(const std::array<float, 3>& worldPosition,
                               std::vector<WorldZoneTransition>& transitions) {
    transitions.clear();
    if (!finitePosition(worldPosition)) return false;

    for (ZoneState& zone : zones_) {
        const double dx = static_cast<double>(worldPosition[0])
                - static_cast<double>(zone.definition.center[0]);
        const double dz = static_cast<double>(worldPosition[2])
                - static_cast<double>(zone.definition.center[2]);
        const double distanceSquared = dx * dx + dz * dz;
        const double transitionRadius = zone.loaded ? zone.definition.unloadRadius
                                                    : zone.definition.loadRadius;
        const bool shouldBeLoaded = distanceSquared <= transitionRadius * transitionRadius;
        if (shouldBeLoaded == zone.loaded) continue;
        zone.loaded = shouldBeLoaded;
        transitions.push_back({zone.definition.id, shouldBeLoaded});
    }
    return true;
}

bool WorldZoneStreamer::isLoaded(std::uint32_t zoneId) const {
    for (const ZoneState& zone : zones_) {
        if (zone.definition.id == zoneId) return zone.loaded;
    }
    return false;
}

std::size_t WorldZoneStreamer::loadedCount() const {
    std::size_t count = 0;
    for (const ZoneState& zone : zones_) {
        if (zone.loaded) ++count;
    }
    return count;
}

}  // namespace nuby
