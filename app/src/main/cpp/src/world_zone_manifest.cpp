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
            if (keyword != "NUBY_WORLD_ZONES" || !(line >> version) || version != 1U) {
                return fail(error, lineNumber, "expected NUBY_WORLD_ZONES 1 header");
            }
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
        if (keyword != "ZONE" || !foundBudget
                || candidate.zones.size() >= kMaximumManifestZones) {
            return fail(error, lineNumber, "expected ZONE after MAX_RESIDENT");
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
            || candidate.maximumResidentZones > candidate.zones.size()) {
        error = "world manifest is missing its header, resident limit, zones, or has an invalid limit";
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

    std::ostringstream output;
    output << "NUBY_WORLD_ZONES 1\nMAX_RESIDENT " << manifest.maximumResidentZones << '\n';
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (const WorldZoneDefinition& zone : manifest.zones) {
        output << "ZONE " << zone.id << ' ' << zone.center[0] << ' ' << zone.center[1]
               << ' ' << zone.center[2] << ' ' << zone.loadRadius << ' '
               << zone.unloadRadius << ' ' << zone.prefetchRadius << ' '
               << std::quoted(zone.sceneAssetPath) << '\n';
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
