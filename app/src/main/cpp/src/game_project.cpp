#include "nuby/game_project.hpp"

#include "nuby/scene_io.hpp"

#include <cctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace nuby {
namespace {
constexpr std::size_t kMaximumDescriptorLine = 512;
constexpr std::size_t kMaximumSceneBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumDisplayName = 128;
constexpr std::size_t kMaximumPackageIdentifier = 128;
constexpr std::size_t kMaximumScenePath = 256;

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool onlyWhitespaceRemains(std::istringstream& line) {
    line >> std::ws;
    return line.peek() == std::char_traits<char>::eof();
}

bool readLine(std::istream& input, std::string& line, std::string& error) {
    if (!std::getline(input, line) || line.size() > kMaximumDescriptorLine) {
        return fail(error, "Project descriptor is truncated or a record is too long");
    }
    return true;
}

bool validPackageIdentifier(const std::string& value) {
    if (value.empty() || value.size() > kMaximumPackageIdentifier) return false;
    std::size_t segmentStart = 0;
    unsigned int segmentCount = 0;
    while (segmentStart < value.size()) {
        const std::size_t separator = value.find('.', segmentStart);
        const std::size_t segmentEnd = separator == std::string::npos
                ? value.size() : separator;
        if (segmentEnd == segmentStart) return false;
        const unsigned char first = static_cast<unsigned char>(value[segmentStart]);
        if (!((first >= 'a' && first <= 'z') || first == '_')) return false;
        for (std::size_t index = segmentStart + 1; index < segmentEnd; ++index) {
            const unsigned char character = static_cast<unsigned char>(value[index]);
            if (!((character >= 'a' && character <= 'z')
                    || (character >= '0' && character <= '9') || character == '_')) {
                return false;
            }
        }
        ++segmentCount;
        if (separator == std::string::npos) break;
        segmentStart = separator + 1;
    }
    return segmentCount >= 2;
}

bool validRelativeScenePath(const std::string& value) {
    if (value.empty() || value.size() > kMaximumScenePath
            || value.front() == '/' || value.find('\\') != std::string::npos
            || value.find(':') != std::string::npos) {
        return false;
    }
    std::size_t segmentStart = 0;
    while (segmentStart < value.size()) {
        const std::size_t separator = value.find('/', segmentStart);
        const std::size_t segmentEnd = separator == std::string::npos
                ? value.size() : separator;
        if (segmentEnd == segmentStart) return false;
        const std::string segment = value.substr(segmentStart, segmentEnd - segmentStart);
        if (segment == "." || segment == "..") return false;
        for (unsigned char character : segment) {
            if (!((character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '_' || character == '-' || character == '.')) {
                return false;
            }
        }
        if (separator == std::string::npos) break;
        segmentStart = separator + 1;
    }
    return true;
}

bool validProject(const GameProject& project) {
    return !project.displayName.empty()
            && project.displayName.size() <= kMaximumDisplayName
            && project.displayName.find('\n') == std::string::npos
            && project.displayName.find('\r') == std::string::npos
            && validPackageIdentifier(project.packageIdentifier)
            && validRelativeScenePath(project.startupScene);
}

}  // namespace

bool GameProjectLoader::load(std::istream& descriptor,
                             const ProjectAssetReader& readAsset,
                             GameProject& project,
                             Scene& scene,
                             std::string& error) {
    error.clear();
    if (!readAsset) return fail(error, "Project asset reader is not configured");

    std::string line;
    if (!readLine(descriptor, line, error)) return false;
    std::istringstream header(line);
    header.imbue(std::locale::classic());
    std::string tag;
    unsigned int version = 0;
    if (!(header >> tag >> version) || tag != "NUBY_PROJECT"
            || version != kCurrentVersion || !onlyWhitespaceRemains(header)) {
        return fail(error, "Project descriptor header is invalid or unsupported");
    }

    GameProject candidate;
    if (!readLine(descriptor, line, error)) return false;
    std::istringstream nameRecord(line);
    if (!(nameRecord >> tag) || tag != "NAME" || !(nameRecord >> std::quoted(candidate.displayName))
            || !onlyWhitespaceRemains(nameRecord)) {
        return fail(error, "Project display name record is invalid");
    }
    if (!readLine(descriptor, line, error)) return false;
    std::istringstream packageRecord(line);
    if (!(packageRecord >> tag >> candidate.packageIdentifier) || tag != "PACKAGE_ID"
            || !onlyWhitespaceRemains(packageRecord)) {
        return fail(error, "Project package identifier record is invalid");
    }
    if (!readLine(descriptor, line, error)) return false;
    std::istringstream sceneRecord(line);
    if (!(sceneRecord >> tag >> candidate.startupScene) || tag != "STARTUP_SCENE"
            || !onlyWhitespaceRemains(sceneRecord)) {
        return fail(error, "Project startup scene record is invalid");
    }
    if (!readLine(descriptor, line, error)) return false;
    std::istringstream endRecord(line);
    if (!(endRecord >> tag) || tag != "END_PROJECT" || !onlyWhitespaceRemains(endRecord)) {
        return fail(error, "Project descriptor terminator is missing");
    }
    while (std::getline(descriptor, line)) {
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            return fail(error, "Unexpected records after project descriptor terminator");
        }
    }
    if (descriptor.bad()) return fail(error, "Project descriptor could not be read");
    if (!validProject(candidate)) return fail(error, "Project metadata contains invalid values");

    std::string sceneText;
    if (!readAsset(candidate.startupScene, sceneText, error)) {
        if (error.empty()) error = "Startup scene asset could not be read";
        return false;
    }
    if (sceneText.empty() || sceneText.size() > kMaximumSceneBytes) {
        return fail(error, "Startup scene is empty or exceeds the size limit");
    }

    Scene candidateScene;
    std::istringstream sceneInput(sceneText);
    sceneInput.imbue(std::locale::classic());
    if (!SceneSerializer::load(candidateScene, sceneInput, error)) return false;

    project = std::move(candidate);
    scene = std::move(candidateScene);
    return true;
}

bool GameProjectLoader::save(const GameProject& project, std::ostream& output,
                             std::string& error) {
    error.clear();
    if (!validProject(project)) return fail(error, "Project metadata contains invalid values");
    output.imbue(std::locale::classic());
    output << "NUBY_PROJECT " << kCurrentVersion << '\n'
           << "NAME " << std::quoted(project.displayName) << '\n'
           << "PACKAGE_ID " << project.packageIdentifier << '\n'
           << "STARTUP_SCENE " << project.startupScene << '\n'
           << "END_PROJECT\n";
    if (!output) return fail(error, "Project descriptor could not be written");
    return true;
}

}  // namespace nuby
