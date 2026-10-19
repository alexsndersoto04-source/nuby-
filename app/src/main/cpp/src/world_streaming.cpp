#include "nuby/world_streaming.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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

bool transitionPriorityLess(const std::pair<double, WorldZoneTransition>& first,
                           const std::pair<double, WorldZoneTransition>& second) {
    if (first.second.load != second.second.load) return first.second.load;
    if (first.first != second.first) return first.first < second.first;
    return first.second.zoneId < second.second.zoneId;
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
                || !std::isfinite(zone.prefetchRadius) || zone.loadRadius <= 0.0f
                || zone.unloadRadius <= zone.loadRadius
                || zone.unloadRadius > kMaximumZoneRadius
                || (zone.prefetchRadius != 0.0f
                        && (zone.prefetchRadius < zone.loadRadius
                                || zone.prefetchRadius > kMaximumZoneRadius))) {
            error = "World zone definition is invalid or duplicated";
            return false;
        }
        WorldZoneDefinition definition = zone;
        if (definition.prefetchRadius == 0.0f) {
            definition.prefetchRadius = definition.loadRadius;
        }
        candidate.push_back({std::move(definition), false, false});
    }

    std::vector<std::pair<double, std::size_t>> candidateScratch;
    candidateScratch.reserve(zones.size());
    std::vector<std::pair<double, WorldZoneTransition>> transitionScratch;
    transitionScratch.reserve(zones.size());
    std::vector<double> priorityByZone(zones.size(), 0.0);
    std::vector<bool> nextLoaded(zones.size(), false);
    zones_.swap(candidate);
    residencyCandidates_.swap(candidateScratch);
    transitionScratch_.swap(transitionScratch);
    priorityByZone_.swap(priorityByZone);
    nextLoaded_.swap(nextLoaded);
    maximumResidentZones_ = std::min(maximumResidentZones, zones_.size());
    error.clear();
    return true;
}

bool WorldZoneStreamer::update(const std::array<float, 3>& worldPosition,
                               std::vector<WorldZoneTransition>& transitions) {
    return update(worldPosition, {0.0f, 0.0f, 0.0f}, transitions);
}

bool WorldZoneStreamer::update(const std::array<float, 3>& worldPosition,
                               const std::array<float, 3>& worldVelocity,
                               std::vector<WorldZoneTransition>& transitions) {
    transitions.clear();
    if (!finitePosition(worldPosition) || !finitePosition(worldVelocity)) return false;

    constexpr double kLookAheadDistance = 6.0;
    constexpr double kLoadedZoneRetentionBonus = 9.0;
    constexpr double kForwardPrefetchDot = 0.35;
    const double speed = std::hypot(static_cast<double>(worldVelocity[0]),
                                    static_cast<double>(worldVelocity[2]));
    const double directionX = speed > 1.0e-4 ? worldVelocity[0] / speed : 0.0;
    const double directionZ = speed > 1.0e-4 ? worldVelocity[2] / speed : 0.0;
    const double predictedX = worldPosition[0] + directionX * kLookAheadDistance;
    const double predictedZ = worldPosition[2] + directionZ * kLookAheadDistance;

    residencyCandidates_.clear();
    for (std::size_t index = 0; index < zones_.size(); ++index) {
        const ZoneState& zone = zones_[index];
        const double toZoneX = static_cast<double>(zone.definition.center[0])
                - worldPosition[0];
        const double toZoneZ = static_cast<double>(zone.definition.center[2])
                - worldPosition[2];
        const double distanceSquared = toZoneX * toZoneX + toZoneZ * toZoneZ;
        const double distance = std::sqrt(distanceSquared);
        const double forwardDot = distance > 1.0e-8
                ? (toZoneX * directionX + toZoneZ * directionZ) / distance : 0.0;
        bool eligible = distanceSquared <= static_cast<double>(zone.definition.unloadRadius)
                * zone.definition.unloadRadius;
        if (zone.loaded && !eligible && zone.prefetched
                && distanceSquared <= static_cast<double>(zone.definition.prefetchRadius)
                        * zone.definition.prefetchRadius) {
            eligible = speed <= 1.0e-4 || forwardDot >= kForwardPrefetchDot;
        } else if (!zone.loaded) {
            eligible = distanceSquared <= static_cast<double>(zone.definition.loadRadius)
                    * zone.definition.loadRadius;
            if (!eligible && speed > 1.0e-4
                    && zone.definition.prefetchRadius > zone.definition.loadRadius) {
                eligible = forwardDot >= kForwardPrefetchDot
                        && distanceSquared <= static_cast<double>(zone.definition.prefetchRadius)
                                * zone.definition.prefetchRadius;
            }
        }
        if (!eligible) continue;
        const double predictedDx = static_cast<double>(zone.definition.center[0]) - predictedX;
        const double predictedDz = static_cast<double>(zone.definition.center[2]) - predictedZ;
        double priorityDistance = predictedDx * predictedDx + predictedDz * predictedDz;
        if (zone.loaded) priorityDistance = std::max(0.0,
                priorityDistance - kLoadedZoneRetentionBonus);
        residencyCandidates_.emplace_back(priorityDistance, index);
    }

    const auto priorityOrder = [](const auto& first, const auto& second) {
        if (first.first != second.first) return first.first < second.first;
        return first.second < second.second;
    };
    if (residencyCandidates_.size() > maximumResidentZones_) {
        std::partial_sort(residencyCandidates_.begin(),
                residencyCandidates_.begin() + maximumResidentZones_,
                residencyCandidates_.end(), priorityOrder);
        residencyCandidates_.resize(maximumResidentZones_);
    }
    std::fill(nextLoaded_.begin(), nextLoaded_.end(), false);
    std::fill(priorityByZone_.begin(), priorityByZone_.end(),
              std::numeric_limits<double>::max());
    for (const auto& candidate : residencyCandidates_) {
        nextLoaded_[candidate.second] = true;
        priorityByZone_[candidate.second] = candidate.first;
    }

    transitionScratch_.clear();
    for (std::size_t index = 0; index < zones_.size(); ++index) {
        ZoneState& zone = zones_[index];
        const double dx = static_cast<double>(zone.definition.center[0]) - worldPosition[0];
        const double dz = static_cast<double>(zone.definition.center[2]) - worldPosition[2];
        const double distanceSquared = dx * dx + dz * dz;
        if (zone.loaded == nextLoaded_[index]) {
            if (zone.loaded && distanceSquared <= static_cast<double>(zone.definition.loadRadius)
                    * zone.definition.loadRadius) {
                zone.prefetched = false;
            }
            continue;
        }
        zone.loaded = nextLoaded_[index];
        zone.prefetched = zone.loaded
                && distanceSquared > static_cast<double>(zone.definition.loadRadius)
                        * zone.definition.loadRadius;
        double priorityDistance = priorityByZone_[index];
        if (!zone.loaded && priorityDistance == std::numeric_limits<double>::max()) {
            const double dx = static_cast<double>(zone.definition.center[0]) - predictedX;
            const double dz = static_cast<double>(zone.definition.center[2]) - predictedZ;
            priorityDistance = dx * dx + dz * dz;
        }
        transitionScratch_.push_back({priorityDistance,
                {zone.definition.id, zone.loaded}});
    }
    std::sort(transitionScratch_.begin(), transitionScratch_.end(), transitionPriorityLess);
    for (const auto& transition : transitionScratch_) {
        transitions.push_back(transition.second);
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
