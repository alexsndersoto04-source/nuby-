#include "nuby/world_streaming.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testZoneLoadingUsesHorizontalDistanceAndStableOrdering() {
    nuby::WorldZoneStreamer streamer;
    std::string error;
    const std::vector<nuby::WorldZoneDefinition> zones{
            {10, {0.0f, 0.0f, 0.0f}, 5.0f, 7.0f, ""},
            {20, {0.0f, 0.0f, 10.0f}, 5.0f, 7.0f, ""}
    };
    expect(streamer.configure(zones, error) && error.empty(),
           "valid world-zone definitions configure successfully");
    std::vector<nuby::WorldZoneTransition> transitions;
    expect(streamer.update({0.0f, 1000.0f, 0.0f}, transitions)
                   && transitions.size() == 1 && transitions[0].zoneId == 10
                   && transitions[0].load,
           "initial residency loads the containing horizontal zone in descriptor order");
    expect(streamer.isLoaded(10) && !streamer.isLoaded(20)
                   && streamer.loadedCount() == 1,
           "loaded-zone state and count are observable");

    expect(streamer.update({0.0f, -500.0f, 6.0f}, transitions)
                   && transitions.size() == 1 && transitions[0].zoneId == 20
                   && transitions[0].load,
           "entering a neighboring zone loads it without vertical-height churn");
    expect(streamer.update({0.0f, 0.0f, 7.1f}, transitions)
                   && transitions.size() == 1 && transitions[0].zoneId == 10
                   && !transitions[0].load,
           "loaded zone remains resident through the hysteresis band then unloads");
    expect(streamer.update({0.0f, 0.0f, 7.1f}, transitions) && transitions.empty(),
           "unchanged player position does not emit duplicate residency transitions");
    expect(streamer.update({0.0f, 0.0f, 2.0f}, transitions)
                   && transitions.size() == 2 && transitions[0].zoneId == 10
                   && transitions[0].load && transitions[1].zoneId == 20
                   && !transitions[1].load,
           "returning to a zone reloads it while the now-distant neighbor unloads");
}

void testGridResidencyHonorsDeterministicBudget() {
    nuby::WorldZoneStreamer streamer;
    std::string error;
    std::vector<nuby::WorldZoneDefinition> zones;
    std::uint32_t id = 1;
    for (float z : {6.0f, 0.0f, -6.0f}) {
        for (float x : {-6.0f, 0.0f, 6.0f}) {
            zones.push_back({id++, {x, 0.0f, z}, 7.0f, 8.0f, ""});
        }
    }
    expect(streamer.configure(zones, error, 3U) && error.empty(),
           "a nine-cell world accepts a bounded resident-zone budget");
    std::vector<nuby::WorldZoneTransition> transitions;
    expect(streamer.update({0.0f, 0.0f, 0.0f}, transitions)
                   && streamer.loadedCount() == 3U
                   && streamer.isLoaded(5U) && streamer.isLoaded(2U)
                   && streamer.isLoaded(4U) && !streamer.isLoaded(6U)
                   && !streamer.isLoaded(8U),
           "the center cell wins, then equal-distance neighbors use manifest order");
    expect(transitions.size() == 3U && transitions[0].zoneId == 5U
                   && transitions[1].zoneId == 2U && transitions[2].zoneId == 4U,
           "initial grid loads emit the nearest cell first, then stable distance ties");

    expect(streamer.update({6.0f, 0.0f, 0.0f}, transitions)
                   && streamer.loadedCount() == 3U
                   && streamer.isLoaded(6U) && streamer.isLoaded(3U)
                   && streamer.isLoaded(5U) && !streamer.isLoaded(4U)
                   && !streamer.isLoaded(9U),
           "moving across a cell transfers the budget to the nearest neighboring residency set");
    expect(streamer.update({6.0f, 0.0f, 0.0f}, transitions) && transitions.empty(),
           "a stable player position does not churn a budgeted grid");
    expect(!streamer.configure(zones, error, 0U) && streamer.zoneCount() == 9U
                   && streamer.loadedCount() == 3U,
           "a zero residency budget is rejected transactionally");
}

