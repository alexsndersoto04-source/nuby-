#pragma once

#include "nuby/world_streaming.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace nuby {

struct WorldZoneManifest {
    std::size_t maximumResidentZones = 1U;
    std::vector<WorldZoneDefinition> zones;
};

// Text manifest for authored, data-driven streamed worlds. Zone IDs are
// required to be 1..N because callers use them as stable compact handles.
bool parseWorldZoneManifest(std::string_view source, WorldZoneManifest& manifest,
                            std::string& error);
bool serializeWorldZoneManifest(const WorldZoneManifest& manifest,
                                std::string& destination, std::string& error);

}  // namespace nuby
