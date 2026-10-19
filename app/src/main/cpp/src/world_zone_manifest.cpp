#include "nuby/world_zone_manifest.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumManifestBytes = 1024U * 1024U;
constexpr std::size_t kMaximumManifestZones = 4096U;
constexpr std::size_t kMaximumManifestObjectives = 256U;
constexpr std::size_t kMaximumAssetPathLength = 1024U;

bool safeRelativeScenePath(const std::string& path) {
    if (path.empty()) return true;
    if (path.size() > kMaximumAssetPathLength || path.front() == '/' || path.front() == '\\') {
        return false;
    }
    std::size_t componentStart = 0U;
    for (std::size_t index = 0U; index <= path.size(); ++index) {
        if (index < path.size()) {
            const unsigned char character = static_cast<unsigned char>(path[index]);
            if (character < 0x20U || path[index] == '\\' || path[index] == ':') return false;
            if (path[index] != '/') continue;
        }
        const std::string component = path.substr(componentStart, index - componentStart);
        if (component.empty() || component == "." || component == "..") return false;
        componentStart = index + 1U;
    }
    return true;
}

const char* objectiveTypeName(WorldObjectiveType type) {
    switch (type) {
        case WorldObjectiveType::Relic: return "RELIC";
        case WorldObjectiveType::Gate: return "GATE";
    }
    return nullptr;
}

bool validObjective(const WorldObjectiveDefinition& objective) {
    if (objective.id == 0U || objectiveTypeName(objective.type) == nullptr) return false;
    for (float coordinate : objective.position) {
        if (!std::isfinite(coordinate) || std::fabs(coordinate) > 100000.0f) return false;
    }
    return true;
}

bool fail(std::string& error, std::size_t line, const char* reason) {
    error = "world manifest line " + std::to_string(line) + ": " + reason;
    return false;
}
}  // namespace

bool parseWorldZoneManifest(std::string_view source, WorldZoneManifest& manifest,
                            std::string& error) {
    error.clear();
    if (source.empty() || source.size() > kMaximumManifestBytes) {
        error = "world manifest is empty or exceeds the size limit";
        return false;
    }

    std::istringstream input{std::string(source)};
    std::string lineText;
    std::size_t lineNumber = 0U;
    bool foundHeader = false;
    bool foundBudget = false;
    unsigned int manifestVersion = 0U;
    WorldZoneManifest candidate;
    while (std::getline(input, lineText)) {
        ++lineNumber;
        const std::size_t comment = lineText.find('#');
        if (comment != std::string::npos) lineText.resize(comment);
        std::istringstream line(lineText);
        std::string keyword;
        if (!(line >> keyword)) continue;
        if (!foundHeader) {
            unsigned int version = 0U;
            if (keyword != "NUBY_WORLD_ZONES" || !(line >> version)
                    || (version != 1U && version != 2U)) {
                return fail(error, lineNumber, "unsupported world manifest version");
            }
            manifestVersion = version;
            std::string trailing;
            if (line >> trailing) return fail(error, lineNumber, "unexpected header fields");
            foundHeader = true;
            continue;
        }
        if (keyword == "MAX_RESIDENT") {
            std::size_t maximum = 0U;
            std::string trailing;
            if (foundBudget || !(line >> maximum) || maximum == 0U
                    || maximum > kMaximumManifestZones || (line >> trailing)) {
                return fail(error, lineNumber, "invalid or duplicate resident-zone limit");
            }
            candidate.maximumResidentZones = maximum;
            foundBudget = true;
            continue;
        }
        if (keyword == "OBJECTIVE") {
            std::string typeName;
            WorldObjectiveDefinition objective;
            if (manifestVersion != 2U || !foundBudget
                    || candidate.objectives.size() >= kMaximumManifestObjectives
                    || !(line >> typeName >> objective.id >> objective.position[0]
                                 >> objective.position[1] >> objective.position[2])) {
                return fail(error, lineNumber, "objective record is unsupported or malformed");
            }
            if (typeName == "RELIC") objective.type = WorldObjectiveType::Relic;
            else if (typeName == "GATE") objective.type = WorldObjectiveType::Gate;
            else return fail(error, lineNumber, "unknown objective type");
            std::string trailing;
            if (line >> trailing) return fail(error, lineNumber, "unexpected objective fields");
            if (objective.id != candidate.objectives.size() + 1U
                    || !validObjective(objective)) {
                return fail(error, lineNumber, "objective IDs or positions are invalid");
            }
            candidate.objectives.push_back(objective);
            continue;
        }
        if (keyword != "ZONE" || !foundBudget
                || candidate.zones.size() >= kMaximumManifestZones) {
            return fail(error, lineNumber, "expected ZONE or OBJECTIVE after MAX_RESIDENT");
        }
        WorldZoneDefinition zone;
        if (!(line >> zone.id >> zone.center[0] >> zone.center[1] >> zone.center[2]
                    >> zone.loadRadius >> zone.unloadRadius >> zone.prefetchRadius
                    >> std::quoted(zone.sceneAssetPath))) {
            return fail(error, lineNumber, "zone record is incomplete or malformed");
        }
        std::string trailing;
        if (line >> trailing) return fail(error, lineNumber, "unexpected zone fields");
        if (zone.id != candidate.zones.size() + 1U) {
            return fail(error, lineNumber, "zone IDs must be contiguous and ordered from one");
        }
        if (!safeRelativeScenePath(zone.sceneAssetPath)) {
            return fail(error, lineNumber, "scene asset path is unsafe or invalid");
        }
        candidate.zones.push_back(std::move(zone));
    }
    if (!foundHeader || !foundBudget || candidate.zones.empty()
            || candidate.maximumResidentZones > candidate.zones.size()
            || (manifestVersion == 2U && candidate.objectives.empty())) {
        error = "world manifest is missing required records or has an invalid resident limit";
        return false;
    }

    WorldZoneStreamer validator;
    if (!validator.configure(candidate.zones, error, candidate.maximumResidentZones)) {
        error = "world manifest contains invalid zones: " + error;
        return false;
    }
    manifest = std::move(candidate);
    error.clear();
    return true;
}

