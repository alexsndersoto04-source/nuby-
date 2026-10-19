#include "nuby/world_zone_manifest.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

const char* kValidManifest =
        "# Authored sector graph\n"
        "NUBY_WORLD_ZONES 1\n"
        "MAX_RESIDENT 2\n"
        "ZONE 1 0 0 0 5 7 5 \"scenes/zones/center.scene\"\n"
        "ZONE 2 8 0 0 5 7 10 \"scenes/zones/east.scene\"\n";

void testParseAndRoundTripPreserveAuthoredStreamingRules() {
    nuby::WorldZoneManifest manifest;
    std::string error;
    expect(nuby::parseWorldZoneManifest(kValidManifest, manifest, error)
                   && error.empty() && manifest.maximumResidentZones == 2U
                   && manifest.zones.size() == 2U
                   && manifest.zones[1].center[0] == 8.0f
                   && manifest.zones[1].prefetchRadius == 10.0f
                   && manifest.zones[1].sceneAssetPath == "scenes/zones/east.scene",
           "parser loads zone positions, prefetch ranges, budget and scene references");
    std::string serialized;
    expect(nuby::serializeWorldZoneManifest(manifest, serialized, error)
                   && error.empty(),
           "valid manifest serializes deterministically");
    nuby::WorldZoneManifest decoded;
    expect(nuby::parseWorldZoneManifest(serialized, decoded, error)
                   && decoded.maximumResidentZones == manifest.maximumResidentZones
                   && decoded.zones.size() == manifest.zones.size()
                   && decoded.zones[0].sceneAssetPath == manifest.zones[0].sceneAssetPath
                   && decoded.zones[1].center == manifest.zones[1].center,
           "serialized world manifest round-trips without losing zone behavior");
}

void testPackagedAdventureManifestMatchesTheAuthoredGrid() {
    std::ifstream file("app/src/main/assets/worlds/sun_ruins.world", std::ios::binary);
    expect(file.good(), "packaged Sun Ruins world manifest exists");
    if (!file.good()) return;
    const std::string source((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    nuby::WorldZoneManifest manifest;
    std::string error;
    expect(nuby::parseWorldZoneManifest(source, manifest, error)
                   && manifest.zones.size() == 9U
                   && manifest.maximumResidentZones == 4U
                   && manifest.zones[8].sceneAssetPath == "scenes/zones/ruins_03.scene",
           "packaged adventure map loads all nine authored cells and its four-cell cap");
}

void testMalformedManifestsAreRejectedTransactionally() {
    nuby::WorldZoneManifest output;
    output.maximumResidentZones = 7U;
    output.zones.push_back({1U, {1.0f, 2.0f, 3.0f}, 2.0f, 3.0f, "kept.scene"});
    const std::string invalid[] = {
        "",
        "NUBY_WORLD_ZONES 2\nMAX_RESIDENT 1\nZONE 1 0 0 0 1 2 2 \"a.scene\"\n",
        "NUBY_WORLD_ZONES 1\nZONE 1 0 0 0 1 2 2 \"a.scene\"\n",
        "NUBY_WORLD_ZONES 1\nMAX_RESIDENT 2\nZONE 1 0 0 0 1 2 2 \"a.scene\"\n",
        "NUBY_WORLD_ZONES 1\nMAX_RESIDENT 1\nZONE 2 0 0 0 1 2 2 \"a.scene\"\n",
        "NUBY_WORLD_ZONES 1\nMAX_RESIDENT 1\nZONE 1 0 0 0 4 2 5 \"a.scene\"\n",
        "NUBY_WORLD_ZONES 1\nMAX_RESIDENT 1\nZONE 1 0 0 0 1 2 5 \"../escape.scene\"\n",
        "NUBY_WORLD_ZONES 1\nMAX_RESIDENT 1\nZONE 1 nan 0 0 1 2 3 \"a.scene\"\n"
    };
    for (const std::string& source : invalid) {
        std::string error;
        expect(!nuby::parseWorldZoneManifest(source, output, error) && !error.empty()
                       && output.maximumResidentZones == 7U
                       && output.zones.size() == 1U
                       && output.zones[0].sceneAssetPath == "kept.scene",
               "invalid manifest does not replace the caller's previous world config");
    }
}

void testSerializerRejectsUnsafeAuthoredPaths() {
    nuby::WorldZoneManifest manifest;
    manifest.maximumResidentZones = 1U;
    manifest.zones.push_back({1U, {0.0f, 0.0f, 0.0f}, 5.0f, 7.0f, "../outside.scene"});
    std::string output = "preserve this";
    std::string error;
    expect(!nuby::serializeWorldZoneManifest(manifest, output, error)
                   && !error.empty() && output == "preserve this",
           "unsafe paths are rejected transactionally when serializing");
}
}  // namespace

int main() {
    testParseAndRoundTripPreserveAuthoredStreamingRules();
    testPackagedAdventureManifestMatchesTheAuthoredGrid();
    testMalformedManifestsAreRejectedTransactionally();
    testSerializerRejectsUnsafeAuthoredPaths();
    if (failures != 0) {
        std::cerr << failures << " world zone manifest test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All world zone manifest tests passed\n";
    return EXIT_SUCCESS;
}
