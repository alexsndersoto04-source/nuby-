#pragma once

#include "nuby/world_streaming.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nuby {

enum class WorldObjectiveType {
    Relic,
    Gate
};

struct WorldObjectiveDefinition {
    std::uint32_t id = 0U;
    WorldObjectiveType type = WorldObjectiveType::Relic;
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
};

struct WorldZoneManifest {
    std::size_t maximumResidentZones = 1U;
    std::vector<WorldZoneDefinition> zones;
    std::vector<WorldObjectiveDefinition> objectives;
};

// Text manifest for authored, data-driven streamed worlds. Zone IDs are
// required to be 1..N because callers use them as stable compact handles.
bool parseWorldZoneManifest(std::string_view source, WorldZoneManifest& manifest,
                            std::string& error);
bool serializeWorldZoneManifest(const WorldZoneManifest& manifest,
                                std::string& destination, std::string& error);

}  // namespace nuby