void testDirectionalPrefetchLoadsAheadAndCancelsBehind() {
    nuby::WorldZoneStreamer streamer;
    std::string error;
    const std::vector<nuby::WorldZoneDefinition> zones{
            {1U, {0.0f, 0.0f, 0.0f}, 5.0f, 7.0f, "center", 5.0f},
            {2U, {8.0f, 0.0f, 0.0f}, 5.0f, 7.0f, "east", 10.0f},
            {3U, {-8.0f, 0.0f, 0.0f}, 5.0f, 7.0f, "west", 10.0f}
    };
    expect(streamer.configure(zones, error, 2U),
           "directional streaming accepts explicit forward prefetch radii");
    std::vector<nuby::WorldZoneTransition> transitions;
    expect(streamer.update({0.0f, 0.0f, 0.0f}, transitions)
                   && transitions.size() == 1U && transitions[0].zoneId == 1U,
           "stationary player loads only the normal resident zone");
    expect(streamer.update({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, transitions)
                   && transitions.size() == 1U && transitions[0].zoneId == 2U
                   && transitions[0].load && streamer.isLoaded(1U),
           "forward motion prefetches the east sector while retaining the current sector");
    expect(streamer.update({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, transitions)
                   && transitions.empty() && streamer.isLoaded(2U),
           "a brief stop does not cancel a useful in-flight prefetched sector");
    expect(streamer.update({0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, transitions)
                   && transitions.size() == 2U && transitions[0].zoneId == 3U
                   && transitions[0].load && transitions[1].zoneId == 2U
                   && !transitions[1].load && streamer.isLoaded(1U),
           "reversing direction prioritizes the west sector and releases stale east prefetch");
}

void testLargeManifestKeepsDeterministicResidencyBounded() {
    constexpr std::size_t zoneCount = 4096U;
    nuby::WorldZoneStreamer streamer;
    std::vector<nuby::WorldZoneDefinition> zones;
    zones.reserve(zoneCount);
    for (std::size_t index = 0; index < zoneCount; ++index) {
        zones.push_back({static_cast<std::uint32_t>(index + 1U),
                {0.0f, 0.0f, 0.0f}, 5.0f, 8.0f, ""});
    }
    std::string error;
    expect(streamer.configure(zones, error, 4U),
           "the maximum supported world manifest can configure a small residency budget");
    std::vector<nuby::WorldZoneTransition> transitions;
    expect(streamer.update({0.0f, 0.0f, 0.0f}, transitions)
                   && streamer.loadedCount() == 4U && transitions.size() == 4U
                   && streamer.isLoaded(1U) && streamer.isLoaded(2U)
                   && streamer.isLoaded(3U) && streamer.isLoaded(4U)
                   && !streamer.isLoaded(5U),
           "a large overlapping manifest selects only the nearest deterministic four cells");
}

void testInvalidInputDoesNotMutateResidency() {
    nuby::WorldZoneStreamer streamer;
    std::string error;
    const std::vector<nuby::WorldZoneDefinition> zones{
            {1, {0.0f, 0.0f, 0.0f}, 3.0f, 5.0f, ""}
    };
    expect(streamer.configure(zones, error), "baseline zone config is valid");
    std::vector<nuby::WorldZoneTransition> transitions;
    expect(streamer.update({0.0f, 0.0f, 0.0f}, transitions)
                   && streamer.isLoaded(1), "baseline zone loads");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    transitions.push_back({99, true});
    expect(!streamer.update({nan, 0.0f, 0.0f}, transitions) && transitions.empty()
                   && streamer.isLoaded(1),
           "non-finite player positions are rejected without changing loaded zones");

    const std::vector<nuby::WorldZoneDefinition> duplicateZones{
            {2, {0.0f, 0.0f, 0.0f}, 3.0f, 5.0f, ""},
            {2, {10.0f, 0.0f, 0.0f}, 3.0f, 5.0f, ""}
    };
    expect(!streamer.configure(duplicateZones, error) && streamer.zoneCount() == 1
                   && streamer.isLoaded(1),
           "invalid reconfiguration is transactional and preserves live state");

    const std::vector<nuby::WorldZoneDefinition> invalidRadius{
            {3, {0.0f, 0.0f, 0.0f}, 5.0f, 4.0f, ""}
    };
    expect(!streamer.configure(invalidRadius, error) && streamer.isLoaded(1),
           "unload radius must exceed load radius and failure preserves residency");
    expect(streamer.update({std::numeric_limits<float>::max(), 0.0f,
                             std::numeric_limits<float>::max()}, transitions)
                   && !streamer.isLoaded(1),
           "very distant finite positions unload a zone without arithmetic overflow");
}

}  // namespace

int main() {
    testZoneLoadingUsesHorizontalDistanceAndStableOrdering();
    testGridResidencyHonorsDeterministicBudget();
    testDirectionalPrefetchLoadsAheadAndCancelsBehind();
    testLargeManifestKeepsDeterministicResidencyBounded();
    testInvalidInputDoesNotMutateResidency();
    if (failures != 0) {
        std::cerr << failures << " world streaming test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All world streaming tests passed\n";
    return EXIT_SUCCESS;
}
