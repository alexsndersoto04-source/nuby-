#pragma once

#include <cstdint>
#include <string>

namespace nuby {

struct AdventureSaveData {
    float playerX = 0.0f;
    float playerZ = 0.0f;
    float playerYaw = 0.0f;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.3f;
    float gateX = 0.0f;
    float gateZ = -11.5f;
    std::uint8_t collectedRelicMask = 0U;
    bool completed = false;
};

// Save files are tied to the current authored adventure layout so changes to
// the map cannot silently restore progress into an incompatible world.
constexpr const char* kAdventureSaveWorldId = "sun_ruins";

bool validateAdventureSaveData(const AdventureSaveData& data, std::string& error);
bool serializeAdventureSave(const AdventureSaveData& data, std::string& output,
                            std::string& error);
bool parseAdventureSave(const std::string& source, AdventureSaveData& output,
                        std::string& error);

}  // namespace nuby
