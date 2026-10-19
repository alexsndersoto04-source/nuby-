#include "nuby/world_streaming.hpp"

#include <algorithm>
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
                                  std::string& error,
                                  std::size_t maximumResidentZones) {
    error.clear();
    if (zones.size() > kMaximumWorldZones
            || (!zones.empty() && maximumResidentZones == 0U)) {
        error = "World zone count or residency budget is invalid";
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

    std::vector<std::pair<double, std::size_t>> candidateScratch;
    candidateScratch.reserve(zones.size());
    std::vector<bool> nextLoaded(zones.size(), false);
    zones_.swap(candidate);
    residencyCandidates_.swap(candidateScratch);
    nextLoaded_.swap(nextLoaded);
    maximumResidentZones_ = std::min(maximumResidentZones, zones_.size());
    error.clear();
    return true;
}

bool WorldZoneStreamer::update(const std::array<float, 3>& worldPosition,
                               std::vector<WorldZoneTransition>& transitions) {
    transitions.clear();
    if (!finitePosition(worldPosition)) return false;

    residencyCandidates_.clear();
    for (std::size_t index = 0; index < zones_.size(); ++index) {
        const ZoneState& zone = zones_[index];
        const double dx = static_cast<double>(worldPosition[0])
                - static_cast<double>(zone.definition.center[0]);
        const double dz = static_cast<double>(worldPosition[2])
                - static_cast<double>(zone.definition.center[2]);
        const double distanceSquared = dx * dx + dz * dz;
        const double transitionRadius = zone.loaded ? zone.definition.unloadRadius
                                                    : zone.definition.loadRadius;
        if (distanceSquared <= transitionRadius * transitionRadius) {
            residencyCandidates_.emplace_back(distanceSquared, index);
        }
    }

    if (residencyCandidates_.size() > maximumResidentZones_) {
        const auto priorityOrder = [](const auto& first, const auto& second) {
            if (first.first != second.first) return first.first < second.first;
            return first.second < second.second;
        };
        std::partial_sort(residencyCandidates_.begin(),
                residencyCandidates_.begin() + maximumResidentZones_,
                residencyCandidates_.end(), priorityOrder);
        residencyCandidates_.resize(maximumResidentZones_);
    }
    std::fill(nextLoaded_.begin(), nextLoaded_.end(), false);
    for (const auto& candidate : residencyCandidates_) {
        nextLoaded_[candidate.second] = true;
    }

    for (std::size_t index = 0; index < zones_.size(); ++index) {
        ZoneState& zone = zones_[index];
        if (zone.loaded == nextLoaded_[index]) continue;
        zone.loaded = nextLoaded_[index];
        transitions.push_back({zone.definition.id, zone.loaded});
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
