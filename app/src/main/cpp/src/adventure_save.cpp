#include "nuby/adventure_save.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace nuby {
namespace {
constexpr std::size_t kMaximumSaveBytes = 4096U;
constexpr std::uint8_t kAllRelicsMask = 0x07U;
constexpr float kGateCompletionRadius = 1.8f;

std::uint32_t checksum(const std::string& value) {
    std::uint32_t hash = 2166136261U;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 16777619U;
    }
    return hash;
}

bool lineHasOnlyExpectedValues(std::istringstream& line) {
    line >> std::ws;
    return line.eof();
}

bool readNamedLine(std::istringstream& source, const char* expectedName,
                   std::string& value) {
    std::string text;
    if (!std::getline(source, text)) return false;
    std::istringstream line(text);
    line.imbue(std::locale::classic());
    std::string name;
    if (!(line >> name >> value) || name != expectedName
            || !lineHasOnlyExpectedValues(line)) return false;
    return true;
}

bool readPlayerLine(std::istringstream& source, AdventureSaveData& candidate) {
    std::string text;
    if (!std::getline(source, text)) return false;
    std::istringstream line(text);
    line.imbue(std::locale::classic());
    std::string name;
    if (!(line >> name >> candidate.playerX >> candidate.playerZ >> candidate.playerYaw)
            || name != "PLAYER" || !lineHasOnlyExpectedValues(line)) return false;
    return true;
}

bool readCameraLine(std::istringstream& source, AdventureSaveData& candidate) {
    std::string text;
    if (!std::getline(source, text)) return false;
    std::istringstream line(text);
    line.imbue(std::locale::classic());
    std::string name;
    if (!(line >> name >> candidate.cameraYaw >> candidate.cameraPitch)
            || name != "CAMERA" || !lineHasOnlyExpectedValues(line)) return false;
    return true;
}

bool readGateLine(std::istringstream& source, AdventureSaveData& candidate) {
    std::string text;
    if (!std::getline(source, text)) return false;
    std::istringstream line(text);
    line.imbue(std::locale::classic());
    std::string name;
    if (!(line >> name >> candidate.gateX >> candidate.gateZ)
            || name != "GATE" || !lineHasOnlyExpectedValues(line)) return false;
    return true;
}

bool readMaskLine(std::istringstream& source, AdventureSaveData& candidate) {
    std::string text;
    if (!std::getline(source, text)) return false;
    std::istringstream line(text);
    line.imbue(std::locale::classic());
    std::string name;
    unsigned int mask = 0U;
    if (!(line >> name >> mask) || name != "RELICS" || mask > kAllRelicsMask
            || !lineHasOnlyExpectedValues(line)) return false;
    candidate.collectedRelicMask = static_cast<std::uint8_t>(mask);
    return true;
}

bool readCompletedLine(std::istringstream& source, AdventureSaveData& candidate) {
    std::string text;
    if (!std::getline(source, text)) return false;
    std::istringstream line(text);
    line.imbue(std::locale::classic());
    std::string name;
    int completed = -1;
    if (!(line >> name >> completed) || name != "COMPLETED"
            || (completed != 0 && completed != 1)
            || !lineHasOnlyExpectedValues(line)) return false;
    candidate.completed = completed == 1;
    return true;
}

bool parseChecksumLine(const std::string& lineText, std::uint32_t& output) {
    std::istringstream line(lineText);
    line.imbue(std::locale::classic());
    std::string name;
    std::string encoded;
    if (!(line >> name >> encoded) || name != "CHECKSUM" || encoded.size() != 8U
            || !lineHasOnlyExpectedValues(line)) return false;
    std::istringstream value(encoded);
    value >> std::hex >> output;
    return !value.fail() && value.eof();
}

}  // namespace

bool validateAdventureSaveData(const AdventureSaveData& data, std::string& error) {
    error.clear();
    if (!std::isfinite(data.playerX) || !std::isfinite(data.playerZ)
            || !std::isfinite(data.playerYaw) || !std::isfinite(data.cameraYaw)
            || !std::isfinite(data.cameraPitch) || !std::isfinite(data.gateX)
            || !std::isfinite(data.gateZ)) {
        error = "save contains non-finite transform values";
        return false;
    }
    if (data.playerX < -16.0f || data.playerX > 16.0f
            || data.playerZ < -20.0f || data.playerZ > 3.0f
            || std::abs(data.playerYaw) > 1000000.0f
            || std::abs(data.cameraYaw) > 1000000.0f
            || data.cameraPitch < 0.08f || data.cameraPitch > 0.72f
            || data.gateX < -16.0f || data.gateX > 16.0f
            || data.gateZ < -20.0f || data.gateZ > 3.0f) {
        error = "save transform is outside the adventure bounds";
        return false;
    }
    if ((data.collectedRelicMask & static_cast<std::uint8_t>(~kAllRelicsMask)) != 0U) {
        error = "save has unknown relic bits";
        return false;
    }
    if (data.completed) {
        const float dx = data.playerX - data.gateX;
        const float dz = data.playerZ - data.gateZ;
        if (data.collectedRelicMask != kAllRelicsMask
                || dx * dx + dz * dz > kGateCompletionRadius * kGateCompletionRadius) {
            error = "completed save does not satisfy the gate objective";
            return false;
        }
    }
    return true;
}