bool serializeWorldZoneManifest(const WorldZoneManifest& manifest,
                                std::string& destination, std::string& error) {
    error.clear();
    if (manifest.zones.empty() || manifest.zones.size() > kMaximumManifestZones
            || manifest.objectives.size() > kMaximumManifestObjectives
            || manifest.maximumResidentZones == 0U
            || manifest.maximumResidentZones > manifest.zones.size()) {
        error = "world manifest counts or residency budget are invalid";
        return false;
    }
    WorldZoneStreamer validator;
    if (!validator.configure(manifest.zones, error, manifest.maximumResidentZones)) {
        error = "world manifest contains invalid zones: " + error;
        return false;
    }
    for (std::size_t index = 0U; index < manifest.zones.size(); ++index) {
        const WorldZoneDefinition& zone = manifest.zones[index];
        if (zone.id != index + 1U || !safeRelativeScenePath(zone.sceneAssetPath)) {
            error = "world manifest IDs or asset paths are invalid";
            return false;
        }
    }

    for (std::size_t index = 0U; index < manifest.objectives.size(); ++index) {
        if (manifest.objectives[index].id != index + 1U
                || !validObjective(manifest.objectives[index])) {
            error = "world manifest objective IDs or positions are invalid";
            return false;
        }
    }

    const unsigned int version = manifest.objectives.empty() ? 1U : 2U;
    std::ostringstream output;
    output << "NUBY_WORLD_ZONES " << version << "\nMAX_RESIDENT "
           << manifest.maximumResidentZones << '\n';
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (const WorldZoneDefinition& zone : manifest.zones) {
        output << "ZONE " << zone.id << ' ' << zone.center[0] << ' ' << zone.center[1]
               << ' ' << zone.center[2] << ' ' << zone.loadRadius << ' '
               << zone.unloadRadius << ' ' << zone.prefetchRadius << ' '
               << std::quoted(zone.sceneAssetPath) << '\n';
    }
    for (const WorldObjectiveDefinition& objective : manifest.objectives) {
        output << "OBJECTIVE " << objectiveTypeName(objective.type) << ' '
               << objective.id << ' ' << objective.position[0] << ' '
               << objective.position[1] << ' ' << objective.position[2] << '\n';
    }
    const std::string candidate = output.str();
    if (candidate.size() > kMaximumManifestBytes) {
        error = "serialized world manifest exceeds the size limit";
        return false;
    }
    destination = candidate;
    return true;
}

}  // namespace nuby