bool serializeAdventureSave(const AdventureSaveData& data, std::string& output,
                            std::string& error) {
    if (!validateAdventureSaveData(data, error)) return false;
    std::ostringstream body;
    body.imbue(std::locale::classic());
    body << "NUBY_ADVENTURE_SAVE 2\n"
         << "WORLD " << kAdventureSaveWorldId << '\n'
         << std::fixed << std::setprecision(6)
         << "PLAYER " << data.playerX << ' ' << data.playerZ << ' '
         << data.playerYaw << '\n'
         << "CAMERA " << data.cameraYaw << ' ' << data.cameraPitch << '\n'
         << "GATE " << data.gateX << ' ' << data.gateZ << '\n'
         << "RELICS " << static_cast<unsigned int>(data.collectedRelicMask) << '\n'
         << "COMPLETED " << (data.completed ? 1 : 0) << '\n';
    const std::string bodyText = body.str();
    std::ostringstream serialized;
    serialized.imbue(std::locale::classic());
    serialized << bodyText << "CHECKSUM " << std::hex << std::setw(8)
               << std::setfill('0') << checksum(bodyText) << '\n';
    const std::string candidate = serialized.str();
    if (candidate.size() > kMaximumSaveBytes) {
        error = "serialized save exceeds the size limit";
        return false;
    }
    output = candidate;
    error.clear();
    return true;
}

bool parseAdventureSave(const std::string& source, AdventureSaveData& output,
                        std::string& error) {
    error.clear();
    if (source.empty() || source.size() > kMaximumSaveBytes) {
        error = "save is empty or exceeds the size limit";
        return false;
    }
    const std::size_t checksumStart = source.rfind("CHECKSUM ");
    if (checksumStart == std::string::npos
            || (checksumStart != 0U && source[checksumStart - 1U] != '\n')) {
        error = "save checksum is missing";
        return false;
    }
    const std::string bodyText = source.substr(0U, checksumStart);
    std::istringstream checksumSource(source.substr(checksumStart));
    std::string checksumLine;
    std::uint32_t parsedChecksum = 0U;
    if (!std::getline(checksumSource, checksumLine)
            || !parseChecksumLine(checksumLine, parsedChecksum)) {
        error = "save checksum line is malformed";
        return false;
    }
    checksumSource >> std::ws;
    if (!checksumSource.eof() || parsedChecksum != checksum(bodyText)) {
        error = "save checksum does not match its contents";
        return false;
    }

    AdventureSaveData candidate;
    std::istringstream body(bodyText);
    body.imbue(std::locale::classic());
    std::string versionLine;
    std::istringstream versionRecord;
    std::string versionTag;
    unsigned int version = 0U;
    std::string versionTrailing;
    if (!std::getline(body, versionLine)) {
        error = "save header is missing";
        return false;
    }
    versionRecord.str(versionLine);
    versionRecord.imbue(std::locale::classic());
    if (!(versionRecord >> versionTag >> version) || versionTag != "NUBY_ADVENTURE_SAVE"
            || (version != 1U && version != 2U) || (versionRecord >> versionTrailing)) {
        error = "save version is unsupported or malformed";
        return false;
    }
    std::string worldId;
    if (!readNamedLine(body, "WORLD", worldId) || worldId != kAdventureSaveWorldId
            || !readPlayerLine(body, candidate) || !readCameraLine(body, candidate)
            || (version >= 2U && !readGateLine(body, candidate))
            || !readMaskLine(body, candidate) || !readCompletedLine(body, candidate)) {
        error = "save fields are malformed or belong to another world version";
        return false;
    }
    body >> std::ws;
    if (!body.eof()) {
        error = "save contains unexpected trailing data";
        return false;
    }
    if (!validateAdventureSaveData(candidate, error)) return false;
    output = candidate;
    error.clear();
    return true;
}

}  // namespace nuby
